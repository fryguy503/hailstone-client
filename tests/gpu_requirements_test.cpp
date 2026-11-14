#include <doctest/doctest.h>

#include "render/gpu_requirements.h"

#include <string>
#include <utility>
#include <vector>

using namespace hailstone::render;

namespace {

GpuSupport usable_gpu(std::string name, GpuKind kind = GpuKind::Other) {
    GpuSupport gpu;
    gpu.name = std::move(name);
    gpu.kind = kind;
    gpu.api_major = 1;
    gpu.api_minor = 3;
    gpu.dynamic_rendering = true;
    gpu.synchronization2 = true;
    gpu.runtime_descriptor_array = true;
    gpu.nonuniform_sampled_image_indexing = true;
    gpu.descriptor_binding_partially_bound = true;
    gpu.descriptor_binding_variable_count = true;
    gpu.separate_depth_stencil_layouts = true;
    gpu.presents = true;
    return gpu;
}

void check_selected(const GpuSelection& selection, std::size_t expected) {
    REQUIRE(selection.index.has_value());
    CHECK(*selection.index == expected);
    CHECK(selection.error.empty());
}

} // namespace

TEST_CASE("GPU selection: automatic preference and first-on-tie are preserved") {
    std::vector<GpuSupport> gpus{usable_gpu("Software"), usable_gpu("Integrated", GpuKind::Integrated),
                                 usable_gpu("Discrete A", GpuKind::Discrete),
                                 usable_gpu("Discrete B", GpuKind::Discrete)};
    check_selected(choose_gpu(gpus), 2);
    check_selected(choose_gpu(gpus, ""), 2);
    gpus[2].presents = false;
    check_selected(choose_gpu(gpus), 3);
    gpus[3].dynamic_rendering = false;
    check_selected(choose_gpu(gpus), 1);
    gpus[1].synchronization2 = false;
    check_selected(choose_gpu(gpus), 0);

    const std::vector<GpuSupport> tied{usable_gpu("Other A"), usable_gpu("Other B")};
    check_selected(choose_gpu(tied), 0);
}

TEST_CASE("GPU selection: an exact requested model overrides automatic priority") {
    const std::vector<GpuSupport> gpus{usable_gpu("Software"), usable_gpu("Integrated", GpuKind::Integrated),
                                      usable_gpu("Discrete A", GpuKind::Discrete),
                                      usable_gpu("Discrete B", GpuKind::Discrete)};
    check_selected(choose_gpu(gpus, "Discrete B"), 3);
    check_selected(choose_gpu(gpus, "Integrated"), 1);
    check_selected(choose_gpu(gpus, "Software"), 0);
}

TEST_CASE("GPU selection: a missing or inexact requested model never falls back") {
    const std::vector<GpuSupport> gpus{usable_gpu("Integrated", GpuKind::Integrated),
                                      usable_gpu("Discrete A", GpuKind::Discrete)};
    for (const char* requested : {"Missing", "discrete a", "Discrete", "Discrete A ", " "}) {
        INFO(requested);
        const auto selection = choose_gpu(gpus, requested);
        CHECK_FALSE(selection.index.has_value());
        CHECK(selection.error.find("was not found") != std::string::npos);
        CHECK(selection.error.find("Integrated") != std::string::npos);
        CHECK(selection.error.find("Discrete A") != std::string::npos);
    }
}

TEST_CASE("GPU selection: duplicate exact models are ambiguous even if only one is usable") {
    std::vector<GpuSupport> gpus{usable_gpu("Same Model", GpuKind::Discrete),
                                 usable_gpu("Same Model", GpuKind::Discrete), usable_gpu("Different")};
    const auto both_usable = choose_gpu(gpus, "Same Model");
    CHECK_FALSE(both_usable.index.has_value());
    CHECK(both_usable.error.find("ambiguous") != std::string::npos);
    CHECK(both_usable.error.find("2 GPUs") != std::string::npos);
    gpus[1].presents = false;
    const auto one_usable = choose_gpu(gpus, "Same Model");
    CHECK_FALSE(one_usable.index.has_value());
    CHECK(one_usable.error == both_usable.error);
    check_selected(choose_gpu(gpus), 0);
}

TEST_CASE("GPU selection: a requested unsupported GPU reports its missing requirements") {
    std::vector<GpuSupport> gpus{usable_gpu("Good", GpuKind::Integrated),
                                 usable_gpu("Unsupported", GpuKind::Discrete)};
    gpus[1].synchronization2 = false;
    gpus[1].presents = false;
    const auto unsupported = choose_gpu(gpus, "Unsupported");
    CHECK_FALSE(unsupported.index.has_value());
    CHECK(unsupported.error.find("requested GPU \"Unsupported\"") != std::string::npos);
    CHECK(unsupported.error.find("synchronization2") != std::string::npos);
    CHECK(unsupported.error.find("drawing to this window") != std::string::npos);
    check_selected(choose_gpu(gpus), 0);

    gpus[1].api_minor = 2;
    const auto old = choose_gpu(gpus, "Unsupported");
    CHECK_FALSE(old.index.has_value());
    CHECK(old.error.find("Vulkan 1.3 (its driver offers 1.2)") != std::string::npos);
    CHECK(old.error.find("synchronization2") == std::string::npos);
}

TEST_CASE("GPU selection: empty or entirely unsupported enumeration fails clearly") {
    const auto empty = choose_gpu({});
    CHECK_FALSE(empty.index.has_value());
    CHECK(empty.error == no_usable_gpu_text({}));
    const auto requested_empty = choose_gpu({}, "Unavailable");
    CHECK_FALSE(requested_empty.index.has_value());
    CHECK(requested_empty.error.find("requested GPU \"Unavailable\"") != std::string::npos);
    CHECK(requested_empty.error.find("Vulkan found no GPU at all") != std::string::npos);

    std::vector<GpuSupport> gpus{usable_gpu("Old"), usable_gpu("No presentation")};
    gpus[0].api_minor = 2;
    gpus[1].presents = false;
    const auto unsupported = choose_gpu(gpus);
    CHECK_FALSE(unsupported.index.has_value());
    CHECK(unsupported.error == no_usable_gpu_text(gpus));

    gpus.push_back(usable_gpu("Last usable"));
    check_selected(choose_gpu(gpus), 2);
    check_selected(choose_gpu(gpus, "Last usable"), 2);
}

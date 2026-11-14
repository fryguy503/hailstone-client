#include <doctest/doctest.h>

#include <memory>

#include "render/gpu.h"
#include "render/gpu_shared_cache.h"
#include "render/retire_queue.h"

using namespace hailstone::render;

namespace {

std::shared_ptr<GpuSharedTexture> counted_texture(unsigned& releases) {
    return {new GpuSharedTexture{}, [&releases](GpuSharedTexture* texture) {
        ++releases;
        delete texture;
    }};
}

} // namespace

TEST_CASE("GPU imported texture destruction releases only its local owner") {
    unsigned releases = 0;
    Texture first;
    first.shared = counted_texture(releases);
    Texture second = first;
    first.destroy(VK_NULL_HANDLE, VK_NULL_HANDLE);
    CHECK_FALSE(first.shared);
    CHECK(first.image == VK_NULL_HANDLE);
    CHECK(first.view == VK_NULL_HANDLE);
    CHECK(releases == 0);
    REQUIRE(second.shared);
    second.destroy(VK_NULL_HANDLE, VK_NULL_HANDLE);
    CHECK_FALSE(second.shared);
    CHECK(releases == 1);
    second.destroy(VK_NULL_HANDLE, VK_NULL_HANDLE);
    CHECK(releases == 1);
}

TEST_CASE("GPU imported texture owner survives both retirement completion points") {
    unsigned releases = 0;
    Texture texture;
    texture.shared = counted_texture(releases);
    RetireQueue retired;
    retired.push({7, 11}, [held = texture]() mutable { held.destroy(VK_NULL_HANDLE, VK_NULL_HANDLE); });
    texture.destroy(VK_NULL_HANDLE, VK_NULL_HANDLE);
    CHECK(releases == 0);
    retired.collect({6, 11});
    CHECK(releases == 0);
    retired.collect({7, 10});
    CHECK(releases == 0);
    retired.collect({7, 11});
    CHECK(releases == 1);
}

TEST_CASE("GPU imported texture moves leave an empty independently destroyable source") {
    unsigned releases = 0;
    Texture source;
    source.shared = counted_texture(releases);
    source.width = 4;
    source.height = 8;
    Texture destination = std::move(source);
    CHECK_FALSE(source.shared);
    CHECK(source.image == VK_NULL_HANDLE);
    CHECK(source.view == VK_NULL_HANDLE);
    CHECK(source.allocation == VK_NULL_HANDLE);
    CHECK(source.width == 0);
    CHECK(source.height == 0);
    source.destroy(VK_NULL_HANDLE, VK_NULL_HANDLE);
    CHECK(releases == 0);
    CHECK(destination.width == 4);
    CHECK(destination.height == 8);
    Texture assigned;
    assigned = std::move(destination);
    CHECK_FALSE(destination.shared);
    CHECK(destination.width == 0);
    destination.destroy(VK_NULL_HANDLE, VK_NULL_HANDLE);
    CHECK(releases == 0);
    assigned.destroy(VK_NULL_HANDLE, VK_NULL_HANDLE);
    CHECK(releases == 1);
}

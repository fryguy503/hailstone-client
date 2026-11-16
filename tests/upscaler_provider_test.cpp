// Upscaler providers (render/upscaler_provider.h, sdk/hailstone_upscaler.h): the requirements a provider adds to the
// Vulkan device and their checks, the names and the Options choices, finding the installed ones, and loading the
// example provider (plugins/example/upscaler) built as fixtures. No GPU: what runs on one (create, resize, evaluate)
// is checked by running the client with --upscaler example.
#include <doctest/doctest.h>

#include <fstream>

#include "platform/shared_library.h"
#include "render/temporal.h"
#include "render/upscaler_provider.h"
#include "scratch_dir.h"

using namespace hailstone::render;
using hailstone::test::ScratchDir;

namespace {
const std::filesystem::path kFixtures = HAILSTONE_TEST_UPSCALERS_DIR;

void touch(const std::filesystem::path& p) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}
} // namespace

TEST_CASE("upscaler provider features: added beside the client's, missing ones named") {
    DeviceFeatures client;
    client.f2.features.samplerAnisotropy = VK_TRUE;
    client.f13.dynamicRendering = VK_TRUE;
    DeviceFeatures wanted;
    wanted.f2.features.shaderInt64 = VK_TRUE;
    wanted.f12.shaderInt8 = VK_TRUE;
    wanted.f13.shaderIntegerDotProduct = VK_TRUE;
    add_features(client, wanted);
    CHECK(client.f2.features.samplerAnisotropy == VK_TRUE); // the client's own stay
    CHECK(client.f13.dynamicRendering == VK_TRUE);
    CHECK(client.f2.features.shaderInt64 == VK_TRUE);
    CHECK(client.f12.shaderInt8 == VK_TRUE);
    CHECK(client.f13.shaderIntegerDotProduct == VK_TRUE);
    CHECK(client.f2.features.geometryShader == VK_FALSE); // nothing else turned on
    CHECK(client.f12.bufferDeviceAddress == VK_FALSE);

    // The chain is the four structs in order, also in a copy.
    const DeviceFeatures copy = client;
    CHECK(copy.f2.pNext == &copy.f11);
    CHECK(copy.f11.pNext == &copy.f12);
    CHECK(copy.f12.pNext == &copy.f13);
    CHECK(copy.f13.pNext == nullptr);

    DeviceFeatures have = wanted;
    CHECK(missing_features(wanted, have).empty());
    have.f12.shaderInt8 = VK_FALSE;
    have.f2.features.shaderInt64 = VK_FALSE;
    const auto missing = missing_features(wanted, have);
    REQUIRE(missing.size() == 2);
    CHECK(missing[0].starts_with("Vulkan 1.0 feature "));
    CHECK(missing[1].starts_with("Vulkan 1.2 feature "));
    // The first and the last field of each struct are walked too.
    DeviceFeatures edges;
    edges.f2.features.robustBufferAccess = VK_TRUE;
    edges.f2.features.inheritedQueries = VK_TRUE;
    edges.f11.storageBuffer16BitAccess = VK_TRUE;
    edges.f11.shaderDrawParameters = VK_TRUE;
    edges.f12.samplerMirrorClampToEdge = VK_TRUE;
    edges.f12.subgroupBroadcastDynamicId = VK_TRUE;
    edges.f13.robustImageAccess = VK_TRUE;
    edges.f13.maintenance4 = VK_TRUE;
    CHECK(missing_features(edges, DeviceFeatures{}).size() == 8);
    DeviceFeatures sum;
    add_features(sum, edges);
    CHECK(missing_features(edges, sum).empty());
}

TEST_CASE("upscaler provider features: a provider's own extension structs, gathered after the four") {
    // XeSS's way: a struct of its own (mutableDescriptorType has no core field) put in the middle of the chain.
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutable_type{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    mutable_type.mutableDescriptorType = VK_TRUE;
    VkPhysicalDeviceShaderIntegerDotProductFeatures other{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES};
    DeviceFeatures f;
    mutable_type.pNext = f.f2.pNext;
    f.f2.pNext = &mutable_type;
    f.f13.pNext = &other;
    REQUIRE(gather_extra_features(f));
    CHECK(f.f2.pNext == &f.f11);
    CHECK(f.f13.pNext == &mutable_type);
    CHECK(mutable_type.pNext == &other);
    CHECK(other.pNext == nullptr);
    CHECK(f.extra == &mutable_type);
    // They go into the device's chain after the client's four, and stay in a copy.
    DeviceFeatures device;
    add_features(device, f);
    CHECK(device.f13.pNext == &mutable_type);
    const DeviceFeatures copy = device;
    CHECK(copy.f13.pNext == &mutable_type);
    // None: the four alone.
    DeviceFeatures plain;
    REQUIRE(gather_extra_features(plain));
    CHECK(plain.f13.pNext == nullptr);
    // A second Vulkan 1.2 struct is refused (the spec forbids two of a type in one chain).
    VkPhysicalDeviceVulkan12Features second{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    DeviceFeatures twice;
    twice.f13.pNext = &second;
    CHECK_FALSE(gather_extra_features(twice));
    // A chain that loops is refused.
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT a{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
    VkPhysicalDeviceShaderIntegerDotProductFeatures b{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES};
    a.pNext = &b;
    b.pNext = &a;
    DeviceFeatures loops;
    loops.f13.pNext = &a;
    CHECK_FALSE(gather_extra_features(loops));
}

TEST_CASE("upscaler provider extensions: the missing ones") {
    CHECK(missing_names({"VK_A", "VK_B"}, {"VK_B", "VK_C"}) == std::vector<std::string>{"VK_A"});
    CHECK(missing_names({}, {"VK_A"}).empty());
}

TEST_CASE("upscaler provider names and display.upscaler") {
    CHECK(upscaler_provider_name_ok("xess"));
    CHECK(upscaler_provider_name_ok("fsr4"));
    CHECK_FALSE(upscaler_provider_name_ok("taa")); // the client's own values
    CHECK_FALSE(upscaler_provider_name_ok("fsr3"));
    CHECK_FALSE(upscaler_provider_name_ok("fsr1"));
    CHECK_FALSE(upscaler_provider_name_ok("off"));
    CHECK_FALSE(upscaler_provider_name_ok("../xess"));
    CHECK_FALSE(upscaler_provider_name_ok("a/b"));
    CHECK_FALSE(upscaler_provider_name_ok(""));

    CHECK(upscaler_from_setting("xess") == Upscaler::External);
    CHECK(upscaler_from_setting("taa") == Upscaler::Taa);
    CHECK(upscaler_from_setting("fsr3") == Upscaler::Fsr3);
    CHECK(upscaler_from_setting("fsr1") == Upscaler::Fsr1);
    CHECK(upscaler_from_setting("off") == Upscaler::Fsr1); // drawn as FSR 1 at full size (render/temporal.h)
    CHECK(upscaler_from_setting("") == Upscaler::Fsr1);
    CHECK(upscaler_from_setting("no such/thing") == Upscaler::Fsr1);
    // parse_upscaler itself still takes an unknown name for FSR 1.
    CHECK(parse_upscaler("xess") == Upscaler::Fsr1);
}

TEST_CASE("upscaler choices: the installed providers after FSR 3.1") {
    const std::vector<std::string> providers{"dlss", "xess"};
    CHECK(upscaler_choice("off", providers) == 0);
    CHECK(upscaler_choice("fsr3", providers) == 3);
    CHECK(upscaler_choice("dlss", providers) == kFirstProviderChoice);
    CHECK(upscaler_choice("xess", providers) == kFirstProviderChoice + 1);
    CHECK(upscaler_choice("fsr4", providers) == 1); // not installed: as any unknown value
    CHECK(upscaler_choice_name(kFirstProviderChoice, providers) == "dlss");
    CHECK(upscaler_choice_name(kFirstProviderChoice + 1, providers) == "xess");
    CHECK(upscaler_choice_name(kFirstProviderChoice + 2, providers) == "fsr1"); // out of range
    CHECK(upscaler_choice_name(kFirstProviderChoice) == "fsr1");               // none installed
    CHECK(upscaler_choice_name(3, providers) == "fsr3");
}

TEST_CASE("installed upscaler providers: folders with a library of their name") {
    ScratchDir dir("upscalers");
    const std::string suffix(hailstone::platform::shared_library_suffix());
    touch(dir.path() / "xess" / ("xess" + suffix));
    touch(dir.path() / "dlss" / ("dlss" + suffix));
    touch(dir.path() / "dlss" / ("nvngx_dlss" + suffix)); // its runtime beside it
    touch(dir.path() / "empty" / "readme.txt");             // no library of its name
    touch(dir.path() / "taa" / ("taa" + suffix));           // a name the client keeps
    touch(dir.path() / ("loose" + suffix));                 // not in a folder
    CHECK(installed_upscaler_providers(dir.path()) == std::vector<std::string>{"dlss", "xess"});
    CHECK(installed_upscaler_providers(dir.path() / "missing").empty());
    CHECK(upscaler_provider_library(dir.path(), "xess") == dir.path() / "xess" / ("xess" + suffix));
}

TEST_CASE("upscaler provider: the example loads, and its table is read") {
    std::string error;
    auto p = UpscalerProvider::load(kFixtures, "example", error);
    REQUIRE_MESSAGE(p, error);
    CHECK(p->name() == "example");
    CHECK(p->label() == "Example (linear blit)");
    CHECK(p->loaded_folder() == kFixtures / "example");
    CHECK_FALSE(p->sharpens());
    std::vector<std::string> extensions{"VK_KHR_surface"};
    std::uint32_t min_api = 0;
    CHECK(p->instance_requirements(extensions, min_api));
    CHECK(extensions == std::vector<std::string>{"VK_KHR_surface"}); // it adds none
    CHECK(min_api == VK_MAKE_API_VERSION(0, 1, 3, 0));
    // Not created: resize and evaluate say no, destroy does nothing.
    CHECK_FALSE(p->resize({640, 360}, {1280, 720}));
    HailstoneUpscalerFrame f{};
    CHECK_FALSE(p->evaluate(f));
    p->destroy();
}

TEST_CASE("upscaler provider: what keeps one out") {
    std::string error;
    CHECK_FALSE(UpscalerProvider::load(kFixtures, "refuse", error));
    CHECK(error.find("said no") != std::string::npos);
    CHECK_FALSE(UpscalerProvider::load(kFixtures, "newer", error));
    CHECK(error.find("newer Hailstone") != std::string::npos);
    CHECK_FALSE(UpscalerProvider::load(kFixtures, "missing", error));
    CHECK(error.find("not installed") != std::string::npos);
    CHECK_FALSE(UpscalerProvider::load(kFixtures, "../refuse", error));
    CHECK(error.find("not a provider's name") != std::string::npos);
    {
        ScratchDir dir("upscalers-bad");
        const std::string suffix(hailstone::platform::shared_library_suffix());
        touch(dir.path() / "junk" / ("junk" + suffix)); // not a library
        CHECK_FALSE(UpscalerProvider::load(dir.path(), "junk", error));
        CHECK(error.find("did not load") != std::string::npos);
    }
    // The client's own upscalers load nothing; a provider that is not there is nullptr (logged).
    CHECK_FALSE(load_upscaler_setting("taa", kFixtures));
    CHECK_FALSE(load_upscaler_setting("fsr1", kFixtures));
    CHECK_FALSE(load_upscaler_setting("missing", kFixtures));
    CHECK(load_upscaler_setting("example", kFixtures));
}

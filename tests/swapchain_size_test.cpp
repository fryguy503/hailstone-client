// render/swapchain_size.h: when the swapchain is made again for a new size.

#include <doctest/doctest.h>

#include "render/swapchain_size.h"
#include "render/vk_util.h"

using namespace hailstone::render;

TEST_CASE("Swapchain size: a surface that lags a resize is caught up; one that differs for good is not chased") {
    const Extent old_size{800, 600}, new_size{1024, 768};
    // The window changed: made again.
    CHECK(swapchain_outdated(new_size, old_size, std::nullopt, old_size));
    // Made for 1024x768 while the surface still said 800x600: the surface is watched.
    CHECK(surface_watch_frames(old_size, new_size) == kSurfaceWatchFrames);
    CHECK_FALSE(swapchain_outdated(new_size, new_size, old_size, old_size)); // still lagging
    CHECK(swapchain_outdated(new_size, new_size, new_size, old_size));       // caught up: made again
    // Made again at the surface's new size, which is the window's: nothing more to watch.
    CHECK(surface_watch_frames(new_size, new_size) == 0);
    // A surface left to the swapchain (no currentExtent): nothing to watch.
    CHECK(surface_watch_frames(std::nullopt, new_size) == 0);
    // A surface that keeps another size for good: once made at it, the comparison with the swapchain's own extent
    // stays equal, so it is not made again every frame (and the watch runs out).
    const Extent scaled{2048, 1536};
    CHECK(surface_watch_frames(scaled, new_size) == kSurfaceWatchFrames);
    CHECK_FALSE(swapchain_outdated(new_size, new_size, scaled, scaled));
    CHECK_FALSE(swapchain_outdated(new_size, new_size, std::nullopt, scaled));
}

TEST_CASE("Swapchain size: a minimised surface (a zero side) waits; an undefined extent does not") {
    CHECK(surface_extent_empty(Extent{0, 0}));   // Windows, minimised
    CHECK(surface_extent_empty(Extent{0, 600}));
    CHECK(surface_extent_empty(Extent{800, 0}));
    CHECK_FALSE(surface_extent_empty(Extent{800, 600}));
    CHECK_FALSE(surface_extent_empty(Extent{1, 1}));
    // The size left to the swapchain (Wayland, say): the window's size is taken instead.
    CHECK_FALSE(surface_extent_empty(Extent{kSurfaceExtentUndefined, kSurfaceExtentUndefined}));
}

TEST_CASE("Scene rect: a dragged size waits until it settles; a held one is made once") {
    SceneResizeDebounce d;
    const Extent made{800, 600};
    CHECK_FALSE(d.update(made, made));
    // A drag: a new size every frame never settles, until the longest wait runs out.
    std::uint32_t frames = 0;
    bool rebuilt = false;
    for (std::uint32_t w = 801; !rebuilt && w < 900; ++w) {
        ++frames;
        rebuilt = d.update(Extent{w, 600}, made);
    }
    CHECK(rebuilt);
    CHECK(frames == kSceneRectMaxWaitFrames);
    // A size that holds: made again after kSceneRectSettleFrames.
    const Extent next{1000, 600};
    for (std::uint32_t i = 1; i < kSceneRectSettleFrames; ++i) CHECK_FALSE(d.update(next, made));
    CHECK(d.update(next, made));
    // Back at the targets' size: nothing to make, and the wait starts afresh.
    CHECK_FALSE(d.update(next, next));
    CHECK_FALSE(d.update(Extent{1001, 600}, next));
    CHECK_FALSE(d.update(next, next));
}

TEST_CASE("Screenshots: only 8-bit RGBA and BGRA swapchains are read back") {
    CHECK(screenshot_format_supported(VK_FORMAT_B8G8R8A8_UNORM));
    CHECK(screenshot_format_supported(VK_FORMAT_R8G8B8A8_SRGB));
    CHECK_FALSE(screenshot_format_supported(VK_FORMAT_R16G16B16A16_SFLOAT));
    CHECK_FALSE(screenshot_format_supported(VK_FORMAT_A2B10G10R10_UNORM_PACK32));
}

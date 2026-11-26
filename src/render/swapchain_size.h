#pragma once

// When the swapchain is made again for a new size (render/vk_context.cpp begin_frame_commands).
//
// A swapchain is made for the window's framebuffer size, but where the surface reports a defined currentExtent its
// images take that instead. The two can differ for good (some scaled or tiling desktops): comparing them would make
// the swapchain again every frame. They can also differ for a moment while a resize settles (the surface still at
// the old size): the swapchain is then made at the old size, and a check of the window alone never makes it again
// once the surface catches up. So: the window size is compared with the size the swapchain was asked for, and for a
// while after a swapchain was made with the surface and the window disagreeing, the surface's currentExtent is
// compared with the swapchain's own extent (the size it has, so a lasting difference makes it at most once).

#include <cstdint>
#include <optional>

namespace hailstone::render {

struct Extent {
    std::uint32_t width = 0, height = 0;
    bool operator==(const Extent&) const = default;
};

// Frames to keep watching the surface after a swapchain was made with its currentExtent not the window's size.
inline constexpr std::uint32_t kSurfaceWatchFrames = 120;

// How many frames to watch the surface for after making a swapchain: `surface` the currentExtent it was made with
// (none when the surface leaves the size to the swapchain), `window` the window size it was asked for.
inline std::uint32_t surface_watch_frames(std::optional<Extent> surface, Extent window) {
    return surface && *surface != window ? kSurfaceWatchFrames : 0;
}

// Whether to make the swapchain again: the window's size is not the one the swapchain was asked for, or (while the
// surface is watched: `surface` given) the surface's defined currentExtent is not the swapchain's extent.
inline bool swapchain_outdated(Extent window, Extent requested, std::optional<Extent> surface, Extent swapchain) {
    if (window != requested) return true;
    return surface && *surface != swapchain;
}

// A surface's currentExtent that leaves the swapchain's size to it (VkSurfaceCapabilitiesKHR: both sides this value).
inline constexpr std::uint32_t kSurfaceExtentUndefined = 0xFFFFFFFFu;

// Whether no swapchain can be made for the surface now: its currentExtent has a zero side. Windows reports 0x0 for a
// minimised window's surface, and a swapchain's imageExtent may not be zero. The window's own state (SDL's, read once
// a frame) can lag that, so the surface is asked before the old swapchain is let go (VkContext::recreate_swapchain),
// and the swapchain is made once the surface has a size again.
inline bool surface_extent_empty(Extent current) {
    if (current.width == kSurfaceExtentUndefined) return false;
    return current.width == 0 || current.height == 0;
}

// When a new size of the 3D view's rectangle (/viewport, VkContext::set_scene_rect) makes the scene targets again.
// Making them waits for the device and rebuilds every target, so while an edge is dragged (a new size each frame)
// it waits for the size to hold for kSceneRectSettleFrames, or at the latest kSceneRectMaxWaitFrames after the
// first change, so a long drag still follows along a few times a second. A size back at the targets' own cancels
// the wait.
inline constexpr std::uint32_t kSceneRectSettleFrames = 4;
inline constexpr std::uint32_t kSceneRectMaxWaitFrames = 30;

struct SceneResizeDebounce {
    // Called once a frame: `wanted` the rectangle's size now, `current` the size the targets were made for. True
    // when they are to be made again now.
    bool update(Extent wanted, Extent current) {
        if (wanted == current) {
            pending_ = false;
            return false;
        }
        if (!pending_ || wanted != last_) {
            stable_ = 0;
            if (!pending_) waited_ = 0;
            pending_ = true;
            last_ = wanted;
        }
        ++stable_;
        ++waited_;
        if (stable_ >= kSceneRectSettleFrames || waited_ >= kSceneRectMaxWaitFrames) {
            pending_ = false;
            return true;
        }
        return false;
    }

private:
    Extent last_{};
    std::uint32_t stable_ = 0, waited_ = 0;
    bool pending_ = false;
};

} // namespace hailstone::render

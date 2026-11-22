/* SPDX-License-Identifier: MIT (sdk/LICENSE) */
/* Hailstone upscaler providers: the C interface a temporal upscaler from outside the client (Intel XeSS, AMD FSR 4,
 * ...) is built against, as an add-on of the kind `upscaler` (docs/plugins.md "Upscaler providers").
 *
 * A provider is a .dll / .so / .dylib in upscalers/<name>/ of the Hailstone folder, named after its folder
 * (<name>.dll, <name>.so, <name>.dylib), with whatever runtime it needs beside it. It is not a game plugin: it loads
 * once, at the start, when display.upscaler names it, before the client makes its Vulkan instance. Its rendering
 * host table is independent of the game plugin capabilities. It runs where the client's own temporal
 * resolve would, on the same inputs: the scene drawn jittered at the render size, its depth, its motion and its
 * reactive mask, and writes the picture at the view's size. The client tonemaps, grades and draws the UI after it,
 * and its frame generation follows the same motion.
 *
 * The order of the calls, all on the client's render thread:
 *   1. hailstone_upscaler(host)     the one export; returns the provider's table, or NULL to stay out
 *   2. instance_requirements        before vkCreateInstance
 *   3. device_requirements          after the GPU is picked, before vkCreateDevice
 *   4. create                       once the device is made
 *   5. resize                       when the render or view size changes (and before the first evaluate)
 *   6. evaluate                     each frame
 *   7. destroy                      before the device goes; then the library is unloaded
 * Any of 1 to 5 may say no (NULL or 0): the client then runs without the provider (AMD FSR 3.1 where the GPU has
 * what it needs, else its own temporal upscaler), with a line in the log, and calls nothing more of it but destroy
 * when create had succeeded. A 0 from evaluate does the same from the next frame on.
 *
 * Vulkan objects cross as integers so this header needs no Vulkan headers: dispatchable handles (VkInstance,
 * VkPhysicalDevice, VkDevice, VkQueue, VkCommandBuffer) as void*, the others (VkImage, VkImageView) as uint64_t,
 * enums (VkFormat, VkImageLayout, VkImageUsageFlags, VkImageAspectFlags) as uint32_t. Cast them back as they are.
 * The client is 64-bit on every platform it is built for.
 *
 * Compatibility: the tables only grow at their end and carry their size. A provider built for an older ABI loads in a
 * newer client; one built for a newer ABI does not load in an older client (check host->abi).
 *
 * Plain C, no C++ types across the boundary. Strings the client passes are valid only during the call; the arrays
 * of names the provider hands back must stay valid until the next call into it. A C++ provider must not let an
 * exception out of any function the client calls.
 *
 * Licences: this header is MIT, so a provider of any licence may include it, and NOTICE.md's plugin and add-on
 * exception covers a provider that talks to Hailstone only through it, including one that loads a vendor's closed
 * runtime. That runtime's own terms are the provider's to meet. */
#ifndef HAILSTONE_UPSCALER_H
#define HAILSTONE_UPSCALER_H

#include <stdint.h>
#include "hailstone_ui_contribution.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAILSTONE_UPSCALER_ABI 1

#if defined(_WIN32)
#define HAILSTONE_UPSCALER_EXPORT __declspec(dllexport)
#else
#define HAILSTONE_UPSCALER_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
#define HAILSTONE_UPSCALER_API extern "C" HAILSTONE_UPSCALER_EXPORT
#else
#define HAILSTONE_UPSCALER_API HAILSTONE_UPSCALER_EXPORT
#endif

#define HAILSTONE_UPSCALER_LOG_INFO 0
#define HAILSTONE_UPSCALER_LOG_WARN 1
#define HAILSTONE_UPSCALER_LOG_ERROR 2

/* What the client hands the provider first. */
typedef struct HailstoneUpscalerHost {
    uint32_t abi;  /* HAILSTONE_UPSCALER_ABI of the client */
    uint32_t size; /* sizeof(HailstoneUpscalerHost) of the client */
    void* host;    /* pass back as the first argument of the functions below */
    /* The folder its library was loaded from (UTF-8, no trailing separator), with everything its package put beside
     * it: load a runtime from here, by its full path (Windows does not search a DLL's own folder for the DLLs it
     * needs). It may be a copy the client made of upscalers/<name>/ (so an update can replace that while this runs):
     * write nothing here. */
    const char* folder;
    /* upscalers/<name>/ itself: keep settings and logs here. */
    const char* config_dir;
    /* A line in the client's log (hailstone.log), with the provider's name in front. */
    void (*log)(void* host, int level, const char* line);
    /* A PFN_vkGetInstanceProcAddr into the client's Vulkan loader: get every other function through it. It may be
     * kept from hailstone_upscaler on, and called from instance_requirements on (the loader starts after the
     * provider is loaded). */
    void* get_instance_proc_addr;
} HailstoneUpscalerHost;

#define HAILSTONE_UPSCALER_DEVICE_REACTIVE 0x1u /* every frame carries a reactive mask (the GPU has independentBlend) */

/* The device the client made (create). */
typedef struct HailstoneUpscalerDevice {
    uint32_t size;          /* sizeof(HailstoneUpscalerDevice) */
    uint32_t api_version;   /* the instance's VkApplicationInfo::apiVersion (1.3) */
    void* instance;         /* VkInstance */
    void* physical_device;  /* VkPhysicalDevice */
    void* device;           /* VkDevice */
    void* queue;            /* VkQueue: the one queue the client draws, computes and presents on */
    uint32_t queue_family;
    uint32_t flags;         /* HAILSTONE_UPSCALER_DEVICE_* */
    void* get_instance_proc_addr; /* PFN_vkGetInstanceProcAddr */
    void* get_device_proc_addr;   /* PFN_vkGetDeviceProcAddr */
} HailstoneUpscalerDevice;

/* An image of the frame. image == 0: none (the reactive mask without independentBlend). */
typedef struct HailstoneUpscalerImage {
    uint64_t image;  /* VkImage */
    uint64_t view;   /* VkImageView: the whole image, one mip, one layer */
    uint32_t format; /* VkFormat */
    uint32_t width, height;
    uint32_t usage;  /* VkImageUsageFlags it was made with */
    uint32_t layout; /* VkImageLayout it is in when evaluate is called, and must be in when it returns */
    uint32_t aspect; /* VkImageAspectFlags of `view` */
} HailstoneUpscalerImage;

#define HAILSTONE_UPSCALER_FRAME_RESET 0x1u /* drop the history: a cut, a teleport, a new zone, new targets */

/* One frame (evaluate). Sizes are in pixels; the render size is the inputs', the view's the output's. */
typedef struct HailstoneUpscalerFrame {
    uint32_t size;          /* sizeof(HailstoneUpscalerFrame) */
    uint32_t flags;         /* HAILSTONE_UPSCALER_FRAME_* */
    void* command_buffer;   /* VkCommandBuffer, recording, outside any render pass or dynamic rendering */
    /* At the render size. Every input was last written by a draw (colour or depth attachment output) and is made
     * visible by the client to any later stage, compute included, before the call:
     *   color    RGBA16F: the scene in linear HDR, before the exposure and the tonemap (SAMPLED, TRANSFER_SRC)
     *   depth    reverse Z (1 near, 0 far, finite far plane), the depth aspect (SAMPLED)
     *   motion   RGBA16F, red and green: where the surface a pixel shows was a frame ago, minus where it is now, in
     *            UV (0..1 over the render size, x right, y down), without the jitter: previous_uv = uv + motion.xy.
     *            In render pixels that is motion.xy * (render width, render height): motion_scale below. Blue and
     *            alpha are the client's own (SAMPLED)
     *   reactive R8: 0..1 how much this frame's colour should replace the history (particles, spell effects and
     *            other blended draws), or none (SAMPLED)
     * all in SHADER_READ_ONLY_OPTIMAL. */
    HailstoneUpscalerImage color, depth, motion, reactive;
    /* At the view's size, RGBA16F (STORAGE, SAMPLED, TRANSFER_DST), in GENERAL, its old contents undefined: write
     * all of it, linear HDR as the colour came in, and leave it in GENERAL. The client makes the writes visible to
     * its fragment shaders after the call. */
    HailstoneUpscalerImage output;
    /* The sub-pixel jitter on this frame's projection, in render pixels (x right, y down, each in -0.5..0.5): the
     * point a pixel t samples is t + 0.5 - jitter (AMD FSR's jitterOffset as it is). The cycle has `jitter_phases`
     * frames (Halton 2, 3 from its second point). */
    float jitter_x, jitter_y;
    uint32_t jitter_phases;
    uint32_t frame_slot;      /* 0 .. frames_in_flight-1: which of the client's frames in flight this is */
    uint32_t frames_in_flight;
    float motion_scale_x, motion_scale_y; /* the render width and height: motion.xy times these is in render pixels */
    /* The camera: its planes (world units) and vertical field of view (radians). */
    float near_plane, far_plane, fov_y;
    /* The exposure the client multiplies the output by before its tonemap; a provider with its own automatic
     * exposure may ignore it. */
    float exposure;
    float frame_seconds;      /* since the last evaluate */
    /* display.sharpness, 0 none .. 1 the most: for a provider with HAILSTONE_UPSCALER_SHARPENS. */
    float sharpness;
} HailstoneUpscalerFrame;

#define HAILSTONE_UPSCALER_SHARPENS 0x1u /* it sharpens its output by HailstoneUpscalerFrame::sharpness itself: the
                                          * client's own sharpening (RCAS) does not run after it */
#define HAILSTONE_UPSCALER_MAX_UI_CONTRIBUTIONS 32u

/* The provider's table, returned by hailstone_upscaler and kept until destroy. Functions through destroy are required. */
typedef struct HailstoneUpscaler {
    uint32_t abi;   /* HAILSTONE_UPSCALER_ABI the provider was built for */
    uint32_t size;  /* sizeof(HailstoneUpscaler) */
    const char* name; /* shown to the player ("Intel XeSS"); the client copies it */
    uint32_t flags;   /* HAILSTONE_UPSCALER_SHARPENS ... */
    uint32_t reserved;
    /* The instance extensions it needs (`*names`, `*count`; none: count 0) and the lowest Vulkan version it needs
     * (VK_MAKE_API_VERSION; 0 any). The client makes a Vulkan 1.3 instance. A name the loader lacks keeps the
     * provider out. 0: it cannot run here (its runtime is missing ...); log why. */
    int (*instance_requirements)(void* host, const char* const** names, uint32_t* count, uint32_t* min_api_version);
    /* For the picked GPU (`instance`, `physical_device`, VkInstance and VkPhysicalDevice): the device extensions it
     * needs and the features. `features` is a VkPhysicalDeviceFeatures2 whose pNext chain holds, in this order,
     * VkPhysicalDeviceVulkan11Features, VkPhysicalDeviceVulkan12Features and VkPhysicalDeviceVulkan13Features, every
     * feature VK_FALSE: set to VK_TRUE what it needs. A feature that core Vulkan 1.1 to 1.3 has goes in through its
     * field there (shaderInt8 in the 1.2 struct, never a VkPhysicalDeviceShaderFloat16Int8Features: the Vulkan spec
     * forbids both in one chain). One of an extension that has none (VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT)
     * goes in as a struct of the provider's own added anywhere in the chain; it must stay valid until create is
     * called, and the provider checks that the GPU has those features (the client checks the core ones and every
     * extension name). The client turns them all on beside its own. An extension or a core feature the GPU lacks, or
     * a second struct of one of the four types, keeps the provider out. 0: this GPU cannot run it (another vendor's
     * ...); log why. */
    int (*device_requirements)(void* host, void* instance, void* physical_device, const char* const** names,
                               uint32_t* count, void* features);
    /* The device is made, with what device_requirements asked for. 0: it failed. */
    int (*create)(void* host, const HailstoneUpscalerDevice* device);
    /* The render (input) and view (output) sizes, before the first evaluate and whenever either changes; the device
     * is idle. The render size is the player's render scale of the view's (display.render_scale, 33 % to 100 % on
     * each side); the next evaluate starts a new history. 0: it cannot upscale between these sizes. */
    int (*resize)(void* host, uint32_t render_width, uint32_t render_height, uint32_t view_width,
                  uint32_t view_height);
    /* Records this frame's upscale into frame->command_buffer. It may bind its own pipelines, descriptor sets and
     * push constants (the client binds its own again after it) and change an image's layout within the call, as
     * long as each is back in the layout given when it returns. 0: it failed. */
    int (*evaluate)(void* host, const HailstoneUpscalerFrame* frame);
    /* Frees everything; the device is idle. Called once create succeeded, before the device is destroyed. */
    void (*destroy)(void* host);

    /* Optional ABI 1 tail. While the provider is active, the UI can ask for a snapshot of notices and disabled
     * controls. Assign *entries and return its count, or return 0. The array, records and strings must remain
     * valid until the next call into the provider. The client copies up to HAILSTONE_UPSCALER_MAX_UI_CONTRIBUTIONS
     * entries. action_label and command must be NULL. This is on the same thread as the other calls. */
    uint32_t (*ui_contributions)(void* host, const HailstoneUiContribution* const** entries);
} HailstoneUpscaler;

/* The one export: `HAILSTONE_UPSCALER_API const HailstoneUpscaler* hailstone_upscaler(const HailstoneUpscalerHost*
 * host);` The host table stays valid until the library is unloaded. NULL: the provider stays out (log why). */
typedef const HailstoneUpscaler* (*HailstoneUpscalerEntry)(const HailstoneUpscalerHost* host);
#define HAILSTONE_UPSCALER_ENTRY "hailstone_upscaler"

#ifdef __cplusplus
}
#endif

#endif

#pragma once

// How the client turns the zone's fog and clip values (OP_NewZone) and the
// player's ClipPlane setting into fog and far-plane distances.
// Source: the original's Handle_OP_NewZone
// @0x4c9ee6..0x4c9f98, CDisplay::SetFog @0x48ae80, ClipPlane keybinds @0x4d873b.

#include <cstdint>

#include <glm/glm.hpp>

namespace hailstone::game {

// The zone's values as sent in OP_NewZone (first fog slot).
struct ZoneFogInput {
    bool fog_enabled = true;   // ztype != 0 (SetFog's first argument, BFog)
    glm::vec3 fog_color{0.0f}; // 0..1
    float fog_minclip = 0.0f;
    float fog_maxclip = 0.0f;
    float minclip = 0.0f;
    float maxclip = 0.0f;
    float fog_density = 0.0f;  // NewZone fog_density (0xddfc84), SetFog's density
    std::uint8_t time_type = 0; // NewZone time_type (__ZoneType): 1, 2, 5 outdoor
};

// Handle_OP_NewZone's fixes (@0x4c9ee6..0x4c9f98): no fog clips become
// 500/600 with grey 0xC0; minclip at least 50; maxclip below 51 becomes
// fog_maxclip x 4; minclip at or above maxclip makes maxclip = minclip x 4.
ZoneFogInput new_zone_fixes(const ZoneFogInput& zone);

// The far plane (CDisplay::SetYon): minclip + ClipPlane x 0.05 x (maxclip -
// minclip), ClipPlane clamped to 0..20, for fixed zone values.
float zone_far_plane(const ZoneFogInput& fixed, int clip_plane);

// SetFog's run-time conditions for the far-plane margin.
struct FogConditions {
    bool special_region = false; // 0xd1f3c0 (camera region / weather type) in 5..10
    bool raining = false;        // bRain
};

// What the renderers draw with: CRender::SetFog's values. The effects' fog
// is exp(-(f density)^2) per vertex, f = clamp((dist - start) / (end - start)
// x 10, 0, 10) (game::fog_factor); start and end are SetFog's truncated ints
// (end at least start + 1). With fog off (ztype 0) CRender turns FOGENABLE
// off: density is 0 here and start and end are pushed past the far plane.
struct ViewDistance {
    glm::vec3 fog_color{0.0f};
    bool fog_enabled = true;
    float fog_start = 0.0f;
    float fog_end = 0.0f;
    float fog_density = 0.0f;
    float far_plane = 0.0f;
};

// CRender::SetFog's arguments as the renderers' ViewDistance.
struct RenderFog;
ViewDistance render_view(const RenderFog& fog, float far_plane);

// clip_plane is the eqclient.ini ClipPlane setting, 0..20 (default 15).
ViewDistance compute_view_distance(const ZoneFogInput& zone, int clip_plane = 15, FogConditions conditions = {});

// CDisplay::SetFog @0x48ae80's distance fixes, for fog distances `start` and
// `end` against the far plane (Yon) `far_plane`:
//   both below 0.1: start = far - 50, end = far - 1; start >= 0; end < start:
//   end = start + 50; then, when fog is on, the camera is not in region type
//   5..10, it is not raining and end >= far (or within 0.01 of it),
//   end = far - CDisplay_CalcFogFadeMargin(start) = far - 0.15 (far - start)
//   (0.15 far when |far - start| < 0.001); finally start >= 0 and, if
//   end <= start, start = end - 1.
glm::vec2 set_fog_distances(float start, float end, float far_plane, bool fog_enabled, FogConditions conditions = {});

// ---------------------------------------------------------------------------
// Actor clip: how far away spawns are drawn.
//
// eqclient.ini ActorClipPlane (CDisplay+0x2d78; the Advanced Display options' slider; read @0x491e69, default 50)
// becomes the camera's actor clip in 0x487900 and CDisplay::UpdateCameraFov @0x48ac70: camera vtable +0x14
// gets min((ActorClipPlane x 5 + 25) x 2, Yon), the Yon read back through vtable +0x10. Spawns past it are not
// drawn; nearer ones are drawn, fogged, all the way out (not cut at the fog end).
inline constexpr int kDefaultActorClipPlane = 50;
float actor_clip_distance(int actor_clip_plane, float far_plane);

// What the spawn renderers do with one spawn. A spawn whose race has an actor is the actor renderer's, drawn or
// not: only a spawn with no actor at all (not even the Human the original falls back to) gets the placeholder box.
enum class SpawnDraw {
    Model,   // its actor's model, drawn
    Clipped, // past the actor clip: nothing drawn, as in the original
    Empty,   // an actor whose model failed to build (e.g. a static actor without a mesh)
    Box,     // no actor loaded at all: the placeholder box
};
// `has_actor` / `has_model` as in ActorRenderer::ModelChoice; `distance` from the camera; `actor_clip` <= 0: none.
SpawnDraw spawn_draw(bool has_actor, bool has_model, float distance, float actor_clip);

// ---------------------------------------------------------------------------
// Camera field of view.
//
// Every EverQuest::Cameras entry is created with +0x40 = 90 (@0x533ac1), and the
// camera switch copies it into CDisplay+0x28f4 (@0x49423b), so the stock FOV is
// 90 units of 512 per circle. CDisplay::UpdateCameraFov @0x48ac70 turns it into
// CCamera::SetFieldOfView(h, v) (vtable +0x24), and BuildProjectionAndView
// @0x10006470 builds D3DXMatrixPerspectiveRH(2 tan(h/2), -w / (h/v), near, far)
// with near = 1 (CDisplay+0x2900, set to 1.0 in the constructor @0x4918a6).
inline constexpr float kStockFovUnits = 90.0f;
inline constexpr float kCameraNear = 1.0f;

struct CameraFov {
    float h = 0.0f; // SetFieldOfView's first argument, 512ths of a circle
    float v = 0.0f; // its second
    // Half-angle tangents of the projection: x = tan(h/2) (the D3DX width / 2
    // at near 1), y = x * v / h (the height / 2).
    float tan_half_x = 0.0f;
    float tan_half_y = 0.0f;
};

// UpdateCameraFov for a `fov_units` camera on a width x height viewport: below
// an aspect of 2 it passes (F, F / aspect), which keeps square pixels and a
// horizontal FOV of exactly F x 0.703125 degrees (63.28 for F = 90); at 2 or
// wider it passes (min(2 atan(tan(v/2) aspect) in units, 200), v = 0.7501876 F).
CameraFov camera_fov(float fov_units, int width, int height);

} // namespace hailstone::game

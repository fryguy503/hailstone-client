// The lighting math (render/scene_lighting.h) shared by the zone, actor and object shaders, classic and modern.
// Each shader declares where its SceneLighting data lives; these functions take the values.

struct PointLightGpu {
    vec4 position_radius; // xyz world position, w radius
    vec4 color;
};

// render/scene_lighting.h SceneLighting, as a block's members (the zone's Scene uniform block, the actor and object
// Lighting buffers).
#define SCENE_LIGHTING_FIELDS                                                                                          \
    vec4 ambient;                                                                                                      \
    vec4 special_ambient;                                                                                              \
    vec4 directional;                                                                                                  \
    vec4 direction;                                                                                                    \
    vec4 bounce;                                                                                                       \
    vec4 params; /* x: 1 modern, y: light count, z: dynamic lights, w: the light floor */                              \
    vec4 to_light; /* the modern mode's daylight */                                                                    \
    vec4 light;                                                                                                        \
    vec4 sky; /* w: the share of the zone's baked colour kept (render::Daylight::baked) */                             \
    vec4 ground; /* w: the share of the baked colour's lamp light kept (render::Daylight::baked_lamps) */              \
    mat4 shadow_view_proj[4];                                                                                          \
    vec4 shadow_splits;                                                                                                \
    vec4 shadow_texel;                                                                                                 \
    vec4 shadow_params; /* x: 1 shadows drawn, y: 1 / map size */                                                      \
    vec4 shadow_origin;                                                                                                \
    vec4 shadow_forward;                                                                                               \
    vec4 haze; /* x: 1 / length, y: 1 / height scale, z: 1 on, w: the sun's glow */                                    \
    vec4 haze_color;                                                                                                   \
    vec4 camera;                                                                                                       \
    vec4 screen; /* x, y: view depth = y / (d + x); z, w: 1 / the target's size (set_screen) */                       \
    vec4 time;   /* x: seconds, wrapped every 1024 */                                                                  \
    vec4 wind;   /* xy: the way it blows, z: strength, w: the gusts' strength */                                         \
    vec4 cover;  /* the cover map (render/cover_map.h): x, y its corner, z: 1 / its size, w: its top */                \
    vec4 cover_params; /* x: 1 / its depth, y: 1 the cover map on, z: 1 the water map on */                            \
    vec4 weather; /* render/weather_renderer.h set_weather: x wet, y snow, z rain falling, w puddles */               \
    vec4 vision;  /* the infravision / ultravision raise of the ambient, added as linear light (modern) */            \
    vec4 probe;   /* render/ambient_probe.h: the baked light where the player stands, w 1 + the sky's share (0 none) */ \
    vec4 reveal_origin; /* game/iso_occlusion.h IsoReveal: xyz the camera, w strength (0 off) */                      \
    vec4 reveal_axis;   /* xyz the way to the player, w their distance */                                             \
    vec4 reveal_shape;  /* x radius at the player, y the height it starts at, z depth margin, w soft share */        \
    vec4 reveal_roof;   /* x the height it cuts above, y its flat radius, z its soft edge, w strength (0 off) */      \
    vec4 point_shadow_light[8]; /* render/point_shadows.h: xyz the light, w its radius */                              \
    vec4 point_shadow_slot[8];  /* x: its cube's first layer, y: strength */                                           \
    vec4 point_shadow_params;   /* x: count, y: 1 / face size, z: the face scale, w: the near distance */              \
    PointLightGpu lights[32];

// The isometric camera's cut through what stands between it and the player (game/iso_occlusion.h, Hailstone's own):
// how much of world point `p` is cut away, 0..1: the cone round the line to the player, or the roof over them.
// game::iso_reveal_cut, line for line.
float reveal_cut(vec3 p, vec4 origin, vec4 axis, vec4 shape, vec4 roof) {
    float cone = 0.0;
    if (origin.w > 0.0) {
        vec3 v = p - origin.xyz;
        float t = dot(v, axis.xyz);
        if (t > 0.0 && t < axis.w - shape.z) {
            float lateral = length(v - t * axis.xyz) * axis.w / t;
            float edge = 1.0 - smoothstep(shape.x * (1.0 - shape.w), shape.x, lateral);
            float height = smoothstep(shape.y, shape.y + 3.0, p.z);                       // kIsoRevealRise
            float depth = 1.0 - smoothstep(axis.w - shape.z - 6.0, axis.w - shape.z, t);  // kIsoRevealDepthFade
            cone = origin.w * edge * height * depth;
        }
    }
    float over = 0.0;
    if (roof.w > 0.0) {
        vec3 player = origin.xyz + axis.xyz * axis.w;
        float flat_d = length(p.xy - player.xy);
        float edge = 1.0 - smoothstep(roof.y - roof.z, roof.y, flat_d);
        over = roof.w * edge * smoothstep(roof.x, roof.x + 1.5, p.z);                      // kIsoRoofRise
    }
    return max(cone, over);
}

// Whether the pixel at `frag` (gl_FragCoord.xy) of world point `p` is dropped: the cut against a 4x4 ordered dither,
// so its edge thins out without blending (and the pass stays opaque).
bool reveal_discard(vec3 p, vec2 frag, vec4 origin, vec4 axis, vec4 shape, vec4 roof) {
    if (origin.w <= 0.0 && roof.w <= 0.0) return false;
    float cut = reveal_cut(p, origin, axis, shape, roof);
    if (cut <= 0.0) return false;
    const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 i = ivec2(frag) & 3;
    return cut > (bayer[i.y * 4 + i.x] + 0.5) / 16.0;
}

// The region shaders' scene light (RegionC1 vs_1_1) at a normal, for geometry without a lit vertex colour:
// ambient + special ambient + the sun or moon (directional, from -Dn) + the bounce light (from Dn).
vec3 scene_light(vec3 n, vec3 ambient, vec3 special, vec3 directional, vec3 dn, vec3 bounce) {
    return ambient + special + directional * max(dot(n, -dn), 0.0) + bounce * max(dot(n, dn), 0.0);
}

// One point light at a surface point: colour x Lambert x (1 - d^2 / r^2), as game::point_light_term. `wrap` softens
// the falloff around the terminator (0 = plain Lambert), for rounded shapes like bodies.
vec3 point_light(PointLightGpu l, vec3 p, vec3 n, float wrap) {
    float r = l.position_radius.w;
    if (r <= 0.0) return vec3(0.0);
    vec3 d = l.position_radius.xyz - p;
    float dist2 = dot(d, d);
    float atten = 1.0 - min(dist2 / (r * r), 1.0);
    if (atten <= 0.0) return vec3(0.0);
    float ndl = dot(n, d * inversesqrt(max(dist2, 1e-6)));
    float lambert = clamp((ndl + wrap) / (1.0 + wrap), 0.0, 1.0);
    return l.color.rgb * lambert * atten;
}

// Display (sRGB) values to linear: textures, vertex colours, the fog colour and the classic results are stored that
// way; the scene target is linear (render/vk_context.h).
vec3 to_linear(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

// The zones' baked light (vertex colours, the special ambient, the MPL term) to linear. Its builders tinted it on a
// display, in display values; converted channel by channel a tinted light grows far more saturated (befallen's
// grey-blue (60, 60, 140) became a vivid pure blue), so most of the conversion scales the colour as a whole by its
// grey's, keeping the tint's balance as it was seen; a share converts as is, for some of linear light's richness.
vec3 baked_to_linear(vec3 c) {
    c = max(c, vec3(0.0));
    float y = dot(c, vec3(0.2126, 0.7152, 0.0722));
    vec3 kept = c * (to_linear(vec3(y)).r / max(y, 1e-4));
    return mix(to_linear(c), kept, 0.7);
}

// The light a character or door takes from the place it stands in (SceneLighting::probe, render/ambient_probe.h):
// the zone's baked colour there as the zone keeps it (`baked` Daylight::baked), and how much of the open sky's light
// reaches it (`sky_share`, 1 with no probe; never under a quarter, so a figure under a roof still reads).
vec3 probe_light(vec4 probe, float baked, out float sky_share) {
    sky_share = 1.0;
    if (probe.w < 0.5) return vec3(0.0);
    sky_share = max(probe.w - 1.0, 0.25);
    return baked_to_linear(probe.rgb * baked) * 0.8;
}

// ---- A texture pack's maps (tools/texpack) ----

// The normal of a normal map texel `xy` (BC5, -1..1, green toward +v) on the surface of normal `n` at `world`,
// texture coordinates `uv`: on a frame built from their screen derivatives, as the meshes have no tangents (Schüler,
// "Normal Mapping Without Precomputed Tangents"). PACK_NORMAL takes the derivatives (fragment shaders only; this file
// is in the vertex shaders too); use it where the whole quad takes the same branch.
#define PACK_NORMAL(n, world, uv, xy) pack_normal(n, dFdx(world), dFdy(world), dFdx(uv), dFdy(uv), xy)
vec3 pack_normal(vec3 n, vec3 dp1, vec3 dp2, vec2 duv1, vec2 duv2, vec2 xy) {
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float scale = inversesqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
    vec3 m = t * scale * xy.x + b * scale * xy.y + n * z;
    float len = length(m);
    return len > 1e-6 ? m / len : n;
}

// The highlight of the light `light` from direction `l` on a dielectric (F0 0.04) seen from `v`: GGX with Schlick's
// Fresnel and Smith's visibility, times n.l. A pack's roughness map is taken into 0.5..1 by the callers: the game's
// surfaces are stone, wood, earth, cloth and skin, never polished.
vec3 pack_specular(vec3 n, vec3 v, vec3 l, vec3 light, float roughness) {
    vec3 h = normalize(v + l);
    float nl = max(dot(n, l), 0.0), nv = max(dot(n, v), 1e-3), nh = max(dot(n, h), 0.0);
    float a = max(roughness * roughness, 0.002), a2 = a * a;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * d * d);
    float k = a * 0.5;
    float vis = 0.25 / ((nl * (1.0 - k) + k) * (nv * (1.0 - k) + k));
    float F = 0.04 + 0.96 * pow(1.0 - max(dot(v, h), 0.0), 5.0);
    return light * (D * vis * F * nl);
}

// The modern mode (render/scene_lighting.h Daylight): ambient from the sky above and the ground below.
vec3 hemisphere(vec3 n, vec3 sky, vec3 ground) {
    return mix(ground, sky, clamp(n.z * 0.5 + 0.5, 0.0, 1.0));
}

// How a modern point light falls off at squared distance `d2` (1 at the light): as the inverse square past a core
// of a fifth of its radius (8 to 16 units), so the light pools round a lamp however far its radius reaches (the big
// city lights, 70 to 200 units, would otherwise light a whole square evenly), and windowed to zero at the radius.
float light_pool(float d2, float r) {
    float window = 1.0 - min(d2 / (r * r), 1.0);
    float core = clamp(r * 0.2, 8.0, 16.0);
    return window * core * core / (core * core + d2);
}

// The modern mode's point light: the light's colour in linear, x 2.5 so a torch reads against daylight, falling off
// as light_pool instead of the classic linear window. `wrap` as point_light.
vec3 point_light_modern(PointLightGpu l, vec3 p, vec3 n, float wrap) {
    float r = l.position_radius.w;
    if (r <= 0.0) return vec3(0.0);
    vec3 d = l.position_radius.xyz - p;
    float dist2 = dot(d, d);
    if (dist2 >= r * r) return vec3(0.0);
    float ndl = dot(n, d * inversesqrt(max(dist2, 1e-6)));
    float lambert = clamp((ndl + wrap) / (1.0 + wrap), 0.0, 1.0);
    return baked_to_linear(l.color.rgb) * 2.5 * lambert * light_pool(dist2, r);
}

// The sun's or moon's shadow at world point `p` (render/shadows.h; the SceneLighting shadow fields): 1 lit, 0 in
// shadow. The point moves 1.5 texels along the normal `n` first, against acne; a 3 x 3 grid of hardware-filtered
// comparisons softens the edge; the shadow fades out over the last 15 % of the shadow distance.
float shadow_visibility(sampler2DArrayShadow map, mat4 view_proj[4], vec4 splits, vec4 texel, vec4 params,
                        vec4 origin, vec4 forward, vec3 p, vec3 n) {
    if (params.x < 0.5) return 1.0;
    float depth = dot(p - origin.xyz, forward.xyz);
    int c = depth < splits.x ? 0 : depth < splits.y ? 1 : depth < splits.z ? 2 : depth < splits.w ? 3 : 4;
    if (c > 3) return 1.0;
    vec4 s = view_proj[c] * vec4(p + n * (texel[c] * 1.5), 1.0);
    vec3 q = s.xyz / s.w;
    vec2 uv = q.xy * 0.5 + 0.5;
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) sum += texture(map, vec4(uv + vec2(x, y) * params.y, float(c), q.z));
    return mix(sum / 9.0, 1.0, smoothstep(splits.w * 0.85, splits.w, depth));
}

// The zone lights' shadows (render/point_shadows.h). The face a direction `d` from the light falls on (0..5: +x,
// -x, +y, -y, +z, -z) and the point on it, -1..1 each way: the face's other two axes over the distance along its
// own, times `scale` (kPointShadowFaceScale). render::point_shadow_face is the same.
uint point_shadow_face(vec3 d, float scale, out vec2 ndc) {
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) {
        ndc = d.yz / max(a.x, 1e-6) * scale;
        return d.x >= 0.0 ? 0u : 1u;
    }
    if (a.y >= a.z) {
        ndc = d.xz / max(a.y, 1e-6) * scale;
        return d.y >= 0.0 ? 2u : 3u;
    }
    ndc = d.xy / max(a.z, 1e-6) * scale;
    return d.z >= 0.0 ? 4u : 5u;
}

// Drawing into face `face` of a light reaching `radius`: the clip position of a point `d` from the light, its x and
// y point_shadow_face's; depth is clipped from a quarter unit out to the radius (the casters write their distance).
vec4 point_shadow_clip(vec3 d, uint face, float scale, float radius) {
    float along = face == 0u ? d.x : face == 1u ? -d.x : face == 2u ? d.y : face == 3u ? -d.y : face == 4u ? d.z : -d.z;
    vec2 xy = face < 2u ? d.yz : face < 4u ? d.xz : d.xy;
    const float kNear = 0.25;
    return vec4(xy * scale, (along - kNear) * radius / max(radius - kNear, 1e-3), along);
}

// How much of the light `light` (its position and radius) reaches world point `p` (normal `n`) past what stands
// between: 1 lit, 0 in shadow; 1 for a light without a shadow (not among the SceneLighting point shadow fields, which
// match on both, so two lights in one spot do not share a cube). The point moves 1.5
// texels along the normal, towards the light's side, first; a 3 x 3 grid of hardware-filtered comparisons softens
// the edge. The slot's strength fades the shadow in and out.
float point_shadow(sampler2DArrayShadow map, vec4 lights[8], vec4 slots[8], vec4 params, vec4 light, vec3 p, vec3 n) {
    int count = int(params.x + 0.5);
    for (int i = 0; i < count; ++i) {
        vec3 dl = lights[i].xyz - light.xyz;
        if (dot(dl, dl) > 0.01 || abs(lights[i].w - light.w) > 0.01) continue;
        vec3 d = p - light.xyz;
        float dist = length(d);
        // A texel's world size there: the face spans twice the distance along its axis.
        float texel = 2.0 * dist * params.y / params.z;
        // Further along the normal the more the surface turns from the light (where acne starts).
        float facing = abs(dot(n, d)) / max(dist, 1e-4);
        vec3 q = d + n * (dot(n, d) <= 0.0 ? 1.0 : -1.0) * texel * (1.5 + 2.5 * (1.0 - facing));
        vec2 ndc;
        uint face = point_shadow_face(q, params.z, ndc);
        float ref = (length(q) - texel * 1.5) / lights[i].w;
        if (ref >= 1.0) return 1.0; // past the cube's reach it holds nothing
        vec2 uv = ndc * 0.5 + 0.5;
        float layer = slots[i].x + float(face);
        float sum = 0.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) sum += texture(map, vec4(uv + vec2(x, y) * params.y, layer, ref));
        return mix(1.0, sum / 9.0, slots[i].y);
    }
    return 1.0;
}

// How far above a point the cover map's top surface still shelters it (render/cover_map.h kShelterReach). Higher up,
// it is a tower's top or an ornament floating over a square (freeportwest's academy), not a roof over the point.
const float kShelterReach = 250.0;

// Whether the sky is open above world point `p` (render/cover_map.h; the SceneLighting cover fields): 1 open, 0 under
// something the cover map holds within kShelterReach. A point a little under the top surface still counts as on it;
// a 2 x 2 grid of hardware-filtered comparisons softens the edge. Outside the map, or with no map, everything is open.
float sky_open(sampler2DArrayShadow map, vec4 cover, vec4 params, float texel, vec3 p) {
    if (params.y < 0.5) return 1.0;
    vec2 uv = (p.xy - cover.xy) * cover.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    float z = (cover.w - p.z - 2.0) * params.x;
    if (z <= 0.0) return 1.0;
    float reach = max((cover.w - p.z - kShelterReach) * params.x, 0.0);
    float sum = 0.0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            vec2 at = uv + (vec2(x, y) - 0.5) * texel;
            sum += min(texture(map, vec4(at, 4.0, z)) + 1.0 - texture(map, vec4(at, 4.0, reach)), 1.0);
        }
    return sum * 0.25;
}

// How deep world point `p` lies under the water the water map (render/cover_map.h; the shadow map's layer 5, read
// as depths) shows above it, in world units: 0 with no water above it, outside the map or with no map. The map fades
// out over the last tenth of its reach (`edge`, 1 inside).
float water_depth(sampler2DArray map, vec4 cover, vec4 params, vec3 p, out float edge) {
    edge = 0.0;
    if (params.z < 0.5) return 0.0;
    vec2 uv = (p.xy - cover.xy) * cover.z;
    vec2 inside = min(uv, 1.0 - uv);
    if (min(inside.x, inside.y) <= 0.0) return 0.0;
    float d = textureLod(map, vec3(uv, 5.0), 0.0).r;
    if (d >= 1.0) return 0.0;
    edge = smoothstep(0.0, 0.1, min(inside.x, inside.y));
    return max(cover.w - d / params.x - p.z, 0.0);
}

float caustic_hash(vec2 p) {
    p = fract(p * vec2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return fract((p.x + p.y) * p.x);
}

// Smooth value noise, 0..1.
float caustic_noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(mix(caustic_hash(i), caustic_hash(i + vec2(1, 0)), u.x),
               mix(caustic_hash(i + vec2(0, 1)), caustic_hash(i + vec2(1, 1)), u.x), u.y);
}

// Caustics: the sunlight the waves focus on what lies under them, a web of bright lines drifting and changing, at
// world point `p` (xy) and time `t`. Two ridged noise fields over a slowly warped plane, their crests where the noise
// crosses its middle; 0 .. about 2.
float caustics(vec2 p, float t) {
    vec2 q = p * 0.07;
    q += 0.8 * vec2(caustic_noise(q * 0.6 + vec2(t * 0.05, 0.0)), caustic_noise(q * 0.6 + vec2(5.2, -t * 0.04)));
    float a = 1.0 - abs(caustic_noise(q + vec2(t * 0.13, t * 0.07)) * 2.0 - 1.0);
    float b = 1.0 - abs(caustic_noise(q * 1.43 + vec2(3.7 - t * 0.09, t * 0.11)) * 2.0 - 1.0);
    return pow(a, 7.0) + pow(b, 7.0);
}

// Under water (the water map over world point `p`): how the sun's (or moon's) light that reaches `p` compares with the
// light above (`sun`, the factor for the sun term) and the sky's (`sky`): both absorbed with the depth, and the sun's
// gathered by the waves into the caustics' web, 0.3 of it between the lines and up to about 4 times on them, its
// pattern taken where the light entered the surface. 1 and 1 out of the water.
void underwater_light(sampler2DArray map, vec4 cover, vec4 params, vec3 p, vec4 to_light, float t, out float sun,
                      out float sky) {
    sun = 1.0;
    sky = 1.0;
    float edge;
    float depth = water_depth(map, cover, params, p, edge);
    if (depth <= 0.05) return;
    float under = smoothstep(0.05, 1.5, depth) * edge;
    float c = to_light.z > 0.05 ? caustics(p.xy + to_light.xy / to_light.z * depth, t) : 0.0;
    sun = mix(1.0, (0.3 + 1.8 * c) * exp(-depth * 0.03), under);
    sky = mix(1.0, exp(-depth * 0.015), under);
}

// Under water (the camera under the water map's surface): the light shafts in the water between the camera and `p`,
// where the sun (`to_light`, `light`) comes down through the waves. Eight steps along the view, each lit as much as
// a slow pattern of the surface above lets light in where the ray to the sun meets it, fading with the depth there
// and the distance from the eye, strongest looking towards the sun; tinted by `water` (the haze colour under water). `jitter` (0..1, per pixel) offsets
// the steps, so their spacing does not show as bands.
vec3 underwater_shafts(sampler2DArray map, vec4 cover, vec4 params, vec3 camera, vec3 p, vec4 to_light, vec4 light,
                       vec3 water, float t, float jitter) {
    float edge;
    float camera_depth = water_depth(map, cover, params, camera, edge);
    if (to_light.z <= 0.05 || camera_depth <= 0.0) return vec3(0.0);
    // Past the map's reach the surface is taken to go on level with the one over the camera.
    float surface = camera.z + camera_depth;
    vec3 d = p - camera;
    float len = min(length(d), 160.0);
    vec3 dir = d / max(length(d), 1e-4);
    float sum = 0.0;
    for (int i = 0; i < 8; ++i) {
        float s = (float(i) + jitter) / 8.0 * len;
        vec3 q = camera + dir * s;
        float e;
        float h = water_depth(map, cover, params, q, e);
        h = mix(max(surface - q.z, 0.0), h, e);
        if (h <= 0.0) continue;
        vec2 entry = q.xy + to_light.xy / to_light.z * h;
        float beam = caustic_noise(entry * 0.06 + vec2(t * 0.03, -t * 0.02));
        beam = smoothstep(0.5, 0.8, beam);
        sum += beam * exp(-h * 0.025) * exp(-s * 0.012);
    }
    // Water scatters mostly forward: the shafts show most looking towards the sun.
    float toward = max(dot(dir, to_light.xyz), 0.0);
    float phase = 0.35 + 2.0 * toward * toward * toward;
    return light.rgb * (water + 0.15) * (sum / 8.0 * len * 0.006 * phase);
}

// The wind's push on a vertex of a plant (render/zone_renderer.cpp, ZoneRenderer::build): `sway` xy the plant's foot,
// z how far this vertex bends at full strength (world units, 0 at the roots), w how much it flutters (its leaves).
// `wind` is SceneLighting::wind. The plant leans with the wind, rocks as gusts roll across the zone in the wind's
// direction, and its leaves shiver on their own.
// An instanced replacement model's placement (render/zone_renderer.cpp GpuInstance; the zone's own geometry has the
// identity, which leaves every value as it is): a point moved by the rows of its 3 x 4 transform, a normal by the
// normal matrix's rows, and a plant's sway (sway_plant measured it in the model's own space, its foot at the origin)
// to where the placement stands, its distances scaled with it.
vec3 instance_point(vec3 p, vec4 row0, vec4 row1, vec4 row2) {
    vec4 h = vec4(p, 1.0);
    return vec3(dot(row0, h), dot(row1, h), dot(row2, h));
}
vec3 instance_normal(vec3 n, vec4 row0, vec4 row1, vec4 row2) {
    vec3 m = vec3(dot(row0.xyz, n), dot(row1.xyz, n), dot(row2.xyz, n));
    return dot(m, m) > 0.0 ? normalize(m) : n;
}
vec4 instance_sway(vec4 sway, vec4 row0, vec4 row1, float scale) {
    return vec4(sway.xy + vec2(row0.w, row1.w), sway.zw * scale);
}

vec3 wind_offset(vec3 p, vec4 sway, vec4 wind, float t) {
    if (wind.z <= 0.0 || sway.z + sway.w <= 0.0) return vec3(0.0);
    float phase = fract(dot(sway.xy, vec2(0.0131, 0.0173))) * 6.2832;
    float roll = dot(sway.xy, wind.xy) * 0.012;
    float gust = 0.5 + 0.5 * sin(t * 0.9 - roll) * sin(t * 0.37 - roll * 0.6 + 1.3);
    float rock = 0.25 * sin(t * 1.7 + phase) + 0.1 * sin(t * 3.3 + phase * 2.1);
    vec2 lean = wind.xy * wind.z * (0.35 + wind.w * gust + rock) * sway.z;
    float shiver = sin(t * 9.0 + dot(p, vec3(0.73, 1.11, 0.57))) * sin(t * 4.3 + dot(p, vec3(1.37, 0.41, 0.93)));
    vec3 leaf = vec3(wind.xy * 0.6 + vec2(-wind.y, wind.x) * 0.3, 0.4) * shiver * sway.w * wind.z * (0.5 + gust);
    // Leaning, it dips a little, so the plant keeps its length.
    float dip = dot(lean, lean) / max(sway.z * 12.0, 1.0);
    return vec3(lean, -dip) + leaf;
}

// The modern mode's haze (render/scene_lighting.h set_haze) over a lit colour at world point `p`: transmittance
// exp(-3.5 (d / length)^2), thinner for points above the camera, towards the haze colour with a glow around the sun.
// Under water (haze.z 2, set_underwater) the light is also absorbed on the way, red first, then green, then blue.
vec3 apply_haze(vec3 color, vec3 p, vec4 camera, vec4 haze, vec4 haze_color, vec4 to_light, vec4 light) {
    if (haze.z < 0.5) return color;
    vec3 d = p - camera.xyz;
    float dist = length(d);
    if (haze.z > 1.5) color *= exp(-dist * vec3(0.018, 0.006, 0.004));
    float x = dist * haze.x;
    float tau = 3.5 * x * x * exp(-max(d.z, 0.0) * haze.y);
    float t = exp(-tau);
    vec3 dir = d / max(dist, 1e-4);
    float glow = pow(max(dot(dir, to_light.xyz), 0.0), 6.0) * haze.w;
    return mix(haze_color.rgb + light.rgb * glow * 0.25, color, t);
}

// Asset-free hidden-window Vulkan smoke probe. This checks actual generic
// pipeline/record/submit/present outcomes and PNG color/depth readback; it does
// not exercise server protocols, terrain projection or live encounters.
//
// world_geometry_probe SHADER_DIR [OUTPUT.png] [MSAA=1] [fsr1|taa|fsr3|taa-fg]
#include <array>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "assets/png_read.h"
#include "assets/zone.h"
#include "assets/ground_refine.h"
#include "app/world_geometry_queries.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "game/world_geometry.h"
#include "game/visual_surface_patch.h"
#include "platform/paths.h"
#include "platform/window.h"
#include "render/vk_context.h"
#include "render/world_geometry_renderer.h"
#include "plugin/world_geometry.h"
#include "probe_surface_renderer.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int floor_benchmark(const fs::path& game_path,std::string_view zone_name) {
    const GameDir game(game_path);
    auto zone=assets::load_zone(game,zone_name);
    const auto refinement=assets::refine_ground(zone); // the client's default rendered geometry
    const auto mesh=game::CollisionMesh::build_visual(zone);
    auto animated=game::AnimatedObjects::build(zone);
    require(mesh.bounds().has_value(),"benchmark zone has no collision bounds");
    const auto center=(mesh.bounds()->first+mesh.bounds()->second)*.5f;
    glm::vec3 origin{};
    float best_distance=std::numeric_limits<float>::max();
    for(const auto& triangle:mesh.triangles()){
        if(!(triangle.normal.z>.5f)||(triangle.flags&(game::kTriangleNoVisualSurface|game::kTriangleInvisible)))continue;
        const auto point=(triangle.v0+triangle.v1+triangle.v2)/3.0f;
        const auto distance=glm::distance(glm::vec2(point),glm::vec2(center));
        if(distance<best_distance){origin=point;best_distance=distance;}
    }
    origin.z+=12;
    plugin::WorldGeometryHost host;
    plugin::WorldQueries queries;
    queries.floor=[&](float x,float y,float z,float& height){
        const auto floor=app::world_geometry_surface_floor(mesh,&animated,
            [](const glm::vec3&){return std::optional<float>{};},{x,y,z});
        if(!floor)return HAILSTONE_WORLD_MISSING;
        height=*floor;return HAILSTONE_WORLD_RESOLVED;
    };
    queries.surface_patch=[&](const HailstoneWorldSurfacePatch& input,HailstoneWorldSurfacePatchResult& output,std::uint32_t& work){
        game::VisualSurfacePatch query;
        for(std::size_t n=0;n<3;++n)query.vertices[n]={input.vertices[n*3],input.vertices[n*3+1],input.vertices[n*3+2]};
        query.maximum_rise=input.maximum_rise;query.tolerance=input.tolerance;
        const auto result=game::certify_visual_surface_patch(query,
            [&](const glm::vec2& low,const glm::vec2& high,const game::VisualTriangleVisitor& visit,std::uint32_t& budget){
                return mesh.visual_surfaces(low,high,visit,budget)&&animated.visual_surfaces(low,high,visit,budget);
            },[&](const glm::vec3& at){return app::world_geometry_surface_floor(mesh,&animated,
                [](const glm::vec3&){return std::optional<float>{};},at);},work);
        if(result.status==game::VisualSurfacePatchStatus::Deferred)return HAILSTONE_WORLD_PATCH_DEFERRED;
        output.flags=result.height_known?HAILSTONE_WORLD_PATCH_HEIGHT_KNOWN:0;
        output.witness[0]=result.witness.x;output.witness[1]=result.witness.y;output.witness[2]=result.witness.z;
        if(result.source_face_known){
            output.flags|=HAILSTONE_WORLD_PATCH_SOURCE_FACE;
            for(std::size_t n=0;n<3;++n){output.source_face[n*3]=result.source_face[n].x;
                output.source_face[n*3+1]=result.source_face[n].y;output.source_face[n*3+2]=result.source_face[n].z;}
        }
        if(result.status==game::VisualSurfacePatchStatus::Planar)return HAILSTONE_WORLD_PATCH_PLANAR;
        return result.status==game::VisualSurfacePatchStatus::Mismatch?HAILSTONE_WORLD_PATCH_MISMATCH:HAILSTONE_WORLD_PATCH_MISSING;
    };
    std::printf("native drawn visual-floor benchmark zone=%.*s triangles=%zu refined_triangles=%zu animated_parts=%zu origin=%.2f,%.2f,%.2f\n",
        static_cast<int>(zone_name.size()),zone_name.data(),mesh.triangle_count(),static_cast<std::size_t>(refinement.triangles),
        animated.part_count(),origin.x,origin.y,origin.z);
    double checksum=0;
    for(const auto sample: {std::pair<std::size_t,float>{79592,200.0f}, {185795,50.0f}, {5800,200.0f}, {23225,50.0f}}){
        std::array<double,3> elapsed{};
        std::size_t resolved=0;
        for(std::size_t run=0;run<elapsed.size();++run){
            HailstoneWorldFrame frame{};frame.size=sizeof(frame);frame.frame=run+1;frame.epoch=1;
            frame.capabilities=HAILSTONE_WORLD_GEOMETRY|HAILSTONE_WORLD_TERRAIN;
            frame.remaining_queries=plugin::WorldGeometryHost::kMaximumQueries;
            host.begin(frame,queries);
            resolved=0;
            const auto start=std::chrono::steady_clock::now();
            for(std::size_t index=0;index<sample.first;++index){
                const float x=origin.x+(static_cast<float>(index%211)/210*2-1)*sample.second;
                const float y=origin.y+(static_cast<float>((index/211)%211)/210*2-1)*sample.second;
                float z=0;
                const int result=host.floor(frame.frame,frame.epoch,x,y,origin.z,&z);
                if(result==HAILSTONE_WORLD_RESOLVED){++resolved;checksum+=z;}
            }
            elapsed[run]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        }
        std::sort(elapsed.begin(),elapsed.end());
        std::printf("queries=%zu radius=%.0f median_ms=%.3f min_ms=%.3f max_ms=%.3f ns_per_query=%.1f resolved=%zu\n",
            sample.first,sample.second,elapsed[1],elapsed[0],elapsed[2],elapsed[1]*1e6/static_cast<double>(sample.first),resolved);
    }
    HailstoneWorldFrame patch_frame{};patch_frame.size=sizeof(patch_frame);patch_frame.frame=10;patch_frame.epoch=1;
    patch_frame.capabilities=HAILSTONE_WORLD_GEOMETRY|HAILSTONE_WORLD_TERRAIN|HAILSTONE_WORLD_SURFACE_PATCH;
    patch_frame.remaining_queries=plugin::WorldGeometryHost::kMaximumQueries;
    host.begin(patch_frame,queries);
    std::array<std::size_t,4> statuses{};
    std::size_t missing_vertices=0;
    const auto patch_start=std::chrono::steady_clock::now();
    for(std::size_t n=0;n<1024;++n){
        const float x=origin.x+(static_cast<float>(n%32)-16)*4;
        const float y=origin.y+(static_cast<float>(n/32)-16)*4;
        const std::array<glm::vec2,3> points{{{x,y},{x+4,y},{x,y+4}}};
        HailstoneWorldSurfacePatch patch{};patch.size=sizeof(patch);patch.frame=10;patch.epoch=1;
        patch.maximum_rise=6;patch.tolerance=.01f;
        bool valid=true;
        for(std::size_t corner=0;corner<3;++corner){
            float height=0;
            if(host.floor(10,1,points[corner].x,points[corner].y,origin.z,&height)!=HAILSTONE_WORLD_RESOLVED){valid=false;break;}
            patch.vertices[corner*3]=points[corner].x;patch.vertices[corner*3+1]=points[corner].y;patch.vertices[corner*3+2]=height;
        }
        if(!valid){++missing_vertices;continue;}
        HailstoneWorldSurfacePatchResult result{};result.size=sizeof(result);
        const auto status=host.surface_patch(&patch,&result);
        ++statuses[static_cast<std::size_t>(status)];
    }
    const auto patch_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-patch_start).count();
    std::printf("patch_grid1024 ms=%.3f planar=%zu mismatch=%zu missing=%zu deferred=%zu missing_vertices=%zu work_used=%u\n",
        patch_ms,statuses[1],statuses[2],statuses[3],statuses[0],missing_vertices,
        patch_frame.remaining_queries-host.frame().remaining_queries);
    std::printf("checksum=%.6f (static placed props and current animated parts included; no connected-server dynamic doors)\n",checksum);
    return 0;
}

std::array<game::WorldGeometryVertex, 6> quad(float left, float bottom, float right, float top, float depth,
                                            std::array<float, 4> color) {
    const game::WorldGeometryVertex a{{left, bottom, depth}, color};
    const game::WorldGeometryVertex b{{right, bottom, depth}, color};
    const game::WorldGeometryVertex c{{right, top, depth}, color};
    const game::WorldGeometryVertex d{{left, top, depth}, color};
    // Opposite winding on the two halves: both must appear with CullNone.
    return {a, b, c, a, d, c};
}

// A static screen-aligned occluder plane, written into the real scene's color
// and reverse-Z depth attachments. This isolates the depth test without adding
// another renderer, shaders, resources or any height-sampling fallback.
void occluder(const render::FrameInfo& frame) {
    VkClearAttachment clear[2]{};
    clear[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear[0].colorAttachment = 0;
    clear[0].clearValue.color = {{0.125f, 0.125f, 0.125f, 1.0f}}; // linear scene color
    clear[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    clear[1].clearValue.depthStencil = {0.6f, 0};
    const auto x = frame.extent.width * 7 / 16;
    const auto y = frame.extent.height / 4;
    VkClearRect rectangle{};
    rectangle.rect.offset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
    rectangle.rect.extent = {frame.extent.width / 8, frame.extent.height / 2};
    rectangle.layerCount = 1;
    vkCmdClearAttachments(frame.cmd, 2, clear, 1, &rectangle);
}

using Rgb = std::array<unsigned, 3>;

std::vector<game::WorldGeometryVertex> read_vertices(const fs::path& path) {
    std::ifstream input(path);
    require(input.good(), "could not open the triangle fixture");
    std::vector<game::WorldGeometryVertex> vertices;
    for (;;) {
        input >> std::ws;
        if (input.eof()) break;
        require(vertices.size() < game::kWorldGeometryMaxVertices, "triangle fixture exceeds the frame budget");
        game::WorldGeometryVertex v;
        require(static_cast<bool>(input >> v.position[0] >> v.position[1] >> v.position[2]
                                         >> v.color[0] >> v.color[1] >> v.color[2] >> v.color[3]),
                "triangle fixture has an incomplete or invalid vertex");
        vertices.push_back(v);
    }
    require(game::validate_world_geometry({{1,1,1},game::WorldGeometryPrimitive::Triangles,vertices}) ==
                game::WorldGeometryValidation::Valid, "triangle fixture failed world geometry validation");
    return vertices;
}

void verify_fixture_png(const fs::path& path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    const auto size=input.tellg();require(input.good()&&size>0&&size<32*1024*1024,"invalid fixture readback size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));input.seekg(0);
    require(static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size))),"fixture readback failed");
    auto image=assets::decode_png(bytes);require(image&&image->format==assets::PixelFormat::RGBA8&&!image->mips.empty(),"fixture PNG failed decoding");
    std::size_t colored=0;const auto& pixels=image->mips.front();
    for(std::size_t at=0;at+3<pixels.size();at+=4){
        const auto r=std::to_integer<unsigned>(pixels[at]),g=std::to_integer<unsigned>(pixels[at+1]),b=std::to_integer<unsigned>(pixels[at+2]);
        if(std::max({r,g,b})>40&&std::max({r,g,b})-std::min({r,g,b})>20)++colored;
    }
    require(colored>1000,"production geometry fixture produced too few colored pixels");
    std::printf("production fixture readback PASS %ux%u colored_pixels=%zu\n",image->width,image->height,colored);
}

Rgb pixel(const assets::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 4;
    const auto& pixels = image.mips.front();
    return {std::to_integer<unsigned>(pixels[index]), std::to_integer<unsigned>(pixels[index + 1]),
            std::to_integer<unsigned>(pixels[index + 2])};
}

Rgb sample(const assets::Image& image, std::uint32_t x_percent, std::uint32_t y_percent) {
    return pixel(image, image.width * x_percent / 100, image.height * y_percent / 100);
}

void verify_png(const fs::path& path, std::uint32_t sample_count, bool temporal) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(file.good(), "screenshot was not written");
    const auto size = file.tellg();
    require(size > 0 && size < 16 * 1024 * 1024, "unexpected screenshot size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    require(static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))),
            "could not read screenshot");
    std::string error;
    auto image = assets::decode_png(bytes, &error);
    if (!image) throw std::runtime_error("PNG readback failed: " + error);
    require(image->format == assets::PixelFormat::RGBA8 && image->width >= 320 && image->height >= 240 &&
                !image->mips.empty() && image->mips.front().size() == std::size_t{image->width} * image->height * 4,
            "unexpected screenshot image layout");
    // Both triangle windings at left are red. The right quad's 0.5 alpha is
    // blended in linear light, then sRGB encoded by the existing post pass.
    for (auto point : {std::array<std::uint32_t, 2>{15, 40}, std::array<std::uint32_t, 2>{25, 60}}) {
        const auto red = sample(*image, point[0], point[1]);
        require(red[0] > 230 && red[1] < 40 && red[2] < 40, "a colored triangle or winding is missing");
    }
    const auto green = sample(*image, 78, 45);
    require(green[0] < 40 && green[1] > 150 && green[1] < 220 && green[2] < 40,
            "linear alpha blend readback did not match");
    const auto hidden = sample(*image, 50, 30);
    require(hidden[0] > 70 && hidden[0] < 130 && hidden[1] > 70 && hidden[1] < 130 &&
                hidden[2] > 70 && hidden[2] < 130,
            "geometry behind the depth plane was not occluded");
    const auto front = sample(*image, 50, 70);
    require(front[0] < 40 && front[1] > 230 && front[2] > 230,
            "geometry in front of the depth plane is missing");
    const auto control = sample(*image, 50, 88);
    require(control[0] < 10 && control[1] < 10 && control[2] < 10,
            "the line's background control pixel is not black");
    // A one-pixel line need not cover every MSAA sample. Resolve averages the
    // covered linear yellow samples with black, then post sRGB-encodes it. Even
    // one covered sample must exceed this threshold (eight byte values allow
    // rounding). This still requires a yellow signature over a long span,
    // distinct from the black control, rather than accepting a missing line.
    const double coverage = 1.0 / static_cast<double>(sample_count);
    const double encoded = coverage <= 0.0031308 ? coverage * 12.92
                                                 : 1.055 * std::pow(coverage, 1.0 / 2.4) - 0.055;
    // A temporal resolve distributes a one-pixel line across neighboring samples.
    // Its visible yellow span remains required; the unfiltered peak applies only to MSAA.
    const auto line_minimum = temporal ? 64u : static_cast<unsigned>(std::floor(encoded * 255.0)) - 8;
    std::uint32_t yellow_pixels = 0;
    Rgb line_peak{};
    for (std::uint32_t y = image->height * 91 / 100; y < image->height * 94 / 100; ++y)
        for (std::uint32_t x = image->width / 4; x < image->width * 3 / 4; ++x) {
            const auto color = pixel(*image, x, y);
            if (color[0] >= line_minimum && color[1] >= line_minimum && color[2] < 10 &&
                std::abs(static_cast<int>(color[0]) - static_cast<int>(color[1])) <= 8) {
                ++yellow_pixels;
                if (color[0] > line_peak[0]) line_peak = color;
            }
        }
    require(yellow_pixels >= image->width / 4, "the one-pixel line pipeline produced no expected span");
    std::printf("readback PASS %ux%u red/alpha/occlusion/front/line alpha=%u,%u,%u occluded=%u,%u,%u "
                "line=%u,%u,%u minimum=%u line_pixels=%u control=%u,%u,%u\n",
                image->width, image->height, green[0], green[1], green[2], hidden[0], hidden[1], hidden[2],
                line_peak[0], line_peak[1], line_peak[2], line_minimum, yellow_pixels,
                control[0], control[1], control[2]);
}

} // namespace

int main(int argc, char** argv) {
    if(argc==4&&std::string_view(argv[1])=="--floor-bench"){
        try{return floor_benchmark(fs::absolute(argv[2]),argv[3]);}
        catch(const std::exception& error){std::fprintf(stderr,"floor benchmark FAILED: %s\n",error.what());return 1;}
    }
    if (argc < 2 || argc > 7 || std::string_view(argv[1]) == "--help") {
        std::puts("usage: world_geometry_probe SHADER_DIR [OUTPUT.png] [MSAA=1] [fsr1|taa|fsr3|taa-fg] [TRIANGLE_VERTICES.txt] [OPAQUE_SURFACE_VERTICES.txt]");
        return argc == 2 ? 0 : 2;
    }
    try {
        const fs::path shader_dir = fs::absolute(argv[1]);
        const fs::path output = fs::absolute(argc >= 3 ? fs::path(argv[2]) : fs::path("build-b7/world_geometry_probe.png"));
        const auto fixture=argc>=6?read_vertices(fs::absolute(argv[5])):std::vector<game::WorldGeometryVertex>{};
        const auto surfaces=argc>=7?read_vertices(fs::absolute(argv[6])):std::vector<game::WorldGeometryVertex>{};
        glm::mat4 camera_view(1.0f),camera_projection(1.0f);
        if(!fixture.empty()){
            glm::vec3 low(fixture.front().position[0],fixture.front().position[1],fixture.front().position[2]),high=low;
            for(const auto& v:fixture){const glm::vec3 at(v.position[0],v.position[1],v.position[2]);low=glm::min(low,at);high=glm::max(high,at);}
            const glm::vec3 middle=(low+high)*.5f;
            constexpr float aspect=4.0f/3.0f;
            const float half_h=std::max({(high.y-low.y)*.5f,(high.x-low.x)*.5f/aspect,1.0f})*1.1f;
            const float reach=std::max(half_h*aspect,high.z-low.z)+10.0f;
            camera_view=glm::lookAtRH(glm::vec3(middle.x,middle.y,high.z+reach*2),middle,glm::vec3(0,1,0));
            camera_projection=glm::orthoRH_ZO(-half_h*aspect,half_h*aspect,-half_h,half_h,reach*4,0.1f);
            if(!surfaces.empty()){
                for(const auto& v:surfaces){const glm::vec3 at(v.position[0],v.position[1],v.position[2]);low=glm::min(low,at);high=glm::max(high,at);}
                const auto center=(low+high)*.5f;
                const float distance=glm::length(high-low)*2+10;
                camera_view=glm::lookAtRH(center+glm::normalize(glm::vec3{.8f,-1.1f,1.6f})*distance,center,glm::vec3{0,0,1});
                glm::vec3 view_low(std::numeric_limits<float>::max()),view_high(std::numeric_limits<float>::lowest());
                for(int corner=0;corner<8;++corner){
                    const glm::vec3 at{corner&1?high.x:low.x,corner&2?high.y:low.y,corner&4?high.z:low.z};
                    const auto point=glm::vec3(camera_view*glm::vec4(at,1));
                    view_low=glm::min(view_low,point);view_high=glm::max(view_high,point);
                }
                const auto view_center=(view_low+view_high)*.5f;
                const float half_height=std::max((view_high.y-view_low.y)*.5f,(view_high.x-view_low.x)*.5f/aspect)*1.1f;
                camera_projection=glm::orthoRH_ZO(view_center.x-half_height*aspect,view_center.x+half_height*aspect,
                    view_center.y-half_height,view_center.y+half_height,distance*4,.1f);
            }
            std::printf("fixture bounds %.2f %.2f %.2f .. %.2f %.2f %.2f vertices=%zu\n",low.x,low.y,low.z,high.x,high.y,high.z,fixture.size());
        }
        std::uint32_t samples = 1;
        if (argc >= 4) {
            const std::string_view value(argv[3]);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), samples);
            require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                        (samples == 1 || samples == 2 || samples == 4 || samples == 8),
                    "MSAA must be 1, 2, 4 or 8");
        }
        fs::create_directories(output.parent_path());
        platform::use_data_dir(output.parent_path() / "world_geometry_probe_data", "world geometry probe");
        const fs::path log_path = output.parent_path() / (output.stem().string() + ".log");
        log::open_file(log_path);
        std::error_code removed;
        fs::remove(output, removed); // only the explicitly named probe output, never a recursive cleanup
        require(!removed, "could not replace the probe's screenshot output");
        platform::WindowDesc description;
        description.title = "Hailstone world geometry probe";
        description.width = fixture.empty()?640:1280;
        description.height = fixture.empty()?480:960;
        description.resizable = false;
        description.hidden = true;
        platform::Window window(description);
        require(!window.focused(), "the hidden probe window unexpectedly has keyboard focus");
        render::VkContext vk(window, true); // requests validation; the existing context logs when the layer is absent
        render::SceneOptions options;
        options.msaa = samples;
        options.tonemap = render::Tonemap::None;
        options.dither = false;
        options.shadow_size = options.light_shadows = 0;
        options.ambient_occlusion = options.distance_blur = options.color_grade = false;
        options.bloom = 0.0f;
        const std::string_view mode = argc >= 5 ? argv[4] : "fsr1";
        require(mode == "fsr1" || mode == "taa" || mode == "fsr3" || mode == "taa-fg", "unknown upscaler mode");
        options.upscaler = mode == "fsr1" ? render::Upscaler::Fsr1 : mode == "fsr3" ? render::Upscaler::Fsr3 : render::Upscaler::Taa;
        options.frame_gen = mode == "taa-fg";
        options.sharpness = 0.0f;
        vk.set_scene_options(options, shader_dir);
        vk.set_vsync(render::VkContext::Vsync::Off);
        require(vk.can_screenshot(), "the surface cannot provide screenshot readback");
        render::WorldGeometryRenderer renderer(vk, shader_dir);
        std::unique_ptr<ProbeSurfaceRenderer> surface_renderer;
        if(!surfaces.empty())surface_renderer=std::make_unique<ProbeSurfaceRenderer>(vk,shader_dir,surfaces);
        std::uint32_t submitted = 0, presented = 0, recorded = 0;
        bool captured = false;
        for (unsigned attempt = 0; attempt < 32 && !captured; ++attempt) {
            require(window.poll_events(), "probe window was closed");
            const auto frame = vk.begin_frame_commands();
            if (!frame) continue;
            if (mode != "fsr1") vk.set_temporal_view(camera_view,camera_projection);
            vk.begin_scene(*frame, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            if(fixture.empty())occluder(*frame);
            // Identity projection deliberately places these test-world positions
            // in clip space. Vulkan's existing flipped scene viewport is used.
            const glm::mat4 view_proj=camera_projection*camera_view;
            if(surface_renderer)surface_renderer->record(*frame,view_proj);
            const bool red_frame = mode != "fsr1" || attempt % 2 == 0;
            const auto left = quad(-0.8f, -0.4f, -0.4f, 0.4f, 0.4f,
                                   red_frame ? std::array<float, 4>{1, 0, 0, 1} : std::array<float, 4>{0, 0, 1, 1});
            const auto right = quad(0.4f, -0.4f, 0.8f, 0.4f, 0.4f, {0, 1, 0, 0.5f});
            const auto behind = quad(-0.12f, 0.2f, 0.12f, 0.6f, 0.4f, {1, 0, 1, 1});
            const auto front = quad(-0.12f, -0.6f, 0.12f, -0.2f, 0.8f, {0, 1, 1, 1});
            const std::array<game::WorldGeometryVertex, 2> line{{
                {{-0.8f, -0.85f, 0.7f}, {1, 1, 0, 1}}, {{0.8f, -0.85f, 0.7f}, {1, 1, 0, 1}}}};
            const auto record = [&](std::uint64_t tag, game::WorldGeometryPrimitive primitive,
                                    std::span<const game::WorldGeometryVertex> vertices) {
                const game::WorldGeometryBatch batch{{4294967297ull, tag, attempt + 1}, primitive, vertices};
                const auto result = renderer.record(*frame, view_proj, batch);
                require(result.id == batch.id && result.frame == vk.frames_begun() &&
                            result.status == render::WorldGeometryRecordStatus::Recorded &&
                            result.recorded_vertices == vertices.size(),
                        "a complete batch did not record with the exact identity and count");
                ++recorded;
            };
            if(!fixture.empty())record(4294967301ull,game::WorldGeometryPrimitive::Triangles,fixture);
            else{
                record(4294967301ull, game::WorldGeometryPrimitive::Triangles, left);
                record(4294967302ull, game::WorldGeometryPrimitive::Triangles, right);
                record(4294967303ull, game::WorldGeometryPrimitive::Triangles, behind);
                record(4294967304ull, game::WorldGeometryPrimitive::Triangles, front);
                record(4294967305ull, game::WorldGeometryPrimitive::Lines, line);
            }
            const bool capture = presented >= 7 && red_frame; // reuse both fenced slots several times, alternating data
            if (capture) require(vk.request_screenshot(output), "screenshot request was refused");
            const auto outcome = vk.end_frame(*frame);
            submitted += outcome.submitted ? 1u : 0u;
            presented += outcome.presented ? 1u : 0u;
            require(!outcome.presented || outcome.submitted, "presentation was reported without submission");
            captured = capture && outcome.submitted && outcome.presented;
        }
        require(captured && presented >= 8, "not enough complete frames reached an accepted presentation");
        vk.wait_idle();
        if(fixture.empty())verify_png(output, static_cast<std::uint32_t>(vk.scene_samples()),vk.temporal());
        else verify_fixture_png(output);
        std::printf("Vulkan PASS gpu=\"%s\" samples=%u mode=%.*s temporal=%u frame_gen=%u submitted=%u presented=%u recorded_batches=%u hidden=1\n",
                    vk.gpu_description().c_str(), static_cast<unsigned>(vk.scene_samples()), static_cast<int>(mode.size()), mode.data(),
                    vk.temporal()?1u:0u,vk.frame_gen_active()?1u:0u,submitted,presented,recorded);
        std::printf("PNG %s\nLOG %s\n", output.string().c_str(), log_path.string().c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "world geometry probe FAILED: %s\n", error.what());
        return 1;
    }
}

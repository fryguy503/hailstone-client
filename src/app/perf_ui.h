#pragma once

// The performance window (developer UI, Ctrl+Shift+P or ui.perf_window=1 in hailstone.ini): the frame rate and
// frame times, where each frame's time goes (the main loop's phases), the renderers' counts, GPU and process
// memory, and the world and zone sessions' traffic.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::net { class GameConnection; struct SessionStats; }
namespace hailstone::render { struct ZoneRenderStats; struct ActorRenderStats; class VkContext; }
namespace hailstone { class SharedFileCache; }

namespace hailstone::app {

// The main loop's phases, in the order run() marks them.
enum class FramePhase : std::uint8_t {
    Sleep,   // the --max-fps wait
    Update,  // login, connection, player movement, spawn smoothing
    Ui,      // ImGui and Lua windows
    Prepare, // sky clock, audio, particles, fog
    Wait,    // VkContext::begin_frame: the frame-in-flight fence and the swapchain image (GPU-bound or vsync)
    Record,  // drawing the zone, actors and UI into the command buffer
    Present, // VkContext::end_frame: submit and present
    Count
};
inline constexpr std::size_t kFramePhases = static_cast<std::size_t>(FramePhase::Count);
using PhaseTimes = std::array<float, kFramePhases>;

// A frame that took this long against the typical frame time (`typical_ms`, 0 while unknown) is a hitch.
bool is_hitch(float frame_ms, float typical_ms);
// The log line for a hitch: its time, the typical time, then the phases that took 1 ms or more, longest first
// ("other" is the time no phase was charged with: events, the window system).
// `sections` (core/frame_sections.h) of 1 ms or more follow, longest first.
std::string hitch_text(float frame_ms, float typical_ms, const PhaseTimes& phases,
                       std::vector<std::pair<const char*, float>> sections = {});

struct PerfInputs {
    const render::ZoneRenderStats* zone = nullptr;
    const render::ActorRenderStats* actors = nullptr;
    const net::GameConnection* conn = nullptr;
    const render::VkContext* gpu = nullptr; // the GPU's name and memory heaps
    const SharedFileCache* shared_files = nullptr;
    std::string zone_name;
    std::size_t spawns = 0;
    glm::vec3 camera{0.0f};
    glm::vec2 display{0.0f};
};

class PerfUi {
public:
    // At the top of each main-loop iteration; the time since the last call is the previous frame's time.
    void begin_frame();
    // At the end of `phase`: the time since the last mark (or begin_frame) is charged to it.
    void mark(FramePhase phase);
    void draw(const PerfInputs& in);

    void toggle() { open_ = !open_; }
    void set_open(bool open) { open_ = open; }
    bool open() const { return open_; }
    // Each hitch (is_hitch) is logged with its phases (ui.perf_log_hitches).
    void set_log_hitches(bool on) { log_hitches_ = on; }
    bool log_hitches() const { return log_hitches_; }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kFrames = 300; // history kept for the graph and percentiles

    struct NetRate {
        Clock::time_point at{};
        std::uint64_t datagrams_sent = 0, datagrams_received = 0, bytes_sent = 0, bytes_received = 0;
        float sent_per_s = 0.0f, received_per_s = 0.0f, bytes_sent_per_s = 0.0f, bytes_received_per_s = 0.0f;
    };
    void session_section(const char* name, const std::optional<net::SessionStats>& stats, NetRate& rate);

    bool open_ = false;
    bool paused_ = false;
    bool log_hitches_ = false;
    float typical_ms_ = 0.0f; // a running average of the frames that were not hitches
    Clock::time_point frame_start_{}, last_mark_{};
    std::uint64_t frames_ = 0;
    Clock::time_point started_ = Clock::now();
    std::array<float, kFrames> frame_ms_{};
    std::array<PhaseTimes, kFrames> phase_ms_{};
    PhaseTimes current_{};
    std::size_t head_ = 0;  // next slot to write
    std::size_t count_ = 0; // filled slots
    NetRate world_rate_, zone_rate_;
};

} // namespace hailstone::app

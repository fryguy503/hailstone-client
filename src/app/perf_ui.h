#pragma once

// The performance window (developer UI, Ctrl+Shift+P or ui.perf_window=1 in hailstone.ini): the frame rate and
// frame times, where each frame's time goes (the main loop's phases), frame generation's rates and costs, the
// renderers' counts, GPU and process memory, and the world and zone sessions' traffic.

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
// The typical frame time hitches are measured against: a running average of the frames that were not hitches. A run
// of kHitchRunReset hitches in a row means the frame time itself changed (a heavier zone, a slower power state, a
// lower refresh): the average starts again from the frame that ends the run, so it does not call every frame a hitch
// for good.
inline constexpr int kHitchRunReset = 30;
struct HitchBaseline {
    float typical_ms = 0.0f; // 0 while unknown
    int run = 0;             // hitches in a row
    // Takes a frame's time; true when it was a hitch.
    bool add(float frame_ms);
};
// The log line for a hitch: its time, the typical time, then the phases that took 1 ms or more, longest first
// ("other" is the time no phase was charged with: events, the window system).
// `sections` (core/frame_sections.h) of 1 ms or more follow, longest first.
std::string hitch_text(float frame_ms, float typical_ms, const PhaseTimes& phases,
                       std::vector<std::pair<const char*, float>> sections = {});

// The frame sections (core/frame_sections.h) of the last `frames` frames: each one's average per frame and its
// slowest frame, longest average first, with the bytes each allocated a frame on average (frame_sections::add_bytes;
// a section with bytes and no time is listed too). A section missing from a frame counts 0 there. Names are compared
// by address, as frame_sections::add does (literals and frame_sections::intern).
class SectionAverages {
public:
    using Sections = std::vector<std::pair<const char*, float>>;
    using Allocations = std::vector<std::pair<const char*, std::size_t>>;
    struct Row {
        const char* name;
        float avg_ms;
        float max_ms;
        float avg_bytes = 0.0f;
        bool has_bytes = false; // bytes were counted for it (the Lua windows), so 0 means none allocated
    };
    explicit SectionAverages(std::size_t frames = 60) : frames_(frames == 0 ? 1 : frames) { history_.reserve(frames_); }
    void add(Sections sections, Allocations bytes = {}) { add_swap(sections, bytes); }
    // add() without a copy: the frame's vectors go into the ring and come back holding the dropped frame's (or
    // empty), whose storage the caller reuses (frame_sections::take_into), so the steady state allocates nothing.
    void add_swap(Sections& sections, Allocations& bytes);
    std::vector<Row> rows() const;
    std::size_t frames() const { return history_.size(); }

private:
    struct Frame {
        Sections sections;
        Allocations bytes;
    };
    std::size_t frames_;
    std::vector<Frame> history_; // a ring of up to frames_ frames
    std::size_t head_ = 0;       // next slot to write once full
};

// The "Frame sections" table is worked out again this often, not every frame: SectionAverages::rows goes over every
// section of 60 frames and sorts them (hundreds with plugin windows), and numbers that change every frame cannot be
// read anyway.
inline constexpr std::chrono::milliseconds kSectionsRefresh{500};
// Whether a view last worked out at `last` (the clock's epoch: never) is due again at `now`.
inline bool refresh_due(std::chrono::steady_clock::time_point last, std::chrono::steady_clock::time_point now,
                        std::chrono::steady_clock::duration every) {
    return last == std::chrono::steady_clock::time_point{} || now - last >= every;
}

// The frame rates over a history of frames: `ms` each real frame's time, `presented` how many images it put on the
// screen (2 with an in-between frame from frame generation, else 1; same length). Rendered counts real frames,
// displayed every image; `generated_share` is the share of real frames that had an in-between frame.
struct FrameRates {
    float rendered_fps = 0.0f;
    float displayed_fps = 0.0f;
    float generated_share = 0.0f;
};
// `generated`, when given (same length), says which frames' second image was an in-between frame (else the real
// frame shown again: a cut); without it, every frame with two images had one.
FrameRates frame_rates(const std::vector<float>& ms, const std::vector<std::uint8_t>& presented,
                       const std::vector<std::uint8_t>& generated = {});

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
    // After the frame is presented: how many images it put on the screen (VkContext::frames_presented).
    void set_presented(std::uint32_t images, bool generated) {
        current_presented_ = static_cast<std::uint8_t>(images);
        current_generated_ = generated ? 1u : 0u;
    }
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
    HitchBaseline baseline_;
    Clock::time_point frame_start_{}, last_mark_{};
    std::uint64_t frames_ = 0;
    Clock::time_point started_ = Clock::now();
    std::array<float, kFrames> frame_ms_{};
    std::array<PhaseTimes, kFrames> phase_ms_{};
    std::array<std::uint8_t, kFrames> presented_{};
    std::array<std::uint8_t, kFrames> generated_{};
    PhaseTimes current_{};
    SectionAverages sections_;
    SectionAverages::Sections taken_sections_; // the frame's sections, their storage recycled (add_swap)
    SectionAverages::Allocations taken_bytes_;
    // The GPU's passes (VkContext::gpu_sections, timed while the window is open), each frame read back once.
    SectionAverages gpu_sections_;
    SectionAverages::Sections gpu_taken_;
    SectionAverages::Allocations gpu_no_bytes_;
    std::uint64_t gpu_serial_ = 0;
    std::vector<SectionAverages::Row> section_rows_; // the table's rows, filtered, as of sections_at_
    Clock::time_point sections_at_{};
    std::uint8_t current_presented_ = 1;
    std::uint8_t current_generated_ = 0;
    std::size_t head_ = 0;  // next slot to write
    std::size_t count_ = 0; // filled slots
    NetRate world_rate_, zone_rate_;
};

} // namespace hailstone::app

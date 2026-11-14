#pragma once

// Building a texture pack (docs/texstudio.md): the textures read back from their archives, upscaled with their maps
// on the GPU, and encoded into the pack's by-hash/ DDS files, in three stages that overlap (a reader, the GPU worker,
// a pool of encoders). A stopped build resumes: a texture's files already in the pack are not made again.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "texpack/materials.h"
#include "texpack/texpack.h"
#include "texpack/upscale.h"

namespace hailstone::texpack {

// The models a GPU worker holds, made on its thread (ncnn wants its nets used from the thread that made them).
class ModelSet {
public:
    virtual ~ModelSet() = default;
    virtual Models models() = 0;
};
using ModelSetFactory = std::function<std::unique_ptr<ModelSet>(std::string* error)>;

// One thread that owns the models and runs tasks with them in turn: the build's textures, and the previews and the
// speed test, which go to the front of the queue.
class GpuWorker {
public:
    explicit GpuWorker(ModelSetFactory factory);
    ~GpuWorker(); // finishes the task running, drops the rest
    GpuWorker(const GpuWorker&) = delete;
    GpuWorker& operator=(const GpuWorker&) = delete;

    using Task = std::function<void(const Models&)>;
    void post(Task task, bool urgent = false);
    // Whether the models loaded (false until the thread has tried; error() says why they did not).
    bool ready() const { return ready_; }
    bool failed() const { return failed_; }
    std::string error() const;
    std::size_t queued() const;

private:
    void run(ModelSetFactory factory);
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    bool stop_ = false;
    std::atomic<bool> ready_{false}, failed_{false};
    std::string error_;
    std::thread thread_;
};

// What a pack was built with (PACK/pack.ini): the tool reads it to show the pack and to tell whether a build would
// add to it or needs a new pack.
struct PackInfo {
    std::string title;          // the player's name for it
    std::string color_model, normal_model, rough_model; // model ids ("" none)
    std::uint32_t scale = 2;
    bool maps = true;
    std::uint64_t textures = 0, files = 0, bytes = 0;
    std::string built;          // when the last build finished, "YYYY-MM-DD HH:MM" local time
    std::string tool_version;   // the tool that built it: as JobOptions::info gives it, else core/version.h's
    bool same_settings(const PackInfo& o) const {
        return color_model == o.color_model && normal_model == o.normal_model && rough_model == o.rough_model &&
               scale == o.scale && maps == o.maps;
    }
};
inline constexpr const char* kPackInfoFile = "pack.ini";
std::optional<PackInfo> read_pack_info(const std::filesystem::path& pack_dir);
bool write_pack_info(const std::filesystem::path& pack_dir, const PackInfo& info);

// The by-hash/ files a pack holds, as "<hash><suffix>" (no extension).
std::set<std::string> pack_contents(const std::filesystem::path& pack_dir);
// The files of `info` the pack still lacks.
std::vector<FileKind> missing_files(const TextureInfo& info, bool maps, const std::set<std::string>& contents);

struct JobStatus {
    enum class State : std::uint8_t { Idle, Running, Paused, Stopping, Done, Cancelled, Failed };
    State state = State::Idle;
    std::size_t total = 0;     // textures with something to make
    std::size_t done = 0;      // of those, finished (or failed)
    std::size_t complete = 0;  // textures already whole in the pack when the build started
    std::size_t failures = 0;
    std::uint64_t files_written = 0, bytes_written = 0;
    double texels_total = 0, texels_done = 0; // source texels times the files made of them, for the rate
    double seconds = 0;                       // running, not paused
    std::string current;                      // a name of the texture on the GPU now
    std::vector<std::string> errors;          // the latest few, oldest first
    std::string fatal;                        // why it stopped, when it failed
    // Seconds left at the rate so far (0 until a few textures are done).
    double eta() const;
};
std::string_view state_name(JobStatus::State state);

struct JobOptions {
    std::filesystem::path pack_dir;
    std::vector<TextureInfo> textures;
    UpscaleSettings settings;
    unsigned encoders = 0;     // 0: the hardware's threads less two
    bool low_impact = false;   // the GPU rests as long as it works, and the encoders are fewer and run at a low priority
    PackInfo info;             // written to pack.ini as the build finishes (its counts filled in)
    // Photo materials (texpack/materials.h): the textures the plan names are composed from the library on the
    // encoders instead of upscaled, and made again when PACK/materials.txt says they were made by other rules (or
    // given back to the upscaler when the plan no longer names them). Both null: none.
    std::shared_ptr<const MaterialPlan> materials;
    std::shared_ptr<MaterialLibrary> material_library;
};

// One build, on threads of its own; the GPU stage runs on `gpu`, which must outlive it.
class PackJob {
public:
    PackJob(JobOptions options, GpuWorker& gpu);
    ~PackJob(); // cancels and waits
    PackJob(const PackJob&) = delete;
    PackJob& operator=(const PackJob&) = delete;

    void pause();
    void resume();
    void cancel();
    // Low impact can change while it runs.
    void set_low_impact(bool on) { low_impact_ = on; }
    JobStatus status() const;
    bool finished() const; // Done, Cancelled or Failed

private:
    struct Loaded;
    struct Made;
    void run();
    void encoder(unsigned index, unsigned count);
    bool finished_locked() const;
    void note_error(std::string text);
    void wait_while_paused();

    JobOptions options_;
    GpuWorker& gpu_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    JobStatus status_;
    std::atomic<bool> cancel_{false}, paused_{false}, low_impact_{false};
    std::deque<std::shared_ptr<Made>> made_; // the GPU's results waiting for an encoder
    std::size_t in_gpu_ = 0;                 // textures posted to the GPU and not back yet
    bool reading_done_ = false;
    std::chrono::steady_clock::time_point resumed_at_;
    std::map<std::uint64_t, std::string> material_record_; // PACK/materials.txt as this build leaves it
    std::thread thread_;
    std::vector<std::thread> encoders_;
};

// A texture made once for the preview (not written to a pack): the source and the pack images, each as RGBA8.
struct Preview {
    TextureInfo info;
    Sources sources;
    Outputs outputs;
    double seconds = 0;
    std::string error;
};

} // namespace hailstone::texpack

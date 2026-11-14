#include "texpack/pack_job.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>

#include "core/build_info.h"
#include "core/config.h"
#include "platform/local_time.h"
#include "platform/process.h"

namespace hailstone::texpack {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---- GpuWorker -----------------------------------------------------------------------------------------------------

GpuWorker::GpuWorker(ModelSetFactory factory) : thread_([this, f = std::move(factory)]() mutable { run(std::move(f)); }) {}

GpuWorker::~GpuWorker() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        tasks_.clear();
    }
    cv_.notify_all();
    thread_.join();
}

void GpuWorker::post(Task task, bool urgent) {
    {
        std::lock_guard lock(mutex_);
        if (urgent) tasks_.push_front(std::move(task));
        else tasks_.push_back(std::move(task));
    }
    cv_.notify_all();
}

std::string GpuWorker::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

std::size_t GpuWorker::queued() const {
    std::lock_guard lock(mutex_);
    return tasks_.size();
}

void GpuWorker::run(ModelSetFactory factory) {
    std::string why;
    std::unique_ptr<ModelSet> set;
    try {
        set = factory(&why);
    } catch (const std::exception& e) {
        why = e.what();
    }
    if (!set) {
        {
            std::lock_guard lock(mutex_);
            error_ = why.empty() ? "the models could not be loaded" : why;
        }
        failed_ = true;
    } else {
        ready_ = true;
    }
    const Models none;
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !tasks_.empty(); });
            if (stop_) break;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        // (a task runs without models when they failed: it sees null models and says so)
        task(set ? set->models() : none);
    }
}

// ---- pack.ini ------------------------------------------------------------------------------------------------------

std::optional<PackInfo> read_pack_info(const fs::path& pack_dir) {
    Config c;
    if (!c.load(pack_dir / kPackInfoFile)) return std::nullopt;
    PackInfo i;
    i.title = c.get_string("title");
    i.color_model = c.get_string("color_model");
    i.normal_model = c.get_string("normal_model");
    i.rough_model = c.get_string("rough_model");
    i.scale = static_cast<std::uint32_t>(std::clamp(c.get_int("scale", 2), 1, 8));
    i.maps = c.get_bool("maps", true);
    auto u64 = [&](const char* key) {
        std::string v = c.get_string(key, "0");
        try {
            return static_cast<std::uint64_t>(std::stoull(v));
        } catch (const std::exception&) {
            return std::uint64_t{0};
        }
    };
    i.textures = u64("textures");
    i.files = u64("files");
    i.bytes = u64("bytes");
    i.built = c.get_string("built");
    i.tool_version = c.get_string("tool_version");
    return i;
}

bool write_pack_info(const fs::path& pack_dir, const PackInfo& i) {
    Config c;
    c.set("title", i.title);
    c.set("color_model", i.color_model);
    c.set("normal_model", i.normal_model);
    c.set("rough_model", i.rough_model);
    c.set_int("scale", static_cast<int>(i.scale));
    c.set_bool("maps", i.maps);
    c.set("textures", std::to_string(i.textures));
    c.set("files", std::to_string(i.files));
    c.set("bytes", std::to_string(i.bytes));
    c.set("built", i.built);
    c.set("tool_version", i.tool_version);
    return c.save(pack_dir / kPackInfoFile);
}

std::set<std::string> pack_contents(const fs::path& pack_dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(pack_dir / "by-hash", ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() != ".dds") continue;
        out.insert(p.stem().string());
    }
    return out;
}

std::vector<FileKind> missing_files(const TextureInfo& info, bool maps, const std::set<std::string>& contents) {
    std::vector<FileKind> out;
    const std::string base = hash_name(info.hash);
    for (FileKind k : pack_files(info, maps))
        if (!contents.contains(base + std::string(suffix(k)))) out.push_back(k);
    return out;
}

// ---- PackJob -------------------------------------------------------------------------------------------------------

double JobStatus::eta() const {
    if (done < 4 || texels_done <= 0 || seconds <= 0) return 0;
    return (texels_total - texels_done) / (texels_done / seconds);
}

std::string_view state_name(JobStatus::State state) {
    using S = JobStatus::State;
    switch (state) {
    case S::Idle: return "idle";
    case S::Running: return "running";
    case S::Paused: return "paused";
    case S::Stopping: return "stopping";
    case S::Done: return "done";
    case S::Cancelled: return "cancelled";
    case S::Failed: return "failed";
    }
    return "idle";
}

struct PackJob::Made {
    TextureInfo info;
    std::vector<FileKind> files;
    std::size_t planned = 0; // files.size() as read (the rate counts them even when the texture failed)
    Outputs outputs;
    const MaterialTile* material = nullptr; // composed by the encoder from `sources`, not upscaled
    std::shared_ptr<Sources> sources;
};

namespace {

struct Todo {
    TextureInfo info;
    std::vector<FileKind> files;
};

std::string now_text() {
    auto tm = platform::local_time(std::time(nullptr));
    if (!tm) return {};
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &*tm);
    return buf;
}

bool write_bytes(const fs::path& path, const std::vector<std::byte>& data) {
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

constexpr std::size_t kInGpu = 6;       // textures read and waiting for (or on) the GPU
constexpr std::size_t kMadeQueue = 24;  // the GPU's results waiting for an encoder
constexpr std::size_t kErrorsKept = 30;

} // namespace

PackJob::PackJob(JobOptions options, GpuWorker& gpu) : options_(std::move(options)), gpu_(gpu) {
    low_impact_ = options_.low_impact;
    status_.state = JobStatus::State::Running;
    resumed_at_ = Clock::now();
    thread_ = std::thread([this] { run(); });
}

PackJob::~PackJob() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

void PackJob::pause() {
    std::lock_guard lock(mutex_);
    if (status_.state != JobStatus::State::Running) return;
    paused_ = true;
    status_.seconds += std::chrono::duration<double>(Clock::now() - resumed_at_).count();
    status_.state = JobStatus::State::Paused;
}

void PackJob::resume() {
    {
        std::lock_guard lock(mutex_);
        if (status_.state != JobStatus::State::Paused) return;
        paused_ = false;
        resumed_at_ = Clock::now();
        status_.state = JobStatus::State::Running;
    }
    cv_.notify_all();
}

void PackJob::cancel() {
    {
        std::lock_guard lock(mutex_);
        if (finished_locked()) return;
        cancel_ = true;
        paused_ = false;
        if (status_.state == JobStatus::State::Running)
            status_.seconds += std::chrono::duration<double>(Clock::now() - resumed_at_).count();
        status_.state = JobStatus::State::Stopping;
    }
    cv_.notify_all();
}

bool PackJob::finished() const {
    std::lock_guard lock(mutex_);
    return finished_locked();
}

bool PackJob::finished_locked() const {
    using S = JobStatus::State;
    return status_.state == S::Done || status_.state == S::Cancelled || status_.state == S::Failed;
}

JobStatus PackJob::status() const {
    std::lock_guard lock(mutex_);
    JobStatus s = status_;
    if (s.state == JobStatus::State::Running) s.seconds += std::chrono::duration<double>(Clock::now() - resumed_at_).count();
    return s;
}

void PackJob::note_error(std::string text) {
    // (with the mutex held)
    status_.errors.push_back(std::move(text));
    if (status_.errors.size() > kErrorsKept) status_.errors.erase(status_.errors.begin());
}

void PackJob::wait_while_paused() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return !paused_ || cancel_; });
}

void PackJob::run() {
    try {
        std::error_code ec;
        fs::create_directories(options_.pack_dir / "by-hash", ec);
        if (ec) throw std::runtime_error("cannot make " + (options_.pack_dir / "by-hash").string() + ": " + ec.message());
        const std::set<std::string> contents = pack_contents(options_.pack_dir);
        const MaterialPlan* plan = options_.materials.get();
        if (plan && !plan->tiles.empty() && !options_.material_library)
            throw std::runtime_error("the photo materials are not loaded");
        material_record_ = read_material_record(options_.pack_dir);
        const bool had_record = !material_record_.empty();
        std::vector<Todo> todo;
        double texels = 0;
        std::size_t complete = 0;
        for (TextureInfo& t : options_.textures) {
            // A tile made by other rules than the plan's (or made from materials and no longer named) is made whole.
            const MaterialTile* tile = plan ? plan->find(t.hash) : nullptr;
            const auto recorded = material_record_.find(t.hash);
            const bool redo = tile ? recorded == material_record_.end() || recorded->second != tile->signature
                                   : recorded != material_record_.end();
            auto files = redo ? pack_files(t, options_.settings.maps) : missing_files(t, options_.settings.maps, contents);
            if (files.empty()) {
                ++complete;
                continue;
            }
            texels += static_cast<double>(t.width) * t.height * static_cast<double>(files.size());
            todo.push_back({std::move(t), std::move(files)});
        }
        options_.textures.clear();
        // In the archives' order, so each is opened once.
        std::sort(todo.begin(), todo.end(), [](const Todo& a, const Todo& b) {
            return a.info.source_archive != b.info.source_archive ? a.info.source_archive < b.info.source_archive
                                                                  : a.info.hash < b.info.hash;
        });
        {
            std::lock_guard lock(mutex_);
            status_.total = todo.size();
            status_.complete = complete;
            status_.texels_total = texels;
        }
        init_encoders();
        unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        unsigned n = options_.encoders ? options_.encoders : std::max(1u, hw > 2 ? hw - 2 : 1u);
        for (unsigned i = 0; i < n; ++i) encoders_.emplace_back([this, i, n] {
            platform::lower_current_thread_priority();
            encoder(i, n);
        });

        auto cache = make_archive_cache();
        for (Todo& t : todo) {
            wait_while_paused();
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return cancel_ || (in_gpu_ < kInGpu && made_.size() < kMadeQueue); });
                if (cancel_) break;
            }
            std::string why;
            auto sources = read_sources(t.info, cache.get(), &why);
            if (!sources) {
                std::lock_guard lock(mutex_);
                note_error(*t.info.names.begin() + ": " + why);
                ++status_.failures;
                ++status_.done;
                continue;
            }
            {
                std::lock_guard lock(mutex_);
                ++in_gpu_;
            }
            auto item = std::make_shared<Made>();
            item->info = std::move(t.info);
            item->files = std::move(t.files);
            item->planned = item->files.size();
            auto src = std::make_shared<Sources>(std::move(*sources));
            if (const MaterialTile* tile = plan ? plan->find(item->info.hash) : nullptr) {
                // (no GPU: the encoder composes it)
                item->material = tile;
                item->sources = std::move(src);
                {
                    std::lock_guard lock(mutex_);
                    --in_gpu_;
                    made_.push_back(item);
                }
                cv_.notify_all();
                continue;
            }
            gpu_.post([this, item, src](const Models& models) {
                if (!cancel_) {
                    {
                        std::lock_guard lock(mutex_);
                        status_.current = *item->info.names.begin();
                    }
                    const auto start = Clock::now();
                    try {
                        if (!models.color) throw std::runtime_error(gpu_.error());
                        item->outputs = upscale_texture(item->info, *src, models, options_.settings, item->files);
                    } catch (const std::exception& e) {
                        std::lock_guard lock(mutex_);
                        note_error(*item->info.names.begin() + ": " + e.what());
                        ++status_.failures;
                        item->files.clear(); // (counted done by the encoder, with nothing to write)
                    }
                    // Low impact: the GPU rests as long as it worked, so a game or a desktop stays smooth beside it.
                    if (low_impact_) std::this_thread::sleep_for(Clock::now() - start);
                } else {
                    item->files.clear();
                }
                {
                    std::lock_guard lock(mutex_);
                    made_.push_back(item);
                    --in_gpu_;
                }
                cv_.notify_all();
            });
        }
        {
            std::unique_lock lock(mutex_);
            reading_done_ = true;
            // (the GPU's tasks hold this job: wait for them all, cancelled or not)
            cv_.wait(lock, [&] { return in_gpu_ == 0; });
        }
        cv_.notify_all();
        for (auto& e : encoders_) e.join();
        encoders_.clear();

        const bool cancelled = cancel_;
        if (had_record || !material_record_.empty()) write_material_record(options_.pack_dir, material_record_);
        PackInfo info = options_.info;
        info.scale = options_.settings.scale;
        info.maps = options_.settings.maps;
        info.built = now_text();
        if (info.tool_version.empty()) info.tool_version = release_version(); // (the caller's own, when it gave one)
        std::uint64_t files = 0, bytes = 0, textures = 0;
        for (fs::directory_iterator it(options_.pack_dir / "by-hash", ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() != ".dds") continue;
            ++files;
            bytes += it->file_size(ec);
            if (it->path().stem().extension().empty()) ++textures; // <hash>.dds, not <hash>.n.dds
        }
        info.files = files;
        info.bytes = bytes;
        info.textures = textures;
        write_pack_info(options_.pack_dir, info);
        std::lock_guard lock(mutex_);
        if (status_.state == JobStatus::State::Running)
            status_.seconds += std::chrono::duration<double>(Clock::now() - resumed_at_).count();
        status_.current.clear();
        status_.state = cancelled ? JobStatus::State::Cancelled : JobStatus::State::Done;
    } catch (const std::exception& e) {
        {
            std::unique_lock lock(mutex_);
            cancel_ = true;
            reading_done_ = true;
            cv_.wait(lock, [&] { return in_gpu_ == 0; });
        }
        cv_.notify_all();
        for (auto& t : encoders_) t.join();
        encoders_.clear();
        std::lock_guard lock(mutex_);
        status_.fatal = e.what();
        status_.state = JobStatus::State::Failed;
    }
}

void PackJob::encoder(unsigned index, unsigned count) {
    for (;;) {
        std::shared_ptr<Made> item;
        {
            std::unique_lock lock(mutex_);
            // Low impact: a quarter of the encoders work, the rest wait.
            while (low_impact_ && index >= std::max(1u, count / 4) && !cancel_ && !(reading_done_ && in_gpu_ == 0))
                cv_.wait_for(lock, std::chrono::milliseconds(200));
            cv_.wait(lock, [&] { return cancel_ || !made_.empty() || (reading_done_ && in_gpu_ == 0); });
            if (made_.empty() || cancel_) {
                if (cancel_) made_.clear();
                return;
            }
            item = std::move(made_.front());
            made_.pop_front();
        }
        cv_.notify_all(); // (the reader waits for room)
        const fs::path dir = options_.pack_dir / "by-hash";
        const std::string base = hash_name(item->info.hash);
        std::uint64_t files = 0, bytes = 0;
        std::string error;
        if (item->material && !item->files.empty() && !cancel_) {
            try {
                item->outputs = compose_tile(*item->material, options_.materials->rules, item->sources->color,
                                             *options_.material_library, options_.materials->size,
                                             options_.settings.maps);
            } catch (const std::exception& e) {
                error = e.what();
            }
            item->sources.reset();
        }
        for (FileKind k : item->files) {
            if (!error.empty()) break;
            if (cancel_) break;
            auto& img = item->outputs.of(k);
            if (!img) continue;
            try {
                auto data = encode_pack_file(std::move(*img), k, item->info.bmp);
                img.reset();
                if (!write_bytes(dir / (base + std::string(suffix(k)) + ".dds"), data)) {
                    error = "cannot write to " + dir.string();
                    break;
                }
                ++files;
                bytes += data.size();
            } catch (const std::exception& e) {
                error = e.what();
                break;
            }
        }
        std::lock_guard lock(mutex_);
        if (!error.empty()) {
            note_error(*item->info.names.begin() + ": " + error);
            ++status_.failures;
        } else if (!cancel_ && !item->files.empty()) {
            if (item->material) material_record_[item->info.hash] = item->material->signature;
            else material_record_.erase(item->info.hash);
        }
        ++status_.done;
        status_.files_written += files;
        status_.bytes_written += bytes;
        status_.texels_done += static_cast<double>(item->info.width) * item->info.height *
                               static_cast<double>(item->planned);
    }
}

} // namespace hailstone::texpack

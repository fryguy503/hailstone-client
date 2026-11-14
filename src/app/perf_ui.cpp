#include "app/perf_ui.h"

#include <imgui.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

#include "core/frame_sections.h"
#include "core/log.h"
#include "core/shared_file_cache.h"
#include "net/game_connection.h"
#include "platform/process.h"
#include "render/actor_renderer.h"
#include "render/vk_context.h"
#include "render/zone_renderer.h"

namespace hailstone::app {

namespace {

constexpr const char* kPhaseNames[] = {"Sleep (--max-fps)", "Update", "UI", "Prepare", "Wait (GPU / vsync)",
                                       "Record", "Present"};
static_assert(std::size(kPhaseNames) == static_cast<std::size_t>(FramePhase::Count));

std::string bytes_text(double b) {
    char buf[32];
    if (b >= 1024.0 * 1024.0 * 1024.0) std::snprintf(buf, sizeof buf, "%.2f GiB", b / (1024.0 * 1024.0 * 1024.0));
    else if (b >= 1024.0 * 1024.0) std::snprintf(buf, sizeof buf, "%.1f MiB", b / (1024.0 * 1024.0));
    else if (b >= 1024.0) std::snprintf(buf, sizeof buf, "%.1f KiB", b / 1024.0);
    else std::snprintf(buf, sizeof buf, "%.0f B", b);
    return buf;
}

void row(const char* label, const char* fmt, ...) IM_FMTARGS(2);
void row(const char* label, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

constexpr ImGuiTableFlags kTable = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

} // namespace

bool is_hitch(float frame_ms, float typical_ms) {
    // Twenty milliseconds at least: below that a slow frame is not seen at 60 Hz.
    return typical_ms > 0.0f && frame_ms >= std::max(20.0f, typical_ms * 2.5f);
}

std::string hitch_text(float frame_ms, float typical_ms, const PhaseTimes& phases,
                       std::vector<std::pair<const char*, float>> sections) {
    std::vector<std::pair<float, const char*>> parts;
    float marked = 0.0f;
    for (std::size_t p = 0; p < kFramePhases; ++p) {
        marked += phases[p];
        parts.emplace_back(phases[p], kPhaseNames[p]);
    }
    parts.emplace_back(frame_ms - marked, "other");
    std::stable_sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    char buf[64];
    std::snprintf(buf, sizeof buf, "hitch %.1f ms (typical %.1f):", static_cast<double>(frame_ms),
                  static_cast<double>(typical_ms));
    std::string text = buf;
    for (const auto& [ms, name] : parts) {
        if (ms < 1.0f) break;
        std::snprintf(buf, sizeof buf, " %s %.1f", name, static_cast<double>(ms));
        text += buf;
    }
    std::stable_sort(sections.begin(), sections.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    const char* sep = "; in it:";
    for (const auto& [name, ms] : sections) {
        if (ms < 1.0f) break;
        std::snprintf(buf, sizeof buf, "%s %s %.1f", sep, name, static_cast<double>(ms));
        text += buf;
        sep = ",";
    }
    return text;
}

void PerfUi::begin_frame() {
    auto now = Clock::now();
    if (frames_++ > 0) {
        float frame = std::chrono::duration<float, std::milli>(now - frame_start_).count();
        auto sections = core::frame_sections::take();
        if (is_hitch(frame, typical_ms_)) {
            if (log_hitches_) log::info("perf: {}", hitch_text(frame, typical_ms_, current_, std::move(sections)));
        } else {
            typical_ms_ = typical_ms_ > 0.0f ? typical_ms_ * 0.95f + frame * 0.05f : frame;
        }
        if (!paused_) {
            frame_ms_[head_] = frame;
            phase_ms_[head_] = current_;
            head_ = (head_ + 1) % kFrames;
            count_ = std::min(count_ + 1, kFrames);
        }
    }
    current_.fill(0.0f);
    frame_start_ = last_mark_ = now;
}

void PerfUi::mark(FramePhase phase) {
    auto now = Clock::now();
    current_[static_cast<std::size_t>(phase)] += std::chrono::duration<float, std::milli>(now - last_mark_).count();
    last_mark_ = now;
}

void PerfUi::session_section(const char* name, const std::optional<net::SessionStats>& stats, NetRate& rate) {
    if (!stats) {
        row(name, "not connected");
        return;
    }
    const net::SessionStats& s = *stats;
    // Rates over at least a second, so they don't flicker.
    auto now = Clock::now();
    float dt = std::chrono::duration<float>(now - rate.at).count();
    if (rate.at == Clock::time_point{} || s.datagrams_sent < rate.datagrams_sent) {
        rate = NetRate{now, s.datagrams_sent, s.datagrams_received, s.bytes_sent, s.bytes_received};
    } else if (dt >= 1.0f) {
        rate.sent_per_s = static_cast<float>(s.datagrams_sent - rate.datagrams_sent) / dt;
        rate.received_per_s = static_cast<float>(s.datagrams_received - rate.datagrams_received) / dt;
        rate.bytes_sent_per_s = static_cast<float>(s.bytes_sent - rate.bytes_sent) / dt;
        rate.bytes_received_per_s = static_cast<float>(s.bytes_received - rate.bytes_received) / dt;
        rate.at = now;
        rate.datagrams_sent = s.datagrams_sent;
        rate.datagrams_received = s.datagrams_received;
        rate.bytes_sent = s.bytes_sent;
        rate.bytes_received = s.bytes_received;
    }
    row(name, "rtt %d ms, %d queued", s.rtt_ms, s.queued_packets);
    row("  sent", "%.0f/s (%s/s), %llu total (%s), %llu acks", rate.sent_per_s, bytes_text(rate.bytes_sent_per_s).c_str(),
        static_cast<unsigned long long>(s.datagrams_sent), bytes_text(static_cast<double>(s.bytes_sent)).c_str(),
        static_cast<unsigned long long>(s.acks_sent));
    row("  received", "%.0f/s (%s/s), %llu total (%s)", rate.received_per_s,
        bytes_text(rate.bytes_received_per_s).c_str(), static_cast<unsigned long long>(s.datagrams_received),
        bytes_text(static_cast<double>(s.bytes_received)).c_str());
    row("  resends", "%lld timeout, %lld fast", static_cast<long long>(s.timeout_resends),
        static_cast<long long>(s.fast_resends));
    row("  dropped", "%llu duplicate, %llu early, %llu bad", static_cast<unsigned long long>(s.duplicates),
        static_cast<unsigned long long>(s.early), static_cast<unsigned long long>(s.bad));
    row("  longest stall", "%lld ms", static_cast<long long>(s.longest_stall_ms));
}

void PerfUi::draw(const PerfInputs& in) {
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P))
        toggle();
    if (!open_) return;

    ImGui::SetNextWindowSize(ImVec2(460, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(480, 80), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Performance", &open_)) {
        ImGui::End();
        return;
    }

    // The history in time order, oldest first.
    std::vector<float> ms(count_);
    for (std::size_t i = 0; i < count_; ++i) ms[i] = frame_ms_[(head_ + kFrames - count_ + i) % kFrames];

    if (!ms.empty()) {
        float sum = std::accumulate(ms.begin(), ms.end(), 0.0f);
        float avg = sum / static_cast<float>(ms.size());
        auto [lo, hi] = std::minmax_element(ms.begin(), ms.end());
        std::vector<float> sorted = ms;
        std::sort(sorted.begin(), sorted.end());
        float p99 = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
        // 1% low: the frame rate of the slowest 1% of frames.
        std::size_t worst = std::max<std::size_t>(1, sorted.size() / 100);
        float worst_avg = std::accumulate(sorted.end() - static_cast<std::ptrdiff_t>(worst), sorted.end(), 0.0f) /
                          static_cast<float>(worst);
        float last = ms.back();

        ImGui::Text("%.1f FPS  (%.2f ms avg)", avg > 0.0f ? 1000.0f / avg : 0.0f, avg);
        ImGui::TextDisabled("last %zu frames: min %.2f, max %.2f, p99 %.2f ms; 1%% low %.1f FPS", ms.size(), *lo, *hi,
                            p99, worst_avg > 0.0f ? 1000.0f / worst_avg : 0.0f);

        float scale = std::max(33.4f, *hi * 1.1f);
        char overlay[48];
        std::snprintf(overlay, sizeof overlay, "%.2f ms", last);
        ImGui::PlotLines("##frametimes", ms.data(), static_cast<int>(ms.size()), 0, overlay, 0.0f, scale,
                         ImVec2(-1.0f, 90.0f));
        ImGui::TextDisabled("graph 0..%.0f ms; 16.7 ms = 60 FPS, 33.3 ms = 30 FPS", scale);
    } else {
        ImGui::TextUnformatted("collecting frames...");
    }
    ImGui::Checkbox("Pause", &paused_);
    ImGui::SameLine();
    ImGui::Checkbox("Log hitches", &log_hitches_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Write each frame of %.0f ms or more (2.5 times the typical frame, 20 ms at least)\n"
                          "to the log with where its time went.",
                          static_cast<double>(std::max(20.0f, typical_ms_ * 2.5f)));

    if (ImGui::CollapsingHeader("Frame phases (avg of the last 60 frames, and the slowest)", ImGuiTreeNodeFlags_DefaultOpen) &&
        count_ > 0) {
        const std::size_t n = std::min<std::size_t>(count_, 60);
        PhaseTimes avg{};
        float total = 0.0f;
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t slot = (head_ + kFrames - 1 - i) % kFrames;
            for (std::size_t p = 0; p < avg.size(); ++p) avg[p] += phase_ms_[slot][p] / static_cast<float>(n);
            total += frame_ms_[slot] / static_cast<float>(n);
        }
        float marked = std::accumulate(avg.begin(), avg.end(), 0.0f);
        // The slowest frame in the history: an average hides a hitch.
        std::size_t slowest = 0;
        for (std::size_t i = 1; i < count_; ++i)
            if (frame_ms_[i] > frame_ms_[slowest]) slowest = i;
        const PhaseTimes& worst = phase_ms_[slowest];
        float worst_marked = std::accumulate(worst.begin(), worst.end(), 0.0f);
        if (ImGui::BeginTable("phases", 4, kTable)) {
            ImGui::TableSetupColumn("Phase");
            ImGui::TableSetupColumn("avg ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("slowest", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("share");
            ImGui::TableHeadersRow();
            auto phase_row = [&](const char* name, float v, float w) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(name);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.2f", static_cast<double>(v));
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2f", static_cast<double>(w));
                ImGui::TableSetColumnIndex(3);
                ImGui::ProgressBar(total > 0.0f ? v / total : 0.0f, ImVec2(-1.0f, 0.0f), "");
            };
            for (std::size_t p = 0; p < avg.size(); ++p) phase_row(kPhaseNames[p], avg[p], worst[p]);
            phase_row("Events and other", std::max(0.0f, total - marked),
                      std::max(0.0f, frame_ms_[slowest] - worst_marked));
            ImGui::EndTable();
        }
        ImGui::TextDisabled("A large Wait means the GPU (or vsync) sets the pace; large Update/UI/Record, the CPU.");
        ImGui::TextDisabled("slowest: the %.1f ms frame of the last %zu.", static_cast<double>(frame_ms_[slowest]),
                            count_);
    }

    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen) && ImGui::BeginTable("render", 2, kTable)) {
        row("Display", "%.0f x %.0f", in.display.x, in.display.y);
        if (in.zone)
            row("Zone", "%zu triangles, %zu draws, %zu textures (%zu missing)", in.zone->triangles, in.zone->draws,
                in.zone->textures, in.zone->missing_textures);
        if (in.actors) {
            row("Actors", "%zu instances (%zu not posed), %zu draws, %.2f ms CPU", in.actors->instances,
                in.actors->unposed, in.actors->draws, in.actors->cpu_ms);
            row("  models", "%zu uploaded, %zu textures (%zu missing)", in.actors->models, in.actors->textures,
                in.actors->missing_textures);
            row("  dressed", "%zu this frame (%.2f ms, upload %.2f ms), %zu waiting", in.actors->dressed,
                in.actors->dress_ms, in.actors->upload_ms, in.actors->undressed);
        }
        row("Spawns", "%zu", in.spawns);
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Memory", ImGuiTreeNodeFlags_DefaultOpen) && ImGui::BeginTable("memory", 2, kTable)) {
        if (auto rss = platform::resident_memory())
            row("Process (resident)", "%s", bytes_text(static_cast<double>(*rss)).c_str());
        if (auto commit = platform::private_commit_memory())
            row("Process (private commit)", "%s", bytes_text(static_cast<double>(*commit)).c_str());
        if (in.shared_files) {
            const auto cache = in.shared_files->stats();
            row("Shared asset backing", "%s", bytes_text(static_cast<double>(cache.mapped_bytes)).c_str());
            row("Shared asset loads", "%llu hits, %llu built, %llu private fallbacks",
                static_cast<unsigned long long>(cache.hits), static_cast<unsigned long long>(cache.builds),
                static_cast<unsigned long long>(cache.fallbacks));
        }
        if (in.gpu) {
            row("GPU", "%s", in.gpu->gpu_name().c_str());
            const auto heaps = in.gpu->heap_use();
            for (std::size_t h = 0; h < heaps.size(); ++h) {
                const render::VkContext::HeapUse& b = heaps[h];
                if (b.usage == 0 && b.allocated == 0) continue;
                char label[48];
                std::snprintf(label, sizeof label, "Heap %zu (%s)", h, b.device_local ? "VRAM" : "system");
                row(label, "%s of %s budget; %s in %u allocations", bytes_text(static_cast<double>(b.usage)).c_str(),
                    bytes_text(static_cast<double>(b.budget)).c_str(),
                    bytes_text(static_cast<double>(b.allocated)).c_str(), b.allocations);
            }
        }
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Network", ImGuiTreeNodeFlags_DefaultOpen) && ImGui::BeginTable("net", 2, kTable)) {
        if (in.conn) {
            session_section("World", in.conn->world_session_stats(), world_rate_);
            session_section("Zone", in.conn->zone_session_stats(), zone_rate_);
        } else {
            row("Sessions", "not connected");
        }
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Session") && ImGui::BeginTable("session", 2, kTable)) {
        if (!in.zone_name.empty()) row("Zone", "%s", in.zone_name.c_str());
        row("Camera", "%.1f, %.1f, %.1f", in.camera.x, in.camera.y, in.camera.z);
        row("Frames", "%llu", static_cast<unsigned long long>(frames_));
        row("Uptime", "%.0f s", std::chrono::duration<float>(Clock::now() - started_).count());
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace hailstone::app

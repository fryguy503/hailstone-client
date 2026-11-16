#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hailstone::game {

struct CanvasBounds {
    float x = 0, y = 0, w = 0, h = 0;
};
struct CanvasTextMetrics {
    float width = 0, height = 0;
    float ink_x0 = 0, ink_y0 = 0, ink_x1 = 0, ink_y1 = 0;
};
struct CanvasImage {
    std::uintptr_t texture = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};
enum class CanvasCommandKind {
    Rect,
    RectFilled,
    Gradient,
    Line,
    Circle,
    CircleFilled,
    Triangle,
    TriangleFilled,
    Text,
    Image
};
struct CanvasCommand {
    CanvasCommandKind kind = CanvasCommandKind::RectFilled;
    float x = 0, y = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float radius = 0, rounding = 0, thickness = 1, size = 16;
    int font = 0, segments = 32;
    std::uint32_t color = 0xffffff, color2 = 0xffffff;
    float alpha = 1, alpha2 = 1;
    std::uintptr_t texture = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    std::string text;
};
struct CanvasSpawn {
    std::uint64_t incarnation = 0;
    std::uint32_t id = 0, level = 0, class_id = 0, race = 0, owner = 0, guild = 0, rider = 0, body_type = 0;
    std::string name;
    float x = 0, y = 0, z = 0, heading = 0;
    bool npc = false, corpse = false, mercenary = false, gm = false;
};
struct CanvasCast {
    std::uint32_t spell_id = 0;
    std::string name;
    double elapsed_ms = 0, duration_ms = 0;
};
struct CanvasContext {
    std::string name;
    bool target = false, self = false, in_group = false, in_raid = false;
    double distance = 0;
    std::optional<double> hp_percent, mana_percent, endurance_percent, aggro_percent;
    std::uint32_t group_mark = 0, raid_mark = 0;
    std::optional<CanvasCast> casting;
};
struct NameplateCanvasCandidate {
    // Assigned afresh by frame(); never an action handle or a reusable spawn identifier.
    std::string token;
    float x = 0, y = 0, depth = 0, native_scale = 1;
    CanvasSpawn spawn;
    CanvasContext context;
};
struct NameplateCanvasFrame {
    std::uint64_t serial = 0;
    double now_ms = 0, dt_ms = 0;
    float font_size = 16;
    int font = 0;
    CanvasBounds viewport;
    std::vector<NameplateCanvasCandidate> candidates;
};
struct NameplateCanvasModel {
    std::string token;
    CanvasBounds bounds;
    std::vector<CanvasCommand> commands;
    float offset_x = 0, offset_y = 0;
    bool through_walls = false, clickable = true, hide = false;
};
using NameplateCanvasOwner = std::uint64_t;
struct NameplateCanvasSubmission {
    NameplateCanvasOwner owner = 0;
    std::uint64_t frame_serial = 0;
    std::size_t candidate_index = 0;
    NameplateCanvasModel model;
};

inline constexpr std::size_t kCanvasMaxCandidates = 1024, kCanvasMaxOwners = 32, kCanvasMaxCommands = 512,
                             kCanvasMaxFrameCommands = 32768, kCanvasMaxTextBytes = 1024;
inline constexpr std::size_t kCanvasMaxModelVertices = 65536, kCanvasMaxFrameVertices = 262144;
std::size_t canvas_command_vertices(const CanvasCommand &command);
std::size_t canvas_model_vertices(const NameplateCanvasModel &model);
bool valid_canvas_command(const CanvasCommand &command);
bool valid_canvas_model(const NameplateCanvasModel &model);

// Owner ids are never reused. Invalidated owners cannot retain actionable submissions or callbacks.
class NameplateCanvasRegistry {
  public:
    using Callback = std::function<std::optional<std::vector<NameplateCanvasModel>>(const NameplateCanvasFrame &)>;
    using Measure = std::function<std::optional<CanvasTextMetrics>(std::string_view, float, int)>;
    using Icon = std::function<std::optional<CanvasImage>(std::uint32_t)>;
    NameplateCanvasRegistry();
    ~NameplateCanvasRegistry();
    NameplateCanvasRegistry(const NameplateCanvasRegistry &) = delete;
    NameplateCanvasRegistry &operator=(const NameplateCanvasRegistry &) = delete;
    NameplateCanvasOwner add(std::string tag, Callback callback, std::function<bool()> valid = {});
    void remove(NameplateCanvasOwner owner);
    void clear();
    bool owner_valid(NameplateCanvasOwner owner) const;
    bool empty() const;
    std::vector<NameplateCanvasSubmission> frame(NameplateCanvasFrame &frame);
    void set_measure(Measure measure);
    std::optional<CanvasTextMetrics> measure(std::string_view text, float size, int font) const;
    void set_icon(Icon icon);
    std::optional<CanvasImage> icon(std::uint32_t id) const;
    std::weak_ptr<void> lifetime() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hailstone::game

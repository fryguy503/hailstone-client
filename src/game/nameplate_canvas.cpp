#include "game/nameplate_canvas.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace hailstone::game {
namespace {
bool number(double n, double lo, double hi) { return std::isfinite(n) && n >= lo && n <= hi; }
bool text(std::string_view s, std::size_t limit) {
    return s.size() <= limit && std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool bounds(const CanvasBounds &b) {
    return number(b.x, -8192, 8192) && number(b.y, -8192, 8192) && number(b.w, 0.01, 4096) && number(b.h, 0.01, 4096);
}
bool valid_frame(const NameplateCanvasFrame &f) {
    return f.candidates.size() <= kCanvasMaxCandidates && number(f.viewport.x, -65536, 65536) &&
           number(f.viewport.y, -65536, 65536) && number(f.viewport.w, 1, 65536) && number(f.viewport.h, 1, 65536) &&
           number(f.now_ms, 0, 9e15) && number(f.dt_ms, 0, 60000) && number(f.font_size, 4, 256) && f.font >= 0 &&
           f.font <= 6 && std::all_of(f.candidates.begin(), f.candidates.end(), [](const auto &c) {
               return c.spawn.id != 0 && number(c.x, -65536, 65536) && number(c.y, -65536, 65536) &&
                      number(c.depth, 0.001, 1e9) && number(c.native_scale, 0.01, 16);
           });
}
} // namespace

bool valid_canvas_command(const CanvasCommand &c) {
    const float coordinates[] = {c.x, c.y, c.x0, c.y0, c.x1, c.y1, c.x2, c.y2};
    if (!std::all_of(std::begin(coordinates), std::end(coordinates), [](float n) { return number(n, -8192, 8192); }) ||
        !number(c.alpha, 0, 1) || !number(c.alpha2, 0, 1) || c.color > 0xffffff || c.color2 > 0xffffff ||
        !number(c.radius, 0, 4096) || !number(c.rounding, 0, 2048) || !number(c.thickness, 0.01, 64) ||
        !number(c.size, 4, 256) || c.font < 0 || c.font > 6 || c.segments < 3 || c.segments > 128 ||
        !text(c.text, kCanvasMaxTextBytes))
        return false;
    switch (c.kind) {
    case CanvasCommandKind::Rect:
    case CanvasCommandKind::RectFilled:
    case CanvasCommandKind::Gradient:
        return c.x1 >= c.x0 && c.y1 >= c.y0;
    case CanvasCommandKind::Line:
    case CanvasCommandKind::Circle:
    case CanvasCommandKind::CircleFilled:
    case CanvasCommandKind::Triangle:
    case CanvasCommandKind::TriangleFilled:
    case CanvasCommandKind::Text:
        return true;
    case CanvasCommandKind::Image:
        return c.texture != 0 && c.x1 >= c.x0 && c.y1 >= c.y0 && number(c.u0, 0, 1) && number(c.v0, 0, 1) &&
               number(c.u1, 0, 1) && number(c.v1, 0, 1) && c.u1 >= c.u0 && c.v1 >= c.v0;
    }
    return false;
}
bool valid_canvas_model(const NameplateCanvasModel &m) {
    return text(m.token, 128) && !m.token.empty() && (m.hide || bounds(m.bounds)) && number(m.offset_x, -4096, 4096) &&
           number(m.offset_y, -4096, 4096) && m.commands.size() <= kCanvasMaxCommands &&
           (m.hide || !m.commands.empty()) && std::all_of(m.commands.begin(), m.commands.end(), valid_canvas_command);
}
std::size_t canvas_command_vertices(const CanvasCommand &c) {
    switch (c.kind) {
    case CanvasCommandKind::Rect:
        return c.rounding > 0 ? 512 : 24;
    case CanvasCommandKind::RectFilled:
    case CanvasCommandKind::Gradient:
        return c.rounding > 0 ? 256 : 4;
    case CanvasCommandKind::Line:
        return 12;
    case CanvasCommandKind::Circle:
    case CanvasCommandKind::CircleFilled:
        return 4 * (static_cast<std::size_t>(std::clamp(c.segments, 3, 128)) + 1);
    case CanvasCommandKind::Triangle:
        return 36;
    case CanvasCommandKind::TriangleFilled:
        return 12;
    case CanvasCommandKind::Text:
        return 4 * (c.text.size() + 1);
    case CanvasCommandKind::Image:
        return 4;
    }
    return kCanvasMaxFrameVertices + 1;
}
std::size_t canvas_model_vertices(const NameplateCanvasModel &m) {
    std::size_t count = 0;
    for (const auto &c : m.commands) {
        const auto cost = canvas_command_vertices(c);
        if (cost > kCanvasMaxFrameVertices - count)
            return kCanvasMaxFrameVertices + 1;
        count += cost;
    }
    return count;
}

struct NameplateCanvasRegistry::Impl {
    struct Entry {
        std::string tag;
        Callback callback;
        std::function<bool()> valid;
    };
    std::map<NameplateCanvasOwner, std::shared_ptr<Entry>> entries;
    NameplateCanvasOwner next = 1;
    std::uint64_t serial = 0;
    bool in_frame = false;
    Measure measure;
    Icon icon;
    std::shared_ptr<void> lifetime = std::make_shared<int>(0);
};
NameplateCanvasRegistry::NameplateCanvasRegistry() : impl_(std::make_unique<Impl>()) {}
NameplateCanvasRegistry::~NameplateCanvasRegistry() = default;
NameplateCanvasOwner NameplateCanvasRegistry::add(std::string tag, Callback callback, std::function<bool()> valid) {
    if (!text(tag, 256) || tag.empty() || !callback || impl_->next == 0)
        return 0;
    // Re-registering an owner's named service retires its old callback and cached hit identity.
    for (auto it = impl_->entries.begin(); it != impl_->entries.end();) {
        if (it->second->tag == tag)
            it = impl_->entries.erase(it);
        else
            ++it;
    }
    if (impl_->entries.size() >= kCanvasMaxOwners)
        return 0;
    const auto owner = impl_->next++;
    impl_->entries.emplace(
        owner, std::make_shared<Impl::Entry>(Impl::Entry{std::move(tag), std::move(callback), std::move(valid)}));
    return owner;
}
void NameplateCanvasRegistry::remove(NameplateCanvasOwner owner) { impl_->entries.erase(owner); }
void NameplateCanvasRegistry::clear() { impl_->entries.clear(); }
bool NameplateCanvasRegistry::empty() const {
    std::vector<NameplateCanvasOwner> owners;
    for (const auto &[id, entry] : impl_->entries) {
        (void)entry;
        owners.push_back(id);
    }
    return std::none_of(owners.begin(), owners.end(), [&](const auto id) { return owner_valid(id); });
}
bool NameplateCanvasRegistry::owner_valid(NameplateCanvasOwner owner) const {
    const auto found = impl_->entries.find(owner);
    if (found == impl_->entries.end())
        return false;
    const auto entry = found->second;
    try {
        const bool valid = !entry->valid || entry->valid();
        const auto current = impl_->entries.find(owner);
        return valid && current != impl_->entries.end() && current->second == entry;
    } catch (...) {
        return false;
    }
}
std::vector<NameplateCanvasSubmission> NameplateCanvasRegistry::frame(NameplateCanvasFrame &frame) {
    std::vector<NameplateCanvasSubmission> out;
    if (impl_->in_frame || !valid_frame(frame) || impl_->serial == UINT64_MAX)
        return out;
    struct Guard {
        bool &value;
        ~Guard() { value = false; }
    } guard{impl_->in_frame};
    impl_->in_frame = true;
    frame.serial = ++impl_->serial;
    std::map<std::string, std::size_t> tokens;
    for (std::size_t i = 0; i < frame.candidates.size(); ++i) {
        auto &token = frame.candidates[i].token;
        token = "frame:" + std::to_string(frame.serial) + ":" + std::to_string(i);
        tokens.emplace(token, i);
    }
    std::vector<std::pair<NameplateCanvasOwner, std::shared_ptr<Impl::Entry>>> owners(impl_->entries.begin(),
                                                                                      impl_->entries.end());
    std::set<std::size_t> claimed;
    std::size_t command_count = 0, vertex_count = 0;
    const auto target =
        std::find_if(frame.candidates.begin(), frame.candidates.end(), [](const auto &c) { return c.context.target; });
    const std::optional<std::size_t> target_index =
        target == frame.candidates.end()
            ? std::nullopt
            : std::optional<std::size_t>(static_cast<std::size_t>(target - frame.candidates.begin()));
    for (const auto &[owner, entry] : owners) {
        if (!owner_valid(owner))
            continue;
        std::optional<std::vector<NameplateCanvasModel>> result;
        try {
            result = entry->callback(frame);
        } catch (...) {
            remove(owner);
            continue;
        }
        if (!owner_valid(owner) || !result || result->size() > kCanvasMaxCandidates)
            continue;
        std::set<std::size_t> own_claims;
        bool valid = true;
        for (const auto &model : *result) {
            const auto candidate = tokens.find(model.token);
            if (!valid_canvas_model(model) || candidate == tokens.end() ||
                !own_claims.insert(candidate->second).second) {
                valid = false;
                break;
            }
        }
        if (!valid)
            continue;
        std::stable_sort(result->begin(), result->end(), [&](const auto &a, const auto &b) {
            return frame.candidates[tokens.at(a.token)].context.target >
                   frame.candidates[tokens.at(b.token)].context.target;
        });
        for (auto &model : *result) {
            const auto index = tokens.at(model.token);
            if (claimed.contains(index))
                continue;
            const auto vertices = canvas_model_vertices(model);
            const bool reserve_target = target_index && !claimed.contains(*target_index) && index != *target_index;
            const auto vertex_limit = kCanvasMaxFrameVertices - (reserve_target ? kCanvasMaxModelVertices : 0);
            const auto command_limit = kCanvasMaxFrameCommands - (reserve_target ? kCanvasMaxCommands : 0);
            if (vertices > kCanvasMaxModelVertices || vertex_count > vertex_limit ||
                vertices > vertex_limit - vertex_count || command_count > command_limit ||
                model.commands.size() > command_limit - command_count)
                continue;
            if (claimed.insert(index).second) {
                command_count += model.commands.size();
                vertex_count += vertices;
                out.push_back({owner, frame.serial, index, std::move(model)});
            }
        }
    }
    // A later callback may disable an earlier owner. Never commit that owner's prepared claims.
    std::erase_if(out, [&](const auto &s) { return !owner_valid(s.owner); });
    return out;
}
void NameplateCanvasRegistry::set_measure(Measure measure) { impl_->measure = std::move(measure); }
std::optional<CanvasTextMetrics> NameplateCanvasRegistry::measure(std::string_view value, float size, int font) const {
    if (!impl_->measure || !text(value, kCanvasMaxTextBytes) || !number(size, 4, 256) || font < 0 || font > 6)
        return std::nullopt;
    try {
        auto m = impl_->measure(value, size, font);
        if (!m)
            return std::nullopt;
        const float values[] = {m->width, m->height, m->ink_x0, m->ink_y0, m->ink_x1, m->ink_y1};
        if (!std::all_of(std::begin(values), std::end(values), [](float n) { return number(n, -65536, 65536); }) ||
            m->width < 0 || m->height < 0)
            return std::nullopt;
        return m;
    } catch (...) {
        return std::nullopt;
    }
}
void NameplateCanvasRegistry::set_icon(Icon icon) { impl_->icon = std::move(icon); }
std::optional<CanvasImage> NameplateCanvasRegistry::icon(std::uint32_t id) const {
    if (!impl_->icon || id > 65535)
        return std::nullopt;
    try {
        auto image = impl_->icon(id);
        if (!image || !image->texture || !number(image->u0, 0, 1) || !number(image->v0, 0, 1) ||
            !number(image->u1, 0, 1) || !number(image->v1, 0, 1) || image->u1 < image->u0 || image->v1 < image->v0)
            return std::nullopt;
        return image;
    } catch (...) {
        return std::nullopt;
    }
}
std::weak_ptr<void> NameplateCanvasRegistry::lifetime() const { return impl_->lifetime; }
} // namespace hailstone::game

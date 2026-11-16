#include "ui/ui_textures.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "assets/jpeg_read.h"
#include "assets/texture.h"
#include "core/game_dir.h"
#include "core/log.h"
#include "render/gpu.h"
#include "render/vk_context.h"
#include "render/vk_util.h"
#include "ui/imgui_vk_textures.h"
#include "ui/item_windows.h"
#include "ui/spell_windows.h"

namespace hailstone::ui {

std::optional<std::string> ui_image_path(std::string_view relative) {
    if (relative.empty() || relative.size() > 4096 || relative.find_first_of(":*?\"<>") != std::string_view::npos ||
        std::any_of(relative.begin(), relative.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        return std::nullopt;
    const std::string path = GameDir::normalize(relative);
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        const auto part = std::string_view(path).substr(begin, end == std::string::npos ? path.size() - begin : end - begin);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            return std::nullopt;
        if (end == std::string::npos) break;
        begin = end + 1;
        if (begin == path.size()) return std::nullopt;
    }
    const auto dot = path.rfind('.');
    if (dot == std::string::npos) return std::nullopt;
    const auto extension = std::string_view(path).substr(dot);
    if (extension != ".dds" && extension != ".tga" && extension != ".png" && extension != ".bmp" &&
        extension != ".jpg" && extension != ".jpeg")
        return std::nullopt;
    return path;
}

std::optional<GridCell> gem_icon_cell(std::int32_t icon) {
    // A_SpellGems: gemicons01..03.tga, 256 x 256, 24 x 24 cells: 10 per row, 10 rows (100 per sheet).
    if (icon < 0 || icon >= 300) return std::nullopt;
    GridCell c;
    c.sheet = icon / 100 + 1;
    int cell = icon % 100;
    c.x = (cell % 10) * 24;
    c.y = (cell / 10) * 24;
    return c;
}

std::optional<GridCell> grid_index_cell(int index, int frames, int frame_w, int frame_h, int cell_w, int cell_h,
                                        bool vertical) {
    if (cell_w <= 0 || cell_h <= 0 || index < 0) return std::nullopt;
    // In 64 bits: the sizes come from the XML (a server's assets may replace it), and 100000 x 100000 one-unit
    // cells overflowed int.
    const int cols = frame_w / cell_w, rows = frame_h / cell_h;
    const std::int64_t per64 = std::int64_t{cols} * rows;
    if (cols <= 0 || rows <= 0 || per64 > std::numeric_limits<int>::max() || index >= per64 * std::max(frames, 0))
        return std::nullopt;
    const int per = static_cast<int>(per64);
    GridCell c;
    c.sheet = index / per + 1;
    const int cell = index % per;
    const int col = vertical ? cell / rows : cell % cols, row = vertical ? cell % rows : cell / cols;
    c.x = col * cell_w;
    c.y = row * cell_h;
    return c;
}

namespace {

// The text between <tag> and </tag> after `from` (and before `limit`), trimmed; npos-safe.
std::optional<std::string> tag_text(const std::string& s, const char* tag, std::size_t from, std::size_t limit,
                                    std::size_t* end = nullptr) {
    std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    std::size_t a = s.find(open, from);
    if (a == std::string::npos || a >= limit) return std::nullopt;
    a += open.size();
    std::size_t b = s.find(close, a);
    if (b == std::string::npos || b > limit) return std::nullopt;
    if (end) *end = b + close.size();
    std::string v = s.substr(a, b - a);
    auto first = v.find_first_not_of(" \t\r\n"), last = v.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : v.substr(first, last - first + 1);
}

int to_int(const std::optional<std::string>& v) {
    if (!v || v->empty()) return 0;
    try {
        return std::stoi(*v);
    } catch (...) {
        return 0;
    }
}

bool valid_animation_reference(const UiTextures::AnimationFrame& frame) {
    const auto& name = frame.file;
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) return false;
    return frame.valid_metadata && frame.x >= 0 && frame.y >= 0 && frame.w > 0 && frame.h > 0;
}

bool frame_integer(const std::optional<std::string>& value) {
    if (!value || value->empty()) return false;
    const char* first = value->data();
    const char* last = first + value->size();
    if (*first == '+') {
        ++first;
        if (first != last && (*first == '-' || *first == '+')) return false;
    }
    if (first == last) return false;
    int number = 0;
    const auto parsed = std::from_chars(first, last, number);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

} // namespace

// Every <Ui2DAnimation item="..."> of one XML file: its first <Frames> block's texture, location and size.
template <typename Map> void scan_animations(const std::string& text, Map& out) {
    std::size_t at = 0;
    while ((at = text.find("<Ui2DAnimation", at)) != std::string::npos) {
        std::size_t end = text.find("</Ui2DAnimation>", at);
        if (end == std::string::npos) break;
        std::size_t q = text.find("item=\"", at);
        if (q != std::string::npos && q < end) {
            q += 6;
            std::size_t q2 = text.find('"', q);
            std::string name = text.substr(q, q2 - q);
            std::size_t frames = text.find("<Frames>", at);
            if (frames != std::string::npos && frames < end) {
                std::size_t fend = text.find("</Frames>", frames);
                if (fend == std::string::npos || fend > end) fend = end;
                auto tex = tag_text(text, "Texture", frames, fend);
                std::size_t loc_end = 0, size_end = 0;
                auto loc = tag_text(text, "Location", frames, fend, &loc_end);
                auto size = tag_text(text, "Size", frames, fend, &size_end);
                if (tex && loc && size) {
                    typename Map::mapped_type f;
                    f.file = *tex;
                    f.x = to_int(tag_text(*loc, "X", 0, loc->size()));
                    f.y = to_int(tag_text(*loc, "Y", 0, loc->size()));
                    f.w = to_int(tag_text(*size, "CX", 0, size->size()));
                    f.h = to_int(tag_text(*size, "CY", 0, size->size()));
                    f.valid_metadata = frame_integer(tag_text(*loc, "X", 0, loc->size())) &&
                        frame_integer(tag_text(*loc, "Y", 0, loc->size())) &&
                        frame_integer(tag_text(*size, "CX", 0, size->size())) &&
                        frame_integer(tag_text(*size, "CY", 0, size->size()));
                    out.try_emplace(name, std::move(f));
                }
            }
        }
        at = end;
    }
}

std::optional<UiTextures::AnimationFrame> find_ui_animation_frame(const std::string& xml, std::string_view name) {
    std::map<std::string, UiTextures::AnimationFrame> frames;
    scan_animations(xml, frames);
    const auto found = frames.find(std::string(name));
    if (found == frames.end() || !valid_animation_reference(found->second)) return std::nullopt;
    return found->second;
}

std::optional<UiTextures::AnimationImage> ui_animation_crop(const UiTextures::Texture& texture,
                                                          const UiTextures::AnimationFrame& frame) {
    if (!texture.id || texture.width <= 0 || texture.height <= 0 || !valid_animation_reference(frame)) return std::nullopt;
    const auto right = static_cast<std::int64_t>(frame.x) + frame.w;
    const auto bottom = static_cast<std::int64_t>(frame.y) + frame.h;
    if (right > texture.width || bottom > texture.height) return std::nullopt;
    UiTextures::AnimationImage image;
    image.width = frame.w; image.height = frame.h; image.cell.id = texture.id;
    image.cell.u0 = static_cast<float>(frame.x) / static_cast<float>(texture.width);
    image.cell.v0 = static_cast<float>(frame.y) / static_cast<float>(texture.height);
    image.cell.u1 = static_cast<float>(right) / static_cast<float>(texture.width);
    image.cell.v1 = static_cast<float>(bottom) / static_cast<float>(texture.height);
    return image;
}

// Every grid <Ui2DAnimation> (<Grid>true</Grid>) of one XML file: its cell size, direction and every frame.
template <typename Map> void scan_grids(const std::string& text, Map& out) {
    std::size_t at = 0;
    while ((at = text.find("<Ui2DAnimation", at)) != std::string::npos) {
        std::size_t end = text.find("</Ui2DAnimation>", at);
        if (end == std::string::npos) break;
        std::size_t q = text.find("item", at);
        q = q == std::string::npos ? q : text.find('"', q);
        auto grid = tag_text(text, "Grid", at, end);
        if (q != std::string::npos && q < end && grid && *grid == "true") {
            std::size_t q2 = text.find('"', q + 1);
            std::string name = text.substr(q + 1, q2 - q - 1);
            typename Map::mapped_type g;
            g.cell_w = to_int(tag_text(text, "CellWidth", at, end));
            g.cell_h = to_int(tag_text(text, "CellHeight", at, end));
            auto v = tag_text(text, "Vertical", at, end);
            g.vertical = v && *v == "true";
            std::size_t frames = at;
            while ((frames = text.find("<Frames>", frames)) != std::string::npos && frames < end) {
                std::size_t fend = text.find("</Frames>", frames);
                if (fend == std::string::npos || fend > end) fend = end;
                auto tex = tag_text(text, "Texture", frames, fend);
                auto loc = tag_text(text, "Location", frames, fend);
                auto size = tag_text(text, "Size", frames, fend);
                if (tex && loc && size) {
                    typename decltype(g.frames)::value_type f;
                    f.file = *tex;
                    f.x = to_int(tag_text(*loc, "X", 0, loc->size()));
                    f.y = to_int(tag_text(*loc, "Y", 0, loc->size()));
                    f.w = to_int(tag_text(*size, "CX", 0, size->size()));
                    f.h = to_int(tag_text(*size, "CY", 0, size->size()));
                    g.frames.push_back(std::move(f));
                }
                frames = fend;
            }
            if (!g.frames.empty()) out.try_emplace(name, std::move(g));
        }
        at = end;
    }
}

// Every <Screen item="..."> of one XML file: its own <Location> and <Size> (the ones before its <Pieces>).
template <typename Map> void scan_screens(const std::string& text, Map& out) {
    std::size_t at = 0;
    while ((at = text.find("<Screen item=\"", at)) != std::string::npos) {
        std::size_t q = at + 14;
        std::size_t q2 = text.find('"', q);
        std::size_t end = text.find("</Screen>", at);
        if (q2 == std::string::npos || end == std::string::npos) break;
        std::size_t limit = std::min(end, text.find("<Pieces>", at));
        std::size_t le = 0, se = 0;
        auto loc = tag_text(text, "Location", at, limit, &le);
        auto size = tag_text(text, "Size", at, limit, &se);
        if (size) {
            typename Map::mapped_type r;
            if (loc) {
                r.x = to_int(tag_text(*loc, "X", 0, loc->size()));
                r.y = to_int(tag_text(*loc, "Y", 0, loc->size()));
            }
            r.w = to_int(tag_text(*size, "CX", 0, size->size()));
            r.h = to_int(tag_text(*size, "CY", 0, size->size()));
            out.try_emplace(text.substr(q, q2 - q), r);
        }
        at = end;
    }
}

struct UiTextures::Entry {
    render::Texture texture;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Texture info;
};

UiTextures::UiTextures(render::VkContext& vk, const GameDir& game) : vk_(vk), game_(game) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f; // the base level: atlas cells must not bleed into each other
    VkSampler sampler = VK_NULL_HANDLE;
    render::vk_check(vkCreateSampler(vk_.device(), &si, nullptr, &sampler), "vkCreateSampler (UI art)");
    sampler_ = sampler;
}

UiTextures::~UiTextures() {
    vk_.wait_idle();
    auto destroy = [&](std::unique_ptr<Entry>& e) {
        if (!e) return;
        if (e->set) ui::remove_imgui_texture(e->set);
        e->texture.destroy(vk_.device(), vk_.allocator());
    };
    for (auto& [name, f] : files_) destroy(f.entry);
    for (auto& [name, e] : pictures_) destroy(e);
    if (sampler_) vkDestroySampler(vk_.device(), static_cast<VkSampler>(sampler_), nullptr);
}

bool is_icon_file(std::string_view name) {
    auto starts = [&](std::string_view p) {
        if (name.size() < p.size()) return false;
        for (std::size_t i = 0; i < p.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(name[i])) != p[i]) return false;
        return true;
    };
    return starts("dragitem") || starts("spells") || starts("gemicons") || starts("menuicons");
}

const UiTextures::Texture* UiTextures::file(std::string_view name) {
    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!all_art_ && !is_icon_file(key)) return nullptr; // the UI draws its own (set_all_art)
    return load(key, "uifiles/default/" + key);
}

const UiTextures::Texture* UiTextures::game_file(std::string_view relative) {
    std::string key(relative);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return load(":" + key, key);
}

const UiTextures::Texture* UiTextures::load(const std::string& key, const std::string& path) {
    ++lookups_;
    if (auto it = files_.find(key); it != files_.end()) {
        it->second.used = lookups_;
        return it->second.entry ? &it->second.entry->info : nullptr;
    }
    // Room for it: the file asked for least recently goes.
    while (files_.size() >= kMaxFiles) {
        auto oldest = least_recently_used(files_, [](const File& f) { return f.used; });
        log::debug("ui art: {} let go ({} files held)", oldest->first, files_.size());
        retire(std::move(oldest->second.entry));
        files_.erase(oldest);
    }
    std::unique_ptr<Entry> e;
    if (auto data = game_.read(path)) e = make(*data, path);
    else log::warn("ui art: {} is missing", path);
    File& slot = files_[key] = File{std::move(e), lookups_};
    return slot.entry ? &slot.entry->info : nullptr;
}

void UiTextures::retire(std::unique_ptr<Entry> e) {
    if (!e) return;
    if (e->set) texture_id_removed(reinterpret_cast<const void*>(e->set));
    // A draw list of the frame being built may hold it: it goes after that frame too.
    vk_.retire([set = e->set, texture = e->texture, device = vk_.device(), allocator = vk_.allocator()]() mutable {
        if (set) ui::remove_imgui_texture(set);
        texture.destroy(device, allocator);
    }, 1);
}

std::unique_ptr<UiTextures::Entry> UiTextures::make(std::span<const std::byte> data, const std::string& what) {
    std::string error;
    std::optional<assets::Image> image = assets::is_jpeg(data) ? assets::decode_jpeg(data, &error)
                                                               : assets::decode_texture(data, {}, &error);
    if (!image) {
        log::warn("ui art: {}: {}", what, error);
        return nullptr;
    }
    assets::Image rgba = assets::to_rgba8(*image);
    rgba.mips.resize(1);
    auto e = std::make_unique<Entry>();
    e->info.width = static_cast<int>(rgba.width);
    e->info.height = static_cast<int>(rgba.height);
    render::UploadBatch batch(vk_);
    e->texture = batch.texture(rgba);
    batch.submit();
    e->set = ui::add_imgui_texture(static_cast<VkSampler>(sampler_), e->texture.view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!e->set) {
        log::warn("ui art: no descriptor left for {}", what);
        retire(std::move(e)); // its upload may still be on the GPU
        return nullptr;
    }
    e->info.id = reinterpret_cast<void*>(e->set);
    return e;
}

const UiTextures::Texture* UiTextures::picture(const std::filesystem::path& file) {
    const std::string key = file.string();
    if (auto it = pictures_.find(key); it != pictures_.end()) return it->second ? &it->second->info : nullptr;
    // Big pictures: only the last few are kept. An old one goes once the frames that may draw it are done (retire:
    // the one being built included), without waiting for the GPU.
    constexpr std::size_t kKept = 3;
    while (picture_order_.size() >= kKept) {
        if (auto it = pictures_.find(picture_order_.front()); it != pictures_.end()) {
            retire(std::move(it->second));
            pictures_.erase(it);
        }
        picture_order_.erase(picture_order_.begin());
    }
    std::unique_ptr<Entry> e;
    std::ifstream in(file, std::ios::binary);
    if (in) {
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        e = make(std::as_bytes(std::span(bytes)), key);
    } else {
        log::warn("ui art: cannot read {}", key);
    }
    picture_order_.push_back(key);
    auto& slot = pictures_[key] = std::move(e);
    return slot ? &slot->info : nullptr;
}

std::optional<UiTextures::Cell> UiTextures::grid(const char* pattern, int sheet, int cell_size, int x, int y) {
    const Texture* t = file(std::vformat(pattern, std::make_format_args(sheet)));
    if (!t || t->width <= 0 || t->height <= 0) return std::nullopt;
    Cell c;
    c.id = t->id;
    c.u0 = static_cast<float>(x) / static_cast<float>(t->width);
    c.v0 = static_cast<float>(y) / static_cast<float>(t->height);
    c.u1 = static_cast<float>(x + cell_size) / static_cast<float>(t->width);
    c.v1 = static_cast<float>(y + cell_size) / static_cast<float>(t->height);
    return c;
}

std::optional<UiTextures::Cell> UiTextures::item_icon(std::uint32_t icon) {
    auto c = icon_cell(icon);
    return c ? grid("dragitem{}.dds", c->sheet, 40, c->x, c->y) : std::nullopt;
}

std::optional<UiTextures::Cell> UiTextures::spell_icon(std::int32_t icon) {
    auto c = spell_icon_cell(icon);
    return c ? grid("spells{:02}.tga", c->sheet, 40, c->x, c->y) : std::nullopt;
}

std::optional<UiTextures::Cell> UiTextures::gem_icon(std::int32_t icon) {
    auto c = gem_icon_cell(icon);
    return c ? grid("gemicons{:02}.tga", c->sheet, 24, c->x, c->y) : std::nullopt;
}

void UiTextures::load_skin() {
    {
        animations_loaded_ = true;
        // Every <Ui2DAnimation item="..."> of the default skin's XML files: its first <Frames> block's texture,
        // location and size. EQUI_Animations.xml holds most; window files add their own (A_PWCS* in
        // EQUI_PlayerWindow.xml). The folder is listed through the GameDir's layers, so a server's assets folder may
        // replace a file or add its own.
        for (const GameDir::Entry& e : game_.list("uifiles/default")) {
            std::string lname = e.name;
            std::transform(lname.begin(), lname.end(), lname.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lname.size() < 4 || lname.substr(lname.size() - 4) != ".xml" || lname.rfind("equi_", 0) != 0) continue;
            auto data = read_file(e.path);
            if (!data) continue;
            std::string text(reinterpret_cast<const char*>(data->data()), data->size());
            scan_animations(text, animations_);
            scan_grids(text, grids_);
            scan_screens(text, screens_);
        }
        log::debug("ui art: {} animations, {} screens", animations_.size(), screens_.size());
    }
}

std::optional<UiTextures::Rect> UiTextures::screen_rect(std::string_view screen) {
    if (!animations_loaded_) load_skin();
    auto it = screens_.find(std::string(screen));
    if (it == screens_.end()) return std::nullopt;
    return it->second;
}

std::optional<UiTextures::Cell> UiTextures::animation(std::string_view name) {
    if (!animations_loaded_) load_skin();
    auto it = animations_.find(std::string(name));
    if (it == animations_.end()) return std::nullopt;
    const Frame& f = it->second;
    const Texture* t = file(f.file);
    if (!t || t->width <= 0 || t->height <= 0) return std::nullopt;
    Cell c;
    c.id = t->id;
    c.u0 = static_cast<float>(f.x) / static_cast<float>(t->width);
    c.v0 = static_cast<float>(f.y) / static_cast<float>(t->height);
    c.u1 = static_cast<float>(f.x + f.w) / static_cast<float>(t->width);
    c.v1 = static_cast<float>(f.y + f.h) / static_cast<float>(t->height);
    return c;
}

std::optional<UiTextures::AnimationImage> UiTextures::animation_image(std::string_view name) {
    if (!animations_loaded_) load_skin();
    const auto found = animations_.find(std::string(name));
    if (found == animations_.end() || !valid_animation_reference(found->second)) return std::nullopt;
    const Texture* texture = game_file("uifiles/default/" + found->second.file);
    return texture ? ui_animation_crop(*texture, found->second) : std::nullopt;
}

std::optional<int> UiTextures::grid_cells(std::string_view name) {
    if (!animations_loaded_) load_skin();
    auto it = grids_.find(std::string(name));
    if (it == grids_.end()) return std::nullopt;
    const Grid& g = it->second;
    const Frame& f = g.frames.front();
    if (g.cell_w <= 0 || g.cell_h <= 0) return 0;
    const std::int64_t cells = std::int64_t{f.w / g.cell_w} * (f.h / g.cell_h) * static_cast<std::int64_t>(g.frames.size());
    return static_cast<int>(std::clamp<std::int64_t>(cells, 0, std::numeric_limits<int>::max()));
}

std::optional<UiTextures::Cell> UiTextures::grid_cell(std::string_view name, int index) {
    if (!animations_loaded_) load_skin();
    auto it = grids_.find(std::string(name));
    if (it == grids_.end()) return std::nullopt;
    const Grid& g = it->second;
    const Frame& first = g.frames.front();
    auto c = grid_index_cell(index, static_cast<int>(g.frames.size()), first.w, first.h, g.cell_w, g.cell_h, g.vertical);
    if (!c) return std::nullopt;
    const Frame& f = g.frames[static_cast<std::size_t>(c->sheet - 1)];
    const Texture* t = file(f.file);
    if (!t || t->width <= 0 || t->height <= 0) return std::nullopt;
    Cell out;
    out.id = t->id;
    // The sums in double: an XML offset near INT_MAX plus the cell's overflowed int.
    out.u0 = static_cast<float>((static_cast<double>(f.x) + c->x) / t->width);
    out.v0 = static_cast<float>((static_cast<double>(f.y) + c->y) / t->height);
    out.u1 = static_cast<float>((static_cast<double>(f.x) + c->x + g.cell_w) / t->width);
    out.v1 = static_cast<float>((static_cast<double>(f.y) + c->y + g.cell_h) / t->height);
    return out;
}

} // namespace hailstone::ui

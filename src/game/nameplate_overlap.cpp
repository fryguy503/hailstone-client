#include "game/nameplate_overlap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>

namespace hailstone::game {
namespace {

bool nearer(const PlateBox& a, const PlateBox& b) {
    if (a.front != b.front) return a.front;
    return a.depth < b.depth;
}

float overlap_area(const PlateBox& a, const PlateBox& b) {
    const float w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0);
    const float h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
    return std::max(w, 0.0f) * std::max(h, 0.0f);
}

} // namespace

std::vector<std::size_t> plate_draw_order(const std::vector<PlateBox>& boxes) {
    std::vector<std::size_t> order;
    plate_draw_order(boxes, order);
    return order;
}

void plate_draw_order(const std::vector<PlateBox>& boxes, std::vector<std::size_t>& order) {
    order.resize(boxes.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return nearer(boxes[b], boxes[a]); });
}

std::vector<float> plate_target_alpha(const std::vector<PlateBox>& boxes) {
    std::vector<float> alpha;
    plate_target_alpha(boxes, alpha);
    return alpha;
}

namespace {

float faded_alpha(float covered) {
    const float t = std::clamp((covered - kPlateFadeFrom) / (kPlateFadeTo - kPlateFadeFrom), 0.0f, 1.0f);
    return 1.0f - (1.0f - kPlateCoveredAlpha) * t * t * (3.0f - 2.0f * t);
}

bool has_area(const PlateBox& b) { return b.x1 - b.x0 > 0.0f && b.y1 - b.y0 > 0.0f; }

// Each pair compared: for a few names, and for boxes with coordinates that are not finite (which the grid cannot place).
void target_alpha_pairs(const std::vector<PlateBox>& boxes, std::vector<float>& alpha) {
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        const PlateBox& b = boxes[i];
        if (b.front) continue;
        const float own = std::max(b.x1 - b.x0, 0.0f) * std::max(b.y1 - b.y0, 0.0f);
        if (own <= 0.0f) continue;
        float covered = 0.0f;
        for (std::size_t j = 0; j < boxes.size(); ++j) {
            if (i != j && nearer(boxes[j], b)) {
                covered = std::max(covered, overlap_area(b, boxes[j]) / own);
                if (covered >= kPlateFadeTo) break; // more overlap cannot fade this name any further
            }
        }
        alpha[i] = faded_alpha(covered);
    }
}

constexpr std::size_t kGridFrom = 32; // fewer names: every pair
constexpr int kMaxGrid = 64;           // cells a side at most

} // namespace

void plate_target_alpha(const std::vector<PlateBox>& boxes, std::vector<float>& alpha) {
    alpha.assign(boxes.size(), 1.0f);
    const std::size_t n = boxes.size();
    bool finite = true;
    for (const PlateBox& b : boxes)
        finite = finite && std::isfinite(b.x0) && std::isfinite(b.x1) && std::isfinite(b.y0) && std::isfinite(b.y1);
    if (n < kGridFrom || !finite) return target_alpha_pairs(boxes, alpha);

    // A coarse grid over the boxes with an area: only a name sharing a cell can overlap one (a point inside both
    // boxes lies in a cell both span, the cell mapping being monotonic), so the largest cover is the one the pairs
    // find. Boxes without area cover nothing and are left out. The largest cover does not depend on the order the
    // nearer names are seen in, so the result is the pairs' exactly.
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    bool any = false;
    for (const PlateBox& b : boxes) {
        if (!has_area(b)) continue;
        if (!any) min_x = b.x0, min_y = b.y0, max_x = b.x1, max_y = b.y1, any = true;
        min_x = std::min(min_x, b.x0), min_y = std::min(min_y, b.y0);
        max_x = std::max(max_x, b.x1), max_y = std::max(max_y, b.y1);
    }
    if (!any) return;
    if (!std::isfinite(max_x - min_x) || !std::isfinite(max_y - min_y)) return target_alpha_pairs(boxes, alpha);
    const int side = std::clamp(static_cast<int>(std::sqrt(static_cast<double>(n))), 1, kMaxGrid);
    const float cell_w = max_x > min_x ? (max_x - min_x) / static_cast<float>(side) : 1.0f;
    const float cell_h = max_y > min_y ? (max_y - min_y) / static_cast<float>(side) : 1.0f;
    auto column = [&](float x) { return std::clamp(static_cast<int>((x - min_x) / cell_w), 0, side - 1); };
    auto row = [&](float y) { return std::clamp(static_cast<int>((y - min_y) / cell_h), 0, side - 1); };
    const auto cells = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    auto cell = [&](int c, int r) { return static_cast<std::size_t>(r) * static_cast<std::size_t>(side) + static_cast<std::size_t>(c); };

    // The cells' members, as one list with each cell's start (kept from call to call: called every frame).
    thread_local std::vector<std::size_t> start, members;
    thread_local std::vector<std::uint32_t> seen;
    start.assign(cells + 1, 0);
    for (const PlateBox& b : boxes) {
        if (!has_area(b)) continue;
        for (int r = row(b.y0); r <= row(b.y1); ++r)
            for (int c = column(b.x0); c <= column(b.x1); ++c) ++start[cell(c, r) + 1];
    }
    for (std::size_t k = 0; k < cells; ++k) start[k + 1] += start[k];
    members.resize(start[cells]);
    {
        thread_local std::vector<std::size_t> fill;
        fill.assign(start.begin(), start.end() - 1);
        for (std::size_t j = 0; j < n; ++j) {
            const PlateBox& b = boxes[j];
            if (!has_area(b)) continue;
            for (int r = row(b.y0); r <= row(b.y1); ++r)
                for (int c = column(b.x0); c <= column(b.x1); ++c) members[fill[cell(c, r)]++] = j;
        }
    }
    seen.assign(n, 0);
    std::uint32_t stamp = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const PlateBox& b = boxes[i];
        if (b.front || !has_area(b)) continue;
        const float own = (b.x1 - b.x0) * (b.y1 - b.y0);
        if (own <= 0.0f) continue;
        ++stamp;
        float covered = 0.0f;
        for (int r = row(b.y0); r <= row(b.y1) && covered < kPlateFadeTo; ++r)
            for (int c = column(b.x0); c <= column(b.x1) && covered < kPlateFadeTo; ++c)
                for (std::size_t m = start[cell(c, r)]; m < start[cell(c, r) + 1]; ++m) {
                    const std::size_t j = members[m];
                    if (seen[j] == stamp) continue;
                    seen[j] = stamp;
                    if (i != j && nearer(boxes[j], b)) {
                        covered = std::max(covered, overlap_area(b, boxes[j]) / own);
                        if (covered >= kPlateFadeTo) break;
                    }
                }
        alpha[i] = faded_alpha(covered);
    }
}

float PlateFade::step(std::uint32_t id, float alpha_target, float dt, bool front) {
    const float goal = front ? 1.0f : std::clamp(alpha_target, 0.0f, 1.0f);
    auto [it, fresh] = entries_.try_emplace(id, Entry{goal, true});
    Entry& e = it->second;
    e.seen = true;
    if (front) e.alpha = 1.0f;
    else if (!fresh) e.alpha += (goal - e.alpha) * (1.0f - std::exp(-std::max(dt, 0.0f) / 0.3f));
    return e.alpha;
}

void PlateFade::end_frame() {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!it->second.seen) it = entries_.erase(it);
        else {
            it->second.seen = false;
            ++it;
        }
    }
}

} // namespace hailstone::game

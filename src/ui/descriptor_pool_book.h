#pragma once
// Which of the ImGui textures' descriptor pools (ui/imgui_vk_textures.cpp) a set is asked from, and when a pool goes.
//
// Each pool holds a fixed number of sets. A full one is not asked (vkAllocateDescriptorSets would only fail), so a
// texture made with many pools up costs one try, not one per pool. A pool left with no set is destroyed unless it is
// the newest, which stays for the next texture (so one texture made and freed over and over does not make and destroy
// a pool each time). Its sets were freed only once no frame in flight could draw them (remove_imgui_texture), so the
// pool can go at once. Kept apart from Vulkan, with the pool a template parameter, so it is tested on its own.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

namespace hailstone::ui {

template <class Pool>
class DescriptorPoolBook {
public:
    explicit DescriptorPoolBook(std::uint32_t sets_per_pool) : capacity_(sets_per_pool) {}

    // The pools with room, newest first: the order to ask them in before making another.
    std::vector<Pool> with_room() const {
        std::vector<Pool> out;
        for (auto it = pools_.rbegin(); it != pools_.rend(); ++it)
            if (it->live < capacity_) out.push_back(it->pool);
        return out;
    }

    // A pool just made: the newest.
    void added(Pool pool) { pools_.push_back({pool, 0}); }

    // A set was made from `pool`.
    void allocated(Pool pool) {
        if (Entry* e = find(pool)) ++e->live;
    }

    // A set from `pool` was freed. The pool to destroy now, if that left it empty and it is not the newest (it is
    // then no longer in the book).
    std::optional<Pool> freed(Pool pool) {
        auto it = std::find_if(pools_.begin(), pools_.end(), [&](const Entry& e) { return e.pool == pool; });
        if (it == pools_.end() || it->live == 0) return std::nullopt;
        if (--it->live > 0 || std::next(it) == pools_.end()) return std::nullopt;
        pools_.erase(it);
        return pool;
    }

    std::size_t pool_count() const { return pools_.size(); }
    // Sets live in `pool` (0 for one not in the book).
    std::uint32_t live(Pool pool) const {
        for (const Entry& e : pools_)
            if (e.pool == pool) return e.live;
        return 0;
    }

private:
    struct Entry {
        Pool pool;
        std::uint32_t live;
    };

    Entry* find(Pool pool) {
        for (Entry& e : pools_)
            if (e.pool == pool) return &e;
        return nullptr;
    }

    std::uint32_t capacity_;
    std::vector<Entry> pools_; // the newest last
};

} // namespace hailstone::ui

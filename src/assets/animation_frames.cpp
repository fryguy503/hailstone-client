#include "assets/character.h"

#include <cmath>
#include <stdexcept>

namespace hailstone::assets {

BoneFrames::BoneFrames(BoneFrames&& other) noexcept : storage_(std::move(other.storage_)) {
    other.clear();
}

BoneFrames& BoneFrames::operator=(BoneFrames&& other) noexcept {
    if (this != &other) {
        storage_ = std::move(other.storage_);
        other.clear();
    }
    return *this;
}

BoneFrames BoneFrames::from_wld(wld::TrackFrames frames, glm::vec3 scale_multiplier) {
    BoneFrames out;
    out.storage_ = WldFrames{std::move(frames), scale_multiplier};
    return out;
}

bool BoneFrames::encoded() const {
    const auto* frames = std::get_if<WldFrames>(&storage_);
    return frames && frames->frames.encoded();
}

bool BoneFrames::mapped() const {
    const auto* frames = std::get_if<WldFrames>(&storage_);
    return frames && frames->frames.mapped();
}

BoneFrames::size_type BoneFrames::size() const {
    if (const auto* frames = std::get_if<WldFrames>(&storage_)) return frames->frames.size();
    return std::get<std::vector<BonePose>>(storage_).size();
}

BoneFrames::size_type BoneFrames::capacity() const {
    if (const auto* frames = std::get_if<WldFrames>(&storage_)) return frames->frames.capacity();
    return std::get<std::vector<BonePose>>(storage_).capacity();
}

BoneFrames::size_type BoneFrames::decoded_capacity() const {
    if (const auto* frames = std::get_if<std::vector<BonePose>>(&storage_)) return frames->capacity();
    return 0;
}

std::size_t BoneFrames::owned_payload_bytes(std::unordered_set<const void*>& seen) const {
    if (const auto* frames = std::get_if<std::vector<BonePose>>(&storage_)) return frames->size() * sizeof(BonePose);
    const auto& frames = std::get<WldFrames>(storage_).frames;
    if (!frames.encoded()) return frames.size() * sizeof(wld::TrackFrame);
    const auto bytes = frames.private_encoded_bytes();
    return !bytes.empty() && seen.insert(bytes.data()).second ? bytes.size() : 0;
}

BonePose BoneFrames::operator[](size_type i) const {
    if (const auto* frames = std::get_if<WldFrames>(&storage_)) {
        // Keep CharacterLibrary's to_pose rules exactly, including invalid rotations/scales and quaternion sign.
        const auto frame = frames->frames[i];
        BonePose pose;
        const auto q = frame.rotation;
        const float length = glm::length(q);
        pose.rotation = std::isfinite(length) && length > 1e-6f ? q / length : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        pose.translation = frame.translation;
        pose.scale = glm::vec3(std::isfinite(frame.scale) ? frame.scale : 1.0f);
        pose.scale *= frames->scale_multiplier;
        return pose;
    }
    return std::get<std::vector<BonePose>>(storage_)[i];
}

BonePose BoneFrames::at(size_type i) const {
    if (i >= size()) throw std::out_of_range("animation frame index");
    return (*this)[i];
}

BonePose& BoneFrames::at(size_type i) {
    if (i >= size()) throw std::out_of_range("animation frame index");
    return materialize()[i];
}

std::vector<BonePose>& BoneFrames::materialize() {
    if (auto* owned = std::get_if<std::vector<BonePose>>(&storage_)) return *owned;
    std::vector<BonePose> decoded;
    decoded.reserve(size());
    const BoneFrames& view = *this;
    for (size_type i = 0; i < view.size(); ++i) decoded.push_back(view[i]);
    storage_ = std::move(decoded);
    return std::get<std::vector<BonePose>>(storage_);
}

} // namespace hailstone::assets

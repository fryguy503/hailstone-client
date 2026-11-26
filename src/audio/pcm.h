#pragma once

// Decoded sound: interleaved float samples at the file's own rate. The game's
// WAVs are PCM (8-bit unsigned, 16/24-bit signed; mono or stereo; 8000..48000 Hz,
// including 22000) and its music MP3s; dr_wav and dr_mp3 decode both.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hailstone::audio {

struct Pcm {
    std::uint32_t rate = 0;
    std::uint32_t channels = 0; // 1 or 2
    std::vector<float> samples; // interleaved
    std::size_t frames() const { return channels ? samples.size() / channels : 0; }
};

// A sine tone, mono at `rate`: `ms` long, its start and end faded over `fade_ms` (no click), peaking at `amplitude`.
// MQ's /beep with no sound is Windows' Beep(0x500, 250): 1280 Hz for 250 ms (app: the Beep sound "Tone").
Pcm make_tone(std::uint32_t rate, double hz, std::uint32_t ms, std::uint32_t fade_ms, float amplitude);

// nullopt for a file that is not a WAV dr_wav can read, or has more than two channels.
std::optional<Pcm> decode_wav(std::span<const std::byte> data);
std::optional<Pcm> decode_mp3(std::span<const std::byte> data);
// By the data: RIFF -> WAV, else MP3.
std::optional<Pcm> decode_sound(std::span<const std::byte> data);

// A 16-bit PCM WAV (RIFF, little-endian on every host) of interleaved samples in -1..1 (clamped), `channels` 1 or 2:
// tools/audiodump's output.
std::vector<std::byte> encode_wav16(std::span<const float> samples, std::uint32_t rate, std::uint16_t channels);

// A playing source the mixer pulls from (music): writes up to `frames` stereo
// frames at `rate` into `out` and returns how many it wrote; fewer = finished.
class Source {
public:
    virtual ~Source() = default;
    virtual std::size_t render(float* out, std::size_t frames, std::uint32_t rate) = 0;
};

// An MP3 streamed from its file bytes (decoded as it plays), `loops` times (0 = forever).
std::unique_ptr<Source> make_mp3_source(std::vector<std::byte> file, std::int32_t loops);
std::unique_ptr<Source> make_mp3_source(std::shared_ptr<const std::vector<std::byte>> file, std::int32_t loops);

} // namespace hailstone::audio

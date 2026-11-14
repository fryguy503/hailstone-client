// libFuzzer entry points (build with -DHAILSTONE_LIBFUZZER=ON and Clang). One executable per parser family, chosen by
// the HAILSTONE_FUZZ_TARGET definition from CMake. Seed corpora: any file of that kind; the deterministic tests in
// fuzz_net_test.cpp and fuzz_assets_test.cpp are the always-on version of the same checks.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string_view>
#include <vector>

#include "assets/character.h"
#include "assets/pfs.h"
#include "assets/texture.h"
#include "assets/wld.h"
#include "core/addon_index.h"
#include "core/addon_manifest.h"
#include "core/addon_source.h"
#include "core/binary_header.h"
#include "core/shared_file_cache.h"
#include "core/update_manifest.h"
#include "core/zip_read.h"
#include "net/session_codec.h"
#include "net/zone_packets.h"
#if defined(HAILSTONE_FUZZ_DECODERS)
#include <iterator>

#include "net/aa_packets.h"
#include "net/achievement_packets.h"
#include "net/ability_packets.h"
#include "net/adventure_packets.h"
#include "net/aggro_packets.h"
#include "net/chat_server.h"
#include "net/door_packets.h"
#include "net/dz_packets.h"
#include "net/group_packets.h"
#include "net/fellowship_packets.h"
#include "net/guild_bank_packets.h"
#include "net/item_packets.h"
#include "net/login.h"
#include "net/market_packets.h"
#include "net/marketplace_packets.h"
#include "net/merchant_packets.h"
#include "net/movement_packets.h"
#include "net/notice_packets.h"
#include "net/quest_npc_packets.h"
#include "net/settings_packets.h"
#include "net/zone_link_packets.h"
#include "net/pet_packets.h"
#include "net/real_estate_packets.h"
#include "net/reward_packets.h"
#include "net/player_packets.h"
#include "net/social_packets.h"
#include "net/spell_packets.h"
#include "net/task_packets.h"
#include "net/trade_packets.h"
#include "net/training_packets.h"
#include "net/tribute_packets.h"
#include "net/world_packets.h"
#if defined(HAILSTONE_HAS_NMS)
#include "nms_packets.h"
#endif
#if defined(HAILSTONE_HAS_TRIUNE)
#include "triune_packets.h"
#include "triune_windows.h"
#endif
#include "fuzz_game_decoders.h"
#include "fuzz_achievements.h"
#endif

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::span<const std::byte> in(reinterpret_cast<const std::byte*>(data), size);
    try {
#if defined(HAILSTONE_FUZZ_PFS)
        auto a = hailstone::assets::PfsArchive::from_memory(std::vector<std::byte>(in.begin(), in.end()));
        for (const auto& n : a.names()) (void)a.read(n);
#elif defined(HAILSTONE_FUZZ_WLD)
        hailstone::assets::wld::WldFile::ReadScope reads;
        auto w = hailstone::assets::wld::WldFile::deferred(
            [in] { return std::vector<std::byte>(in.begin(), in.end()); }, "fuzz");
        for (const auto& f : w.fragments())
            if (const auto* m = f.get<hailstone::assets::wld::MeshDef>()) (void)hailstone::assets::wld::build_mesh(w, *m, f.name);
        // The shared cache's path: track keys stay encoded in the file's bytes and are decoded on read.
        const auto encoded = hailstone::assets::wld::WldFile::parse(
            hailstone::ImmutableFileBytes::owned(std::vector<std::byte>(in.begin(), in.end())), "fuzz", true);
        for (const auto& f : encoded.fragments())
            if (const auto* def = f.get<hailstone::assets::wld::TrackDef>()) {
                const auto track = hailstone::assets::AnimationTrack::from_wld(def->frames);
                for (std::size_t i = 0; i < track.size(); ++i) (void)track[i];
            }
#elif defined(HAILSTONE_FUZZ_TEXTURE)
        (void)hailstone::assets::decode_texture(in);
#elif defined(HAILSTONE_FUZZ_SOE)
        namespace soe = hailstone::net::soe;
        soe::SessionParams params;
        params.crc_bytes = 2;
        params.passes = {soe::EncodePass::Compression, soe::EncodePass::Xor};
        std::vector<std::byte> v(in.begin(), in.end());
        (void)soe::decode_datagram(params, v);
        (void)soe::split_combined(v);
        (void)soe::split_app_combined(v);
        (void)soe::decode_app(v);
        soe::InboundStream stream;
        std::vector<soe::Bytes> out;
        (void)stream.push(v, out);
#elif defined(HAILSTONE_FUZZ_PACKETS)
        // First two bytes pick the decoder-agnostic opcode; the rest is the payload. Decoders must not throw.
        if (size >= 2) {
            hailstone::net::AppPacket p(static_cast<std::uint16_t>(data[0] | data[1] << 8), std::vector<std::byte>(in.begin() + 2, in.end()));
            (void)hailstone::net::decode_spawn(p);
            (void)hailstone::net::decode_player_profile(p);
            (void)hailstone::net::decode_new_zone(p);
            (void)hailstone::net::decode_channel_message(p);
            (void)hailstone::net::decode_zone_points(p);
        }
#elif defined(HAILSTONE_FUZZ_DECODERS)
        // Every typed decoder of tests/fuzz_decoders.inc: byte 0 picks it, bytes 1..2 are the opcode, the rest the
        // payload.
        using Fn = void (*)(const hailstone::net::AppPacket&);
        static const Fn kDecoders[] = {
#define HAILSTONE_FUZZ_DECODER(fn) [](const hailstone::net::AppPacket& p) { (void)hailstone::net::fn(p); },
#define HAILSTONE_FUZZ_PLUGIN_DECODER(ns, fn) [](const hailstone::net::AppPacket& p) { (void)::ns::fn(p.data); },
#define HAILSTONE_FUZZ_GAME_DECODER(fn) [](const hailstone::net::AppPacket& p) { ::hailstone::fuzz_game::feed(&::hailstone::game::fn, p.data); },
#include "fuzz_decoders.inc"
#undef HAILSTONE_FUZZ_GAME_DECODER
#undef HAILSTONE_FUZZ_PLUGIN_DECODER
#undef HAILSTONE_FUZZ_DECODER
        };
        if (size >= 3) {
            hailstone::net::AppPacket p(static_cast<std::uint16_t>(data[1] | data[2] << 8), std::vector<std::byte>(in.begin() + 3, in.end()));
            kDecoders[data[0] % std::size(kDecoders)](p);
            // Additional context-backed coverage; public default calls intentionally reject
            // state layouts without a received catalog and cannot fuzz their inner fields.
            // Alternate format: byte0 chooses an achievement parser, remainder is its body.
            hailstone::fuzz_achievements::feed(in);
        }
#elif defined(HAILSTONE_FUZZ_ZIP)
        // The updater's inputs (docs/updater.md): a package zip read whole, and the same bytes as a manifest.
        hailstone::ZipReader zip;
        if (zip.open(std::vector<std::byte>(in.begin(), in.end())))
            for (const auto& e : zip.entries()) (void)zip.extract(e, [](std::span<const std::byte>) { return true; });
        (void)hailstone::parse_update_manifest(std::string_view(reinterpret_cast<const char*>(data), size),
                                          hailstone::Ed25519PublicKey{});
        // The add-on manager's (docs/addons.md): a manifest, a pasted link, a plugin library's header.
        const std::string_view text(reinterpret_cast<const char*>(data), size);
        (void)hailstone::parse_addon_manifest(text);
        (void)hailstone::parse_addon_link(text);
        (void)hailstone::read_binary_header(in);
        (void)hailstone::parse_addon_index(text, hailstone::Ed25519PublicKey{});
#else
#error "define one of HAILSTONE_FUZZ_PFS, HAILSTONE_FUZZ_WLD, HAILSTONE_FUZZ_TEXTURE, HAILSTONE_FUZZ_SOE, HAILSTONE_FUZZ_PACKETS, HAILSTONE_FUZZ_DECODERS, HAILSTONE_FUZZ_ZIP"
#endif
    } catch (const std::exception&) {
        // A reported error is the expected outcome for malformed input; crashes and sanitizer reports are not.
    }
    return 0;
}

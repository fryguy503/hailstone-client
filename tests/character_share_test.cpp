// assets::SharedCharacterCache and CharacterLookups: what zones' character libraries share, and what a zone's own
// archives keep out of it.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>

#include "assets/character.h"
#include "game/appearance.h"
#include "game_files.h"
#include "pfs_fixtures.h"
#include "scratch_dir.h"
#include "wld_fixtures.h"

using namespace hailstone;
using namespace hailstone::assets;
using test::pfs_fixtures::make_pfs;

namespace {

std::shared_ptr<const CharacterArchive> archive(const std::string& label, const std::vector<std::string>& files) {
    std::vector<std::pair<std::string, std::vector<std::byte>>> entries;
    for (const auto& f : files) entries.emplace_back(f, std::vector<std::byte>(8, std::byte{0x2a}));
    auto a = std::make_shared<CharacterArchive>();
    a->archive = std::make_shared<PfsArchive>(PfsArchive::from_memory(make_pfs(entries), label));
    return a;
}

// A WLD of static actors "<code>_ACTORDEF", each drawn by the fixture mesh.
std::vector<std::byte> actors_wld(std::initializer_list<std::string> codes) {
    using namespace test::wld_fixtures;
    WldBuilder b;
    auto bmp = b.add(0x03, b.str("A_SPRITE"), bitmap_name("a.bmp"));
    ByteWriter info;
    info.le(std::uint32_t{0});
    info.le(std::uint32_t{1});
    info.le(bmp);
    auto info_ref = b.add(0x04, b.str("A_SPRITEINFO"), info);
    ByteWriter sprite;
    sprite.le(info_ref);
    sprite.le(std::uint32_t{0});
    auto mat = b.add(0x30, b.str("A_MDF"), material(0x80000001, b.add(0x05, 0, sprite)));
    auto list = b.add(0x31, b.str("A_MP"), refs({mat, mat, mat})); // the fixture mesh uses slots 0 and 2
    for (const auto& code : codes) {
        auto m = b.add(0x36, b.str(code + "_DMSPRITEDEF"), mesh(list, false));
        ByteWriter ref;
        ref.le(m);
        ref.le(std::uint32_t{0});
        auto sprite_ref = b.add(0x2d, 0, ref);
        ByteWriter actor;
        actor.le(std::uint32_t{0}); // flags
        actor.le(std::int32_t{0});  // callback
        actor.le(std::uint32_t{0}); // actions
        actor.le(std::uint32_t{1}); // sprites
        actor.le(std::int32_t{0});  // bounds
        actor.le(sprite_ref);
        b.add(0x14, b.str(code + "_ACTORDEF"), actor);
    }
    return b.build();
}

// An archive file: a WLD of `codes`' actors and `files` (each holding `fill`).
std::vector<std::byte> archive_bytes(const std::string& label, std::initializer_list<std::string> codes,
                                     const std::vector<std::string>& files, std::byte fill) {
    std::vector<std::pair<std::string, std::vector<std::byte>>> entries;
    entries.emplace_back(label.substr(0, label.find('.')) + ".wld", actors_wld(codes));
    for (const auto& f : files) entries.emplace_back(f, std::vector<std::byte>(8, fill));
    return make_pfs(entries);
}

std::shared_ptr<const CharacterArchive> actor_archive(const std::string& label, std::initializer_list<std::string> codes,
                                                      const std::vector<std::string>& files, std::byte fill) {
    auto a = std::make_shared<CharacterArchive>();
    a->archive = std::make_shared<PfsArchive>(PfsArchive::from_memory(archive_bytes(label, codes, files, fill), label));
    for (const auto& member : a->archive->names())
        if (member.ends_with(".wld")) a->wlds.push_back(wld::WldFile::parse(*a->archive->read(member), label + ":" + member));
    return a;
}

bool has(const CharacterLookups::Names& names, const std::string& name) {
    return names && std::find(names->begin(), names->end(), name) != names->end();
}

} // namespace

TEST_CASE("character lookups: a library's lookups are noted, and a nested record's join the outer one") {
    CharacterLibrary lib({archive("global_chr.s3d", {"humch0001.bmp"})});
    CHECK_FALSE(CharacterLookups::active());
    CharacterLookups outer;
    CHECK(CharacterLookups::active());
    CHECK(lib.read_file("HUMCH0001.BMP"));
    CharacterLookups::Names inner_names;
    {
        CharacterLookups inner;
        CHECK_FALSE(lib.has_file("nothere.bmp")); // a name looked up and not found counts too
        CHECK_FALSE(lib.has_model("XYZ"));
        inner_names = inner.names();
    }
    CHECK(has(inner_names, "fnothere.bmp"));
    CHECK(has(inner_names, "aXYZ"));
    CHECK_FALSE(has(inner_names, "fhumch0001.bmp"));
    auto all = outer.names();
    CHECK(has(all, "fhumch0001.bmp"));
    CHECK(has(all, "fnothere.bmp"));
    CHECK(std::is_sorted(all->begin(), all->end()));
}

TEST_CASE("character lookups: a fresh thread has its own collector and restores the empty state") {
    CHECK_FALSE(CharacterLookups::active());
    {
        CharacterLookups outer;
        CharacterLookups::note('f', "main");
        bool before = true, during = false, after = true;
        CharacterLookups::Names worker_names;
        std::thread worker([&] {
            before = CharacterLookups::active();
            {
                CharacterLookups inner;
                CharacterLookups::note('f', "worker");
                during = CharacterLookups::active();
                worker_names = inner.names();
            }
            after = CharacterLookups::active();
        });
        worker.join();
        CHECK_FALSE(before);
        CHECK(during);
        CHECK_FALSE(after);
        REQUIRE(worker_names);
        CHECK(has(worker_names, "fworker"));
        CHECK_FALSE(has(worker_names, "fmain"));
        CHECK(CharacterLookups::active());
        CHECK(has(outer.names(), "fmain"));
        CHECK_FALSE(has(outer.names(), "fworker"));
    }
    CHECK_FALSE(CharacterLookups::active());
}

TEST_CASE("character lookups: only names a zone archive defines keep a result out of the shared cache") {
    auto global = archive("global_chr.s3d", {"humch0001.bmp", "elfch0001.bmp"});
    // The zone's archive has its own copy of one global texture and one texture of its own.
    auto zone = archive("qeynos2_chr.s3d", {"elfch0001.bmp", "qcmch0001.bmp"});
    CharacterLibrary plain({global, zone}, {}, 1);
    CharacterLibrary other({global}, {}, 1);

    auto names_of = [](const CharacterLibrary& lib, const std::string& file) {
        CharacterLookups rec;
        lib.read_file(file);
        return rec.names();
    };
    CHECK(plain.unaffected(names_of(plain, "humch0001.bmp")));
    CHECK_FALSE(plain.unaffected(names_of(plain, "elfch0001.bmp"))); // the zone has one by that name
    CHECK_FALSE(plain.unaffected(names_of(plain, "qcmch0001.bmp"))); // only the zone has it
    CHECK(plain.unaffected(names_of(plain, "missing.bmp")));         // nobody has it: the same everywhere
    // Without zone archives everything is unaffected; what the other zone looked up is affected here.
    CHECK(other.unaffected(names_of(other, "elfch0001.bmp")));
    CHECK_FALSE(plain.unaffected(names_of(other, "qcmch0001.bmp")));
    // Without a first zone archive (CharacterLibrary::load, the tools) nothing is a zone's.
    CharacterLibrary all({global, zone});
    CHECK(all.unaffected(names_of(all, "qcmch0001.bmp")));
}

TEST_CASE("shared character cache: what a library builds from the global archives alone goes into it") {
    auto shared = std::make_shared<SharedCharacterCache>();
    auto global = archive("gequip.s3d", {"it5.bmp"});
    CharacterLibrary a({global}, {}, 1);
    a.set_shared_cache(shared);
    CHECK_FALSE(a.item_mesh(5)); // no IT5 actor anywhere: nothing, and that is shared too
    REQUIRE(shared->items.contains(5));
    CHECK(has(shared->items.at(5).lookups, "aIT5"));

}

TEST_CASE("shared character cache: next_zone keeps what was handed out since the last zone change") {
    SharedCharacterCache cache;
    auto model = std::make_shared<const CharacterModel>();
    auto mesh = std::make_shared<const CharacterMesh>();
    auto names = std::make_shared<const std::vector<std::string>>();
    cache.models.emplace("HUM", SharedCharacterCache::Entry<std::shared_ptr<const CharacterModel>>{model, names, 1});
    cache.owners[model.get()] = {names, nullptr};
    cache.dresses.emplace(std::pair{model.get(), std::string("key")},
                          SharedCharacterCache::Entry<std::shared_ptr<const CharacterMesh>>{mesh, names, 1});
    cache.owners[mesh.get()] = {names, model.get()};
    cache.items.emplace(7, SharedCharacterCache::Entry<std::optional<CharacterLibrary::ItemMesh>>{std::nullopt, names, 1});

    cache.next_zone(); // all used in generation 1: kept
    CHECK(cache.generation() == 2);
    CHECK(cache.models.size() == 1);
    CHECK(cache.dresses.size() == 1);
    CHECK(cache.items.size() == 1);

    cache.models.at("HUM").used = 2; // the model handed out in zone 2, its dress and the item not
    cache.next_zone();
    CHECK(cache.models.size() == 1);
    CHECK(cache.dresses.empty());
    CHECK(cache.items.empty());
    CHECK(cache.owners.size() == 1);

    cache.next_zone(); // nothing handed out in zone 3
    CHECK(cache.models.empty());
    CHECK(cache.owners.empty());
}

TEST_CASE("shared character cache: libraries of two zones hand out the same global models and appearances" *
          doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    CharacterArchiveCache cache(false);
    auto first = cache.library_for_zone(game, "qeynos2");
    auto hum = first->model("HUM");
    REQUIRE(hum);
    game::Appearance look;
    auto dressed = first->dress(*hum, look);
    REQUIRE(dressed);
    CHECK(first->shares(hum.get()));
    CHECK(first->shares(dressed.get()));

    // The same zone again (the bazaar to the bazaar): everything the first one built is handed out.
    auto again = cache.library_for_zone(game, "qeynos2");
    CHECK(again->model("HUM") == hum);
    CHECK(again->dress(*hum, look) == dressed);

    auto other = cache.library_for_zone(game, "qeynos");
    if (other->shares(hum.get())) { // qeynos's own archives define nothing HUM looked up
        CHECK(other->model("HUM") == hum);
        CHECK(other->dress(*hum, look) == dressed);
    } else {
        CHECK(other->model("HUM") != hum);
    }
}

TEST_CASE("global archive index: zones' libraries share it, and a zone's actors override global ones as before") {
    // Global archives at level 0 (AAA, BBB) and level 2 (CCC); the zone's (level 2) redefine AAA and CCC.
    auto g0 = actor_archive("global_chr.s3d", {"AAA", "BBB"}, {"shared.bmp"}, std::byte{1});
    auto g2 = actor_archive("global2_chr.s3d", {"CCC"}, {}, std::byte{2});
    auto zone = actor_archive("tst_chr.s3d", {"AAA", "CCC", "ZZZ"}, {"shared.bmp", "zone.bmp"}, std::byte{3});
    auto index = std::make_shared<const CharacterArchiveIndex>(std::vector{g0, g2}, std::vector<int>{0, 2});
    CharacterLibrary in_zone(index, {zone});
    CharacterLibrary elsewhere(index, {});
    CHECK(in_zone.global_index() == index.get());
    CHECK(elsewhere.global_index() == index.get());
    CHECK(in_zone.archives().size() == 3);

    auto source = [](const CharacterLibrary& lib, const std::string& code) { return lib.build(code).source; };
    CHECK(source(in_zone, "AAA").starts_with("tst_chr.s3d"));     // a later level wins: the zone's
    CHECK(source(in_zone, "BBB").starts_with("global_chr.s3d"));  // only the global one
    CHECK(source(in_zone, "CCC").starts_with("global2_chr.s3d")); // the same level: the first loaded
    CHECK(source(in_zone, "ZZZ").starts_with("tst_chr.s3d"));
    CHECK(source(elsewhere, "AAA").starts_with("global_chr.s3d")); // the zone's tables are its own
    CHECK_FALSE(elsewhere.has_model("ZZZ"));
    // Files: the first archive's copy.
    REQUIRE(in_zone.read_file("shared.bmp"));
    CHECK(in_zone.read_file("shared.bmp")->front() == std::byte{1});
    CHECK(in_zone.read_file("zone.bmp")->front() == std::byte{3});

    // The same answers as one library over all the archives (how a zone's library was made before the index).
    CharacterLibrary flat({g0, g2, zone}, {0, 2, 2}, 2);
    for (const char* code : {"AAA", "BBB", "CCC", "ZZZ"}) CHECK(source(flat, code) == source(in_zone, code));
    // What the zone defines keeps a result out of the shared cache, as with the flat library.
    auto names_of = [](const CharacterLibrary& lib, const std::string& code) {
        CharacterLookups rec;
        lib.has_model(code);
        return rec.names();
    };
    CHECK_FALSE(in_zone.unaffected(names_of(in_zone, "AAA")));
    CHECK(in_zone.unaffected(names_of(in_zone, "BBB")));
    CHECK(elsewhere.unaffected(names_of(in_zone, "AAA")));
}

TEST_CASE("global archive index: CharacterArchiveCache builds it once, and again after clear()") {
    test::ScratchDir dir("chr-index");
    auto write = [&](const std::string& name, const std::vector<std::byte>& bytes) {
        std::ofstream(dir.path() / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    write("global_chr.s3d", archive_bytes("global_chr.s3d", {"AAA", "BBB"}, {}, std::byte{1}));
    write("tst_chr.s3d", archive_bytes("tst_chr.s3d", {"AAA"}, {}, std::byte{3}));
    write("global5_chr.s3d", archive_bytes("global5_chr.s3d", {"CCC"}, {}, std::byte{4}));
    GameDir game(dir.path());
    // The lease rules alone: decoded WLDs go as the last lease ends (wld::WldFile's retained set off).
    const auto saved_budget = wld::WldFile::retain_budget();
    wld::WldFile::release_retained();
    wld::WldFile::set_retain_budget(0);
    struct Restore {
        std::size_t budget;
        ~Restore() { wld::WldFile::set_retain_budget(budget); }
    } restore{saved_budget};
    CharacterArchiveCache cache(false);
    auto a = cache.library_for_zone(game, "tst");
    auto b = cache.library_for_zone(game, "other");
    REQUIRE(a->global_index());
    CHECK(a->global_index() == b->global_index());
    for (const auto& ar : a->archives())
        for (const auto& w : ar->wlds) CHECK_FALSE(w.materialized());
    CHECK(a->has_model("CCC"));
    for (const auto& ar : a->archives())
        for (const auto& w : ar->wlds) CHECK_FALSE(w.materialized());
    CHECK(a->build("AAA").source.starts_with("tst_chr.s3d"));
    CHECK(b->build("AAA").source.starts_with("global_chr.s3d"));
    CHECK(a->build("BBB").source.starts_with("global_chr.s3d"));
    auto model = a->model("BBB");
    REQUIRE(model);
    const auto saved_vertices = model->vertices;
    auto look = a->dress(*model, game::Appearance{});
    REQUIRE(look);
    auto actor = a->actor_mesh("BBB");
    REQUIRE(actor);
    CHECK(actor->mesh.indices == model->indices);
    // A cached model and look own their geometry after all source leases have ended.
    for (const auto& ar : a->archives())
        for (const auto& w : ar->wlds) CHECK_FALSE(w.materialized());
    auto again = a->model("BBB");
    CHECK(again == model);
    CHECK(a->dress(*model, game::Appearance{}) == look);
    CHECK(model->vertices.size() == saved_vertices.size());
    REQUIRE_FALSE(model->vertices.empty());
    CHECK(model->vertices.front().position == saved_vertices.front().position);
    CHECK(a->cache_memory().models == 1); // local and shared entries refer to the same object
    CHECK(a->cache_memory().appearances == 1);
    for (const auto& ar : a->archives())
        if (ar->archive->label() == "global5_chr.s3d")
            for (const auto& w : ar->wlds) CHECK_FALSE(w.materialized());
    cache.clear(); // another asset folder or overlay: read and indexed again
    auto c = cache.library_for_zone(game, "tst");
    CHECK(c->global_index() != a->global_index());
    CHECK(c->build("AAA").source.starts_with("tst_chr.s3d"));
}

TEST_CASE("models built one after another from one character WLD decode it once while it is retained") {
    test::ScratchDir dir("chr-retain");
    auto bytes = archive_bytes("global_chr.s3d", {"AAA", "BBB", "CCC"}, {}, std::byte{1});
    std::ofstream(dir.path() / "global_chr.s3d", std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    GameDir game(dir.path());
    const auto saved_budget = wld::WldFile::retain_budget();
    struct Restore {
        std::size_t budget;
        ~Restore() { wld::WldFile::set_retain_budget(budget); }
    } restore{saved_budget};
    auto decodes = [](const CharacterLibrary& lib) {
        std::size_t n = 0;
        for (const auto& ar : lib.archives())
            for (const auto& w : ar->wlds) n += w.decode_stats().decodes;
        return n;
    };
    SUBCASE("retained (the default budget)") {
        wld::WldFile::set_retain_budget(wld::WldFile::kDefaultRetainBudget);
        CharacterArchiveCache cache(false);
        auto lib = cache.library_for_zone(game, "tst");
        for (const char* code : {"AAA", "BBB", "CCC"}) REQUIRE(lib->model(code)); // each its own cache miss
        CHECK(decodes(*lib) == 1);
    }
    SUBCASE("not retained: each model decodes it again") {
        wld::WldFile::release_retained();
        wld::WldFile::set_retain_budget(0);
        CharacterArchiveCache cache(false);
        auto lib = cache.library_for_zone(game, "tst");
        for (const char* code : {"AAA", "BBB", "CCC"}) REQUIRE(lib->model(code));
        CHECK(decodes(*lib) == 3);
    }
}

TEST_CASE("global archive index: a zone's library resolves as one over all its archives" *
          doctest::skip(!hailstone::test::game_dir())) {
    // Every model a zone's own archives define, built through the shared index and through one library over the
    // global and zone archives: the same definition and the same clips. Also the time a zone's library takes.
    const auto& game = *hailstone::test::game_dir();
    CharacterArchiveCache cache(true);
    cache.library_for_zone(game, "");
    for (const char* zone : {"qeynos2", "gfaydark", "poknowledge"}) {
        auto t0 = std::chrono::steady_clock::now();
        auto lib = cache.library_for_zone(game, zone);
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE(std::string(zone) << "'s character library: " << ms << " ms");
        const auto& all = lib->archives();
        const std::size_t first = lib->global_index()->archives().size();
        std::vector<int> levels;
        auto by_name = global_archive_levels(game);
        for (std::size_t i = 0; i < first; ++i) {
            std::string label = all[i]->archive->label();
            label = label.substr(label.find_last_of("/\\") + 1);
            auto it = by_name.find(label);
            levels.push_back(it == by_name.end() ? kGlobalResourceLevel : it->second);
        }
        CharacterLibrary flat(all, levels, first);
        CHECK(flat.model_codes() == lib->model_codes());
        CharacterLibrary zone_only({all.begin() + static_cast<std::ptrdiff_t>(first), all.end()});
        for (const auto& code : zone_only.model_codes()) {
            auto a = flat.model(code);
            auto b = lib->model(code);
            REQUIRE(bool(a) == bool(b));
            if (!a) continue;
            CHECK(a->source == b->source);
            CHECK(a->clips.size() == b->clips.size());
            CHECK(a->vertices.size() == b->vertices.size());
        }
    }
}

TEST_CASE("character libraries: one zone's dressed on a worker thread while the main thread uses the next zone's") {
    // render::ActorRenderer dresses on a worker while the main thread chooses models and, at a zone change, makes
    // the next zone's library (SharedCharacterCache::next_zone): the libraries of one CharacterArchiveCache share
    // a mutex, as they share their caches. Run under ThreadSanitizer to see a race (docs/hardening.md).
    test::ScratchDir dir("chr-threads");
    auto write = [&](const std::string& name, const std::vector<std::byte>& bytes) {
        std::ofstream(dir.path() / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    write("global_chr.s3d", archive_bytes("global_chr.s3d", {"AAA", "BBB", "IT5"}, {"a.bmp"}, std::byte{1}));
    write("tst_chr.s3d", archive_bytes("tst_chr.s3d", {"AAA"}, {"a.bmp"}, std::byte{3}));
    GameDir game(dir.path());
    CharacterArchiveCache cache(false);
    auto zone = cache.library_for_zone(game, "tst");
    std::shared_ptr<const CharacterMesh> first;
    int missing = 0; // (no assertions on the worker thread)
    std::thread worker([&] {
        for (int i = 0; i < 200; ++i) {
            for (const char* code : {"AAA", "BBB"}) {
                auto m = zone->model(code);
                if (!m) {
                    ++missing;
                    continue;
                }
                game::Appearance look;
                look.texture = static_cast<std::uint8_t>(i % 7);
                auto mesh = zone->dress(*m, look);
                if (i == 0 && std::string(code) == "AAA") first = mesh;
            }
            zone->texture("a.bmp", false);
            zone->item_mesh(5);
        }
    });
    std::vector<std::shared_ptr<const CharacterLibrary>> others;
    for (int i = 0; i < 20; ++i) {
        auto next = cache.library_for_zone(game, i % 2 ? "tst" : "other");
        auto m = next->model("BBB");
        CHECK(m);
        if (m) next->shares(m.get());
        next->has_model("ZZZ");
        next->read_file("a.bmp");
        others.push_back(std::move(next));
    }
    worker.join();
    CHECK(missing == 0);
    REQUIRE(first);
    game::Appearance look;
    look.texture = 0;
    CHECK(zone->dress(*zone->model("AAA"), look) == first); // the library's own cache, not another zone's
}

TEST_CASE("character libraries: dressed looks nobody wears are dropped as the cache grows, worn ones kept") {
    // Each distinct look (an NPC tint here) is a whole dressed mesh: a zone that shows many must not keep them all.
    test::ScratchDir dir("chr-dress-cap");
    auto write = [&](const std::string& name, const std::vector<std::byte>& bytes) {
        std::ofstream(dir.path() / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    write("global_chr.s3d", archive_bytes("global_chr.s3d", {"BBB"}, {"a.bmp"}, std::byte{1}));
    write("tst_chr.s3d", archive_bytes("tst_chr.s3d", {"AAA"}, {"a.bmp"}, std::byte{3}));
    GameDir game(dir.path());
    CharacterArchiveCache cache(false);
    auto zone = cache.library_for_zone(game, "tst");
    auto model = zone->model("AAA");
    REQUIRE(model);
    game::Appearance worn_look;
    worn_look.npc_tint = 1;
    auto worn = zone->dress(*model, worn_look);
    REQUIRE(worn);
    std::size_t most = 0;
    for (std::uint32_t tint = 2; tint < 3000; ++tint) {
        game::Appearance look;
        look.npc_tint = tint;
        CHECK(zone->dress(*model, look)); // used and let go
        if (tint % 50 == 0) most = std::max(most, zone->cache_memory().appearances);
    }
    CHECK(most <= 1024);
    CHECK(zone->cache_memory().appearances < 1024);
    // The worn look is still the same mesh; a dropped one is dressed again.
    CHECK(zone->dress(*model, worn_look) == worn);
    game::Appearance again;
    again.npc_tint = 2;
    CHECK(zone->dress(*model, again));
}

TEST_CASE("character libraries: dressing real models on a worker while the main thread builds others" *
          doctest::skip(!hailstone::test::game_dir())) {
    const auto& game = *hailstone::test::game_dir();
    CharacterArchiveCache cache(true);
    auto lib = cache.library_for_zone(game, "qeynos2");
    std::size_t textures = 0;
    std::thread worker([&] {
        for (const char* code : {"HUM", "HUF", "ELM", "DAF", "BAM", "QCM"}) {
            auto m = lib->model(code);
            if (!m) continue;
            for (int t = 0; t < 3; ++t) {
                game::Appearance look;
                look.texture = static_cast<std::uint8_t>(t);
                auto mesh = lib->dress(*m, look);
                if (!mesh) continue;
                for (const auto& mat : mesh->materials)
                    if (!mat.frames.empty() && lib->texture(mat.frames.front(), false)) ++textures;
            }
        }
    });
    for (const char* code : {"GNM", "TRM", "OGF", "SKE", "GOB", "ORC", "WOL", "BEA"}) {
        lib->model(code);
        lib->has_model(code);
    }
    auto next = cache.library_for_zone(game, "qeynos");
    next->model("HUM");
    worker.join();
    CHECK(textures > 0);
}

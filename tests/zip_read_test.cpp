// core/zip_read.h: archives from ZipWriter read back, hand-built archives with the faults the reader must refuse
// (unsafe names, wrong sizes and CRCs, truncation), the Unix permission bits, and a fuzz-style pass over mutated
// archives.

#include <doctest/doctest.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <zlib.h>

#include "core/byte_io.h"
#include "core/zip_read.h"
#include "core/windows_names.h"
#include "core/zip_write.h"
#include "fuzz_support.h"
#include "scratch_dir.h"

using namespace hailstone;

namespace {

std::tm some_time() {
    std::tm t{};
    t.tm_year = 126;
    t.tm_mon = 10;
    t.tm_mday = 1;
    t.tm_hour = 12;
    return t;
}

std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out;
    for (char c : s) out.push_back(static_cast<std::byte>(c));
    return out;
}

std::string extract_text(ZipReader& zip, const ZipEntry& e, bool* ok = nullptr) {
    std::string out;
    bool r = zip.extract(e, [&](std::span<const std::byte> piece) {
        out.append(reinterpret_cast<const char*>(piece.data()), piece.size());
        return true;
    });
    if (ok) *ok = r;
    return out;
}

// One stored entry of a hand-built archive, with the fields a test may get wrong on purpose.
struct RawEntry {
    std::string name;
    std::string data;
    std::uint16_t made_by = 20;      // MS-DOS
    std::uint32_t external = 0;
    std::uint16_t flags = 0;
    std::uint16_t method = 0;
    std::optional<std::uint32_t> crc = std::nullopt, size = std::nullopt; // when set, written in place of the right ones
};

std::vector<std::byte> build_zip(const std::vector<RawEntry>& entries) {
    ByteWriter w;
    std::vector<std::uint32_t> offsets;
    auto crc_of = [](const RawEntry& e) {
        return e.crc ? *e.crc
                     : static_cast<std::uint32_t>(
                           crc32(0L, reinterpret_cast<const Bytef*>(e.data.data()), static_cast<uInt>(e.data.size())));
    };
    for (const auto& e : entries) {
        offsets.push_back(static_cast<std::uint32_t>(w.size()));
        w.le<std::uint32_t>(0x04034b50);
        w.le<std::uint16_t>(20);
        w.le<std::uint16_t>(e.flags);
        w.le<std::uint16_t>(e.method);
        w.le<std::uint32_t>(0);
        w.le<std::uint32_t>(crc_of(e));
        w.le<std::uint32_t>(static_cast<std::uint32_t>(e.data.size()));
        w.le<std::uint32_t>(e.size.value_or(static_cast<std::uint32_t>(e.data.size())));
        w.le<std::uint16_t>(static_cast<std::uint16_t>(e.name.size()));
        w.le<std::uint16_t>(0);
        w.fixed_string(e.name, e.name.size());
        w.fixed_string(e.data, e.data.size());
    }
    const auto dir = static_cast<std::uint32_t>(w.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        w.le<std::uint32_t>(0x02014b50);
        w.le<std::uint16_t>(e.made_by);
        w.le<std::uint16_t>(20);
        w.le<std::uint16_t>(e.flags);
        w.le<std::uint16_t>(e.method);
        w.le<std::uint32_t>(0);
        w.le<std::uint32_t>(crc_of(e));
        w.le<std::uint32_t>(static_cast<std::uint32_t>(e.data.size()));
        w.le<std::uint32_t>(e.size.value_or(static_cast<std::uint32_t>(e.data.size())));
        w.le<std::uint16_t>(static_cast<std::uint16_t>(e.name.size()));
        w.le<std::uint16_t>(0);
        w.le<std::uint16_t>(0);
        w.le<std::uint16_t>(0);
        w.le<std::uint16_t>(0);
        w.le<std::uint32_t>(e.external);
        w.le<std::uint32_t>(offsets[i]);
        w.fixed_string(e.name, e.name.size());
    }
    const auto dir_size = static_cast<std::uint32_t>(w.size() - dir);
    w.le<std::uint32_t>(0x06054b50);
    w.le<std::uint16_t>(0);
    w.le<std::uint16_t>(0);
    w.le<std::uint16_t>(static_cast<std::uint16_t>(entries.size()));
    w.le<std::uint16_t>(static_cast<std::uint16_t>(entries.size()));
    w.le<std::uint32_t>(dir_size);
    w.le<std::uint32_t>(dir);
    w.le<std::uint16_t>(0);
    return w.take();
}

std::vector<std::byte> writer_zip() {
    ZipWriter w;
    const std::tm t = some_time();
    std::string big(100000, 'a'); // deflates well
    for (std::size_t i = 0; i < big.size(); i += 7) big[i] = static_cast<char>('a' + i % 26);
    REQUIRE(w.add("hailstone.txt", std::string_view("Hailstone"), t));
    REQUIRE(w.add("ui/modern/lib/big.lua", big, t));
    REQUIRE(w.add("shaders/empty.spv", std::string_view(), t));
    return w.finish();
}

} // namespace

TEST_CASE("zip_read: safe paths") {
    CHECK(is_safe_zip_path("hailstone"));
    CHECK(is_safe_zip_path("ui/modern/login.lua"));
    CHECK(is_safe_zip_path("browser/"));
    CHECK(is_safe_zip_path("a.b/..c/d..e")); // dots inside a name are fine
    CHECK_FALSE(is_safe_zip_path(""));
    CHECK_FALSE(is_safe_zip_path("/etc/passwd"));
    CHECK_FALSE(is_safe_zip_path("../hailstone"));
    CHECK_FALSE(is_safe_zip_path("ui/../../x"));
    CHECK_FALSE(is_safe_zip_path("./x"));
    CHECK_FALSE(is_safe_zip_path("a//b"));
    CHECK_FALSE(is_safe_zip_path("a\\b"));
    CHECK_FALSE(is_safe_zip_path("C:/x"));
    CHECK_FALSE(is_safe_zip_path("c:x"));
    CHECK_FALSE(is_safe_zip_path("a/b//"));
    CHECK_FALSE(is_safe_zip_path("/"));
    CHECK_FALSE(is_safe_zip_path(std::string_view("a\0b", 3)));
    CHECK_FALSE(is_safe_zip_path("a\nb"));
    // Names Windows aliases are refused on every platform: a trailing dot or space is dropped there, and a device
    // name opens the device, in any case and with any extension.
    for (const char* bad : {"evil.lua.", "evil.lua ", "dir./x", "dir /x", "d..", "CON", "con.txt", "ui/Nul.dds",
                            "prn", "AUX.wav", "com1", "COM9.txt", "lpt1/x", "Lpt9.x", "con .txt", "conin$",
                            "CONOUT$.log", "com\xc2\xb9", "LPT\xc2\xb3.txt"})
        CHECK_MESSAGE(!is_safe_zip_path(bad), bad);
    for (const char* ok : {"console.txt", "com0.txt", "com10", "lpt.txt", "nul_x.txt", "a/aux2/x.txt", "xcon.txt",
                           "com\xc2\xb4", ".hidden"})
        CHECK_MESSAGE(is_safe_zip_path(ok), ok);
}

TEST_CASE("zip_read: ZipWriter's archives read back, stored and deflated") {
    ZipReader zip;
    REQUIRE(zip.open(writer_zip()));
    REQUIRE(zip.entries().size() == 3);
    const ZipEntry* small = zip.find("hailstone.txt");
    const ZipEntry* big = zip.find("ui/modern/lib/big.lua");
    const ZipEntry* empty = zip.find("shaders/empty.spv");
    REQUIRE(small);
    REQUIRE(big);
    REQUIRE(empty);
    CHECK(small->method == 0);
    CHECK(big->method == 8);
    CHECK(big->packed < big->size);
    CHECK(extract_text(zip, *small) == "Hailstone");
    const std::string text = extract_text(zip, *big);
    CHECK(text.size() == 100000);
    CHECK(text[7] == static_cast<char>('a' + 7 % 26));
    bool ok = false;
    CHECK(extract_text(zip, *empty, &ok).empty());
    CHECK(ok);
    CHECK(zip.find("nothing") == nullptr);
}

TEST_CASE("zip_read: from a file, into files") {
    test::ScratchDir dir("zip_read");
    const auto archive = dir.path() / "pkg.zip";
    {
        const auto bytes = writer_zip();
        std::ofstream out(archive, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    ZipReader zip;
    REQUIRE(zip.open(archive));
    const auto out = dir.path() / "out";
    for (const auto& e : zip.entries()) REQUIRE(zip.extract_to_file(e, out / e.name));
    CHECK(std::filesystem::file_size(out / "ui/modern/lib/big.lua") == 100000);
    CHECK(std::filesystem::file_size(out / "shaders/empty.spv") == 0);
    std::ifstream in(out / "hailstone.txt");
    std::string line;
    std::getline(in, line);
    CHECK(line == "Hailstone");
    // No temporary file is left beside them.
    std::size_t files = 0;
    for (const auto& f : std::filesystem::recursive_directory_iterator(out))
        if (f.is_regular_file()) ++files;
    CHECK(files == 3);
    CHECK_FALSE(zip.open(dir.path() / "missing.zip"));
    CHECK_FALSE(zip.error().empty());
}

TEST_CASE("zip_read: Unix permission bits") {
    RawEntry exe{"hailstone", "#!", 3 << 8 | 20, 0100755u << 16};
    RawEntry data{"shaders/a.spv", "x", 3 << 8 | 20, 0100644u << 16};
    RawEntry dos{"readme.txt", "x"};
    RawEntry folder{"browser/", "", 3 << 8 | 20, 040755u << 16};
    ZipReader zip;
    REQUIRE(zip.open(build_zip({exe, data, dos, folder})));
    CHECK(zip.find("hailstone")->unix_mode == 0100755u);
    CHECK(zip.find("shaders/a.spv")->unix_mode == 0100644u);
    CHECK(zip.find("readme.txt")->unix_mode == 0);
    CHECK(zip.find("browser/")->is_directory());

    test::ScratchDir dir("zip_read_mode");
    for (const auto& e : zip.entries()) REQUIRE(zip.extract_to_file(e, dir.path() / e.name));
    CHECK(std::filesystem::is_directory(dir.path() / "browser"));
#ifndef _WIN32
    using std::filesystem::perms;
    const auto p = std::filesystem::status(dir.path() / "hailstone").permissions();
    CHECK((p & perms::owner_exec) != perms::none);
    CHECK((p & perms::others_exec) != perms::none);
    const auto q = std::filesystem::status(dir.path() / "shaders/a.spv").permissions();
    CHECK((q & perms::owner_exec) == perms::none);
    CHECK((q & perms::owner_write) != perms::none);
#endif
}

TEST_CASE("zip_read: unsafe names refuse the whole archive") {
    for (const char* bad : {"../evil", "/abs", "a\\b", "C:/x", "a//b", "./x", "ok/../../x", "x.lua.", "nul"}) {
        ZipReader zip;
        CHECK_FALSE(zip.open(build_zip({{"fine.txt", "x"}, {bad, "x"}})));
        CHECK(zip.error().find("unsafe") != std::string::npos);
        CHECK(zip.entries().empty());
    }
}

TEST_CASE("zip_read: a file named twice is refused, in any case") {
    ZipReader zip;
    CHECK_FALSE(zip.open(build_zip({{"a/b.txt", "x"}, {"a/b.txt", "y"}})));
    CHECK_FALSE(zip.open(build_zip({{"Hailstone", "x"}, {"hailstone", "y"}})));
    CHECK_FALSE(zip.open(build_zip({{"ui", "x"}, {"UI/", ""}})));
    CHECK(zip.open(build_zip({{"ui/", ""}, {"ui/a", "y"}})));
}

TEST_CASE("zip_read: windows_file_key folds case and each part's trailing dots and spaces") {
    CHECK(windows_file_key("UI/Modern/Login.LUA") == "ui/modern/login.lua");
    CHECK(windows_file_key("a. /b.txt. ") == "a/b.txt");
    CHECK(windows_file_key("a/b") != windows_file_key("a/c"));
    CHECK(windows_file_key("..c") == "..c");
}

TEST_CASE("zip_read: wrong CRC, wrong sizes, unsupported entries") {
    {
        ZipReader zip;
        REQUIRE(zip.open(build_zip({{"a", "hello", 20, 0, 0, 0, 0x12345678u}})));
        bool ok = true;
        extract_text(zip, zip.entries()[0], &ok);
        CHECK_FALSE(ok);
        CHECK(zip.error().find("CRC") != std::string::npos);
    }
    {
        // A stored entry saying it is larger than its data.
        ZipReader zip;
        REQUIRE(zip.open(build_zip({{"a", "hello", 20, 0, 0, 0, std::nullopt, 6}})));
        bool ok = true;
        extract_text(zip, zip.entries()[0], &ok);
        CHECK_FALSE(ok);
    }
    {
        // Deflated data that inflates past the size the entry declares stops there (the zip bomb guard).
        const std::string plain(5000, 'z');
        std::vector<std::byte> packed(compressBound(static_cast<uLong>(plain.size())));
        z_stream z{};
        REQUIRE(deflateInit2(&z, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK);
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(plain.data()));
        z.avail_in = static_cast<uInt>(plain.size());
        z.next_out = reinterpret_cast<Bytef*>(packed.data());
        z.avail_out = static_cast<uInt>(packed.size());
        REQUIRE(deflate(&z, Z_FINISH) == Z_STREAM_END);
        packed.resize(z.total_out);
        deflateEnd(&z);
        RawEntry e{"bomb", std::string(reinterpret_cast<const char*>(packed.data()), packed.size())};
        e.method = 8;
        e.size = 100;
        e.crc = static_cast<std::uint32_t>(
            crc32(0L, reinterpret_cast<const Bytef*>(plain.data()), static_cast<uInt>(plain.size())));
        ZipReader zip;
        REQUIRE(zip.open(build_zip({e})));
        std::size_t got = 0;
        CHECK_FALSE(zip.extract(zip.entries()[0], [&](std::span<const std::byte> piece) {
            got += piece.size();
            return true;
        }));
        CHECK(got <= 100);
        CHECK(zip.error().find("larger") != std::string::npos);
    }
    {
        RawEntry e{"a", "x"};
        e.method = 14; // LZMA
        ZipReader zip;
        REQUIRE(zip.open(build_zip({e})));
        bool ok = true;
        extract_text(zip, zip.entries()[0], &ok);
        CHECK_FALSE(ok);
        CHECK(zip.error().find("method") != std::string::npos);
    }
    {
        RawEntry e{"a", "x"};
        e.flags = 1; // encrypted
        ZipReader zip;
        CHECK_FALSE(zip.open(build_zip({e})));
    }
    {
        // A sink that refuses stops the extraction.
        ZipReader zip;
        REQUIRE(zip.open(build_zip({{"a", "hello"}})));
        CHECK_FALSE(zip.extract(zip.entries()[0], [](std::span<const std::byte>) { return false; }));
    }
}

// Without zip64 the end record counts at most 0xfffe entries (0xffff means a zip64 record follows): the next file is
// refused, not written into an archive that would read wrongly.
TEST_CASE("zip_write: no more files than the end record can count, and dates past 2107 clamped") {
    std::tm when{};
    when.tm_year = 80;
    when.tm_mday = 1;
    ZipWriter z;
    for (int i = 0; i < 0xfffe; ++i) REQUIRE(z.add("f" + std::to_string(i), std::string_view(), when));
    CHECK_FALSE(z.add("one-too-many", std::string_view(), when));
    ZipReader zip;
    REQUIRE(zip.open(std::vector<std::byte>(z.finish())));
    CHECK(zip.entries().size() == 0xfffe);
    CHECK(zip.find("one-too-many") == nullptr);

    // 2200-03-04: the year field is 7 bits from 1980, so it becomes 2107 and the month and day stay.
    std::tm late{};
    late.tm_year = 300;
    late.tm_mon = 2;
    late.tm_mday = 4;
    ZipWriter d;
    REQUIRE(d.add("x", std::string_view("x"), late));
    const auto& bytes = d.finish();
    const unsigned date = std::to_integer<unsigned>(bytes[12]) | std::to_integer<unsigned>(bytes[13]) << 8;
    CHECK((date >> 9) == 127);
    CHECK(((date >> 5) & 15) == 3);
    CHECK((date & 31) == 4);
}

TEST_CASE("zip_read: truncated and non-zip input is refused cleanly") {
    const auto good = writer_zip();
    for (std::size_t n = 0; n < good.size(); n += 97) {
        ZipReader zip;
        CHECK_FALSE(zip.open(std::vector<std::byte>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n))));
        CHECK_FALSE(zip.error().empty());
    }
    ZipReader zip;
    CHECK_FALSE(zip.open(as_bytes("this is not a zip archive, though it is long enough to have an end")));
    CHECK_FALSE(zip.open(std::vector<std::byte>{}));
    // An archive with a comment after its end record still opens.
    auto commented = good;
    commented[commented.size() - 2] = std::byte{5};
    for (char c : std::string("hello")) commented.push_back(static_cast<std::byte>(c));
    CHECK(zip.open(commented));
}

TEST_CASE("zip_read: fuzz") {
    fuzz::QuietLog quiet;
    const auto seed_writer = writer_zip();
    const auto seed_raw = build_zip({{"hailstone", "#!/bin/sh", 3 << 8 | 20, 0100755u << 16}, {"a/", ""}, {"a/b", "bb"}});
    auto run = [](const fuzz::Bytes& input) {
        return fuzz::run_case("zip_read", input, fuzz::Throws::Never, [](std::span<const std::byte> data) {
            ZipReader zip;
            if (!zip.open(std::vector<std::byte>(data.begin(), data.end()))) return;
            for (const auto& e : zip.entries()) {
                CHECK(is_safe_zip_path(e.name));
                (void)zip.extract(e, [](std::span<const std::byte>) { return true; });
            }
        });
    };
    for (const auto* seed : {&seed_writer, &seed_raw}) {
        for (std::size_t n = 0; n <= seed->size(); n += 3)
            run(fuzz::Bytes(seed->begin(), seed->begin() + static_cast<std::ptrdiff_t>(n)));
        fuzz::Rng rng(0x5a17 + seed->size());
        for (int i = 0; i < 3000; ++i) run(fuzz::mutate(*seed, rng));
    }
}

// The error names the file in UTF-8 whatever the system's code page (path::string() throws on Windows for a name the
// code page lacks, without the UTF-8 manifest): core/path_utf8.h.
TEST_CASE("zip: a file that cannot be opened is named in UTF-8") {
    const std::filesystem::path missing = std::filesystem::path(u8"no-such-folder-é世") / u8"packü.zip";
    hailstone::ZipReader zip;
    CHECK_FALSE(zip.open(missing));
    CHECK(zip.error().find("pack\xc3\xbc.zip") != std::string::npos);
    CHECK(zip.error().find("no-such-folder-\xc3\xa9\xe4\xb8\x96") != std::string::npos);
}

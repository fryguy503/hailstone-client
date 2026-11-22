// The UI font's faces (ui/font_faces.h): the 'name' table's family and style, and a family's regular and bold files.
#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/path_utf8.h"
#include "scratch_dir.h"
#include "ui/font_faces.h"
#include "ui/imgui_layer.h"

using namespace hailstone::ui;

namespace {

void be16(std::vector<unsigned char>& d, std::uint16_t v) {
    d.push_back(static_cast<unsigned char>(v >> 8));
    d.push_back(static_cast<unsigned char>(v));
}
void be32(std::vector<unsigned char>& d, std::uint32_t v) {
    be16(d, static_cast<std::uint16_t>(v >> 16));
    be16(d, static_cast<std::uint16_t>(v));
}

// A TrueType file holding only a name table with the Windows platform's family (1) and subfamily (2),
// given as UTF-16BE bytes.
std::vector<unsigned char> font_file_utf16(const std::vector<unsigned char>& f, const std::vector<unsigned char>& st) {
    std::vector<unsigned char> name;
    be16(name, 0);
    be16(name, 2);
    be16(name, 6 + 2 * 12);
    for (int i = 0; i < 2; ++i) {
        be16(name, 3);
        be16(name, 1);
        be16(name, 0x409);
        be16(name, static_cast<std::uint16_t>(i + 1));
        be16(name, static_cast<std::uint16_t>(i == 0 ? f.size() : st.size()));
        be16(name, static_cast<std::uint16_t>(i == 0 ? 0 : f.size()));
    }
    name.insert(name.end(), f.begin(), f.end());
    name.insert(name.end(), st.begin(), st.end());
    std::vector<unsigned char> d;
    be32(d, 0x00010000);
    be16(d, 1);
    be16(d, 16);
    be16(d, 0);
    be16(d, 0);
    be32(d, 0x6e616d65); // 'name'
    be32(d, 0);
    be32(d, 12 + 16);
    be32(d, static_cast<std::uint32_t>(name.size()));
    d.insert(d.end(), name.begin(), name.end());
    return d;
}

std::vector<unsigned char> utf16(const std::string& s) {
    std::vector<unsigned char> out;
    for (char c : s) {
        out.push_back(0);
        out.push_back(static_cast<unsigned char>(c));
    }
    return out;
}

std::vector<unsigned char> font_file(const std::string& family, const std::string& style) {
    return font_file_utf16(utf16(family), utf16(style));
}

} // namespace

TEST_CASE("UI fonts: built-in chat glyphs draw curly quotes with straight quote shapes") {
    const ImFontConfig cfg = detail::ui_font_config(1.0f);
    ImFontGlyphRangesBuilder ranges;
    ranges.AddRanges(cfg.GlyphRanges);
    CHECK(ranges.GetBit(0x00e9)); // Latin-1 remains available
    CHECK(ranges.GetBit(0x2019)); // the selected face is asked for a real curly apostrophe

    ImFontAtlas atlas;
    ImFont* font = atlas.AddFontDefault(&cfg);
    REQUIRE(font);
    REQUIRE(detail::build_ui_font_atlas(atlas));
    const ImFontGlyph* apostrophe = font->FindGlyphNoFallback('\'');
    const ImFontGlyph* quote = font->FindGlyphNoFallback('"');
    REQUIRE(apostrophe);
    REQUIRE(quote);
    CHECK(font->FindGlyphNoFallback(0x2018) == apostrophe);
    CHECK(font->FindGlyphNoFallback(0x2019) == apostrophe);
    CHECK(font->FindGlyphNoFallback(0x201c) == quote);
    CHECK(font->FindGlyphNoFallback(0x201d) == quote);
    CHECK(font->GetCharAdvance(0x2019) == apostrophe->AdvanceX);
    CHECK(font->GetCharAdvance(0x1000) == font->FallbackAdvanceX);
}

TEST_CASE("UI fonts: installed regular and bold faces retain their typographic chat glyphs") {
    const auto faces = scan_font_faces(system_font_dirs());
    FontFiles files;
    for (const char* family : {"Arial", "Liberation Sans", "DejaVu Sans", "Noto Sans"}) {
        files = find_font_family(faces, family);
        if (!files.regular.empty() && !files.bold.empty()) break;
    }
    if (files.regular.empty() || files.bold.empty()) return; // headless hosts can run with only the built-in font

    const ImFontConfig cfg = detail::ui_font_config(1.0f);
    ImFontAtlas atlas;
    ImFont* regular = atlas.AddFontFromFileTTF(hailstone::path_utf8(files.regular).c_str(), 14.0f, &cfg);
    ImFont* bold = atlas.AddFontFromFileTTF(hailstone::path_utf8(files.bold).c_str(), 20.0f, &cfg);
    REQUIRE(regular);
    REQUIRE(bold);
    REQUIRE(detail::build_ui_font_atlas(atlas));
    constexpr ImWchar codepoints[] = {0x00e9, 0x2014, 0x2019, 0x201d, 0x2026};
    for (ImFont* font : {regular, bold}) {
        for (ImWchar codepoint : codepoints) {
            const ImFontGlyph* glyph = font->FindGlyphNoFallback(codepoint);
            REQUIRE(glyph);
            CHECK(glyph->Codepoint == codepoint);
            CHECK(glyph->Visible);
        }
    }
}

TEST_CASE("font faces: the name table's family and style") {
    auto face = parse_font_face(font_file("Test Sans", "Bold Italic"));
    REQUIRE(face);
    CHECK(face->family == "Test Sans");
    CHECK(face->style == "Bold Italic");
    CHECK(face->bold);
    CHECK(face->italic);
    face = parse_font_face(font_file("Test Sans", "SemiBold"));
    REQUIRE(face);
    CHECK_FALSE(face->bold);
    std::vector<unsigned char> junk(64, 7);
    CHECK_FALSE(parse_font_face(junk));
}

TEST_CASE("font faces: UTF-16 surrogate pairs, and U+FFFD for an unpaired half") {
    // A (U+1D400 as D835 DC00) B; a high surrogate before 'C'; a lone low surrogate; a high one at the end.
    std::vector<unsigned char> f = {0, 'A', 0xd8, 0x35, 0xdc, 0x00, 0, 'B', 0xd8, 0x00, 0, 'C', 0xdc, 0x01, 0xd8, 0x01};
    auto face = parse_font_face(font_file_utf16(f, utf16("Regular")));
    REQUIRE(face);
    CHECK(face->family == "A\xf0\x9d\x90\x80" "B\xef\xbf\xbd" "C\xef\xbf\xbd\xef\xbf\xbd");
}

TEST_CASE("font faces: a family's regular and bold files, by name in any case") {
    std::vector<FontFace> faces;
    auto add = [&](std::string family, std::string style, std::string file) {
        FontFace f = *parse_font_face(font_file(family, style));
        f.file = file;
        faces.push_back(f);
    };
    add("Test Sans", "Bold", "bold.ttf");
    add("Test Sans", "Light", "light.ttf");
    add("Test Sans", "Regular", "regular.ttf");
    add("Test Sans", "Italic", "italic.ttf");
    add("Other", "Book", "other.ttf");
    FontFiles files = find_font_family(faces, "test sans");
    CHECK(files.regular == "regular.ttf");
    CHECK(files.bold == "bold.ttf");
    CHECK(find_font_family(faces, "Other").regular == "other.ttf");
    CHECK(find_font_family(faces, "Missing").regular.empty());
    auto families = font_families(faces);
    REQUIRE(families.size() == 2);
    CHECK(families[0] == "Other");
}

TEST_CASE("font faces: a file whose name is not ASCII is found") {
    // On Windows path::string() is in the ANSI code page and throws for a name it cannot show; the scan must not.
    hailstone::test::ScratchDir dir("font_faces");
    const std::filesystem::path file = dir.path() / std::filesystem::path(u8"Schrift-\u00e4\u65e5\u672c.ttf");
    {
        auto bytes = font_file("Test Sans", "Regular");
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::vector<std::filesystem::path> dirs{dir.path()};
    auto faces = scan_font_faces(dirs);
    REQUIRE(faces.size() == 1);
    CHECK(faces[0].family == "Test Sans");
    CHECK(faces[0].file == file);
}

// read_font_face reads the table directory and the name table alone: a name table far into the file is found, and an
// offset past the end of the file (up to 4 GB, or one whose sum with the length wraps) is no face. (These pass on the
// old code too, where the 4 GB image usually allocates and the read then falls short: the sparse-file case below is
// the one that shows the fix.)
TEST_CASE("font faces: the name table at any offset, and a broken offset is no face") {
    hailstone::test::ScratchDir dir("font_faces_offset");
    auto write = [&](const std::string& name, const std::vector<unsigned char>& d) {
        const auto path = dir.path() / name;
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
        return path;
    };
    auto set32 = [](std::vector<unsigned char>& d, std::size_t at, std::uint32_t v) {
        for (std::size_t k = 0; k < 4; ++k) d[at + k] = static_cast<unsigned char>(v >> (24 - 8 * k));
    };
    const auto plain = font_file("Far Sans", "Regular");
    // The name table moved 100000 bytes in (past the 4 KB the directory is read from).
    std::vector<unsigned char> far(plain.begin(), plain.begin() + 28);
    far.resize(100000, 0);
    far.insert(far.end(), plain.begin() + 28, plain.end());
    set32(far, 12 + 8, 100000);
    auto face = read_font_face(write("far.ttf", far));
    REQUIRE(face);
    CHECK(face->family == "Far Sans");

    std::vector<unsigned char> broken = plain;
    set32(broken, 12 + 8, 0xfff00000u);
    CHECK_FALSE(read_font_face(write("huge.ttf", broken)));
    set32(broken, 12 + 8, 0xfffffff0u); // + its length wraps in 32 bits
    CHECK_FALSE(read_font_face(write("wrap.ttf", broken)));
}

#ifndef _WIN32
// A name table that really is near 4 GB in (a sparse file: the gap takes no disk here, where NTFS would write it), its
// offset's sum with its length wrapping in 32 bits. Read into an image of its own size, it is found; an image sized
// from that sum was a few bytes long and the table was read 4 GB past it (a crash even without the sanitizer).
TEST_CASE("font faces: a name table near 4 GB in, its end past 32 bits, is read into a buffer of its size") {
    hailstone::test::ScratchDir dir("font_faces_sparse");
    std::vector<unsigned char> plain = font_file("Edge Sans", "Regular");
    const std::uint32_t offset = 0xfffffff0u;
    REQUIRE(plain.size() - 28 > 0x10); // so offset + length passes 2^32
    for (std::size_t k = 0; k < 4; ++k) plain[12 + 8 + k] = static_cast<unsigned char>(offset >> (24 - 8 * k));
    const auto path = dir.path() / "edge.ttf";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(plain.data()), 28);
        out.seekp(static_cast<std::streamoff>(offset));
        out.write(reinterpret_cast<const char*>(plain.data() + 28), static_cast<std::streamsize>(plain.size() - 28));
        REQUIRE(out);
    }
    auto face = read_font_face(path);
    REQUIRE(face);
    CHECK(face->family == "Edge Sans");
}
#endif

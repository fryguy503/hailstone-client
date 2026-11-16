#include <doctest/doctest.h>

#include <fstream>
#include <string>

#include "core/binary_measurement.h"
#include "scratch_dir.h"

using namespace hailstone;

TEST_CASE("binary measurement: whole file digests preserve bytes and match FNV-1a63 and SHA256 examples") {
    test::ScratchDir dir("binary_measurement");
    const auto path = dir.path() / "binary.dat";
    { std::ofstream file(path, std::ios::binary); file << "abc"; }
    const auto abc = measure_binary_file(path);
    REQUIRE(abc);
    CHECK(abc->byte_count == 3);
    CHECK(abc->fnv1a63 == 0x671fa2190541574bull);
    CHECK(to_hex(abc->sha256) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string content(131079, 'x');
    content[65535] = '\0'; content[65536] = static_cast<char>(0xff); content.back() = '\0';
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file.write(content.data(), static_cast<std::streamsize>(content.size())); }
    const auto streamed = measure_binary_file(path);
    REQUIRE(streamed);
    CHECK(streamed->byte_count == content.size());
    CHECK(streamed->sha256 == sha256(content));
    CHECK(streamed->fnv1a63 <= 0x7fffffffffffffffull);
    CHECK_FALSE(measure_binary_file(path, content.size() - 1));
}

TEST_CASE("binary measurement: empty files are measured and invalid file sources fail") {
    test::ScratchDir dir("binary_measurement_empty");
    const auto path = dir.path() / "empty.dat";
    { std::ofstream file(path, std::ios::binary); }
    const auto empty = measure_binary_file(path);
    REQUIRE(empty);
    CHECK(empty->byte_count == 0);
    CHECK(empty->fnv1a63 == 0x4bf29ce484222325ull);
    CHECK(empty->sha256 == sha256(std::string_view{}));
    CHECK_FALSE(measure_binary_file(dir.path()));
    CHECK_FALSE(measure_binary_file(dir.path() / "missing"));
}

TEST_CASE("binary measurement: the platform resolves the running executable independently of game assets") {
    const auto executable = measure_process_executable();
    REQUIRE(executable);
    CHECK(executable->byte_count > 0);
    CHECK(executable->fnv1a63 <= 0x7fffffffffffffffull);
    CHECK(executable->sha256 != Sha256Digest{});
}

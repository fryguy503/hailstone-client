#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "platform/process.h"
#include "platform/shared_file.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

namespace {
std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

std::string mapped_text(const platform::ReadOnlyMapping& mapping) {
    const auto data = mapping.bytes();
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

fs::path private_cache(const test::ScratchDir& dir) {
    // macOS's system temp path may itself start with the /var -> /private/var alias.
    return fs::canonical(dir.path()) / "cache";
}

std::size_t temporary_count(const fs::path& directory) {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().filename().string().starts_with(".tmp-")) ++count;
    return count;
}

void write_file(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(static_cast<bool>(out));
}
} // namespace

TEST_CASE("shared_file: a published mapping survives moves and independent opens") {
    test::ScratchDir scratch("shared_mapping");
    const fs::path cache = private_cache(scratch);
    REQUIRE(platform::prepare_private_cache_directory(cache));
    const fs::path blob = cache / "asset.blob";
    auto temporary = platform::CacheTempFile::create(cache);
    REQUIRE(temporary);
    REQUIRE(temporary->write(bytes("asset bytes")));
    REQUIRE(temporary->publish(blob) == platform::PublishResult::Published);
    CHECK(temporary_count(cache) == 0);
    CHECK_FALSE(temporary->write(bytes("late write")));

    platform::ReadOnlyMapping owner;
    const std::byte* original = nullptr;
    {
        auto initial = platform::ReadOnlyMapping::open(blob);
        REQUIRE(initial);
        original = initial->bytes().data();
        owner = std::move(*initial);
        CHECK(initial->bytes().empty());
    }
    CHECK(owner.bytes().data() == original);
    CHECK(mapped_text(owner) == "asset bytes");
    auto other = platform::ReadOnlyMapping::open(blob);
    REQUIRE(other);
    CHECK(mapped_text(*other) == "asset bytes");
    platform::ReadOnlyMapping moved(std::move(owner));
    CHECK(owner.bytes().empty());
    CHECK(moved.bytes().data() == original);
    CHECK(mapped_text(moved) == "asset bytes");
}

TEST_CASE("shared_file: publication never replaces a content key and abandoned files are removed") {
    test::ScratchDir scratch("shared_publish");
    const fs::path cache = private_cache(scratch);
    REQUIRE(platform::prepare_private_cache_directory(cache));
    const fs::path blob = cache / "same-key.blob";
    {
        auto first = platform::CacheTempFile::create(cache);
        REQUIRE(first);
        REQUIRE(first->write(bytes("original")));
        REQUIRE(first->publish(blob) == platform::PublishResult::Published);
    }
    auto existing = platform::ReadOnlyMapping::open(blob);
    REQUIRE(existing);
    {
        auto competitor = platform::CacheTempFile::create(cache);
        REQUIRE(competitor);
        REQUIRE(competitor->write(bytes("replacement")));
        CHECK(competitor->publish(blob) == platform::PublishResult::Exists);
        CHECK(mapped_text(*existing) == "original");
        auto winner = platform::ReadOnlyMapping::open(blob);
        REQUIRE(winner);
        CHECK(mapped_text(*winner) == "original");
    }
    CHECK(temporary_count(cache) == 0);
    {
        auto abandoned = platform::CacheTempFile::create(cache);
        REQUIRE(abandoned);
        REQUIRE(abandoned->write(bytes("partial")));
        CHECK(temporary_count(cache) == 1);
        platform::CacheTempFile moved(std::move(*abandoned));
        CHECK_FALSE(abandoned->write(bytes("moved")));
        CHECK(temporary_count(cache) == 1);
    }
    CHECK(temporary_count(cache) == 0);
    {
        auto wrong_folder = platform::CacheTempFile::create(cache);
        REQUIRE(wrong_folder);
        REQUIRE(wrong_folder->write(bytes("outside")));
        CHECK(wrong_folder->publish(fs::canonical(scratch.path()) / "outside.blob") == platform::PublishResult::Error);
    }
    CHECK(temporary_count(cache) == 0);
    CHECK_FALSE(fs::exists(scratch.path() / "outside.blob"));
}

TEST_CASE("shared_file: shared and exclusive locks are scoped and stable") {
    test::ScratchDir scratch("shared_lock");
    const fs::path cache = private_cache(scratch);
    REQUIRE(platform::prepare_private_cache_directory(cache));
    const fs::path lock = cache / "use.lock";
    {
        auto first = platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Shared);
        auto second = platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Shared);
        REQUIRE(first);
        REQUIRE(second);
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive));
        first.reset();
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive));
        platform::ScopedFileLock moved(std::move(*second));
        second.reset();
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive));
    }
    REQUIRE(fs::is_regular_file(lock));
    {
        auto exclusive = platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive);
        REQUIRE(exclusive);
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Shared));
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive));
        platform::ScopedFileLock moved;
        moved = std::move(*exclusive);
        exclusive.reset();
        CHECK_FALSE(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Shared));
    }
    CHECK(platform::ScopedFileLock::try_acquire(lock, platform::FileLockMode::Exclusive).has_value());
    CHECK(fs::is_regular_file(lock));
}

TEST_CASE("shared_file: private directories reject existing public leaves and unexpected types") {
    test::ScratchDir scratch("shared_private");
    const fs::path base = fs::canonical(scratch.path());
    const fs::path nested = base / "new" / "private";
    REQUIRE(platform::prepare_private_cache_directory(nested));
    CHECK(platform::prepare_private_cache_directory(nested));
    CHECK_FALSE(platform::ReadOnlyMapping::open(nested));
    CHECK_FALSE(platform::ScopedFileLock::try_acquire(nested, platform::FileLockMode::Shared));
    const fs::path file = nested / "file";
    write_file(file, "x");
    CHECK_FALSE(platform::prepare_private_cache_directory(file));
    CHECK_FALSE(platform::prepare_private_cache_directory(file / "child"));
    CHECK_FALSE(platform::CacheTempFile::create(file));
    write_file(nested / "empty", "");
    CHECK_FALSE(platform::ReadOnlyMapping::open(nested / "empty"));
    CHECK_FALSE(platform::ReadOnlyMapping::open(nested / "missing"));
#if !defined(_WIN32)
    const fs::path public_leaf = base / "public";
    fs::create_directory(public_leaf);
    fs::permissions(public_leaf, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec,
                    fs::perm_options::replace);
    const auto permissions = fs::status(public_leaf).permissions();
    CHECK_FALSE(platform::prepare_private_cache_directory(public_leaf));
    CHECK(fs::status(public_leaf).permissions() == permissions);
    CHECK_FALSE(platform::CacheTempFile::create(public_leaf));
    CHECK_FALSE(platform::ScopedFileLock::try_acquire(public_leaf / "lock", platform::FileLockMode::Shared));
    CHECK((fs::status(nested).permissions() & fs::perms::all) == fs::perms::owner_all);
#endif
}

TEST_CASE("shared_file: symlinks cannot redirect mappings locks directories or publication") {
    test::ScratchDir scratch("shared_links");
    const fs::path cache = private_cache(scratch);
    REQUIRE(platform::prepare_private_cache_directory(cache));
    const fs::path original = cache / "original";
    const fs::path link = cache / "link";
    write_file(original, "untouched");
    std::error_code ec;
    fs::create_symlink(original, link, ec);
    if (ec) {
        MESSAGE("This environment cannot create a symlink; link-specific checks did not run: " << ec.message());
        return;
    }
    CHECK_FALSE(platform::ReadOnlyMapping::open(link));
    CHECK_FALSE(platform::ScopedFileLock::try_acquire(link, platform::FileLockMode::Exclusive));
    auto temporary = platform::CacheTempFile::create(cache);
    REQUIRE(temporary);
    REQUIRE(temporary->write(bytes("replacement")));
    CHECK(temporary->publish(link) == platform::PublishResult::Exists);
    CHECK(fs::is_symlink(link));
    auto untouched = platform::ReadOnlyMapping::open(original);
    REQUIRE(untouched);
    CHECK(mapped_text(*untouched) == "untouched");

    const fs::path alias = fs::canonical(scratch.path()) / "alias";
    fs::create_directory_symlink(cache, alias, ec);
    REQUIRE_FALSE(ec);
    CHECK_FALSE(platform::prepare_private_cache_directory(alias));
    CHECK_FALSE(platform::prepare_private_cache_directory(alias / "child"));
    CHECK_FALSE(platform::ReadOnlyMapping::open(alias / "original"));
    CHECK_FALSE(platform::CacheTempFile::create(alias));
}

TEST_CASE("shared_file: stamps describe the opened source and detect size time and identity changes") {
    test::ScratchDir scratch("shared_stamp");
    const fs::path original = scratch.path() / "original";
    const fs::path other = scratch.path() / "other";
    write_file(original, "first");
    write_file(other, "first");
    std::FILE* opened = platform::open_c_file(original, "rb");
    REQUIRE(opened);
    const auto initial = platform::file_stamp(opened);
    const auto repeated = platform::file_stamp(opened);
    std::fclose(opened);
    REQUIRE(initial);
    CHECK(initial == repeated);
    CHECK(initial->size == 5);
    std::FILE* separate = platform::open_c_file(other, "rb");
    REQUIRE(separate);
    const auto different = platform::file_stamp(separate);
    std::fclose(separate);
    REQUIRE(different);
    CHECK(different->size == initial->size);
    CHECK(different != initial);
    const auto time = fs::last_write_time(original);
    fs::last_write_time(original, time + std::chrono::seconds(2));
    opened = platform::open_c_file(original, "rb");
    REQUIRE(opened);
    const auto retimed = platform::file_stamp(opened);
    std::fclose(opened);
    REQUIRE(retimed);
    CHECK(retimed->size == initial->size);
    CHECK(retimed != initial);
    write_file(original, "longer contents");
    opened = platform::open_c_file(original, "rb");
    REQUIRE(opened);
    const auto resized = platform::file_stamp(opened);
    std::fclose(opened);
    REQUIRE(resized);
    CHECK(resized->size == 15);
    CHECK(resized != retimed);
    CHECK_FALSE(platform::file_stamp(nullptr));
}

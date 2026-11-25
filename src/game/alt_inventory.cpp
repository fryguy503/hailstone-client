#include "game/alt_inventory.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "core/ascii_case.h"
#include "core/atomic_file.h"
#include "core/byte_io.h"
#include "core/character_files.h"
#include "core/path_utf8.h"
#include "core/server_profile.h"
#include "game/bank_snapshot.h"

namespace hailstone::game {

namespace {

constexpr auto kMagic = std::array{'H', 'S', 'A', 'I'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kMaxItems = 8192, kMaxText = 1024;
constexpr std::size_t kMaxIndexBytes = 36 * 1024 * 1024, kMaxBankBytes = 16 * 1024 * 1024;
constexpr std::int64_t kHeartbeat = 5, kExpires = 15;

using Session = AltInventoryCharacter::Session;

bool active(std::int64_t seen, std::int64_t now) {
    return seen > 0 && seen <= now && now - seen <= kExpires;
}

std::vector<AltInventoryItem> collect(const Inventory& inventory, bool bank) {
    std::vector<AltInventoryItem> items;
    auto add = [&](const net::Item& item, std::string_view bag, std::int16_t slot, std::int16_t sub) {
        if (item.id == 0 || item.name.empty() || items.size() == kMaxItems) return;
        const auto count = item.stackable() ? item.stack_count : 1;
        if (count == 0) return;
        items.push_back({item.name.substr(0, kMaxText), std::string(bag.substr(0, kMaxText)),
                         item.id, item.icon, count, slot, sub});
    };
    for (const auto& [key, item] : inventory.items()) {
        const auto [type, slot] = key;
        if (bank ? type != net::kInvBank || slot < 0
                 : type != net::kInvPossessions || slot < net::kSlotGeneral1 || slot > net::kSlotGeneral10)
            continue;
        add(item, {}, slot, -1);
        if (!item.is_bag()) continue;
        for (std::int16_t sub = 0; sub < item.bag_slots; ++sub) {
            if (const auto* contents = inventory.at(net::ItemSlot{type, slot, sub, -1})) add(*contents, item.name, slot, sub);
        }
    }
    return items;
}

AltInventoryArea known_bank(const BankSnapshot& bank) {
    if (bank.seen <= 0) return {};
    Inventory inventory;
    inventory.load(bank.items);
    return {bank.seen, collect(inventory, true)};
}

void write_text(ByteWriter& w, std::string_view text) {
    text = text.substr(0, kMaxText);
    w.le(static_cast<std::uint16_t>(text.size()));
    w.bytes(std::as_bytes(std::span(text.data(), text.size())));
}

std::string read_text(ByteReader& r) {
    const auto len = r.le<std::uint16_t>();
    if (len > kMaxText) throw std::out_of_range("inventory index text");
    const auto bytes = r.bytes(len);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void write_area(ByteWriter& w, const AltInventoryArea& area) {
    w.le(area.seen);
    w.le(static_cast<std::uint32_t>(area.items.size()));
    for (const auto& item : area.items) {
        w.le(item.id);
        w.le(item.icon);
        w.le(item.count);
        w.le(item.slot);
        w.le(item.sub);
        write_text(w, item.name);
        write_text(w, item.bag);
    }
}

AltInventoryArea read_area(ByteReader& r, bool bank) {
    AltInventoryArea area;
    area.seen = r.le<std::int64_t>();
    const auto count = r.le<std::uint32_t>();
    if (area.seen < 0 || (area.seen == 0 && count != 0) || count > kMaxItems)
        throw std::out_of_range("inventory index area");
    area.items.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        AltInventoryItem item;
        item.id = r.le<std::uint32_t>();
        item.icon = r.le<std::uint32_t>();
        item.count = r.le<std::uint32_t>();
        item.slot = r.le<std::int16_t>();
        item.sub = r.le<std::int16_t>();
        item.name = read_text(r);
        item.bag = read_text(r);
        if (item.id == 0 || item.count == 0 || item.name.empty() || item.slot < 0 || item.sub < -1 ||
            (!bank && (item.slot < net::kSlotGeneral1 || item.slot > net::kSlotGeneral10)))
            throw std::out_of_range("inventory index slot");
        area.items.push_back(std::move(item));
    }
    return area;
}

std::string encode(const AltInventoryCharacter& character, std::string_view world) {
    ByteWriter w;
    for (char c : kMagic) w.le(static_cast<std::uint8_t>(c));
    w.le(kVersion);
    write_text(w, world);
    write_text(w, character.name);
    w.le(static_cast<std::uint8_t>(character.bank_live));
    write_area(w, character.carried);
    write_area(w, character.bank);
    const auto& bytes = w.data();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::optional<AltInventoryCharacter> read(const std::filesystem::path& path, std::string_view world) {
    const auto data = platform::ReadOnlyMapping::open(path);
    if (!data || data->bytes().size() > kMaxIndexBytes) return std::nullopt;
    try {
        ByteReader r(data->bytes());
        for (char c : kMagic)
            if (r.le<std::uint8_t>() != static_cast<std::uint8_t>(c)) return std::nullopt;
        if (r.le<std::uint32_t>() != kVersion || !iequals(read_text(r), world)) return std::nullopt;
        AltInventoryCharacter character;
        character.name = read_text(r);
        if (character.name.empty() || !iequals(sanitize_file_name(character.name), path_utf8(path.stem())))
            return std::nullopt;
        const auto live = r.le<std::uint8_t>();
        if (live > 1) return std::nullopt;
        character.bank_live = live != 0;
        character.carried = read_area(r, false);
        character.bank = read_area(r, true);
        if (!r.empty() || character.carried.seen == 0 || (character.bank_live && character.bank.seen != character.carried.seen))
            return std::nullopt;
        return character;
    } catch (const std::out_of_range&) {
        return std::nullopt;
    }
}

void supplement_bank(AltInventoryCharacter& character, const std::filesystem::path& path) {
    if (character.bank_live) return;
    const auto data = platform::ReadOnlyMapping::open(path);
    if (!data || data->bytes().size() > kMaxBankBytes) return;
    if (auto bank = decode_bank_snapshot(data->bytes()); bank && bank->seen > character.bank.seen)
        character.bank = known_bank(*bank);
}

} // namespace

AltInventoryIndex::AltInventoryIndex(std::filesystem::path characters, std::string world, std::string character)
    : characters_(std::move(characters)), directory_(characters_ / ".alt"), world_(std::move(world)) {
    const std::string key = sanitize_file_name(character);
    snapshot_ = directory_ / (key + ".dat");
    lock_file_ = directory_ / (key + ".lock");
    local_.name = std::move(character);
    local_.session = Session::Local;
    if (auto saved = read(snapshot_, world_)) local_.bank = std::move(saved->bank);
}

bool AltInventoryIndex::matches(const std::filesystem::path& characters, const std::string& world,
                                 const std::string& character) const {
    return characters_ == characters && world_ == world && local_.name == character;
}

void AltInventoryIndex::update(const Inventory& inventory, const BankSnapshot& last_bank, bool bank_live,
                               std::int64_t now) {
    if (now <= 0 || local_.carried.seen == now) return;
    auto carried = collect(inventory, false);
    pending_ = pending_ || carried != local_.carried.items || local_.bank_live != bank_live;
    local_.carried = {now, std::move(carried)};
    if (bank_live) {
        auto bank = collect(inventory, true);
        pending_ = pending_ || bank != local_.bank.items;
        local_.bank = {now, std::move(bank)};
    } else if (last_bank.seen > local_.bank.seen) {
        local_.bank = known_bank(last_bank);
        pending_ = true;
    }
    local_.bank_live = bank_live;

    if (!lock_ && platform::prepare_private_cache_directory(directory_))
        lock_ = platform::ScopedFileLock::try_acquire(lock_file_, platform::FileLockMode::Exclusive);
    if (!lock_ || (!pending_ && now >= last_write_ && now - last_write_ < kHeartbeat)) return;
    if (write_file_atomic(snapshot_, encode(local_, world_))) {
        pending_ = false;
        last_write_ = now;
    }
}

std::vector<AltInventoryCharacter> AltInventoryIndex::characters(std::int64_t now) const {
    std::vector<AltInventoryCharacter> results{local_};
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory_, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path() == snapshot_ || it->path().extension() != ".dat") continue;
        auto saved = read(it->path(), world_);
        if (!saved || iequals(saved->name, local_.name)) continue;
        auto lock_path = it->path();
        lock_path.replace_extension(".lock");
        std::error_code lock_ec;
        if (active(saved->carried.seen, now) && std::filesystem::is_regular_file(lock_path, lock_ec) &&
            !platform::ScopedFileLock::try_acquire(lock_path, platform::FileLockMode::Shared))
            saved->session = Session::Boxed;
        else
            saved->bank_live = false;
        results.push_back(std::move(*saved));
    }
    // Existing bank snapshots remain useful before an alt has run the index, and may be newer than its index.
    ec.clear();
    for (std::filesystem::directory_iterator it(characters_, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = path_utf8(it->path().filename());
        std::error_code dir_ec;
        if (name.starts_with('.') || !std::filesystem::is_directory(it->symlink_status(dir_ec))) continue;
        auto existing = std::find_if(results.begin(), results.end(), [&](const auto& character) {
            return iequals(sanitize_file_name(character.name), name);
        });
        const auto bank = CharacterFiles{it->path()}.bank_snapshot();
        if (existing != results.end()) {
            if (existing->session != Session::Local) supplement_bank(*existing, bank);
        } else {
            AltInventoryCharacter character;
            character.name = name;
            supplement_bank(character, bank);
            if (character.bank.seen > 0) results.push_back(std::move(character));
        }
    }
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
        return a.session != b.session ? a.session < b.session : icompare(a.name, b.name) < 0;
    });
    return results;
}

} // namespace hailstone::game

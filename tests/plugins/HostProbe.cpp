// A compiled test plugin (tests/native_plugins_test.cpp), in C++ against the SDK alone: it calls the host table from
// where a plugin should not, to check that the client holds.
//   /probethread   calls host functions from a std::thread, then reports what they answered (ability_state too,
//   when the host table has it)
//   /probethrow    lets an exception out (not a std::exception); so do ${Probe.Throw} and OnUpdateImGui
//   /probestate n  web_state(tab 1) with a struct whose size says n bytes, as a plugin built with a shorter one
//   /probemessage n  web_message(tab 1) into a buffer of n bytes (0: no buffer)
//   /probetcp port    tcp_open("127.0.0.1", port); /probetcpsend h  tcp_send(h, "hello") and tcp_state(h)
//   /probebuffs [n], /probepetbuffs [n], /probespawns [n]  get_buff_data_bulk / get_pet_buff_data_bulk /
//   get_spawns_data_bulk into n bytes (the whole buffer without n), read through the SDK's HS_Bulk_* structs cast
//   onto the buffer
//   /probextargets [n]  get_xtarget_data_bulk the same way, through HS_Bulk_XTargetData
//   /probetargetbuffs [n]  get_target_buff_data_bulk the same way, through HS_Bulk_ListHeader,
//   HS_Bulk_TargetBuffFixedRecord and the caster's name after each
//   /probeaas [n], /probediscs [n]  get_avail_aa_data_bulk / get_avail_disc_data_bulk the same way, through
//   HS_Bulk_ListHeader and HAILSTONE_ID_NAME_RECORD
//   /probebuffnames [n]  get_buff_name_data_bulk the same way, through HS_Bulk_BuffNameFixedRecord and the caster's
//   name after each
//   /proberaid [n]  get_raid_data_bulk the same way, through HS_Bulk_RaidHeader / HS_Bulk_RaidData
//   /probecursor   cursor_origin, when the host table has it
//   /probeability <kind> <name>  ability_state then use_ability, when the host table has them
//   /probeabilities [n]  get_ability_data_bulk the same way, through HS_Bulk_ListHeader and HS_Bulk_AbilityRecord
// Built again with HAILSTONE_PROBE_THROW_INIT (HostProbeThrow): InitializePlugin registers a command and a TLO, then
// throws.
#include <cstddef>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "hailstone_byte_io.h"
#include "hailstone_plugin.h"

HAILSTONE_PLUGIN

// Pin the released prefix (0.7.1 ends at get_xtarget_data_bulk) and the entries after it: the table only grows at its
// end. All supported targets use one function-pointer size.
static_assert(offsetof(PluginHostApi, get_xtarget_data_bulk) == offsetof(PluginHostApi, caps) + 80 * sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, get_target_buff_data_bulk) == offsetof(PluginHostApi, caps) + 81 * sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, get_buff_name_data_bulk) == offsetof(PluginHostApi, caps) + 84 * sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, get_raid_data_bulk) == offsetof(PluginHostApi, caps) + 85 * sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, get_ability_data_bulk) == offsetof(PluginHostApi, caps) + 88 * sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, select_raid_member) == offsetof(PluginHostApi, caps) + 89 * sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, set_class_name) == offsetof(PluginHostApi, get_map_policy) + sizeof(PluginHostApi::caps));

namespace {

std::vector<unsigned char> received_item_bytes;

void nothing(void*, const char*) {}

void probe_thread(void*, const char*) {
    int added = -1;
    std::uint32_t caps = 99;
    void* imgui = &added;
    std::thread t([&] {
        added = host->add_command(host->host, "/fromthread", nothing, nullptr);
        caps = host->caps(host->host);
        imgui = host->imgui_context(host->host);
        host->write_chat(host->host, "[Probe] written from the thread", 0);
        host->do_command(host->host, "/echo from the thread");
    });
    t.join();
    WriteChatf("[Probe] thread: add_command %d, caps %u, imgui %s", added, static_cast<unsigned>(caps),
               imgui ? "set" : "null");
    if (HAILSTONE_HOST_HAS(host, ability_state)) {
        int state = 7;
        std::int64_t remaining = -2, reuse = -2;
        std::thread([&] { state = host->ability_state(host->host, 0, "Kick", &remaining, &reuse); }).join();
        WriteChatf("[Probe] thread: ability_state %d, %lld of %lld ms", state, static_cast<long long>(remaining),
                   static_cast<long long>(reuse));
    }
}

void probe_throw(void*, const char*) { throw 42; }

void probe_state(void*, const char* args) {
    HailstoneWebState st;
    std::memset(&st, 0x5a, sizeof st);
    st.size = static_cast<size_t>(std::strtoul(args, nullptr, 10));
    const int r = host->web_state(host->host, 1, &st);
    WriteChatf("[Probe] state %d, size %u, url %s, title %s", r, static_cast<unsigned>(st.size),
               r && st.size > offsetof(HailstoneWebState, url) ? st.url : "-",
               static_cast<unsigned char>(st.title[0]) == 0x5a ? "untouched" : st.title);
}

void probe_message(void*, const char* args) {
    char buf[64];
    const std::size_t n = std::strtoul(args, nullptr, 10);
    const int r = host->web_message(host->host, 1, n ? buf : nullptr, n < sizeof buf ? n : sizeof buf);
    WriteChatf("[Probe] message %d, %s", r, n && r >= 0 ? buf : "-");
}

void probe_tcp(void*, const char* args) {
    const auto port = static_cast<std::uint16_t>(std::strtoul(args, nullptr, 10));
    WriteChatf("[Probe] tcp open %d", static_cast<int>(host->tcp_open(host->host, "127.0.0.1", port)));
}

void probe_tcp_send(void*, const char* args) {
    const auto handle = static_cast<std::int32_t>(std::strtol(args, nullptr, 10));
    const long long sent = host->tcp_send(host->host, handle, "hello", 5);
    WriteChatf("[Probe] tcp send %lld state %d", sent, static_cast<int>(host->tcp_state(host->host, handle)));
}

// A bulk read's buffer size: the command's number, at most `whole`; `whole` without one (no argument, only spaces,
// or no number at its start).
std::int32_t size_arg(const char* args, std::size_t whole) {
    if (!args) return static_cast<std::int32_t>(whole);
    char* end = nullptr;
    const std::size_t n = std::strtoul(args, &end, 10);
    if (end == args) return static_cast<std::int32_t>(whole); // nothing read as a number
    return static_cast<std::int32_t>(n < whole ? n : whole);
}

static_assert(sizeof(HS_Bulk_BuffData) == HAILSTONE_BUFF_RECORD_SIZE);
static_assert(sizeof(HS_Bulk_PetBuffData) == HAILSTONE_PET_BUFF_RECORD_SIZE);

// The occupied slots one line each; the empty ones only counted, and only when laid out as empty.
void probe_buffs(void*, const char* args) {
    unsigned char buf[4096]; // at least 62 * HAILSTONE_BUFF_RECORD_SIZE (1798), or nothing comes back
    const std::int32_t r = host->get_buff_data_bulk(host->host, buf, size_arg(args, sizeof buf));
    const std::size_t count = r > 0 ? static_cast<std::size_t>(r) / sizeof(HS_Bulk_BuffData) : 0;
    const auto* recs = reinterpret_cast<const HS_Bulk_BuffData*>(buf);
    int empty = 0;
    for (std::size_t slot = 0; slot < count; ++slot) {
        const HS_Bulk_BuffData* rec = recs + slot;
        if (rec->ID == 0) { // an empty slot: every field 0, including the counter type
            ++empty;
            continue;
        }
        WriteChatf("[Probe] buff slot %d: spell %d, caster %d, tics %d, hits %d, spell type %d, counter type %d, "
                   "counters %d, slot type %u",
            static_cast<int>(slot), rec->ID, rec->casterID, rec->duration, rec->hitcount, rec->spelltype,
            rec->counterType, rec->counterTotal, static_cast<unsigned>(rec->slotType)); // HAILSTONE_BUFF_SLOT_*
    }
    WriteChatf("[Probe] buffs %d bytes, %d empty", r, empty);
}

static_assert(sizeof(HS_Bulk_BuffNameFixedRecord) == HAILSTONE_BUFF_NAME_RECORD_SIZE);

// get_buff_name_data_bulk: the occupied slots one line each, as /probebuffs but with the caster's name; the empty
// ones only counted. Each record is followed by its caster's name, so they are walked one by one.
void probe_buff_names(void*, const char* args) {
    unsigned char buf[8192]; // at least every buff and song slot (62 * HAILSTONE_BUFF_NAME_RECORD_SIZE plus the names)
    const std::int32_t r = host->get_buff_name_data_bulk(host->host, buf, size_arg(args, sizeof buf));
    const std::size_t size = r > 0 ? static_cast<std::size_t>(r) : 0;
    std::size_t at = 0;
    int empty = 0;
    for (int slot = 0; size - at >= sizeof(HS_Bulk_BuffNameFixedRecord); ++slot) {
        const auto* rec = reinterpret_cast<const HS_Bulk_BuffNameFixedRecord*>(buf + at);
        at += sizeof(HS_Bulk_BuffNameFixedRecord);
        const auto len = rec->casterNameLength > 0 ? static_cast<std::size_t>(rec->casterNameLength) : 0;
        if (len > size - at) break; // a name running past the end: the "left" count shows it
        const std::string caster(reinterpret_cast<const char*>(buf + at), len);
        at += len;
        if (rec->ID == 0) { // an empty slot: every field 0, including the counter type and the name length
            ++empty;
            continue;
        }
        WriteChatf("[Probe] buff name slot %d: spell %d, caster %s, tics %d, hits %d, spell type %d, counter type %d, "
                   "counters %d, slot type %u",
            slot, rec->ID, caster.empty() ? "-" : caster.c_str(), rec->duration, rec->hitcount, rec->spelltype,
            rec->counterType, rec->counterTotal, static_cast<unsigned>(rec->slotType)); // HAILSTONE_BUFF_SLOT_*
    }
    WriteChatf("[Probe] buff names %d bytes, %d empty, %zu bytes left", r, empty, size - at);
}

void probe_pet_buffs(void*, const char* args) {
    unsigned char buf[1024];
    const std::int32_t r = host->get_pet_buff_data_bulk(host->host, buf, size_arg(args, sizeof buf));
    const std::size_t count = r > 0 ? static_cast<std::size_t>(r) / sizeof(HS_Bulk_PetBuffData) : 0;
    const auto* recs = reinterpret_cast<const HS_Bulk_PetBuffData*>(buf);
    for (std::size_t i = 0; i < count; ++i) {
        const HS_Bulk_PetBuffData* rec = recs + i;
        WriteChatf("[Probe] pet buff: spell %d, tics %d, spell type %d", rec->id, rec->duration, rec->spellType);
    }
    WriteChatf("[Probe] pet buffs %d bytes", r);
}

static_assert(sizeof(HS_Bulk_SpawnHeader) == HS_BULK_SPAWN_HEADER_SIZE);
static_assert(sizeof(HS_Bulk_SpawnFixedRecord) == HS_BULK_SPAWN_RECORD_SIZE);

// The header, then each spawn one line (id, level, class, race, HP percent, con colour, name), then the bytes left
// over after the records the header counts (0 when every record's strings are framed right). With n, also how many
// bytes past the first n the host changed (guard bytes; 0 unless it wrote past the end).
void probe_spawns(void*, const char* args) {
    static unsigned char buf[1024*1024];
    constexpr unsigned char guard = 0xA5;
    std::memset(buf, guard, sizeof buf);
    const std::int32_t n = size_arg(args, sizeof buf);
    const std::int32_t r = host->get_spawns_data_bulk(host->host, buf, n);
    if (n < static_cast<std::int32_t>(sizeof buf)) {
        std::size_t changed = 0;
        for (std::size_t i = static_cast<std::size_t>(n); i < sizeof buf; ++i) changed += buf[i] != guard;
        WriteChatf("[Probe] spawns past the end %zu", changed);
    }
    if (r <= 0 || static_cast<std::size_t>(r) < sizeof(HS_Bulk_SpawnHeader)) {
        WriteChatf("[Probe] spawns %d bytes", static_cast<int>(r));
        return;
    }
    const auto size = static_cast<std::size_t>(r);

    const auto* header = reinterpret_cast<const HS_Bulk_SpawnHeader*>(buf);
    std::size_t at = sizeof(HS_Bulk_SpawnHeader);

    for (std::uint32_t i = 0; i < header->spawnCount; ++i) {
        if (size - at < sizeof(HS_Bulk_SpawnFixedRecord)) break; // a short buffer: stop, the "left" count shows it
        const auto* rec = reinterpret_cast<const HS_Bulk_SpawnFixedRecord*>(buf + at);
        at += sizeof(HS_Bulk_SpawnFixedRecord);

        // BodyDesc, CleanName, DisplayName, Name, RaceName, Suffix, TypeDesc: each a u16 length and its bytes.
        hailstone::ByteReader rd(buf + at, size - at);
        std::string name;
        for (int s = 0; s < 7; ++s) {
            const auto len = rd.le<std::uint16_t>();
            const auto text = rd.bytes(len);
            if (s == 3) name.assign(reinterpret_cast<const char*>(text.data()), text.size());
        }
        at += rd.position();

        WriteChatf("[Probe] spawn %d: level %d, class %d, race %d, hp %d%%, con %d, name %s", rec->id, rec->level,
            rec->classID, rec->raceID, rec->pctHPs, rec->conColorID, name.c_str());
    }
    WriteChatf("[Probe] spawns %u, player %.2f %.2f %.2f, %zu bytes left", header->spawnCount,
        header->playerX, header->playerY, header->playerZ, size - at);
}
static_assert(sizeof(HS_Bulk_XTargetData) == HS_BULK_XTARGET_RECORD_SIZE);

// Each extended target slot one line: slot, type (HS_BULK_XTARGET_*), spawn id, aggro percent, HP percent.
void probe_xtargets(void*, const char* args) {
    unsigned char buf[512];
    const std::int32_t r = host->get_xtarget_data_bulk(host->host, buf, size_arg(args, sizeof buf));
    const std::size_t count = r > 0 ? static_cast<std::size_t>(r) / sizeof(HS_Bulk_XTargetData) : 0;
    const auto* recs = reinterpret_cast<const HS_Bulk_XTargetData*>(buf);
    for (std::size_t slot = 0; slot < count; ++slot) {
        const HS_Bulk_XTargetData* rec = recs + slot;
        WriteChatf("[Probe] xtarget %d: type %d, spawn %d, aggro %d%%, hp %d%%", static_cast<int>(slot),
            rec->targetType, rec->id, rec->aggroPct, rec->pctHPs);
    }
    WriteChatf("[Probe] xtargets %d bytes", static_cast<int>(r));
}

static_assert(sizeof(HS_Bulk_TargetBuffFixedRecord) == HAILSTONE_TARGET_BUFF_RECORD_SIZE);

// Each entry one line, by the buff slot the server sent (spell id, tics, spell type, caster's name); a slot number
// skipped is one the server sent nothing for. An all-zero record (no spell, no name) is only counted as empty; the
// host sends none, so the count shows one sent again. Then the header's count and total, the records read after it
// (at most the count; fewer only when the header and the bytes disagree) and the bytes left over after the last whole
// record (0 when the count and every name are framed right). With n, also how many bytes past the first n the host
// changed (guard bytes; 0 unless it wrote past the end).
void probe_target_buffs(void*, const char* args) {
    unsigned char buf[2048];
    constexpr unsigned char guard = 0xA5;
    std::memset(buf, guard, sizeof buf);
    const std::int32_t n = size_arg(args, sizeof buf);
    const std::int32_t r = host->get_target_buff_data_bulk(host->host, buf, n);
    if (n < static_cast<std::int32_t>(sizeof buf)) {
        std::size_t changed = 0;
        for (std::size_t i = static_cast<std::size_t>(n); i < sizeof buf; ++i) changed += buf[i] != guard;
        WriteChatf("[Probe] target buffs past the end %zu", changed);
    }
    const std::size_t size = r > 0 ? static_cast<std::size_t>(r) : 0;
    if (size < sizeof(HS_Bulk_ListHeader)) {
        WriteChatf("[Probe] target buffs %d bytes", static_cast<int>(r));
        return;
    }
    const auto* header = reinterpret_cast<const HS_Bulk_ListHeader*>(buf);
    std::size_t at = sizeof(HS_Bulk_ListHeader);
    int count = 0;
    int empty = 0;
    while (count < header->count && size - at >= sizeof(HS_Bulk_TargetBuffFixedRecord)) {
        const auto* rec = reinterpret_cast<const HS_Bulk_TargetBuffFixedRecord*>(buf + at);
        const auto len = rec->casterNameLength > 0 ? static_cast<std::size_t>(rec->casterNameLength) : 0;
        if (len > size - at - sizeof(HS_Bulk_TargetBuffFixedRecord)) break; // a name past the end: "left" shows it
        ++count;
        at += sizeof(HS_Bulk_TargetBuffFixedRecord);
        if (rec->spellID == 0 && rec->duration == 0 && rec->spellType == 0 && rec->slot == 0 &&
            rec->casterNameLength == 0) {
            ++empty; // an empty entry: every field 0 and no name follows
            continue;
        }
        const std::string caster(reinterpret_cast<const char*>(buf + at), len);
        at += len;
        WriteChatf("[Probe] target buff slot %d: spell %d, tics %d, spell type %d, caster %s", rec->slot, rec->spellID,
            rec->duration, rec->spellType, caster.empty() ? "-" : caster.c_str());
    }
    WriteChatf("[Probe] target buffs count %d, total %d, %d bytes, %d records, %d empty, %zu bytes left",
        static_cast<int>(header->count), static_cast<int>(header->total), static_cast<int>(r), count, empty,
        size - at);
}

static_assert(sizeof(HAILSTONE_ID_NAME_RECORD) == HAILSTONE_ID_NAME_RECORD_SIZE);
static_assert(sizeof(HS_Bulk_ListHeader) == HS_BULK_LIST_HEADER_SIZE);

// get_avail_aa_data_bulk, the owned alternate abilities: each record one line by its position (id, name), then the
// header's count and total, the bytes and the bytes left over after the records it counts (0 when the count and
// every name are framed right). With n, also how many bytes past the first n the host changed (guard bytes; 0 unless it wrote past the end).
void probe_aa(void*, const char* args) {
    unsigned char buf[8192];
    constexpr unsigned char guard = 0xA5;
    std::memset(buf, guard, sizeof buf);
    const std::int32_t n = size_arg(args, sizeof buf);
    const std::int32_t r = host->get_avail_aa_data_bulk(host->host, buf, n);
    if (n < static_cast<std::int32_t>(sizeof buf)) {
        std::size_t changed = 0;
        for (std::size_t i = static_cast<std::size_t>(n); i < sizeof buf; ++i) changed += buf[i] != guard;
        WriteChatf("[Probe] aas past the end %zu", changed);
    }
    const std::size_t size = r > 0 ? static_cast<std::size_t>(r) : 0;
    if (size < sizeof(HS_Bulk_ListHeader)) {
        WriteChatf("[Probe] aas %d bytes", static_cast<int>(r));
        return;
    }
    const auto* header = reinterpret_cast<const HS_Bulk_ListHeader*>(buf);
    std::size_t at = sizeof(HS_Bulk_ListHeader);
    for (std::int32_t i = 0; i < header->count; ++i) {
        if (size - at < sizeof(HAILSTONE_ID_NAME_RECORD)) break; // a count past the bytes: the "left" count shows it
        const auto* rec = reinterpret_cast<const HAILSTONE_ID_NAME_RECORD*>(buf + at);
        const std::size_t len = rec->nameSize > 0 ? static_cast<std::size_t>(rec->nameSize) : 0;
        if (len > size - at - sizeof(HAILSTONE_ID_NAME_RECORD)) break; // a name running past the end
        at += sizeof(HAILSTONE_ID_NAME_RECORD);
        const std::string name(reinterpret_cast<const char*>(buf + at), len);
        at += len;
        WriteChatf("[Probe] aas %d: ability %d, name %s", static_cast<int>(i), rec->id,
            name.empty() ? "-" : name.c_str());
    }
    WriteChatf("[Probe] aas count %d, total %d, %d bytes, %zu bytes left", static_cast<int>(header->count),
        static_cast<int>(header->total), static_cast<int>(r), size - at);
}

// get_avail_disc_data_bulk, the combat abilities: each record one line by its position (id, name), then the
// header's count and total, the bytes and the bytes left over after the records it counts (0 when the count and
// every name are framed right). With n, also how many bytes past the first n the host changed (guard bytes; 0 unless it wrote past the end).
void probe_disc(void*, const char* args) {
    unsigned char buf[8192];
    constexpr unsigned char guard = 0xA5;
    std::memset(buf, guard, sizeof buf);
    const std::int32_t n = size_arg(args, sizeof buf);
    const std::int32_t r = host->get_avail_disc_data_bulk(host->host, buf, n);
    if (n < static_cast<std::int32_t>(sizeof buf)) {
        std::size_t changed = 0;
        for (std::size_t i = static_cast<std::size_t>(n); i < sizeof buf; ++i) changed += buf[i] != guard;
        WriteChatf("[Probe] discs past the end %zu", changed);
    }
    const std::size_t size = r > 0 ? static_cast<std::size_t>(r) : 0;
    if (size < sizeof(HS_Bulk_ListHeader)) {
        WriteChatf("[Probe] discs %d bytes", static_cast<int>(r));
        return;
    }
    const auto* header = reinterpret_cast<const HS_Bulk_ListHeader*>(buf);
    std::size_t at = sizeof(HS_Bulk_ListHeader);
    for (std::int32_t i = 0; i < header->count; ++i) {
        if (size - at < sizeof(HAILSTONE_ID_NAME_RECORD)) break; // a count past the bytes: the "left" count shows it
        const auto* rec = reinterpret_cast<const HAILSTONE_ID_NAME_RECORD*>(buf + at);
        const std::size_t len = rec->nameSize > 0 ? static_cast<std::size_t>(rec->nameSize) : 0;
        if (len > size - at - sizeof(HAILSTONE_ID_NAME_RECORD)) break; // a name running past the end
        at += sizeof(HAILSTONE_ID_NAME_RECORD);
        const std::string name(reinterpret_cast<const char*>(buf + at), len);
        at += len;
        WriteChatf("[Probe] discs %d: spell %d, name %s", static_cast<int>(i), rec->id,
            name.empty() ? "-" : name.c_str());
    }
    WriteChatf("[Probe] discs count %d, total %d, %d bytes, %zu bytes left", static_cast<int>(header->count),
        static_cast<int>(header->total), static_cast<int>(r), size - at);
}

static_assert(sizeof(HS_Bulk_RaidHeader) == HS_BULK_RAID_HEADER_SIZE);
static_assert(sizeof(HS_Bulk_RaidData) == HS_BULK_RAID_RECORD_SIZE);

// The header, then each member one line: slot, name, spawn id, group, flags, level, class, HP/mana/endurance
// percent, pet spawn id and pet HP percent.
void probe_raid(void*, const char* args) {
    if (!HAILSTONE_HOST_HAS(host, get_raid_data_bulk)) {
        WriteChatf("[Probe] raid: not in this client");
        return;
    }
    unsigned char buf[16384];
    const std::int32_t r = host->get_raid_data_bulk(host->host, buf, size_arg(args, sizeof buf));
    if (r < static_cast<std::int32_t>(sizeof(HS_Bulk_RaidHeader))) {
        WriteChatf("[Probe] raid %d bytes", r);
        return;
    }
    const auto* header = reinterpret_cast<const HS_Bulk_RaidHeader*>(buf);
    WriteChatf("[Probe] raid active %d, count %u, total %u, leader %u, target %u, locked %d, loot %d, %d bytes",
        header->active, header->count, header->total, header->leaderIndex, header->targetIndex, header->locked,
        header->lootType, r);
    // The reference reader: records start at header.size and step by header.recordSize, so a later client that
    // appends a record field is still read correctly.
    std::size_t at = header->size >= sizeof(HS_Bulk_RaidHeader) ? header->size : sizeof(HS_Bulk_RaidHeader);
    if (header->recordSize < sizeof(HS_Bulk_RaidData)) {
        WriteChatf("[Probe] raid record size %u, smaller than this reader's %zu", header->recordSize,
            sizeof(HS_Bulk_RaidData));
        return;
    }
    for (std::uint32_t i = 0; i < header->count && at + header->recordSize <= static_cast<std::size_t>(r); ++i) {
        const auto* rec = reinterpret_cast<const HS_Bulk_RaidData*>(buf + at);
        WriteChatf("[Probe] raid %u: %s %u g%d f%u l%d c%d hp%d mp%d ep%d pet%u ph%d", i, rec->name, rec->spawnID,
            rec->group, rec->flags, rec->level, rec->classID, rec->pctHPs, rec->pctMana, rec->pctEndurance,
            rec->petSpawnID, rec->petPctHPs);
        WriteChatf("[Probe] raid pos %u: %.2f %.2f %.2f %.2f", i, rec->x, rec->y, rec->z, rec->distance);
        at += header->recordSize;
    }
}

void probe_cursor(void*, const char*) {
    if (!HAILSTONE_HOST_HAS(host, cursor_origin)) {
        WriteChatf("[Probe] cursor origin: not in this client");
        return;
    }
    WriteChatf("[Probe] cursor origin %u", static_cast<unsigned>(host->cursor_origin(host->host)));
}

void probe_binary(void*, const char* arguments) {
    char* end = nullptr;
    const auto kind = static_cast<std::uint32_t>(std::strtoul(arguments, &end, 10));
    HailstoneBinaryMeasurement measured;
    std::memset(&measured, 0x5a, sizeof(measured));
    measured.size = sizeof(measured);
    while (*end == ' ') ++end;
    if (*end) measured.size = static_cast<std::uint32_t>(std::strtoul(end, nullptr, 10));
    const auto result = HAILSTONE_HOST_HAS(host, measure_binary) ? host->measure_binary(host->host, kind, &measured) : 0;
    if (!result || measured.size < sizeof(measured)) {
        WriteChatf("[Probe] binary result %d untouched %d", result,
                   measured.kind == 0x5a5a5a5au && measured.sha256[0] == 0x5a ? 1 : 0);
        return;
    }
    char digest[65]{};
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < 32; ++i) {
        digest[i * 2] = hex[measured.sha256[i] >> 4];
        digest[i * 2 + 1] = hex[measured.sha256[i] & 15];
    }
    WriteChatf("[Probe] binary kind %u bytes %llu fnv %llu sha %s", measured.kind,
               static_cast<unsigned long long>(measured.byte_count), static_cast<unsigned long long>(measured.fnv1a63), digest);
}
void probe_binary_thread(void*, const char*) {
    int answer = -1;
    HailstoneBinaryMeasurement measured{}; measured.size = sizeof(measured);
    std::thread thread([&] { answer = host->measure_binary(host->host, HAILSTONE_BINARY_CALLING_PLUGIN, &measured); });
    thread.join();
    WriteChatf("[Probe] binary thread %d", answer);
}

void probe_decode(void*, const char* arguments) {
    HailstoneItem item;
    HailstoneItemSlot slot;
    std::memset(&item, 0x5a, sizeof(item));
    std::memset(&slot, 0x5a, sizeof(slot));
    item.size = *arguments ? static_cast<std::uint32_t>(std::strtoul(arguments, nullptr, 10)) : static_cast<std::uint32_t>(sizeof(item));
    std::array<unsigned char, sizeof(item)> old_item;
    std::array<unsigned char, sizeof(slot)> old_slot;
    std::memcpy(old_item.data(), &item, sizeof(item));
    std::memcpy(old_slot.data(), &slot, sizeof(slot));
    const auto result = host->decode_item_packet(host->host, received_item_bytes.data(), received_item_bytes.size(), &slot, &item);
    WriteChatf("[Probe] decode %d slot_unchanged %d item_unchanged %d id %u", result,
        std::memcmp(old_slot.data(), &slot, sizeof(slot)) == 0,
        std::memcmp(old_item.data(), &item, sizeof(item)) == 0, item.id);
}

HailstoneSpawn saved_spawn{};
void probe_services(void*, const char*) {
    saved_spawn = {}; saved_spawn.size = sizeof(saved_spawn);
    const auto found = host->get_spawn(host->host, 12, &saved_spawn);
    WriteChatf("[Probe] services spawn %d token %llu flags %u mana %u", found,
        static_cast<unsigned long long>(saved_spawn.incarnation), saved_spawn.observation_flags, saved_spawn.mana_percent);
}
// /probeselectraid [stale]: select_raid_member on the spawn /probeservices read, with its incarnation or a later one.
void probe_select(void*, const char* args) {
    const auto incarnation = saved_spawn.incarnation + (args && *args ? 1u : 0u);
    const auto result = host->select_raid_member(host->host, saved_spawn.id, incarnation);
    WriteChatf("[Probe] raid selection %d", result);
}
// /proberaidmove <name> <group>: raid_action HAILSTONE_RAID_MOVE.
void probe_raid_move(void*, const char* args) {
    char name[64] = {};
    int group = 0;
    if (!args || std::sscanf(args, "%63s %d", name, &group) != 2) return;
    WriteChatf("[Probe] raid move %d", host->raid_action(host->host, name, HAILSTONE_RAID_MOVE, group));
}

std::uint32_t map_flags = 0;
std::uint64_t map_revision = 0;
int map_format = 0;
struct OldMapPolicy72 { std::uint32_t size, flags; char name[64]; };
struct OldMapPolicy80 { OldMapPolicy72 prefix; std::uint64_t revision; };
static_assert(sizeof(OldMapPolicy72)==72 && alignof(OldMapPolicy72)==4);
static_assert(sizeof(OldMapPolicy80)==80 && offsetof(HailstoneMapPolicy,revision)==72 && offsetof(HailstoneMapPolicy,context)==80);
int old_map_policy(void* destination, bool with_revision) {
    OldMapPolicy80 old{};
    old.prefix.size=with_revision?80:72;old.prefix.flags=map_flags;
    std::memcpy(old.prefix.name,"Probe map",sizeof("Probe map"));old.revision=map_revision;
    std::memcpy(destination,&old,old.prefix.size);
    return 1;
}
int map_policy(HailstoneMapPolicy* out) {
    if (!map_flags && !map_format) return 0;
    if (map_format==7 || map_format==8) return old_map_policy(out,map_format==8);
    out->flags = map_flags;
    out->revision = map_revision;
    out->context = UINT64_MAX; // provider answers cannot choose the host context
    std::memcpy(out->name, "Probe map", sizeof("Probe map"));
    if (map_format == 1) out->size = 71;
    if (map_format == 2) out->flags |= 128;
    if (map_format == 3) std::memset(out->name, 'X', sizeof(out->name));
    if (map_format == 4) out->name[0] = '\n';
    if (map_format == 5) {
        HailstoneMapPolicy nested{}; nested.size = sizeof(nested);
        WriteChatf("[Probe] nested map %d", host->get_map_policy(host->host, &nested));
    }
    if (map_format == 6) host->set_active(host->host, 0);
    return 1;
}
void probe_map_set(void*, const char* args) {
    char* rest = nullptr;
    map_flags = static_cast<std::uint32_t>(std::strtoul(args, &rest, 10));
    map_format = rest ? static_cast<int>(std::strtol(rest, &rest, 10)) : 0;
    map_revision = rest ? std::strtoull(rest,nullptr,10) : 0;
    host->set_active(host->host, 1);
}
void probe_map_prefix(void*, const char* args) {
    const auto size=static_cast<std::uint32_t>(std::strtoul(args,nullptr,10));
    if(size!=72 && size!=80 && size!=88)return;
    alignas(8) std::array<unsigned char,104> storage;
    storage.fill(0x5a);
    auto* bytes=storage.data()+4; // a real old record may be aligned to four, not eight
    std::memcpy(bytes,&size,sizeof(size));
    const int result=host->get_map_policy(host->host,reinterpret_cast<HailstoneMapPolicy*>(bytes));
    OldMapPolicy72 old{};std::memcpy(&old,bytes,sizeof(old));
    bool canary=true;
    for(std::size_t i=0;i<4;++i)canary=canary&&storage[i]==0x5a;
    for(std::size_t i=4+size;i<storage.size();++i)canary=canary&&storage[i]==0x5a;
    std::uint64_t revision=0,context=0;
    if(size>=80)std::memcpy(&revision,bytes+72,sizeof(revision));
    if(size>=88)std::memcpy(&context,bytes+80,sizeof(context));
    WriteChatf("[Probe] map prefix %u result %d flags %u canary %d revision %d context %d",size,result,old.flags,
        canary?1:0,revision?1:0,context&&context!=UINT64_MAX?1:0);
}
void probe_map_cycle(void*,const char*) { host->set_active(host->host,0);host->set_active(host->host,1); }
void probe_map(void*, const char* args) {
    HailstoneMapPolicy policy;
    std::memset(&policy, 0x5a, sizeof(policy));
    policy.size = *args ? static_cast<std::uint32_t>(std::strtoul(args, nullptr, 10)) : sizeof(policy);
    const int result = host->get_map_policy(host->host, &policy);
    WriteChatf("[Probe] map result %d flags %u untouched %d name %s", result, policy.flags,
        static_cast<unsigned char>(policy.name[0]) == 0x5a,
        result && policy.size > offsetof(HailstoneMapPolicy, name) ? policy.name : "-");
}
void probe_map_thread(void*, const char*) {
    HailstoneMapPolicy policy{}; policy.size = sizeof(policy);
    int result = -1;
    std::thread thread([&] { result = host->get_map_policy(host->host, &policy); }); thread.join();
    WriteChatf("[Probe] map thread %d", result);
}

// /probeability <kind> <name>: ability_state then use_ability, both added later in ABI 3.
void probe_ability(void*, const char* args) {
    if (!HAILSTONE_HOST_HAS(host, ability_state) || !HAILSTONE_HOST_HAS(host, use_ability)) {
        WriteChatf("[Probe] ability: not in this client");
        return;
    }
    int kind = -1;
    char name[64] = {};
    if (!args || std::sscanf(args, "%d %63s", &kind, name) != 2) {
        WriteChatf("[Probe] ability: kind and name?");
        return;
    }
    std::int64_t remaining = -1, reuse = -1;
    const int state = host->ability_state(host->host, kind, name, &remaining, &reuse);
    const int used = host->use_ability(host->host, kind, name);
    WriteChatf("[Probe] ability %d %s: state %d, %lld of %lld ms, use %d", kind, name, state,
               static_cast<long long>(remaining), static_cast<long long>(reuse), used);
}

static_assert(sizeof(HS_Bulk_AbilityRecord) == HAILSTONE_ABILITY_RECORD_SIZE);

// get_ability_data_bulk: each entry one line (kind, id, ready, remaining and reuse ms, name), then the header's
// count and total, the bytes, the records read and the bytes left over. With n, also how many bytes past the first n
// the host changed (guard bytes).
void probe_abilities(void*, const char* args) {
    if (!HAILSTONE_HOST_HAS(host, get_ability_data_bulk)) {
        WriteChatf("[Probe] abilities: not in this client");
        return;
    }
    unsigned char buf[32768];
    constexpr unsigned char guard = 0xA5;
    std::memset(buf, guard, sizeof buf);
    const std::int32_t n = size_arg(args, sizeof buf);
    const std::int32_t r = host->get_ability_data_bulk(host->host, buf, n);
    if (n < static_cast<std::int32_t>(sizeof buf)) {
        std::size_t changed = 0;
        for (std::size_t i = static_cast<std::size_t>(n); i < sizeof buf; ++i) changed += buf[i] != guard;
        WriteChatf("[Probe] abilities past the end %zu", changed);
    }
    const std::size_t size = r > 0 ? static_cast<std::size_t>(r) : 0;
    if (size < sizeof(HS_Bulk_ListHeader)) {
        WriteChatf("[Probe] abilities %d bytes", static_cast<int>(r));
        return;
    }
    const auto* header = reinterpret_cast<const HS_Bulk_ListHeader*>(buf);
    std::size_t at = sizeof(HS_Bulk_ListHeader);
    int count = 0;
    while (count < header->count && size - at >= sizeof(HS_Bulk_AbilityRecord)) {
        const auto* rec = reinterpret_cast<const HS_Bulk_AbilityRecord*>(buf + at);
        at += sizeof(HS_Bulk_AbilityRecord);
        const std::size_t len = rec->nameSize > 0 ? static_cast<std::size_t>(rec->nameSize) : 0;
        if (len > size - at) break; // a name running past the end: "left" shows it
        const std::string name(reinterpret_cast<const char*>(buf + at), len);
        at += len;
        ++count;
        WriteChatf("[Probe] ability %d: kind %d, id %d, ready %d, %lld of %lld ms, name %s", count - 1, rec->kind,
                   rec->id, rec->ready, static_cast<long long>(rec->remainingMs), static_cast<long long>(rec->reuseMs),
                   name.empty() ? "-" : name.c_str());
    }
    WriteChatf("[Probe] abilities count %d, total %d, %d bytes, %d records, %zu bytes left", header->count,
               header->total, static_cast<int>(r), count, size - at);
}

int probe_tlo(void*, const char*, const char* member, const char*, HailstoneValue* out) {
    if (member && std::strcmp(member, "Throw") == 0) throw std::runtime_error("the TLO threw");
    out->kind = HAILSTONE_STRING;
    out->s = "probe";
    return 1;
}

} // namespace

HAILSTONE_API void InitializePlugin(void) {
    AddCommand("/probethread", probe_thread, nullptr);
    AddCommand("/probethrow", probe_throw, nullptr);
    AddCommand("/probestate", probe_state, nullptr);
    AddCommand("/probemessage", probe_message, nullptr);
    AddCommand("/probetcp", probe_tcp, nullptr);
    AddCommand("/probetcpsend", probe_tcp_send, nullptr);
    AddCommand("/probebuffs", probe_buffs, nullptr);
    AddCommand("/probebuffnames", probe_buff_names, nullptr);
    AddCommand("/probepetbuffs", probe_pet_buffs, nullptr);
    AddCommand("/probespawns", probe_spawns, nullptr);
    AddCommand("/probextargets", probe_xtargets, nullptr);
    AddCommand("/probetargetbuffs", probe_target_buffs, nullptr);
    AddCommand("/probeaas", probe_aa, nullptr);
    AddCommand("/probediscs", probe_disc, nullptr);
    AddCommand("/proberaid", probe_raid, nullptr);
    AddCommand("/probecursor", probe_cursor, nullptr);
    AddCommand("/probebinary", probe_binary, nullptr);
    AddCommand("/probebinarythread", probe_binary_thread, nullptr);
    AddCommand("/probedecode", probe_decode, nullptr);
    AddCommand("/probeservices", probe_services, nullptr);
    AddCommand("/probeselectraid", probe_select, nullptr);
    AddCommand("/proberaidmove", probe_raid_move, nullptr);
    AddCommand("/probemapset", probe_map_set, nullptr);
    AddCommand("/probemappolicy", probe_map, nullptr);
    AddCommand("/probemapthread", probe_map_thread, nullptr);
    AddCommand("/probemapprefix", probe_map_prefix, nullptr);
    AddCommand("/probemapcycle", probe_map_cycle, nullptr);
    AddCommand("/probeability", probe_ability, nullptr);
    AddCommand("/probeabilities", probe_abilities, nullptr);
    AddTopLevelObject("Probe", probe_tlo, nullptr);
#ifdef HAILSTONE_PROBE_THROW_INIT
    throw std::runtime_error("InitializePlugin threw");
#endif
}
HAILSTONE_API void ShutdownPlugin(void) {
    RemoveCommand("/probethread");
    RemoveCommand("/probethrow");
    RemoveCommand("/probestate");
    RemoveCommand("/probemessage");
    RemoveCommand("/probetcp");
    RemoveCommand("/probetcpsend");
    RemoveCommand("/probebuffs");
    RemoveCommand("/probebuffnames");
    RemoveCommand("/probepetbuffs");
    RemoveCommand("/probespawns");
    RemoveCommand("/probextargets");
    RemoveCommand("/probetargetbuffs");
    RemoveCommand("/probeaas");
    RemoveCommand("/probediscs");
    RemoveCommand("/proberaid");
    RemoveCommand("/probecursor");
    RemoveCommand("/probebinary");
    RemoveCommand("/probebinarythread");
    RemoveCommand("/probedecode");
    RemoveCommand("/probeservices");
    RemoveCommand("/probeselectraid");
    RemoveCommand("/proberaidmove");
    RemoveCommand("/probemapset");
    RemoveCommand("/probemappolicy");
    RemoveCommand("/probemapthread");
    RemoveCommand("/probemapprefix");
    RemoveCommand("/probemapcycle");
    RemoveCommand("/probeability");
    RemoveCommand("/probeabilities");
    RemoveTopLevelObject("Probe");
}
HAILSTONE_API void OnUpdateImGui(void) { throw std::runtime_error("OnUpdateImGui threw"); }
HAILSTONE_API int OnPacket(int, int, HailstonePacket* packet) {
    received_item_bytes.clear();
    if (packet && packet->data && packet->size && packet->size <= 262144) {
        received_item_bytes.resize(packet->size);
        std::memcpy(received_item_bytes.data(), packet->data, packet->size);
    }
    return 0;
}
HAILSTONE_API const HailstoneHooks* hailstone_plugin_hooks(void) {
    static HailstoneHooks hooks{};
    hooks.size = sizeof(hooks);
    hooks.map_policy = map_policy;
    return &hooks;
}

// A compiled test plugin (tests/native_plugins_test.cpp), in C++ against the SDK alone: it calls the host table from
// where a plugin should not, to check that the client holds.
//   /probethread   calls host functions from a std::thread, then reports what they answered (ability_state too,
//   when the host table has it)
//   /probethrow    lets an exception out (not a std::exception); so do ${Probe.Throw} and OnUpdateImGui
//   /probestate n  web_state(tab 1) with a struct whose size says n bytes, as a plugin built with a shorter one
//   /probemessage n  web_message(tab 1) into a buffer of n bytes (0: no buffer)
//   /probetcp port    tcp_open("127.0.0.1", port); /probetcpsend h  tcp_send(h, "hello") and tcp_state(h)
//   /probebuffs [n], /probepetbuffs [n], /probespawns [n]  get_buff_data_bulk / get_pet_buff_data_bulk /
//   get_spawns_data_bulk into n bytes of the plugin's bulk buffer (acquire_bulk_buffer; all of it without n), read
//   through the SDK's HS_Bulk_* structs cast onto it; with n, also how many bytes past the first n the host changed
//   (guard bytes; 0 unless it wrote past the end). See BulkProbe.
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
//   /probespells n id...  get_spells_data_bulk for those ids into n bytes, through HS_Bulk_SpellFixedRecord and the
//   strings and effect lines after each
//   /probespellsbyname [n] name  get_spells_by_name_data_bulk for that name the same way, printed as /probespells
//   does
//   /<name>buf ... (the plugin's own name, lower case, so two copies loaded at once each have theirs): the bulk
//   buffer, acquire_bulk_buffer / release_bulk_buffer; see probe_buffer
// Built again with HAILSTONE_PROBE_THROW_INIT (HostProbeThrow): InitializePlugin registers a command and a TLO, then
// throws.
#include <cstddef>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
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
static_assert(offsetof(PluginHostApi, nameplate_register) == offsetof(PluginHostApi, set_class_name) + sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, get_spells_data_bulk) ==
              offsetof(PluginHostApi, nameplate_spell_icon) + sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, set_ui_contribution) ==
              offsetof(PluginHostApi, get_spells_data_bulk) + sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, remove_ui_contribution) ==
              offsetof(PluginHostApi, set_ui_contribution) + sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, get_spells_by_name_data_bulk) ==
              offsetof(PluginHostApi, remove_ui_contribution) + sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, acquire_bulk_buffer) ==
              offsetof(PluginHostApi, get_spells_by_name_data_bulk) + sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, release_bulk_buffer) ==
              offsetof(PluginHostApi, acquire_bulk_buffer) + sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, spell_id_limit) ==
              offsetof(PluginHostApi, release_bulk_buffer) + sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, spell_data_ready) ==
              offsetof(PluginHostApi, spell_id_limit) + sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, get_large_dialog) ==
              offsetof(PluginHostApi, spell_data_ready) + sizeof(PluginHostApi::caps));
static_assert(offsetof(PluginHostApi, answer_large_dialog) == offsetof(PluginHostApi, get_large_dialog) + sizeof(PluginHostApi::caps));
static_assert(sizeof(HailstoneLargeDialog) == 40);

namespace {

std::vector<unsigned char> received_item_bytes;
HailstoneLargeDialog dialog_snapshot{};

void probe_dialog(void*, const char*) {
    HailstoneLargeDialog d{}; d.size = sizeof(d);
    HailstonePlayer p{}; p.size = sizeof(p);
    const int ok = HAILSTONE_HOST_HAS(host, get_large_dialog) ? host->get_large_dialog(host->host, &d) : 0;
    if (ok) dialog_snapshot = d;
    const int player = host->get_player(host->host, &p);
    WriteChatf("[Probe] dialog %d flags %u token %d camp %d ids %u %u %u playercamp %d", ok, d.flags,
        d.token != 0, d.camp_token != 0, d.popup_id, d.negative_id, d.close_id,
        player && (p.observation_flags & HAILSTONE_PLAYER_CAMPING) != 0);
}
void probe_dialog_answer(void*, const char* args) {
    int button = 1; unsigned flags = 0;
    if (args) std::sscanf(args, "%d %u", &button, &flags);
    const int ok = HAILSTONE_HOST_HAS(host, answer_large_dialog) ? host->answer_large_dialog(host->host,
        dialog_snapshot.token, dialog_snapshot.camp_token, button, flags) : 0;
    WriteChatf("[Probe] dialog answer %d", ok);
}
void probe_dialog_active(void*, const char* args) { host->set_active(host->host, args && *args == '1'); }
void probe_dialog_thread(void*, const char*) {
    int read = 1, answer = 1;
    std::thread t([&] {
        HailstoneLargeDialog d{}; d.size = sizeof(d);
        read = host->get_large_dialog(host->host, &d);
        answer = host->answer_large_dialog(host->host, dialog_snapshot.token, dialog_snapshot.camp_token, 1, 1);
    });
    t.join();
    WriteChatf("[Probe] dialog thread %d %d", read, answer);
}

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

// A bulk read's buffer size: the command's number, or -1 (the whole buffer) without one (no argument, only spaces, or
// no number at its start).
long long size_arg(const char* args) {
    if (!args) return -1;
    char* end = nullptr;
    const unsigned long long n = std::strtoull(args, &end, 10);
    if (end == args) return -1; // nothing read as a number
    return n < static_cast<unsigned long long>(HS_BULK_BUFFER_SIZE) ? static_cast<long long>(n) : HS_BULK_BUFFER_SIZE;
}

// A bulk probe's buffer: this plugin's bulk buffer (acquire_bulk_buffer), released when the probe returns. The read
// is handed its first n bytes: `requested`, at most the whole buffer, or the whole buffer for -1. Below the whole,
// the bytes past n are guard bytes first, and past_end says how many the host changed (0 unless it wrote past the
// end).
class BulkProbe {
public:
    BulkProbe(const char* what, long long requested) : what_(what) {
        std::int32_t room = 0;
        buf = host->acquire_bulk_buffer(host->host, &room);
        if (!buf) {
            WriteChatf("[Probe] %s: no buffer", what);
            return;
        }
        room_ = room;
        n = requested < 0 || requested > room ? room : static_cast<std::int32_t>(requested);
        if (requested >= 0) std::memset(buf + n, kGuard, static_cast<std::size_t>(room_ - n));
        guarded_ = requested >= 0;
    }
    ~BulkProbe() {
        if (buf) host->release_bulk_buffer(host->host);
    }
    BulkProbe(const BulkProbe&) = delete;
    BulkProbe& operator=(const BulkProbe&) = delete;
    explicit operator bool() const { return buf != nullptr; }
    void past_end() const {
        if (!guarded_) return;
        std::size_t changed = 0;
        for (std::int32_t i = n; i < room_; ++i) changed += buf[i] != kGuard;
        WriteChatf("[Probe] %s past the end %zu", what_, changed);
    }

    unsigned char* buf = nullptr;
    std::int32_t n = 0;

private:
    static constexpr unsigned char kGuard = 0xA5;
    const char* what_;
    std::int32_t room_ = 0;
    bool guarded_ = false;
};

static_assert(sizeof(HS_Bulk_BuffData) == HAILSTONE_BUFF_RECORD_SIZE);
static_assert(sizeof(HS_Bulk_PetBuffData) == HAILSTONE_PET_BUFF_RECORD_SIZE);

// The occupied slots one line each; the empty ones only counted, and only when laid out as empty.
void probe_buffs(void*, const char* args) {
    if (!HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] buffs: not in this client");
        return;
    }
    const BulkProbe probe("buffs", size_arg(args));
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r = host->get_buff_data_bulk(host->host, buf, probe.n);
    probe.past_end();
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
    if (!HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] buff names: not in this client");
        return;
    }
    const BulkProbe probe("buff names", size_arg(args));
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r = host->get_buff_name_data_bulk(host->host, buf, probe.n);
    probe.past_end();
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
    if (!HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] pet buffs: not in this client");
        return;
    }
    const BulkProbe probe("pet buffs", size_arg(args));
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r = host->get_pet_buff_data_bulk(host->host, buf, probe.n);
    probe.past_end();
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
// over after the records the header counts (0 when every record's strings are framed right).
void probe_spawns(void*, const char* args) {
    if (!HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] spawns: not in this client");
        return;
    }
    const BulkProbe probe("spawns", size_arg(args));
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r = host->get_spawns_data_bulk(host->host, buf, probe.n);
    probe.past_end();
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
    if (!HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] xtargets: not in this client");
        return;
    }
    const BulkProbe probe("xtargets", size_arg(args));
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r = host->get_xtarget_data_bulk(host->host, buf, probe.n);
    probe.past_end();
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
// record (0 when the count and every name are framed right).
void probe_target_buffs(void*, const char* args) {
    if (!HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] target buffs: not in this client");
        return;
    }
    const BulkProbe probe("target buffs", size_arg(args));
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r = host->get_target_buff_data_bulk(host->host, buf, probe.n);
    probe.past_end();
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
// every name are framed right).
void probe_aa(void*, const char* args) {
    if (!HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] aas: not in this client");
        return;
    }
    const BulkProbe probe("aas", size_arg(args));
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r = host->get_avail_aa_data_bulk(host->host, buf, probe.n);
    probe.past_end();
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
// every name are framed right).
void probe_disc(void*, const char* args) {
    if (!HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] discs: not in this client");
        return;
    }
    const BulkProbe probe("discs", size_arg(args));
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r = host->get_avail_disc_data_bulk(host->host, buf, probe.n);
    probe.past_end();
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
    if (!HAILSTONE_HOST_HAS(host, get_raid_data_bulk) || !HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] raid: not in this client");
        return;
    }
    const BulkProbe probe("raid", size_arg(args));
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r = host->get_raid_data_bulk(host->host, buf, probe.n);
    probe.past_end();
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
static_assert(offsetof(HailstoneMapPolicy,npc_find_mode)==88 && offsetof(HailstoneMapPolicy,actor_guides)==96 &&
              sizeof(HailstoneMapPolicy)==104);
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
    if ((map_format == 9 || map_format == 10) && out->size >= offsetof(HailstoneMapPolicy, actor_guides))
        out->npc_find_mode = HAILSTONE_NPC_FIND_SERVER_ONLY;
    if (map_format == 9) out->size = offsetof(HailstoneMapPolicy, actor_guides);
    if (map_format == 10 && out->size >= sizeof(*out)) out->actor_guides = HAILSTONE_MAP_ACTOR_GUIDES;
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
    if(size!=72 && size!=80 && size!=88 && size!=96 && size!=104)return;
    alignas(8) std::array<unsigned char,112> storage;
    storage.fill(0x5a);
    auto* bytes=storage.data()+4; // a real old record may be aligned to four, not eight
    std::memcpy(bytes,&size,sizeof(size));
    const int result=host->get_map_policy(host->host,reinterpret_cast<HailstoneMapPolicy*>(bytes));
    OldMapPolicy72 old{};std::memcpy(&old,bytes,sizeof(old));
    bool canary=true;
    for(std::size_t i=0;i<4;++i)canary=canary&&storage[i]==0x5a;
    for(std::size_t i=4+size;i<storage.size();++i)canary=canary&&storage[i]==0x5a;
    std::uint64_t revision=0,context=0,find_mode=0,actor_guides=0;
    if(size>=80)std::memcpy(&revision,bytes+72,sizeof(revision));
    if(size>=88)std::memcpy(&context,bytes+80,sizeof(context));
    if(size>=96)std::memcpy(&find_mode,bytes+88,sizeof(find_mode));
    if(size>=104)std::memcpy(&actor_guides,bytes+96,sizeof(actor_guides));
    WriteChatf("[Probe] map prefix %u result %d flags %u canary %d revision %d context %d server find %d actor guides %d",
        size,result,old.flags,canary?1:0,revision?1:0,context&&context!=UINT64_MAX?1:0,
        find_mode==HAILSTONE_NPC_FIND_SERVER_ONLY,actor_guides==HAILSTONE_MAP_ACTOR_GUIDES);
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

bool unregister_in_nameplate = false;
bool nameplate_strides = false; // /probenameplate stride: also claim the second candidate with other command strides

void probe_nameplate_frame(void* userdata, const HailstoneNameplateFrame* frame) {
    if (!frame || frame->size < sizeof(*frame) || !frame->candidate_count) return;
    const auto& actor = *hailstone_nameplate_candidate(frame, 0);
    // The second candidate, found by the host's stride rather than by this SDK's sizeof.
    const auto* second = hailstone_nameplate_candidate(frame, 1);
    const bool stride = frame->candidate_size >= sizeof(HailstoneNameplateCandidate) &&
        HAILSTONE_NAMEPLATE_CANDIDATE_HAS(frame, cast_name) && second && second->spawn_id == 11 &&
        !(second->flags & HAILSTONE_NAMEPLATE_TARGET) && !hailstone_nameplate_candidate(frame, frame->candidate_count);
    const bool snapshot = actor.spawn_id == 10 && actor.incarnation == 17 &&
        (actor.flags & HAILSTONE_NAMEPLATE_TARGET) && (actor.flags & HAILSTONE_NAMEPLATE_INVISIBLE_UNDEAD) &&
        (actor.flags & HAILSTONE_NAMEPLATE_MANA_KNOWN) && !(actor.flags & HAILSTONE_NAMEPLATE_ENDURANCE_KNOWN) &&
        actor.mana_percent == 0 && std::string_view(actor.caption.data, actor.caption.bytes) == "Guard";
    HailstoneNameplateTextMetrics metrics{};
    metrics.size = sizeof(metrics);
    const int measured = host->nameplate_measure_text(host->host, "Guard", 5, 12, 0, &metrics);
    HailstoneNameplateImage image{};
    image.size = sizeof(image);
    HailstoneNameplateCommand commands[2]{};
    char label[] = "native";
    commands[0].kind = HAILSTONE_NAMEPLATE_TEXT;
    commands[0].color = 0x123456;
    commands[0].alpha = 1;
    commands[0].font_size = 12;
    commands[0].text = {label, sizeof(label) - 1};
    commands[1].kind = HAILSTONE_NAMEPLATE_IMAGE;
    commands[1].color = 0xffffff;
    commands[1].alpha = 1;
    commands[1].x1 = 4;
    commands[1].y1 = 4;
    commands[1].texture = 123;
    HailstoneNameplateModel model{};
    model.size = sizeof(model);
    model.flags = HAILSTONE_NAMEPLATE_CLICKABLE;
    model.frame = frame->frame;
    model.bounds = {-20, -10, 40, 20};
    model.commands = commands;
    model.command_count = 2;
    model.command_size = sizeof(commands[0]);
    const int no_image = host->nameplate_submit(host->host, &model);
    const int icon = host->nameplate_spell_icon(host->host, 37, &image);
    commands[1].texture = image.texture;
    commands[1].u0 = image.u0;
    commands[1].v0 = image.v0;
    commands[1].u1 = image.u1;
    commands[1].v1 = image.v1;
    ++model.frame;
    const int stale = host->nameplate_submit(host->host, &model);
    model.frame = frame->frame;
    const int accepted = host->nameplate_submit(host->host, &model);
    const int duplicate = host->nameplate_submit(host->host, &model);
    label[0] = 'X'; // the host must own its copy at the time of submit
    // A newer SDK's command record (extra bytes at its end, an unaligned stride) for the second candidate: the host
    // reads the fields it knows. A stride shorter than the host's record is refused.
    struct Wider {
        HailstoneNameplateCommand command;
        unsigned char extra[13];
    };
    unsigned char wide_bytes[2 * sizeof(Wider)]{};
    std::memset(wide_bytes, 0xa5, sizeof(wide_bytes));
    char wide_label[] = "wide";
    HailstoneNameplateCommand wide_command{};
    wide_command.kind = HAILSTONE_NAMEPLATE_TEXT;
    wide_command.color = 0x654321;
    wide_command.alpha = 1;
    wide_command.font_size = 12;
    wide_command.text = {wide_label, sizeof(wide_label) - 1};
    const std::uint32_t wide_stride = sizeof(HailstoneNameplateCommand) + 13;
    std::memcpy(wide_bytes, &wide_command, sizeof(wide_command));
    wide_command.color = 0x0000ff;
    std::memcpy(wide_bytes + wide_stride, &wide_command, sizeof(wide_command));
    HailstoneNameplateModel wide{};
    wide.size = sizeof(wide);
    wide.frame = frame->frame;
    wide.candidate_index = 1;
    wide.bounds = {-20, -10, 40, 20};
    wide.commands = wide_bytes;
    wide.command_count = 2;
    wide.command_size = sizeof(HailstoneNameplateCommand) - 4;
    int narrow = -1, zero = -1, wider = -1;
    if (nameplate_strides) {
        narrow = host->nameplate_submit(host->host, &wide);
        wide.command_size = 0;
        zero = host->nameplate_submit(host->host, &wide);
        wide.command_size = wide_stride;
        wider = host->nameplate_submit(host->host, &wide);
    }
    WriteChatf("[Probe] nameplate data %d measure %d width %.0f icon %d no_image %d wrong_frame %d accepted %d duplicate %d",
               snapshot, measured, static_cast<double>(metrics.width), icon, no_image, stale, accepted, duplicate);
    WriteChatf("[Probe] nameplate stride %d narrow %d zero %d wider %d", stride, narrow, zero, wider);
    if (*static_cast<const bool*>(userdata)) host->nameplate_unregister(host->host);
}

void probe_nameplate(void*, const char* args) {
    if (!args || !HAILSTONE_HOST_HAS(host, nameplate_spell_icon)) return;
    if (std::strcmp(args, "off") == 0) {
        WriteChatf("[Probe] nameplate unregister %d", host->nameplate_unregister(host->host));
        return;
    }
    if (std::strcmp(args, "outside") == 0) {
        HailstoneNameplateModel model{};
        model.size = sizeof(model);
        HailstoneNameplateTextMetrics metrics{};
        metrics.size = sizeof(metrics);
        HailstoneNameplateImage image{};
        image.size = sizeof(image);
        WriteChatf("[Probe] nameplate outside %d %d %d", host->nameplate_submit(host->host, &model),
                   host->nameplate_measure_text(host->host, "Guard", 5, 12, 0, &metrics),
                   host->nameplate_spell_icon(host->host, 37, &image));
        return;
    }
    unregister_in_nameplate = std::strcmp(args, "self-retire") == 0;
    nameplate_strides = std::strcmp(args, "stride") == 0;
    const int registered = host->nameplate_register(host->host, probe_nameplate_frame, &unregister_in_nameplate);
    WriteChatf("[Probe] nameplate register %d", registered);
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
// count and total, the bytes, the records read and the bytes left over.
void probe_abilities(void*, const char* args) {
    if (!HAILSTONE_HOST_HAS(host, get_ability_data_bulk) || !HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] abilities: not in this client");
        return;
    }
    const BulkProbe probe("abilities", size_arg(args));
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r = host->get_ability_data_bulk(host->host, buf, probe.n);
    probe.past_end();
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

static_assert(sizeof(HS_Bulk_SpellFixedRecord) == HS_BULK_SPELL_RECORD_SIZE);
static_assert(sizeof(HS_Bulk_SpellSlot) == HS_BULK_SPELL_SLOT_SIZE);

// A uint16_t-prefixed string at buf + at, within size; false when it runs past the end.
bool read_str16(const unsigned char* buf, std::size_t size, std::size_t& at, std::string& out) {
    std::uint16_t len = 0;
    if (size - at < sizeof len) return false;
    std::memcpy(&len, buf + at, sizeof len);
    at += sizeof len;
    if (len > size - at) return false;
    out.assign(reinterpret_cast<const char*>(buf + at), len);
    at += len;
    return true;
}

// The records as /probespells prints them (see probe_spells): the `r` bytes the host wrote into `buf`.
void print_spells(const unsigned char* buf, std::int32_t r) {
    const std::size_t size = r > 0 ? static_cast<std::size_t>(r) : 0;
    if (size < HS_BULK_SPELL_HEADER_SIZE) {
        WriteChatf("[Probe] spells %d bytes", static_cast<int>(r));
        return;
    }
    std::int32_t count = 0;
    std::memcpy(&count, buf, sizeof count);
    std::size_t at = HS_BULK_SPELL_HEADER_SIZE;
    for (std::int32_t i = 0; i < count; ++i) {
        if (size - at < sizeof(HS_Bulk_SpellFixedRecord)) break;
        const auto* rec = reinterpret_cast<const HS_Bulk_SpellFixedRecord*>(buf + at);
        const std::size_t recordSize = rec->recordSize > 0 ? static_cast<std::size_t>(rec->recordSize) : 0;
        if (recordSize < sizeof(HS_Bulk_SpellFixedRecord) || recordSize > size - at) break;
        const std::size_t end = at + recordSize;
        std::size_t p = at + sizeof(HS_Bulk_SpellFixedRecord);
        std::string name;
        std::string description;
        std::string names[5]; // targetTypeDesc, resistTypeDesc, category, subcategory, subcategory2
        std::vector<std::string> lines;
        std::vector<HS_Bulk_SpellSlot> slots;
        std::vector<std::uint8_t> classLevels;
        if (rec->found) {
            // slotCount slots, slotSize bytes apart: copy the fields this SDK knows, skip any a later client added.
            const std::size_t slotSize = rec->slotSize > 0 ? static_cast<std::size_t>(rec->slotSize) : 0;
            for (std::int32_t k = 0; k < rec->slotCount && slotSize > 0 && end - p >= slotSize; ++k) {
                HS_Bulk_SpellSlot slot{};
                std::memcpy(&slot, buf + p, slotSize < sizeof slot ? slotSize : sizeof slot);
                slots.push_back(slot);
                p += slotSize;
            }
            // classCount levels, one byte each, class 1 first.
            for (std::int32_t k = 0; k < rec->classCount && p < end; ++k) classLevels.push_back(buf[p++]);
            std::string text;
            // stringCount strings: the ones this SDK knows, then any a later client appended, skipped.
            for (int k = 0; k < rec->stringCount; ++k) {
                if (!read_str16(buf, end, p, text)) break;
                if (k == 0) name = text;
                if (k == 8) description = text;
                if (k >= 9 && k <= 13) names[k - 9] = text;
            }
            for (std::int32_t k = 0; k < rec->effectCount; ++k) {
                if (!read_str16(buf, end, p, text)) break;
                lines.push_back(text);
            }
        }
        WriteChatf("[Probe] spell %d: id %d, found %d, %zu bytes, name %s, mana %d, target %d, flags %u, slots %d, "
                   "classes %d, strings %d, effects %d",
                   static_cast<int>(i), rec->id, rec->found, recordSize, name.empty() ? "-" : name.c_str(), rec->mana,
                   static_cast<int>(rec->targetType), rec->flags, rec->slotCount, rec->classCount, rec->stringCount,
                   rec->effectCount);
        for (std::size_t k = 0; k < classLevels.size(); ++k)
            if (classLevels[k] != 0 && classLevels[k] != 255)
                WriteChatf("[Probe] spell %d class %zu: level %d", static_cast<int>(i), k + 1,
                           static_cast<int>(classLevels[k]));
        for (std::size_t k = 0; k < slots.size(); ++k)
            if (slots[k].effect != 254)
                WriteChatf("[Probe] spell %d slot %zu: effect %d, base %d, base2 %d, max %d, formula %d",
                           static_cast<int>(i), k + 1, slots[k].effect, slots[k].base, slots[k].base2, slots[k].max,
                           slots[k].formula);
        if (rec->found)
            WriteChatf("[Probe] spell %d description %d: %s", static_cast<int>(i), rec->descriptionId,
                       description.empty() ? "-" : description.c_str());
        if (rec->found)
            WriteChatf("[Probe] spell %d target %s, resist %s, category %d %s, subcategory %d %s, subcategory2 %d %s",
                       static_cast<int>(i), names[0].c_str(), names[1].c_str(), rec->category, names[2].c_str(),
                       rec->subcategory, names[3].c_str(), rec->subcategory2, names[4].c_str());
        for (const std::string& line : lines) WriteChatf("[Probe] spell %d effect: %s", static_cast<int>(i), line.c_str());
        if (p != end) WriteChatf("[Probe] spell %d: %zu bytes unread", static_cast<int>(i), end - p);
        at = end;
    }
    WriteChatf("[Probe] spells count %d, %d bytes, %zu bytes left", static_cast<int>(count), static_cast<int>(r),
               size - at);
}

// The count, then each record one line (id, found, its size, name, mana, target type, flags, slots, classes,
// strings, effect lines), each used effect slot (SPA not 254, read stepping by slotSize), each class that can use
// it (its level not 0 or 255), its
// description (the ninth string), the target type's, resist type's and categories' names (the tenth to fourteenth)
// and each effect line, stepping by recordSize; then the bytes left over after the last record (0 when every record is framed
// right). The first number is the buffer size (at most the bulk buffer's), the rest the spell ids; also how many bytes
// past the first n the host changed.
void probe_spells(void*, const char* args) {
    if (!HAILSTONE_HOST_HAS(host, get_spells_data_bulk) || !HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] spells: not in this client");
        return;
    }
    std::vector<std::int32_t> nums;
    for (const char* p = args ? args : "";;) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p) break;
        nums.push_back(static_cast<std::int32_t>(v));
        p = end;
    }
    std::vector<std::int32_t> ids;
    if (nums.size() > 1) ids.assign(nums.begin() + 1, nums.end());
    const BulkProbe probe("spells", nums.empty() || nums[0] < 0 ? 0 : nums[0]);
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r =
        host->get_spells_data_bulk(host->host, ids.data(), static_cast<std::int32_t>(ids.size()), buf, probe.n);
    probe.past_end();
    print_spells(buf, r);
}

// /probespellsbyname [n] name: get_spells_by_name_data_bulk for the name (the rest of the line, spaces and all) into
// the bulk buffer, printed as /probespells prints its records. With n (a number first), only n bytes of it, and how
// many bytes past them the host changed; the whole buffer without.
void probe_spells_by_name(void*, const char* args) {
    if (!HAILSTONE_HOST_HAS(host, get_spells_by_name_data_bulk) || !HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] spells by name: not in this client");
        return;
    }
    const char* name = args ? args : "";
    while (*name == ' ') ++name;
    long long requested = -1;
    if (std::isdigit(static_cast<unsigned char>(*name))) {
        char* end = nullptr;
        requested = std::strtoll(name, &end, 10);
        name = end;
        while (*name == ' ') ++name;
    }
    const BulkProbe probe("spells", requested);
    if (!probe) return;
    unsigned char* buf = probe.buf;
    const std::int32_t r = host->get_spells_by_name_data_bulk(host->host, name, buf, probe.n);
    probe.past_end();
    print_spells(buf, r);
}

// The plugin's bulk buffer, under commands named after this copy of the plugin (/<name>buf...), so that a second copy
// loaded beside it can be asked from inside the first's command with DoCommand. Each line starts "[Probe] <name>".
//   buf              acquire, then acquire again (the same buffer?); kept, never released here
//   bufwrite v       acquire, set the first and last 16 bytes to v, release
//   bufread          acquire, print the first and last byte, release (what this plugin last left: never zeroed)
//   bufrelease       acquire, release
//   bufhold v cmds   acquire, set the first and last 16 bytes to v, run each of cmds (split at ';'), then whether a
//                    fresh acquire gives the same buffer and its first and last byte; release
//   bufpass cmd      acquire, release, run cmd
//   bufthread        acquire from another thread
std::string own_name() { return host->name(host->host); }
std::string own_command(const char* what) {
    std::string c = "/" + own_name() + what;
    for (char& ch : c) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return c;
}
const char* const buffer_commands[] = {"buf", "bufwrite", "bufread", "bufrelease", "bufhold", "bufpass", "bufthread"};

void print_acquired(const char* what, const unsigned char* buf, std::int32_t size) {
    if (buf) WriteChatf("[Probe] %s %s: %d bytes", own_name().c_str(), what, static_cast<int>(size));
    else WriteChatf("[Probe] %s %s: none, size %d", own_name().c_str(), what, static_cast<int>(size));
}

void probe_buffer(void* user, const char* args) {
    const std::string what = static_cast<const char*>(user);
    const std::string name = own_name();
    if (!HAILSTONE_HOST_HAS(host, release_bulk_buffer)) {
        WriteChatf("[Probe] %s buffer: not in this client", name.c_str());
        return;
    }
    const char* rest = args ? args : "";
    while (*rest == ' ') ++rest;
    std::int32_t size = -1;
    if (what == "bufthread") {
        unsigned char sentinel = 0;
        unsigned char* got = &sentinel; // the call must set it: null off the client's thread
        std::thread([&] { got = host->acquire_bulk_buffer(host->host, &size); }).join();
        print_acquired("thread", got, size);
        return;
    }
    unsigned char* buf = host->acquire_bulk_buffer(host->host, &size);
    print_acquired("acquire", buf, size);
    if (!buf) return;
    const std::size_t n = static_cast<std::size_t>(size);
    if (what == "buf") {
        std::int32_t again_size = -1;
        const unsigned char* again = host->acquire_bulk_buffer(host->host, &again_size);
        WriteChatf("[Probe] %s again: %s, %d bytes", name.c_str(), again == buf ? "same" : "other",
                   static_cast<int>(again_size));
        return; // kept: the host ends the hold when this command returns
    }
    if (what == "bufwrite" || what == "bufhold") {
        char* end = nullptr;
        const auto v = static_cast<unsigned char>(std::strtoul(rest, &end, 10));
        rest = end;
        std::memset(buf, v, 16);
        std::memset(buf + n - 16, v, 16);
        WriteChatf("[Probe] %s wrote %d", name.c_str(), static_cast<int>(v));
    }
    if (what == "bufread") {
        WriteChatf("[Probe] %s read %d %d", name.c_str(), static_cast<int>(buf[0]), static_cast<int>(buf[n - 1]));
    } else if (what == "bufhold") {
        for (std::string cmds = rest; !cmds.empty();) {
            const std::size_t cut = cmds.find(';');
            std::string cmd = cmds.substr(0, cut);
            cmds = cut == std::string::npos ? std::string() : cmds.substr(cut + 1);
            while (!cmd.empty() && cmd.front() == ' ') cmd.erase(cmd.begin());
            if (!cmd.empty()) DoCommand(cmd.c_str());
        }
        std::int32_t again_size = -1;
        const unsigned char* again = host->acquire_bulk_buffer(host->host, &again_size);
        WriteChatf("[Probe] %s still: %s, read %d %d", name.c_str(), again == buf ? "same" : "other",
                   static_cast<int>(buf[0]), static_cast<int>(buf[n - 1]));
    } else if (what == "bufpass") {
        host->release_bulk_buffer(host->host);
        DoCommand(rest);
        return;
    }
    host->release_bulk_buffer(host->host);
}

int probe_tlo(void*, const char*, const char* member, const char*, HailstoneValue* out) {
    if (member && std::strcmp(member, "Throw") == 0) throw std::runtime_error("the TLO threw");
    out->kind = HAILSTONE_STRING;
    out->s = "probe";
    return 1;
}

} // namespace

HAILSTONE_API void InitializePlugin(void) {
    AddCommand("/probedialog", probe_dialog, nullptr);
    AddCommand("/probedialoganswer", probe_dialog_answer, nullptr);
    AddCommand("/probedialogactive", probe_dialog_active, nullptr);
    AddCommand("/probedialogthread", probe_dialog_thread, nullptr);
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
    AddCommand("/probenameplate", probe_nameplate, nullptr);
    AddCommand("/probeability", probe_ability, nullptr);
    AddCommand("/probeabilities", probe_abilities, nullptr);
    AddCommand("/probespells", probe_spells, nullptr);
    AddCommand("/probespellsbyname", probe_spells_by_name, nullptr);
    for (const char* what : buffer_commands)
        AddCommand(own_command(what).c_str(), probe_buffer, const_cast<char*>(what));
    AddTopLevelObject("Probe", probe_tlo, nullptr);
#ifdef HAILSTONE_PROBE_THROW_INIT
    throw std::runtime_error("InitializePlugin threw");
#endif
}
HAILSTONE_API void ShutdownPlugin(void) {
    RemoveCommand("/probedialog");
    RemoveCommand("/probedialoganswer");
    RemoveCommand("/probedialogactive");
    RemoveCommand("/probedialogthread");
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
    RemoveCommand("/probenameplate");
    RemoveCommand("/probeability");
    RemoveCommand("/probeabilities");
    for (const char* what : buffer_commands) RemoveCommand(own_command(what).c_str());
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

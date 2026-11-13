// A compiled test plugin (tests/native_plugins_test.cpp), in C++ against the SDK alone: it calls the host table from
// where a plugin should not, to check that the client holds.
//   /probethread   calls host functions from a std::thread, then reports what they answered
//   /probethrow    lets an exception out (not a std::exception); so do ${Probe.Throw} and OnUpdateImGui
//   /probestate n  web_state(tab 1) with a struct whose size says n bytes, as a plugin built with a shorter one
//   /probemessage n  web_message(tab 1) into a buffer of n bytes (0: no buffer)
//   /probetcp port    tcp_open("127.0.0.1", port); /probetcpsend h  tcp_send(h, "hello") and tcp_state(h)
//   /probebuffs [n], /probepetbuffs [n], /probespawns [n]  get_buff_data_bulk / get_pet_buff_data_bulk /
//   get_spawns_data_bulk into n bytes (the whole buffer without n), read with ByteReader
// Built again with HAILSTONE_PROBE_THROW_INIT (HostProbeThrow): InitializePlugin registers a command and a TLO, then
// throws.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "hailstone_byte_io.h"
#include "hailstone_plugin.h"

HAILSTONE_PLUGIN

namespace {

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

// A bulk read's buffer size: the command's number, at most `whole`; `whole` without one.
std::int32_t size_arg(const char* args, std::size_t whole) {
    const std::size_t n = args && *args ? std::strtoul(args, nullptr, 10) : whole;
    return static_cast<std::int32_t>(n < whole ? n : whole);
}

// The occupied slots one line each; the empty ones only counted, and only when laid out as empty.
void probe_buffs(void*, const char* args) {
    unsigned char buf[4096]; // at least 62 * HAILSTONE_BUFF_RECORD_SIZE (1798), or nothing comes back
    const std::int32_t r = host->get_buff_data_bulk(host->host, buf, size_arg(args, sizeof buf));
    hailstone::ByteReader rd(buf, r > 0 ? static_cast<std::size_t>(r) : 0);
    int empty = 0;
    for (int slot = 0; rd.remaining() >= HAILSTONE_BUFF_RECORD_SIZE; ++slot) {
        const auto spell = rd.le<std::int32_t>(), caster = rd.le<std::int32_t>(), tics = rd.le<std::int32_t>(),
            hits = rd.le<std::int32_t>(), type = rd.le<std::int32_t>(), counter = rd.le<std::int32_t>(),
            base = rd.le<std::int32_t>();
        const auto song = rd.le<std::uint8_t>();
        if (spell == 0 || spell == -1) { // an empty slot: the client's empty spell id goes out as it is (-1)
            ++empty;
            continue;
        }
        WriteChatf("[Probe] buff %d: %d %d %d %d %d %d %d %u", slot, static_cast<int>(spell),
            static_cast<int>(caster), static_cast<int>(tics), static_cast<int>(hits), static_cast<int>(type),
            static_cast<int>(counter), static_cast<int>(base), static_cast<unsigned>(song));
    }
    WriteChatf("[Probe] buffs %d bytes, %d empty", static_cast<int>(r), empty);
}

void probe_pet_buffs(void*, const char* args) {
    unsigned char buf[1024];
    const std::int32_t r = host->get_pet_buff_data_bulk(host->host, buf, size_arg(args, sizeof buf));
    hailstone::ByteReader rd(buf, r > 0 ? static_cast<std::size_t>(r) : 0);
    while (rd.remaining() >= HAILSTONE_PET_BUFF_RECORD_SIZE) {
        const auto spell = rd.le<std::int32_t>(), tics = rd.le<std::int32_t>(), type = rd.le<std::int32_t>();
        WriteChatf("[Probe] pet buff: %d %d %d", static_cast<int>(spell), static_cast<int>(tics),
            static_cast<int>(type));
    }
    WriteChatf("[Probe] pet buffs %d bytes", static_cast<int>(r));
}

// The header, then each spawn one line (id, level, class, race, HP percent, name), then the bytes left over after
// the records the header counts (0 when every record's strings are framed right).
void probe_spawns(void*, const char* args) {
    static unsigned char buf[1024*1024];
    const std::int32_t r = host->get_spawns_data_bulk(host->host, buf, size_arg(args, sizeof buf));
    if (r <= 0) {
        WriteChatf("[Probe] spawns %d bytes", static_cast<int>(r));
        return;
    }
    hailstone::ByteReader rd(buf, static_cast<std::size_t>(r));
    const auto count = rd.le<std::uint32_t>();
    const float px = rd.f32(), py = rd.f32(), pz = rd.f32();
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t start = rd.position();
        const auto id = rd.le<std::int32_t>();
        rd.seek(start + 16);
        const auto class_id = rd.le<std::int32_t>();
        rd.seek(start + 60);
        const auto level = rd.le<std::int32_t>();
        rd.seek(start + 84);
        const auto pct = rd.le<std::int64_t>();
        rd.seek(start + 104);
        const auto race = rd.le<std::int32_t>();
        rd.seek(start + 128); // the fixed part's end: then BodyDesc, CleanName, DisplayName, Name, RaceName, Suffix, TypeDesc
        std::string name;
        for (int s = 0; s < 7; ++s) {
            const auto len = rd.le<std::uint16_t>();
            const auto text = rd.bytes(len);
            if (s == 3) name.assign(reinterpret_cast<const char*>(text.data()), text.size());
        }
        WriteChatf("[Probe] spawn %d: %d %d %d %lld %s", static_cast<int>(id), static_cast<int>(level),
                   static_cast<int>(class_id), static_cast<int>(race), static_cast<long long>(pct), name.c_str());
    }
    WriteChatf("[Probe] spawns %u, player %d %d %d, %d bytes left", static_cast<unsigned>(count), static_cast<int>(px),
               static_cast<int>(py), static_cast<int>(pz), static_cast<int>(rd.remaining()));
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
    AddCommand("/probepetbuffs", probe_pet_buffs, nullptr);
    AddCommand("/probespawns", probe_spawns, nullptr);
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
    RemoveCommand("/probepetbuffs");
    RemoveCommand("/probespawns");
    RemoveTopLevelObject("Probe");
}
HAILSTONE_API void OnUpdateImGui(void) { throw std::runtime_error("OnUpdateImGui threw"); }

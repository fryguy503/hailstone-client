#include "mq/tlo.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>

#include "core/float_chars.h"
#include "game/item_links.h"
#include "mq/calc.h"
#include "net/game_connection.h"
#include "plugin/policy.h"

namespace hailstone::mq {

namespace {

char low(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string_view trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = low(c);
    return out;
}

// MQ's IsNumberToComma: digits and dots up to a comma (not first) or the end; empty is not.
bool number_to_comma(std::string_view s) {
    if (s.empty()) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if ((c >= '0' && c <= '9') || c == '.') continue;
        return c == ',' && i != 0;
    }
    return true;
}

// MQ's PrettifyNumber (GetNumberFormat, invariant locale): the whole part of "-1234567.50" grouped by thousands
// with ',' ("-1,234,567.50").
std::string prettify(std::string text) {
    const std::size_t digits = !text.empty() && text[0] == '-' ? 1 : 0;
    std::size_t end = text.find('.');
    if (end == std::string::npos) end = text.size();
    for (std::size_t at = end; at > digits + 3; at -= 3) text.insert(at - 3, 1, ',');
    return text;
}

} // namespace

bool CaseLess::operator()(std::string_view a, std::string_view b) const {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](char x, char y) { return low(x) < low(y); });
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return low(x) == low(y); });
}

bool icontains(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char x, char y) { return low(x) == low(y); });
    return it != haystack.end();
}

std::vector<std::string> split_index(std::string_view index) {
    std::vector<std::string> out;
    if (trim(index).empty()) return out;
    while (true) {
        std::size_t comma = index.find(',');
        out.emplace_back(trim(index.substr(0, comma)));
        if (comma == std::string_view::npos) break;
        index.remove_prefix(comma + 1);
    }
    return out;
}

bool parse_int(std::string_view text, std::int64_t& out) {
    text = trim(text);
    if (text.empty()) return false;
    auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && p == text.data() + text.size();
}

bool parse_float(std::string_view text, double& out) {
    text = trim(text);
    if (text.empty()) return false;
    auto [p, ec] = float_from_chars(text.data(), text.data() + text.size(), out); // (core/float_chars.h: Xcode 15)
    return ec == std::errc() && p == text.data() + text.size();
}

std::optional<std::int64_t> whole_number(double d) {
    if (!(d >= -0x1p63 && d < 0x1p63)) return std::nullopt; // NaN fails both
    return static_cast<std::int64_t>(d);
}

Registry::Registry() {
    // MQ's pure types and their common members.
    Type& t_int = type("int");
    Type& t_float = type("float");
    Type& t_string = type("string");
    Type& t_bool = type("bool");
    int_ = &t_int;
    float_ = &t_float;
    string_ = &t_string;
    bool_ = &t_bool;
    // MQ's int and int64 are one type here (Value holds 64 bits): a value that fits 32 bits has int's members
    // (Hex as "0x%X", LowPart and HighPart its 16-bit halves), a larger one int64's (Hex as "0x%llX", the 32-bit
    // halves). MQ2BasicTypes.cpp is the specification.
    const auto fits32 = [](std::int64_t i) { return i >= INT32_MIN && i <= INT32_MAX; };
    t_int.members["Float"] = [](const Context&, const Value& v, std::string_view) {
        return Value::number(static_cast<double>(static_cast<float>(v.i)));
    };
    t_int.members["Double"] = [](const Context&, const Value& v, std::string_view) {
        return Value::number(static_cast<double>(v.i));
    };
    t_int.members["Hex"] = [fits32](const Context&, const Value& v, std::string_view) {
        if (fits32(v.i)) return Value::string(std::format("0x{:X}", static_cast<std::uint32_t>(v.i)));
        return Value::string(std::format("0x{:X}", static_cast<std::uint64_t>(v.i)));
    };
    t_int.members["Reverse"] = [](const Context&, const Value& v, std::string_view) {
        auto u = static_cast<std::uint32_t>(v.i);
        return Value::integer((u >> 24) | ((u >> 8) & 0xff00) | ((u << 8) & 0xff0000) | (u << 24));
    };
    t_int.members["LowPart"] = [fits32](const Context&, const Value& v, std::string_view) {
        const auto u = static_cast<std::uint64_t>(v.i);
        return Value::integer(static_cast<std::int64_t>(fits32(v.i) ? (u & 0xffffu) : (u & 0xffffffffu)));
    };
    t_int.members["HighPart"] = [fits32](const Context&, const Value& v, std::string_view) {
        const auto u = static_cast<std::uint64_t>(v.i);
        return Value::integer(static_cast<std::int64_t>(fits32(v.i) ? ((u >> 16) & 0xffffu) : (u >> 32)));
    };
    t_int.members["Prettify"] = [](const Context&, const Value& v, std::string_view index) {
        const int decimals = std::clamp(int_from_text(index, 0), 0, 9);
        std::string text = std::to_string(v.i);
        if (decimals) text += "." + std::string(static_cast<std::size_t>(decimals), '0');
        return Value::string(prettify(std::move(text)));
    };
    t_float.members["Int"] = [](const Context&, const Value& v, std::string_view) {
        const auto n = whole_number(v.f);
        return n ? Value::integer(*n) : Value::nil();
    };
    t_float.members["Deci"] = [](const Context&, const Value& v, std::string_view) {
        return Value::string(std::format("{:.1f}", v.f));
    };
    t_float.members["Centi"] = [](const Context&, const Value& v, std::string_view) {
        return Value::string(std::format("{:.2f}", v.f));
    };
    t_float.members["Milli"] = [](const Context&, const Value& v, std::string_view) {
        return Value::string(std::format("{:.3f}", v.f));
    };
    t_float.members["Precision"] = [](const Context&, const Value& v, std::string_view index) {
        std::int64_t n = 0;
        if (!parse_int(index, n)) return Value::nil(); // MQ: NULL unless the index is a number
        return Value::string(std::format("{:.{}f}", v.f, static_cast<int>(std::clamp<std::int64_t>(n, 0, 12))));
    };
    t_float.members["Raw"] = [](const Context&, const Value& v, std::string_view) {
        float f = static_cast<float>(v.f);
        std::uint32_t bits;
        std::memcpy(&bits, &f, 4);
        return Value::integer(bits);
    };
    // MQ's float and double Prettify print the value's bits as an int64 (VarPtr.Int64) before grouping them: a
    // deliberate difference, here the number itself is grouped, with 2 decimals unless the index says.
    t_float.members["Prettify"] = [](const Context&, const Value& v, std::string_view index) {
        const int decimals = std::clamp(int_from_text(index, 2), 0, 9);
        if (!std::isfinite(v.f)) return Value::string(std::format("{:.{}f}", v.f, decimals));
        return Value::string(prettify(std::format("{:.{}f}", v.f, decimals)));
    };

    // MQ's string members (MQ2StringType::GetMember). A member that needs an index is NULL without one.
    t_string.members["Length"] = [](const Context&, const Value& v, std::string_view) {
        return Value::integer(static_cast<std::int64_t>(v.s.size()));
    };
    t_string.members["Upper"] = [](const Context&, const Value& v, std::string_view) {
        std::string s = v.s;
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return Value::string(s);
    };
    t_string.members["Lower"] = [](const Context&, const Value& v, std::string_view) { return Value::string(lower(v.s)); };
    // Left[n]: the first n characters; Left[-n] all but the last n. 0 (or no number) is NULL.
    t_string.members["Left"] = [](const Context&, const Value& v, std::string_view index) {
        const std::int64_t n = int_from_text(index, 0);
        if (n == 0) return Value::nil();
        const auto len = static_cast<std::int64_t>(v.s.size());
        const std::int64_t keep = n > 0 ? std::min(n, len) : std::max<std::int64_t>(0, len + n);
        return Value::string(v.s.substr(0, static_cast<std::size_t>(keep)));
    };
    // Right[n]: the last n characters; Right[-n] all but the first n.
    t_string.members["Right"] = [](const Context&, const Value& v, std::string_view index) {
        const std::int64_t n = int_from_text(index, 0);
        if (n == 0) return Value::nil();
        const auto len = static_cast<std::int64_t>(v.s.size());
        const std::int64_t keep = n > 0 ? std::min(n, len) : std::max<std::int64_t>(0, len + n);
        return Value::string(v.s.substr(static_cast<std::size_t>(len - keep)));
    };
    // Mid[start,length]: from the 1-based start (below 1 is 1); a length missing, negative or past the end is all.
    t_string.members["Mid"] = [](const Context&, const Value& v, std::string_view index) {
        const auto comma = index.find(',');
        if (comma == std::string_view::npos) return Value::nil();
        const auto len = static_cast<std::int64_t>(v.s.size());
        const std::int64_t start = std::max<std::int64_t>(0, std::int64_t{int_from_text(index.substr(0, comma), 0)} - 1);
        std::int64_t n = int64_from_text(index.substr(comma + 1), len);
        if (start >= len) return Value::string("");
        if (n < 0 || n > len) n = len;
        return Value::string(v.s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(n)));
    };
    t_string.members["Find"] = [](const Context&, const Value& v, std::string_view index) {
        if (index.empty()) return Value::nil();
        std::string hay = lower(v.s), needle = lower(index);
        auto at = hay.find(needle);
        return at == std::string::npos ? Value::nil() : Value::integer(static_cast<std::int64_t>(at) + 1);
    };
    // Compare / CompareCS: below, at or above 0 as the string sorts before, with or after the index (MQ's _stricmp /
    // strcmp). Only the sign is MQ's; the size is 1 here.
    t_string.members["Compare"] = [](const Context&, const Value& v, std::string_view index) {
        if (index.empty()) return Value::nil();
        const int r = lower(v.s).compare(lower(index));
        return Value::integer(r < 0 ? -1 : r > 0 ? 1 : 0);
    };
    t_string.members["CompareCS"] = [](const Context&, const Value& v, std::string_view index) {
        if (index.empty()) return Value::nil();
        const int r = std::string_view(v.s).compare(index);
        return Value::integer(r < 0 ? -1 : r > 0 ? 1 : 0);
    };
    t_string.members["Equal"] = [](const Context&, const Value& v, std::string_view index) {
        return index.empty() ? Value::nil() : Value::boolean(iequals(v.s, index));
    };
    t_string.members["NotEqual"] = [](const Context&, const Value& v, std::string_view index) {
        return index.empty() ? Value::nil() : Value::boolean(!iequals(v.s, index));
    };
    t_string.members["EqualCS"] = [](const Context&, const Value& v, std::string_view index) {
        return index.empty() ? Value::nil() : Value::boolean(v.s == index);
    };
    t_string.members["NotEqualCS"] = [](const Context&, const Value& v, std::string_view index) {
        return index.empty() ? Value::nil() : Value::boolean(v.s != index);
    };
    // Arg[n] / Arg[n,c]: MQ's GetArg, by spaces and tabs or by the character after the comma; empty is NULL.
    t_string.members["Arg"] = [](const Context&, const Value& v, std::string_view index) {
        if (!number_to_comma(index)) return Value::nil();
        const auto comma = index.find(',');
        const char sep = comma != std::string_view::npos && comma + 1 < index.size() ? index[comma + 1] : '\0';
        std::string arg = get_arg(v.s, int_from_text(index.substr(0, comma), 0), false, sep);
        return arg.empty() ? Value::nil() : Value::string(std::move(arg));
    };
    t_string.members["Count"] = [](const Context&, const Value& v, std::string_view index) {
        if (index.empty()) return Value::nil();
        return Value::integer(std::count(v.s.begin(), v.s.end(), index[0]));
    };
    // Token[n,c]: the n-th field between the characters c. MQ looks for each next c from one past the last one,
    // so in a string that starts with c field 2 is the text after its second c (",a,b": Token[2,,] is "b").
    t_string.members["Token"] = [](const Context&, const Value& v, std::string_view index) {
        if (!number_to_comma(index)) return Value::nil();
        const auto comma = index.find(',');
        if (comma == std::string_view::npos || comma + 1 >= index.size()) return Value::nil();
        const char sep = index[comma + 1];
        const int n = int_from_text(index.substr(0, comma), 0);
        if (n < 1) return Value::nil();
        std::size_t pos = 0;
        for (int k = 1; k < n; ++k) {
            pos = v.s.find(sep, pos + 1);
            if (pos == std::string::npos) return Value::nil();
        }
        if (pos != 0) ++pos;
        const auto end = v.s.find(sep, pos);
        return Value::string(v.s.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
    };
    // Replace[search,replacement]: every search (case-sensitive) replaced, left to right. The result is cut to
    // kMaxReplaced bytes (an MQ string's room, MAX_STRING less its terminator): nested in itself, a Replace that
    // makes its text longer would otherwise grow it exponentially (${String[${String[...].Replace[a,aaaa]}]...}).
    t_string.members["Replace"] = [](const Context&, const Value& v, std::string_view index) {
        constexpr std::size_t kMaxReplaced = 2047;
        const auto comma = index.find(',');
        if (comma == std::string_view::npos) return Value::nil();
        const std::string_view search = index.substr(0, comma), with = index.substr(comma + 1);
        if (v.s.empty() || search.empty()) return Value::nil();
        std::string out;
        std::size_t from = 0;
        for (std::size_t at = v.s.find(search); at != std::string::npos && out.size() < kMaxReplaced;
             at = v.s.find(search, from)) {
            out.append(v.s, from, at - from);
            out += with;
            from = at + search.size();
        }
        if (out.size() < kMaxReplaced) out.append(v.s, from, kMaxReplaced - out.size());
        if (out.size() > kMaxReplaced) out.resize(kMaxReplaced);
        return Value::string(std::move(out));
    };
    t_string.members["StripLinks"] = [](const Context&, const Value& v, std::string_view) {
        return Value::string(game::strip_links(v.s));
    };
}

Registry::~Registry() = default;

Type& Registry::type(const std::string& name) {
    auto it = types_.find(name);
    if (it != types_.end()) return *it->second;
    auto t = std::make_unique<Type>();
    t->name = name;
    Type& ref = *t;
    types_.emplace(name, std::move(t));
    return ref;
}

const Type* Registry::find_type(std::string_view name) const {
    auto it = types_.find(name);
    return it == types_.end() ? nullptr : it->second.get();
}

void Registry::add_tlo(std::string name, TloFn fn, std::string_view description, TloOrigin origin) {
    TloRegistration entry{std::move(fn), std::string(description), origin};
    const auto [it, inserted] = tlos_.try_emplace(std::move(name), std::move(entry));
    if (inserted) {
        ++tlo_catalog_serial_;
    } else {
        if (it->second.description != description || it->second.origin != origin) ++tlo_catalog_serial_;
        it->second = std::move(entry);
    }
}

void Registry::remove_tlo(std::string_view name) {
    auto it = tlos_.find(name);
    if (it != tlos_.end()) {
        tlos_.erase(it);
        ++tlo_catalog_serial_;
    }
}

bool Registry::has_tlo(std::string_view name) const { return tlos_.find(name) != tlos_.end(); }

std::vector<std::string> Registry::tlos() const {
    std::vector<std::string> out;
    out.reserve(tlos_.size());
    for (const auto& entry : tlos_) out.push_back(entry.first);
    return out;
}

std::vector<TloCatalogEntry> Registry::tlo_catalog() const {
    std::vector<TloCatalogEntry> out;
    out.reserve(tlos_.size());
    for (const auto& [name, entry] : tlos_) out.push_back({name, entry.description, entry.origin});
    return out;
}

std::vector<std::string> Registry::complete(const Context& ctx, std::string_view path) const {
    // The parts: name[index] separated by dots outside brackets.
    struct Part {
        std::string name, index;
    };
    std::vector<Part> parts(1);
    int depth = 0;
    for (char c : path) {
        if (c == '[' && depth++ == 0) continue;
        if (c == ']' && depth > 0 && --depth == 0) continue;
        if (depth > 0) parts.back().index += c;
        else if (c == '.') parts.emplace_back();
        else parts.back().name += c;
    }
    const std::string prefix = parts.back().name;
    auto starts = [&](std::string_view name) {
        if (name.size() < prefix.size()) return false;
        for (std::size_t i = 0; i < prefix.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(name[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
                return false;
        return true;
    };
    std::vector<std::string> out;
    if (parts.size() == 1) {
        for (const auto& entry : tlos_)
            if (starts(entry.first) && tlo_allowed(ctx, entry.first)) out.push_back(entry.first);
        return out;
    }
    // A blocked root does not get the fallback member completion of a temporarily absent object.
    if (!tlo_allowed(ctx, parts[0].name)) return out;
    Value v = tlo(ctx, parts[0].name, parts[0].index);
    const Type* t = nullptr;
    if (v.is_nil() && parts.size() == 2) {
        // The type a NULL TLO has when it is there.
        static constexpr std::pair<std::string_view, std::string_view> kTypes[] = {
            {"Me", "character"}, {"Target", "spawn"}, {"Spawn", "spawn"}, {"NearestSpawn", "spawn"}, {"Pet", "spawn"},
            {"Group", "group"}, {"Zone", "zone"}, {"Spell", "spell"}, {"Cursor", "item"}, {"FindItem", "item"}};
        for (auto [tlo_name, type_name] : kTypes)
            if (CaseLess{}(tlo_name, parts[0].name) == CaseLess{}(parts[0].name, tlo_name)) t = find_type(type_name);
    }
    for (std::size_t i = 1; i + 1 < parts.size() && !v.is_nil(); ++i) v = member(ctx, v, parts[i].name, parts[i].index);
    if (!t) {
        switch (v.kind) {
        case Value::Kind::Nil: return out;
        case Value::Kind::Bool: t = bool_; break;
        case Value::Kind::Int: t = int_; break;
        case Value::Kind::Float: t = float_; break;
        case Value::Kind::String: t = string_; break;
        case Value::Kind::Object: t = v.type; break;
        }
    }
    for (int d = 0; t && d < 8; ++d, t = t->parent)
        for (const auto& [name, fn] : t->members)
            if (starts(name)) out.push_back(name);
    std::sort(out.begin(), out.end(), CaseLess{});
    out.erase(std::unique(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
                  return !CaseLess{}(a, b) && !CaseLess{}(b, a);
              }),
              out.end());
    return out;
}

bool Registry::tlo_allowed(const Context& ctx, std::string_view name) const {
    if (!ctx.conn) return true;
    const auto& rules = ctx.conn->server_rules();
    if (!rules || rules->provisional || rules->tlos.allowed(name)) return true;
    const auto scoped = plugin::caller_tlo_proof();
    const auto proof = scoped ? scoped->lock() : plugin::TloCaller{};
    if (!proof) return false;
    const auto identity = proof->identity();
    return rules->tlos.allowed(name, identity) && plugin::addon_allowed(rules->addons, identity);
}

bool Registry::allow_tlo(const Context& ctx, std::string_view name) const {
    if (tlo_allowed(ctx, name)) return true;
    // Only the client's own thread tells (a lent thread's chat line could interleave with it); the notice is what
    // keeps a blocked root from reading as "no target" in a script that just sees nil.
    if (!denial_notice_ || !plugin::on_main_thread() || plugin::on_lent_thread()) return false;
    if (denials_conn_ != ctx.conn || denials_serial_ != ctx.conn->server_rules_serial()) {
        denials_told_.clear();
        denials_conn_ = ctx.conn;
        denials_serial_ = ctx.conn->server_rules_serial();
    }
    const auto it = tlos_.find(name);
    const std::string root(it != tlos_.end() ? std::string_view(it->first) : name);
    const std::string caller(plugin::caller_name());
    if (!denials_told_.emplace(caller, root).second) return false;
    // The command policy's blocked_message is for commands; this one names the root, as a script's author needs it.
    denial_notice_(caller.empty() ? std::format("[MQ] the server blocks TLO {}", root)
                                  : std::format("[MQ] {}: the server blocks TLO {}", caller, root));
    return false;
}

Value Registry::tlo(const Context& ctx, std::string_view name, std::string_view index) const {
    auto it = tlos_.find(name);
    if (it == tlos_.end() || !allow_tlo(ctx, it->first)) return Value::nil();
    return it->second.fn(ctx, index);
}

Value Registry::member(const Context& ctx, const Value& self, std::string_view name, std::string_view index) const {
    const Type* t = nullptr;
    switch (self.kind) {
    case Value::Kind::Nil: return Value::nil();
    case Value::Kind::Bool: t = bool_; break;
    case Value::Kind::Int: t = int_; break;
    case Value::Kind::Float: t = float_; break;
    case Value::Kind::String: t = string_; break;
    case Value::Kind::Object: t = self.type; break;
    }
    Value v = self;
    for (int depth = 0; t && depth < 8; ++depth) {
        auto it = t->members.find(name);
        if (it != t->members.end()) return it->second(ctx, v, index);
        if (t->any_member) {
            Value found = t->any_member(ctx, v, name, index);
            if (!found.is_nil()) return found;
        }
        if (t->convert) v = t->convert(ctx, v);
        if (v.is_nil()) return v;
        t = t->parent;
    }
    // An object without the member: its plain value may have it (Target.Name.Length).
    if (self.kind == Value::Kind::Object) {
        Value p = plain(ctx, self);
        if (p.kind != Value::Kind::Object && !p.is_nil()) return member(ctx, p, name, index);
    }
    return Value::nil();
}

Value Registry::plain(const Context& ctx, const Value& v) const {
    Value x = v;
    for (int depth = 0; x.kind == Value::Kind::Object && depth < 8; ++depth) {
        const Type* t = x.type;
        while (t && !t->to_value) t = t->parent;
        if (!t) return Value::string(x.type->name);
        x = t->to_value(ctx, x);
    }
    return x.kind == Value::Kind::Object ? Value::string(x.type->name) : x;
}

std::string Registry::text(const Context& ctx, const Value& v) const {
    Value p = plain(ctx, v);
    switch (p.kind) {
    case Value::Kind::Nil: return "NULL";
    case Value::Kind::Bool: return p.b ? "TRUE" : "FALSE";
    case Value::Kind::Int: return std::to_string(p.i);
    case Value::Kind::Float: return std::format("{:.2f}", p.f);
    case Value::Kind::String: return p.s;
    case Value::Kind::Object: break;
    }
    return "NULL";
}

} // namespace hailstone::mq

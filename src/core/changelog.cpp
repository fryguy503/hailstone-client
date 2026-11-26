#include "core/changelog.h"

namespace hailstone {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

void append(std::string& entry, std::string_view words) {
    for (char c : words)
        if (c != '`') entry += c;
}

} // namespace

std::string_view changelog_heading(std::string_view version) {
    const std::size_t dash = version.find('-');
    if (dash != std::string_view::npos && version.substr(dash + 1).starts_with("nightly")) return "Unreleased";
    return version;
}

std::vector<std::string> changelog_section(std::string_view text, std::string_view version) {
    std::vector<std::string> out;
    if (version.empty()) return out;
    bool inside = false;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line.starts_with("## ")) {
            if (inside) break;
            std::string_view heading = trim(line.substr(3));
            const std::size_t space = heading.find(' ');
            inside = heading.substr(0, space) == version;
            continue;
        }
        if (!inside || line.starts_with("# ")) continue;
        const std::string_view body = trim(line);
        if (body.empty()) continue;
        if (line.starts_with("- ") || line.starts_with("* ")) {
            out.emplace_back();
            append(out.back(), trim(line.substr(2)));
        } else if (!out.empty()) {
            out.back() += ' ';
            append(out.back(), body);
        } else {
            // A line before the first entry (an introduction) is an entry of its own.
            out.emplace_back();
            append(out.back(), body);
        }
    }
    return out;
}

} // namespace hailstone

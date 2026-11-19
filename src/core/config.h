#pragma once

// Small key=value settings file. Lines starting with '#' or ';' are comments;
// keys keep their insertion order when saved. Values are stored as text.
// Two clients may run at once with the same hailstone.ini: when the file changed on disk since this Config read or
// wrote it, save() keeps the other program's keys and writes only the keys set or erased here over them.
//
// Hailstone: a character's own settings. While a character layer is attached (attach_character()), every key but the
// shared ones (is_shared_key: the asset folder, add-ons, updates, the login, the window ...) is read from and written
// to the character's file, never hailstone.ini; the file starts as a copy of hailstone.ini's keys the first time the
// character enters. So clients run at once can each play with settings of their own (a lower render scale on a
// second character's client, say).

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hailstone {

class Config {
public:
    // Reads the file; returns false (and leaves the config empty) if it cannot be read.
    bool load(const std::filesystem::path& path);
    // Reads key=value lines from text (what load() does with the file's contents).
    void parse(std::string_view text);
    // Writes all keys, creating the parent directory, through a temporary file renamed over the old one (a crash
    // never leaves half a file). When `path` is the file last read or written and it changed since, the file's
    // keys are read first and only the ones set or erased since go over them; this config then holds the merged
    // keys. Returns false on failure.
    bool save(const std::filesystem::path& path);
    std::string to_string() const;

    std::optional<std::string> get(std::string_view key) const;
    std::string get_string(std::string_view key, std::string_view fallback = {}) const;
    int get_int(std::string_view key, int fallback) const;
    bool get_bool(std::string_view key, bool fallback) const;
    // The number the value starts with (strtof, so "1.5x" is 1.5); `fallback` for none, an empty value, NaN or an
    // infinity, which a clamp would not hold.
    float get_float(std::string_view key, float fallback) const;

    void set(std::string_view key, std::string_view value);
    void set_int(std::string_view key, int value) { set(key, std::to_string(value)); }
    void set_bool(std::string_view key, bool value) { set(key, value ? "1" : "0"); }
    void erase(std::string_view key);

    // A value for this run only (a command-line flag of the client: --lighting, --msaa ...): get() gives it over
    // hailstone.ini's and the character's, and save() never writes it. Setting or erasing the key (the Options
    // window) drops it, so the player's own change takes over and is saved as usual.
    void set_run_override(std::string_view key, std::string_view value);

    // The keys as get() sees them: with a character layer, hailstone.ini's shared keys and the character's others.
    std::vector<std::pair<std::string, std::string>> entries() const;

    // The character's settings file over this one (empty `path`: none, as detach_character()). Its keys are read; when
    // it cannot be read (the character's first entry), it starts with this config's keys that are not shared, and is
    // written at once. Saving the character's keys is save()'s, whatever path it is given. Returns false when the
    // file could not be read or written (the layer is attached all the same).
    bool attach_character(const std::filesystem::path& path);
    void detach_character() { layer_.reset(); layer_path_.clear(); }
    const std::filesystem::path& character_path() const { return layer_path_; }
    // The keys that stay in hailstone.ini with a character layer: those of the client, the login and the window, and
    // those taken only at start.
    static bool is_shared_key(std::string_view key);

private:
    Config* layer_for(std::string_view key) const;
    std::optional<std::string> stored(std::string_view key) const; // get() without the run's overrides
    void drop_run_override(std::string_view key);
    void store(std::string_view key, std::string_view value); // set() without dropping a run's override
    std::vector<std::pair<std::string, std::string>> entries_;
    std::vector<std::string> changed_;   // keys set (to a new value) or erased since the last load or save
    std::filesystem::path disk_path_;    // the file last read or written
    std::optional<std::string> disk_text_; // what it held then
    std::unique_ptr<Config> layer_;        // the character's settings (attach_character), none when no character
    std::filesystem::path layer_path_;
    std::vector<std::pair<std::string, std::string>> run_overrides_; // set_run_override's, never saved
};

// The settings file of the client: hailstone.ini in the data folder (platform::config_file()).
std::filesystem::path default_config_path();

} // namespace hailstone

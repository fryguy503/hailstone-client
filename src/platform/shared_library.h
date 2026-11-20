#pragma once

// A shared library loaded at run time (an upscaler provider, render/upscaler_provider.h; a compiled plugin,
// plugin/native_plugins.cpp): SDL's loader on Linux and macOS; on Windows LoadLibraryExW, so that what the library
// needs is looked for beside it and in the system's safe folders, never in the current folder or PATH.

#include <filesystem>
#include <string>
#include <string_view>

namespace hailstone::platform {

// The loader itself, for a handle kept by hand: nullptr (with *error) when `path` does not load.
void* load_library(const std::filesystem::path& path, std::string* error);
void* library_symbol(void* library, const char* name); // nullptr when it has no such export
void unload_library(void* library);                   // nullptr: nothing

// Windows: libraries asked for by name only (a driver's, an overlay's, one a plugin loads) are looked for in the
// program's folder, System32 and AddDllDirectory's folders, not the current folder or PATH. Elsewhere nothing. Once,
// first thing in main.
void restrict_dll_search();

class SharedLibrary {
public:
    SharedLibrary() = default;
    // Loads `path`; loaded() says whether it did, error() why not.
    explicit SharedLibrary(const std::filesystem::path& path);
    ~SharedLibrary();
    SharedLibrary(SharedLibrary&& other) noexcept;
    SharedLibrary& operator=(SharedLibrary&& other) noexcept;
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    bool loaded() const { return handle_ != nullptr; }
    const std::string& error() const { return error_; }
    // The exported function `name`, or nullptr.
    void* symbol(const char* name) const;
    void unload();

private:
    void* handle_ = nullptr;
    std::string error_;
};

// This platform's suffix for a shared library: ".dll", ".dylib" or ".so".
std::string_view shared_library_suffix();

} // namespace hailstone::platform

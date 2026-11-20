#include "platform/shared_library.h"

#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_loadso.h>
#endif

#include "core/path_utf8.h"

namespace hailstone::platform {

#if defined(_WIN32)
namespace {
std::string last_error_text() {
    const DWORD code = GetLastError();
    wchar_t* text = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string out;
    if (n && text) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            out.resize(static_cast<std::size_t>(bytes));
            WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(n), out.data(), bytes, nullptr, nullptr);
        }
    }
    if (text) LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out.empty() ? "error " + std::to_string(code) : out;
}
} // namespace
#endif

void* load_library(const std::filesystem::path& path, std::string* error) {
#if defined(_WIN32)
    // The library's own folder, then the system's safe places (the program's folder, System32, AddDllDirectory's):
    // what it needs is never taken from the current folder or PATH, where anyone may have put a DLL of that name.
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!h && error) *error = last_error_text();
    return reinterpret_cast<void*>(h);
#else
    void* h = SDL_LoadObject(path_utf8(path).c_str());
    if (!h && error) *error = SDL_GetError();
    return h;
#endif
}

void* library_symbol(void* library, const char* name) {
    if (!library) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return reinterpret_cast<void*>(SDL_LoadFunction(static_cast<SDL_SharedObject*>(library), name));
#endif
}

void unload_library(void* library) {
    if (!library) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(library));
#else
    SDL_UnloadObject(static_cast<SDL_SharedObject*>(library));
#endif
}

void restrict_dll_search() {
#if defined(_WIN32)
    // A library the client or one it loads asks for by name (no path) is looked for in the program's folder, System32
    // and the folders added with AddDllDirectory only, not the current folder or PATH. SDL3.dll and the client's other
    // DLLs beside the program are imports, resolved before this runs, and the program's folder stays searched.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#endif
}

// In the body: error_ is constructed after handle_, so it cannot be written from handle_'s initializer.
SharedLibrary::SharedLibrary(const std::filesystem::path& path) { handle_ = load_library(path, &error_); }

SharedLibrary::~SharedLibrary() { unload(); }

SharedLibrary::SharedLibrary(SharedLibrary&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), error_(std::move(other.error_)) {}

SharedLibrary& SharedLibrary::operator=(SharedLibrary&& other) noexcept {
    if (this != &other) {
        unload();
        handle_ = std::exchange(other.handle_, nullptr);
        error_ = std::move(other.error_);
    }
    return *this;
}

void* SharedLibrary::symbol(const char* name) const { return library_symbol(handle_, name); }

void SharedLibrary::unload() {
    unload_library(handle_);
    handle_ = nullptr;
}

std::string_view shared_library_suffix() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

} // namespace hailstone::platform

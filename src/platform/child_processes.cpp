#include "platform/child_processes.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>

#include <algorithm>

namespace hailstone::platform {

ChildProcesses::~ChildProcesses() {
    for (SDL_Process* p : children_) SDL_DestroyProcess(p); // the SDL object only, not the program
}

bool ChildProcesses::start(const std::vector<std::string>& args, std::string& error) {
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    // Its standard streams go nowhere, not to this program's: SDL_CreateProcess hands on stdout and stderr, and on
    // Windows a window started without a console (from Explorer, a shortcut) has none to hand on, so the start
    // failed with "DuplicateHandle(): The handle is invalid". Each Hailstone writes its own log file anyway.
    const SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        error = SDL_GetError();
        return false;
    }
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, static_cast<void*>(argv.data()));
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process* p = SDL_CreateProcessWithProperties(props);
    if (!p) error = SDL_GetError();
    SDL_DestroyProperties(props);
    if (!p) return false;
    children_.push_back(p);
    return true;
}

void ChildProcesses::poll() {
    std::erase_if(children_, [](SDL_Process* p) {
        if (!SDL_WaitProcess(p, false, nullptr)) return false;
        SDL_DestroyProcess(p);
        return true;
    });
}

} // namespace hailstone::platform

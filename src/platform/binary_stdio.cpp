#include "platform/binary_stdio.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <cstdint>
#include <cstdio>

namespace hailstone::platform {

#if defined(_WIN32)
namespace {

bool usable(HANDLE h) { return h != nullptr && h != INVALID_HANDLE_VALUE; }

// A C stream on a handle this function owns from then on (closed with the stream, or here when that fails).
std::FILE* stream_on(HANDLE h, int flags, const char* mode) {
    const int fd = _open_osfhandle(reinterpret_cast<std::intptr_t>(h), flags | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) {
        CloseHandle(h);
        return nullptr;
    }
    std::FILE* f = _fdopen(fd, mode);
    if (!f) _close(fd);
    return f;
}

} // namespace
#endif

bool take_protocol_stdio(ProtocolStreams& streams) {
    std::fflush(stdout);
#if defined(_WIN32)
    // The client's pipes are the standard handles (SDL hands them over in STARTUPINFO, inheritable); the CRT's
    // descriptors 0 and 1 wrap the same handles.
    const HANDLE self = GetCurrentProcess();
    const HANDLE std_in = GetStdHandle(STD_INPUT_HANDLE), std_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!usable(std_in) || !usable(std_out)) return false;
    HANDLE in = nullptr, out = nullptr;
    if (!DuplicateHandle(self, std_in, self, &in, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
    if (!DuplicateHandle(self, std_out, self, &out, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        CloseHandle(in);
        return false;
    }
    // The originals: not passed on even where the CRT does not hold (and so close) them below.
    SetHandleInformation(std_in, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(std_out, HANDLE_FLAG_INHERIT, 0);
    // NUL in their place, inheritable as standard handles are: _dup2 closes the CRT's 0 and 1 (and the originals
    // with them), and a GUI program's CRT does not set the standard handles itself.
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof inherit;
    inherit.bInheritHandle = TRUE;
    const HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                   OPEN_EXISTING, 0, nullptr);
    const int nul_fd = usable(nul) ? _open_osfhandle(reinterpret_cast<std::intptr_t>(nul), _O_BINARY) : -1;
    if (nul_fd < 0) {
        if (usable(nul)) CloseHandle(nul);
        CloseHandle(in);
        CloseHandle(out);
        return false;
    }
    const bool moved = _dup2(nul_fd, 0) == 0 && _dup2(nul_fd, 1) == 0;
    _close(nul_fd);
    if (moved) {
        SetStdHandle(STD_INPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(0)));
        SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(1)));
    }
    streams.in = stream_on(in, _O_RDONLY, "rb");
    streams.out = stream_on(out, _O_WRONLY, "wb");
#else
    // F_DUPFD_CLOEXEC: the copies are made closed across exec, with no moment in which a fork could take them.
    const int in = ::fcntl(0, F_DUPFD_CLOEXEC, 3);
    if (in < 0) return false;
    const int out = ::fcntl(1, F_DUPFD_CLOEXEC, 3);
    if (out < 0) {
        ::close(in);
        return false;
    }
    const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    // dup2 leaves 0 and 1 open across exec, as standard streams are.
    const bool moved = null >= 0 && ::dup2(null, 0) == 0 && ::dup2(null, 1) == 1;
    if (null >= 0) ::close(null);
    streams.in = moved ? ::fdopen(in, "rb") : nullptr;
    streams.out = moved ? ::fdopen(out, "wb") : nullptr;
    if (!streams.in) ::close(in);
    if (!streams.out) ::close(out);
#endif
    if (moved && streams.in && streams.out) return true;
    if (streams.in) std::fclose(streams.in);
    if (streams.out) std::fclose(streams.out);
    streams = {};
    return false;
}

bool stream_inherited_by_children(std::FILE* f) {
    if (!f) return false;
#if defined(_WIN32)
    const int fd = _fileno(f);
    if (fd < 0) return false;
    const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    DWORD flags = 0;
    return usable(h) && GetHandleInformation(h, &flags) && (flags & HANDLE_FLAG_INHERIT) != 0;
#else
    const int fd = ::fileno(f);
    if (fd < 0) return false;
    const int flags = ::fcntl(fd, F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC) == 0;
#endif
}

} // namespace hailstone::platform

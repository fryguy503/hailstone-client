#include "platform/mapped_file.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace hailstone::platform {

// The guarded copy. A read of a mapped page the file no longer backs faults; the copy is caught there and reports
// false, and the caller throws. Only a plain memcpy runs inside the guard, so leaving it in the middle skips no
// destructor and leaves nothing half-built.
#if defined(_WIN32)
namespace {
// No C++ object with a destructor may live in a function with __try (MSVC C2712), hence its own function.
bool guarded_copy(void* to, const void* from, std::size_t n) noexcept {
    __try {
        std::memcpy(to, from, n);
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}
} // namespace
#else
namespace {
// Where the calling thread's guarded copy jumps back to, or null outside one. A pointer, so the handler only loads it.
thread_local sigjmp_buf* t_guard = nullptr;
struct sigaction g_previous{};         // SIGBUS's action before the guard's handler took it
std::atomic<bool> g_passed_on{false}; // an unguarded SIGBUS was handed to g_previous once already
std::mutex g_install;

void on_bus(int sig, siginfo_t* info, void* context) {
    (void)context;
    if (sigjmp_buf* back = t_guard) siglongjmp(*back, 1);
    // Not a guarded copy: the earlier action is put back. A fault the kernel raised (si_code > 0) happens again when
    // the handler returns and goes to it with its real details; a signal sent by kill is raised again. Should the
    // earlier handler hand it back here (it put this one back as its own predecessor), the default ends it.
    if (g_passed_on.exchange(true)) {
        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(SIGBUS, &dfl, nullptr);
    } else {
        sigaction(SIGBUS, &g_previous, nullptr);
    }
    if (info != nullptr && info->si_code > 0) return;
    std::raise(sig);
}

bool is_guard(const struct sigaction& sa) {
    return (sa.sa_flags & SA_SIGINFO) != 0 && sa.sa_sigaction == on_bus;
}

// Puts the guard's handler on SIGBUS unless it is there. Checked on every read, not once: a handler installed later
// (the crash handler, a test framework's per-test one) replaces it, and putting it back over that one keeps that one
// as its predecessor. One query of the action per read (a system call, well under a microsecond).
void ensure_guard() {
    struct sigaction current{};
    if (sigaction(SIGBUS, nullptr, &current) != 0 || is_guard(current)) return;
    std::lock_guard lock(g_install);
    struct sigaction sa{};
    sa.sa_sigaction = on_bus;
    sigemptyset(&sa.sa_mask);
    // SA_NODEFER: SIGBUS is not blocked inside the handler, so the jump back leaves the signal mask as it was and the
    // guard need not save it (sigsetjmp(..., 0), no system call per copy).
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    struct sigaction previous{};
    if (sigaction(SIGBUS, &sa, &previous) != 0 || is_guard(previous)) return;
    g_previous = previous;
    g_passed_on = false;
}

bool guarded_copy(void* to, const void* from, std::size_t n) noexcept {
    sigjmp_buf back;
    if (sigsetjmp(back, 0) != 0) {
        t_guard = nullptr;
        return false;
    }
    t_guard = &back;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::memcpy(to, from, n);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    t_guard = nullptr;
    return true;
}
} // namespace
#endif

void MappedFile::read(std::size_t offset, std::span<std::byte> out) const {
    if (offset > size_ || out.size() > size_ - offset) throw std::out_of_range("MappedFile::read past end");
    if (out.empty()) return;
#if !defined(_WIN32)
    ensure_guard();
#endif
    if (!guarded_copy(out.data(), view_bytes_ + offset, out.size()))
        throw std::runtime_error("a mapped file could not be read (truncated by another program, or a read error)");
}

MappedFile::~MappedFile() { reset(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : view_bytes_(other.view_bytes_), view_(other.view_), size_(other.size_), file_(other.file_),
      mapping_(other.mapping_) {
    other.view_bytes_ = nullptr;
    other.view_ = nullptr;
    other.size_ = 0;
    other.file_ = nullptr;
    other.mapping_ = nullptr;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        reset();
        view_bytes_ = other.view_bytes_;
        view_ = other.view_;
        size_ = other.size_;
        file_ = other.file_;
        mapping_ = other.mapping_;
        other.view_bytes_ = nullptr;
        other.view_ = nullptr;
        other.size_ = 0;
        other.file_ = nullptr;
        other.mapping_ = nullptr;
    }
    return *this;
}

void MappedFile::reset() noexcept {
#if defined(_WIN32)
    if (view_ != nullptr) {
        ::UnmapViewOfFile(view_);
        view_ = nullptr;
        view_bytes_ = nullptr;
    }
    if (mapping_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
    if (file_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(file_));
        file_ = nullptr;
    }
#else
    if (view_ != nullptr) {
        ::munmap(view_, size_);
        view_ = nullptr;
        view_bytes_ = nullptr;
    }
#endif
    size_ = 0;
}

MappedFile MappedFile::open(const std::filesystem::path& path) {
#if defined(_WIN32)
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot read " + path.string());
    LARGE_INTEGER sz{};
    if (::GetFileSizeEx(file, &sz) == 0 || sz.QuadPart < 0) {
        ::CloseHandle(file);
        throw std::runtime_error("cannot read " + path.string());
    }
    if (sz.QuadPart == 0) {
        ::CloseHandle(file);
        return MappedFile{};
    }
    if (static_cast<std::uint64_t>(sz.QuadPart) > static_cast<std::uint64_t>(SIZE_MAX)) {
        ::CloseHandle(file);
        throw std::runtime_error("cannot map " + path.string());
    }
    const std::size_t size = static_cast<std::size_t>(sz.QuadPart);
    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        ::CloseHandle(file);
        throw std::runtime_error("cannot map " + path.string());
    }
    void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        ::CloseHandle(mapping);
        ::CloseHandle(file);
        throw std::runtime_error("cannot map " + path.string());
    }
    MappedFile out;
    out.view_bytes_ = static_cast<const std::byte*>(view);
    out.view_ = view;
    out.size_ = size;
    out.file_ = static_cast<void*>(file);
    out.mapping_ = static_cast<void*>(mapping);
    return out;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot read " + path.string());
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0) {
        ::close(fd);
        throw std::runtime_error("cannot read " + path.string());
    }
    if (st.st_size == 0) {
        ::close(fd);
        return MappedFile{};
    }
    if (static_cast<std::uint64_t>(st.st_size) > static_cast<std::uint64_t>(SIZE_MAX)) {
        ::close(fd);
        throw std::runtime_error("cannot map " + path.string());
    }
    const std::size_t size = static_cast<std::size_t>(st.st_size);
    void* view = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) {
        ::close(fd);
        throw std::runtime_error("cannot map " + path.string());
    }
    // The default readahead (no madvise): inflating a member reads its blocks in order.
    ::close(fd);
    MappedFile out;
    out.view_bytes_ = static_cast<const std::byte*>(view);
    out.view_ = view;
    out.size_ = size;
    return out;
#endif
}

} // namespace hailstone::platform

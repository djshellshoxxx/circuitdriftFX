#include "network/SharedMemory.h"
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cd::network {
namespace {
std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
bool safeName(const std::string& n) {
    if (n.size() < 2 || n.size() > 31 || n[0] != '/') return false;
    for (std::size_t i = 1; i < n.size(); ++i) {
        const char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}
}  // namespace

std::string sharedMemoryName(const Uuid128& g, std::uint64_t session) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "/cdip-%016llx", static_cast<unsigned long long>(mix(g.hi ^ mix(g.lo ^ mix(session)))));
    return buf;
}

#if defined(_WIN32)
namespace {
std::wstring winName(const std::string& n) { return L"Local\\cd" + std::wstring(n.begin() + 1, n.end()); }
}

SharedMemoryRegion::~SharedMemoryRegion() {
    if (map_) UnmapViewOfFile(map_);
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

std::unique_ptr<SharedMemoryRegion> SharedMemoryRegion::create(const GroupSpec& spec, std::string& error) {
    std::unique_ptr<SharedMemoryRegion> r(new SharedMemoryRegion());
    r->name_ = sharedMemoryName(spec.groupId, spec.sessionKeyHash);
    r->size_ = sizeof(SharedLayout);
    const auto bytes = static_cast<unsigned long long>(r->size_);
    SetLastError(0);
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(bytes >> 32), DWORD(bytes & 0xFFFFFFFFu), winName(r->name_).c_str());
    if (!h) { error = "CreateFileMapping failed"; return nullptr; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(h); error = "shared memory object already exists"; return nullptr; }
    r->handle_ = h;
    r->map_ = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, r->size_);
    if (!r->map_) { error = "MapViewOfFile failed"; return nullptr; }
    r->layout_ = initializeLayout(r->map_, r->size_, spec, error);
    if (!r->layout_) return nullptr;
    r->owner_ = true;
    return r;
}

std::unique_ptr<SharedMemoryRegion> SharedMemoryRegion::openByName(const std::string& name, std::string& error) {
    if (!safeName(name)) { error = "invalid shared memory name"; return nullptr; }
    std::unique_ptr<SharedMemoryRegion> r(new SharedMemoryRegion());
    r->name_ = name;
    HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, winName(name).c_str());
    if (!h) { error = "OpenFileMapping failed"; return nullptr; }
    r->handle_ = h;
    r->map_ = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!r->map_) { error = "MapViewOfFile failed"; return nullptr; }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(r->map_, &mbi, sizeof mbi)) { error = "VirtualQuery failed"; return nullptr; }
    r->size_ = std::size_t(mbi.RegionSize);
    if (!validateLayout(r->map_, r->size_, error)) return nullptr;
    r->layout_ = static_cast<SharedLayout*>(r->map_);
    return r;
}
#else
SharedMemoryRegion::~SharedMemoryRegion() {
    if (map_) munmap(map_, size_);
    if (fd_ >= 0) close(fd_);
    if (owner_) shm_unlink(name_.c_str());
}

std::unique_ptr<SharedMemoryRegion> SharedMemoryRegion::create(const GroupSpec& spec, std::string& error) {
    std::unique_ptr<SharedMemoryRegion> r(new SharedMemoryRegion());
    r->name_ = sharedMemoryName(spec.groupId, spec.sessionKeyHash);
    r->size_ = sizeof(SharedLayout);
    r->fd_ = shm_open(r->name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (r->fd_ < 0) { error = errno == EEXIST ? "shared memory object already exists" : "shm_open failed"; return nullptr; }
    r->owner_ = true;  // unlink on destruction, also on later failure
    if (ftruncate(r->fd_, off_t(r->size_)) != 0) { error = "ftruncate failed"; return nullptr; }
    r->map_ = mmap(nullptr, r->size_, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd_, 0);
    if (r->map_ == MAP_FAILED) { r->map_ = nullptr; error = "mmap failed"; return nullptr; }
    r->layout_ = initializeLayout(r->map_, r->size_, spec, error);
    if (!r->layout_) return nullptr;
    return r;
}

std::unique_ptr<SharedMemoryRegion> SharedMemoryRegion::openByName(const std::string& name, std::string& error) {
    if (!safeName(name)) { error = "invalid shared memory name"; return nullptr; }
    std::unique_ptr<SharedMemoryRegion> r(new SharedMemoryRegion());
    r->name_ = name;
    r->fd_ = shm_open(name.c_str(), O_RDWR, 0);
    if (r->fd_ < 0) { error = "shm_open failed"; return nullptr; }
    struct stat st {};
    if (fstat(r->fd_, &st) != 0 || st.st_size < 0) { error = "fstat failed"; return nullptr; }
    r->size_ = std::size_t(st.st_size);
    if (r->size_ < sizeof(SharedLayout)) { error = "region smaller than declared layout"; return nullptr; }
    if (r->size_ > sizeof(SharedLayout)) r->size_ = sizeof(SharedLayout);  // never trust a larger size
    r->map_ = mmap(nullptr, r->size_, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd_, 0);
    if (r->map_ == MAP_FAILED) { r->map_ = nullptr; error = "mmap failed"; return nullptr; }
    if (!validateLayout(r->map_, r->size_, error)) return nullptr;
    r->layout_ = static_cast<SharedLayout*>(r->map_);
    return r;
}
#endif

std::unique_ptr<SharedMemoryRegion> SharedMemoryRegion::open(const Uuid128& group, std::uint64_t session, std::string& error) {
    auto r = openByName(sharedMemoryName(group, session), error);
    if (!r) return nullptr;
    const auto& h = r->layout()->header;
    if (h.groupHi != group.hi || h.groupLo != group.lo || h.sessionHash != session) { error = "group/session mismatch"; return nullptr; }
    return r;
}

}  // namespace cd::network

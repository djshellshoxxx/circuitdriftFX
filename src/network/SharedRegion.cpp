// CircuitDrift network: shared-region transports (see SharedRegion.h). Owner: cd_network.
// Threading: non-audio only (allocation, system calls).
#include "network/SharedRegion.h"
#include <cerrno>
#include <cstring>
#include <new>
#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cd::network {

std::shared_ptr<ArenaRegion> ArenaRegion::create(const std::string& name, std::size_t bytes) {
    std::shared_ptr<ArenaRegion> a(new ArenaRegion());
    a->bytes_ = (bytes + 63u) & ~std::size_t(63u);
    a->mem_ = static_cast<std::byte*>(::operator new(a->bytes_, std::align_val_t(64)));
    std::memset(a->mem_, 0, a->bytes_);
    a->name_ = name;
    return a;
}

ArenaRegion::~ArenaRegion() {
    if (mem_) ::operator delete(mem_, std::align_val_t(64));
}

std::shared_ptr<ArenaRegion> ArenaDirectory::create(const std::string& name, std::size_t bytes, std::string& error) {
    if (name.empty() || bytes < sizeof(RegionHeader) || bytes > kMaxRegionBytes) { error = "bad arena name/size"; return nullptr; }
    if (map_.count(name)) { error = "arena exists: " + name; return nullptr; }
    auto a = ArenaRegion::create(name, bytes);
    map_[name] = a;
    return a;
}

std::unique_ptr<SharedRegion> ArenaDirectory::open(const std::string& name, std::string& error) const {
    auto it = map_.find(name);
    if (it == map_.end()) { error = "no arena " + name; return nullptr; }
    return std::make_unique<ArenaView>(it->second);
}

std::string PosixShmRegion::nameForSession(const Uuid& session) {
    static const char* hex = "0123456789abcdef";
    std::string n = "/cdip-";
    for (std::size_t i = 0; i < 12; ++i) { n += hex[session.b[i] >> 4]; n += hex[session.b[i] & 15]; }
    return n;
}

#if defined(__linux__) || defined(__APPLE__)
namespace {
bool validName(const std::string& n) {
    if (n.size() < 2 || n.size() > 31 || n[0] != '/') return false;
    for (std::size_t i = 1; i < n.size(); ++i) {
        const char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}
}  // namespace

std::unique_ptr<PosixShmRegion> PosixShmRegion::create(const std::string& name, std::size_t bytes, std::string& error) {
    if (!validName(name)) { error = "invalid shm name"; return nullptr; }
    if (bytes < sizeof(RegionHeader) || bytes > kMaxRegionBytes) { error = "invalid shm size"; return nullptr; }
    const int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) { error = std::string("shm_open(create): ") + std::strerror(errno); return nullptr; }
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0 || ::ftruncate(fd, off_t(bytes)) != 0) {
        error = std::string("shm setup: ") + std::strerror(errno);
        ::close(fd);
        ::shm_unlink(name.c_str());
        return nullptr;
    }
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { error = std::string("mmap: ") + std::strerror(errno); ::shm_unlink(name.c_str()); return nullptr; }
    std::unique_ptr<PosixShmRegion> r(new PosixShmRegion());
    r->mem_ = static_cast<std::byte*>(p);
    r->bytes_ = bytes;
    r->name_ = name;
    r->owner_ = true;
    std::memset(r->mem_, 0, bytes);
    return r;
}

std::unique_ptr<PosixShmRegion> PosixShmRegion::open(const std::string& name, std::string& error) {
    if (!validName(name)) { error = "invalid shm name"; return nullptr; }
    const int fd = ::shm_open(name.c_str(), O_RDWR, 0);
    if (fd < 0) { error = std::string("shm_open(open): ") + std::strerror(errno); return nullptr; }
    struct stat st {};
    if (::fstat(fd, &st) != 0) { error = "fstat failed"; ::close(fd); return nullptr; }
    if (st.st_uid != ::geteuid()) { error = "shm object not owned by this user"; ::close(fd); return nullptr; }
    if ((st.st_mode & 077) != 0) { error = "shm object has group/other permissions"; ::close(fd); return nullptr; }
    if (st.st_size < off_t(sizeof(RegionHeader)) || std::uint64_t(st.st_size) > kMaxRegionBytes) {
        error = "shm object size out of range"; ::close(fd); return nullptr;
    }
    const std::size_t bytes = std::size_t(st.st_size);
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { error = std::string("mmap: ") + std::strerror(errno); return nullptr; }
    std::unique_ptr<PosixShmRegion> r(new PosixShmRegion());
    r->mem_ = static_cast<std::byte*>(p);
    r->bytes_ = bytes;
    r->name_ = name;
    r->owner_ = false;
    return r;
}

PosixShmRegion::~PosixShmRegion() {
    if (mem_) ::munmap(mem_, bytes_);
    if (owner_) ::shm_unlink(name_.c_str());
}
#else
std::unique_ptr<PosixShmRegion> PosixShmRegion::create(const std::string&, std::size_t, std::string& error) {
    error = "POSIX shared memory not supported on this platform (G05)";
    return nullptr;
}
std::unique_ptr<PosixShmRegion> PosixShmRegion::open(const std::string&, std::string& error) {
    error = "POSIX shared memory not supported on this platform (G05)";
    return nullptr;
}
PosixShmRegion::~PosixShmRegion() = default;
#endif

}  // namespace cd::network

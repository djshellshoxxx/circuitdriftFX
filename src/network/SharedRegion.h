// CircuitDrift network: transport abstraction for prepared shared-memory regions (CD-F11).
// Owner: cd_network. Creation/open/mapping is NON-AUDIO (system calls); the audio thread only
// touches memory that was mapped before it started. No sockets, no Internet traffic.
//
// Implementations:
//  * ArenaRegion — in-process, 64-byte aligned heap arena with the same layout rules; several
//    "views" share one arena (tests, single-process hosts). An ArenaDirectory object (no globals)
//    stands in for the OS name space.
//  * PosixShmRegion — shm_open/mmap (Linux/macOS). Created O_CREAT|O_EXCL with mode 0600 (and
//    fchmod 0600 against umask surprises); the name embeds 96 random bits of the session token.
//    open() refuses objects not owned by the effective uid, with any group/other permission bits,
//    or with a size outside [sizeof(RegionHeader), kMaxRegionBytes]. The creator unlinks the name
//    on destruction. Windows named sections are NOT implemented (G05).
#pragma once
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include "network/Protocol.h"

namespace cd::network {

class SharedRegion {
public:
    virtual ~SharedRegion() = default;
    virtual std::byte* data() noexcept = 0;
    virtual std::size_t size() const noexcept = 0;
    virtual const std::string& name() const noexcept = 0;
};

class ArenaRegion final : public SharedRegion {
public:
    // A fresh zeroed arena (bytes rounded up to 64).
    static std::shared_ptr<ArenaRegion> create(const std::string& name, std::size_t bytes);
    ~ArenaRegion() override;
    std::byte* data() noexcept override { return mem_; }
    std::size_t size() const noexcept override { return bytes_; }
    const std::string& name() const noexcept override { return name_; }
private:
    ArenaRegion() = default;
    std::byte* mem_ = nullptr;
    std::size_t bytes_ = 0;
    std::string name_;
};

// A non-owning view of an arena that keeps it alive (what a "second process" would map).
class ArenaView final : public SharedRegion {
public:
    explicit ArenaView(std::shared_ptr<ArenaRegion> a) : a_(std::move(a)) {}
    std::byte* data() noexcept override { return a_->data(); }
    std::size_t size() const noexcept override { return a_->size(); }
    const std::string& name() const noexcept override { return a_->name(); }
private:
    std::shared_ptr<ArenaRegion> a_;
};

// In-process name space for arenas (explicit object, never a global).
class ArenaDirectory {
public:
    std::shared_ptr<ArenaRegion> create(const std::string& name, std::size_t bytes, std::string& error);
    std::unique_ptr<SharedRegion> open(const std::string& name, std::string& error) const;
    void remove(const std::string& name) { map_.erase(name); }
private:
    std::map<std::string, std::shared_ptr<ArenaRegion>> map_;
};

class PosixShmRegion final : public SharedRegion {
public:
    static constexpr bool kSupported =
#if defined(__linux__) || defined(__APPLE__)
        true;
#else
        false;
#endif
    // "/cdip-" + 24 lowercase hex chars of the session token (fits macOS PSHMNAMLEN 31).
    static std::string nameForSession(const Uuid& session);
    static std::unique_ptr<PosixShmRegion> create(const std::string& name, std::size_t bytes, std::string& error);
    static std::unique_ptr<PosixShmRegion> open(const std::string& name, std::string& error);
    ~PosixShmRegion() override;
    std::byte* data() noexcept override { return mem_; }
    std::size_t size() const noexcept override { return bytes_; }
    const std::string& name() const noexcept override { return name_; }
    bool owner() const noexcept { return owner_; }
private:
    PosixShmRegion() = default;
    std::byte* mem_ = nullptr;
    std::size_t bytes_ = 0;
    std::string name_;
    bool owner_ = false;
};

}  // namespace cd::network

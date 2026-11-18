// CircuitDrift network: optional cross-process transport (shared memory) for a SharedLayout.
// POSIX: shm_open/ftruncate/mmap, mode 0600 (owner only). Windows: CreateFileMappingW/MapViewOfFileW
// in the per-session "Local\" namespace with the default (creator-user) DACL.
// Limitations (G05, unverified on target hosts): sandboxed plugin hosts may deny shm/mapping
// creation (create/open then fail with a message and the caller stays on the in-process or local-PSU
// path); names are derived from the group UUID and session hash, so a same-user process that knows
// both can attach (the session key is the isolation boundary, not an OS ACL beyond same-user);
// the layout must be built by the same ABI on both sides. Open validates the header (magic,
// version, sizes, shape constants, sample rate) before returning; anything else is rejected.
// Worker-thread only (system calls). Never call from audio.
#pragma once
#include <memory>
#include <string>
#include "network/InstanceNetwork.h"

namespace cd::network {

// Deterministic, filesystem-safe object name (<= 31 chars for macOS): "/cdip-" + 16 hex digits.
std::string sharedMemoryName(const Uuid128& group, std::uint64_t sessionHash);

class SharedMemoryRegion {
public:
    ~SharedMemoryRegion();
    SharedMemoryRegion(const SharedMemoryRegion&) = delete;
    SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;
    // Creates exclusively (fails if it exists), sizes, initialises the group header.
    static std::unique_ptr<SharedMemoryRegion> create(const GroupSpec& spec, std::string& error);
    // Opens an existing region and validates it, including that group id/session hash match.
    static std::unique_ptr<SharedMemoryRegion> open(const Uuid128& group, std::uint64_t sessionHash, std::string& error);
    // Test hook: open an arbitrary name without the group check (still validates the layout).
    static std::unique_ptr<SharedMemoryRegion> openByName(const std::string& name, std::string& error);

    SharedLayout* layout() const noexcept { return layout_; }
    std::size_t size() const noexcept { return size_; }
    const std::string& name() const noexcept { return name_; }

private:
    SharedMemoryRegion() = default;
    void* map_ = nullptr;
    std::size_t size_ = 0;
    SharedLayout* layout_ = nullptr;
    std::string name_;
    bool owner_ = false;
    void* handle_ = nullptr;  // Windows HANDLE
    int fd_ = -1;
};

}  // namespace cd::network

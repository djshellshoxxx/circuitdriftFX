#include "media/ByteSink.h"
#include <atomic>
#include <cstring>
#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#define CD_MEDIA_POSIX 1
#endif

namespace cd::media {

bool MemorySink::write(const void* data, std::size_t n) {
    if (aborted_ || committed_) return false;
    const auto* p = static_cast<const std::uint8_t*>(data);
    bytes_.insert(bytes_.end(), p, p + n);
    return true;
}
bool MemorySink::patch(std::uint64_t offset, const void* data, std::size_t n) {
    if (aborted_ || committed_ || offset > bytes_.size() || n > bytes_.size() - offset) return false;
    std::memcpy(bytes_.data() + offset, data, n);
    return true;
}

#ifdef CD_MEDIA_POSIX
namespace {
std::atomic<unsigned> g_tempCounter{0};  // uniquifier for temp names only
}
FileSink::FileSink(const std::string& finalPath) : final_(finalPath) {
    temp_ = finalPath + ".tmp." + std::to_string(static_cast<long long>(::getpid())) + "." +
            std::to_string(g_tempCounter.fetch_add(1));
    fd_ = ::open(temp_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd_ < 0) { temp_.clear(); done_ = true; }
}
bool FileSink::write(const void* data, std::size_t n) {
    if (fd_ < 0 || done_) return false;
    const auto* p = static_cast<const char*>(data);
    while (n > 0) {
        const ssize_t w = ::write(fd_, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        if (w == 0) return false;
        p += w; n -= static_cast<std::size_t>(w);
    }
    return true;
}
bool FileSink::patch(std::uint64_t offset, const void* data, std::size_t n) {
    if (fd_ < 0 || done_) return false;
    const auto* p = static_cast<const char*>(data);
    while (n > 0) {
        const ssize_t w = ::pwrite(fd_, p, n, static_cast<off_t>(offset));
        if (w < 0) { if (errno == EINTR) continue; return false; }
        if (w == 0) return false;
        p += w; n -= static_cast<std::size_t>(w); offset += static_cast<std::uint64_t>(w);
    }
    return true;
}
bool FileSink::commit() {
    if (fd_ < 0 || done_) return false;
    const bool synced = ::fsync(fd_) == 0;
    const bool closed = ::close(fd_) == 0;
    fd_ = -1;
    done_ = true;
    if (!synced || !closed || ::rename(temp_.c_str(), final_.c_str()) != 0) {
        ::unlink(temp_.c_str());
        return false;
    }
    return true;
}
void FileSink::abort() noexcept {
    if (done_) return;
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (!temp_.empty()) ::unlink(temp_.c_str());
    done_ = true;
}
FileSink::~FileSink() { abort(); }
#else
FileSink::FileSink(const std::string& finalPath) : final_(finalPath) {}
bool FileSink::write(const void*, std::size_t) { return false; }
bool FileSink::patch(std::uint64_t, const void*, std::size_t) { return false; }
bool FileSink::commit() { return false; }
void FileSink::abort() noexcept { done_ = true; }
FileSink::~FileSink() = default;
#endif

}  // namespace cd::media

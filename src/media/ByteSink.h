// CircuitDrift media: output sinks for encoders (P13). Owner: cd_media.
// Threading: sinks are used only from the capture worker thread, never from the audio thread.
// Contract: write() appends; patch() overwrites already-written bytes (WAV/RIFF size fixups);
// commit() makes the output visible (atomic temp+rename for files); abort() discards everything
// and must leave no partial output. Any false return means the sink is dead (disk full etc.).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cd::media {

class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual bool write(const void* data, std::size_t n) = 0;
    virtual bool patch(std::uint64_t offset, const void* data, std::size_t n) = 0;
    virtual bool commit() = 0;
    virtual void abort() noexcept = 0;
};

// In-memory sink (tests, previews). abort() clears the buffer.
class MemorySink final : public ByteSink {
public:
    bool write(const void* data, std::size_t n) override;
    bool patch(std::uint64_t offset, const void* data, std::size_t n) override;
    bool commit() override { committed_ = true; return true; }
    void abort() noexcept override { bytes_.clear(); aborted_ = true; }
    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    bool committed() const noexcept { return committed_; }
    bool aborted() const noexcept { return aborted_; }
private:
    std::vector<std::uint8_t> bytes_;
    bool committed_ = false, aborted_ = false;
};

// POSIX file sink: writes "<path>.tmp.<pid>.<n>" created O_EXCL with mode 0600 (owner only),
// fsyncs, then rename()s onto <path> on commit(). abort()/destructor unlink the temp file, so a
// failed or cancelled job leaves nothing behind. On non-POSIX platforms ok() is false.
class FileSink final : public ByteSink {
public:
    explicit FileSink(const std::string& finalPath);
    ~FileSink() override;
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;
    bool ok() const noexcept { return fd_ >= 0; }
    bool write(const void* data, std::size_t n) override;
    bool patch(std::uint64_t offset, const void* data, std::size_t n) override;
    bool commit() override;
    void abort() noexcept override;
private:
    std::string final_, temp_;
    int fd_ = -1;
    bool done_ = false;
};

}  // namespace cd::media

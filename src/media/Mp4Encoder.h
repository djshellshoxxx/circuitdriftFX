// CircuitDrift media: MP4 encoder interface (CD-F03, gate G07). No encoder ships in this library:
// the default factory returns an encoder whose capability probe reports
// "unavailable on this platform (G07)" and whose begin() fails. The Windows Media Foundation /
// macOS AVAssetWriter adapters implement Mp4Encoder in the plugin layer later (G07 verification
// of SDK APIs/codecs, A/V timing and resource tests is still open). Never silently substitute
// another format: callers offer PNG/GIF explicitly.
// Threading: worker thread only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace cd::media {

struct Mp4Config {
    std::string destinationPath;            // final path; adapter writes a temp file, renames after finalize
    std::uint32_t width = 1080, height = 1920;  // defaults: 9:16 portrait (spec)
    std::uint32_t fps = 30;
    std::uint32_t videoBitrate = 8'000'000;
    bool audio = false;                     // default: audio off
    std::uint32_t sampleRate = 48000;       // AAC 48 kHz / 192 kbit/s when audio is on
    std::uint32_t channels = 2;
    std::uint32_t audioBitrate = 192'000;
};

struct Mp4Capability {
    bool available = false;
    std::string reason;                     // human text when unavailable
};

struct VideoFrameIn {
    const std::uint8_t* rgba = nullptr;     // width*height*4, top-down
    std::uint32_t width = 0, height = 0;
    std::int64_t ptsSamples = 0;            // audio-sample-index PTS (spec); sampleRate gives the base
    bool repeated = false;                  // encoder fell behind: last frame repeated at this timestamp
};
struct AudioChunkIn {
    const float* interleaved = nullptr;     // channels interleaved float PCM
    std::size_t frames = 0;
    std::int64_t startSample = 0;           // timestamp of first frame, in sample index
};

enum class Mp4State : unsigned char { Idle, Recording, Finalizing, Completed, Cancelled, Failed };

class Mp4Encoder {
public:
    virtual ~Mp4Encoder() = default;
    virtual Mp4Capability probe(const Mp4Config& cfg) = 0;
    virtual bool begin(const Mp4Config& cfg, std::string& error) = 0;
    virtual bool addVideoFrame(const VideoFrameIn& frame, std::string& error) = 0;
    virtual bool addAudio(const AudioChunkIn& chunk, std::string& error) = 0;
    // Mux finalize + atomic rename into destinationPath. Only a true return means a file exists.
    virtual bool finish(std::string& error) = 0;
    // Abort and delete temporary files (disk full / cancel / codec failure).
    virtual void cancel() noexcept = 0;
    virtual Mp4State state() const noexcept = 0;
};

inline constexpr const char* kMp4UnavailableReason = "MP4 export unavailable on this platform (G07)";

// Default (and only in this library): the unavailable encoder.
std::unique_ptr<Mp4Encoder> createUnavailableMp4Encoder();
std::unique_ptr<Mp4Encoder> createPlatformMp4Encoder();  // returns the unavailable encoder until G07 closes

// Validates an MP4 request against the spec (dims even for 4:2:0, fps 30, <=60 s, bitrate sane).
bool validateMp4Config(const Mp4Config& cfg, double durationSeconds, std::string& error);

}  // namespace cd::media

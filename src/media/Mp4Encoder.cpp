#include "media/Mp4Encoder.h"

namespace cd::media {
namespace {
class UnavailableMp4Encoder final : public Mp4Encoder {
public:
    Mp4Capability probe(const Mp4Config&) override { return {false, kMp4UnavailableReason}; }
    bool begin(const Mp4Config&, std::string& error) override { error = kMp4UnavailableReason; state_ = Mp4State::Failed; return false; }
    bool addVideoFrame(const VideoFrameIn&, std::string& error) override { error = kMp4UnavailableReason; return false; }
    bool addAudio(const AudioChunkIn&, std::string& error) override { error = kMp4UnavailableReason; return false; }
    bool finish(std::string& error) override { error = kMp4UnavailableReason; return false; }
    void cancel() noexcept override { if (state_ != Mp4State::Failed) state_ = Mp4State::Cancelled; }
    Mp4State state() const noexcept override { return state_; }
private:
    Mp4State state_ = Mp4State::Idle;
};
}  // namespace

std::unique_ptr<Mp4Encoder> createUnavailableMp4Encoder() { return std::make_unique<UnavailableMp4Encoder>(); }
std::unique_ptr<Mp4Encoder> createPlatformMp4Encoder() { return createUnavailableMp4Encoder(); }

bool validateMp4Config(const Mp4Config& cfg, double durationSeconds, std::string& error) {
    if (cfg.width < 2 || cfg.height < 2 || cfg.width > 4096 || cfg.height > 4096) { error = "MP4 dimensions out of range"; return false; }
    if ((cfg.width & 1u) || (cfg.height & 1u)) { error = "H.264 4:2:0 needs even dimensions"; return false; }
    if (cfg.fps != 30) { error = "MP4 export is 30 FPS"; return false; }
    if (!(durationSeconds > 0.0) || durationSeconds > 60.0) { error = "MP4 duration must be in (0, 60] s"; return false; }
    if (cfg.videoBitrate < 100'000 || cfg.videoBitrate > 50'000'000) { error = "video bitrate out of range"; return false; }
    if (cfg.audio) {
        if (cfg.channels < 1 || cfg.channels > 2) { error = "audio channels must be 1 or 2"; return false; }
        if (cfg.sampleRate < 8000 || cfg.sampleRate > 384000) { error = "audio sample rate out of range"; return false; }
    }
    if (cfg.destinationPath.empty()) { error = "destination path is empty"; return false; }
    error.clear();
    return true;
}

}  // namespace cd::media

#include "media/MediaTypes.h"
namespace cd::media {
const char* errorName(MediaError e) noexcept {
    switch (e) {
        case MediaError::Ok: return "Ok";
        case MediaError::InvalidArgument: return "InvalidArgument";
        case MediaError::Unavailable: return "Unavailable";
        case MediaError::MemoryCap: return "MemoryCap";
        case MediaError::SinkWriteFailed: return "SinkWriteFailed";
        case MediaError::AudioOverflow: return "AudioOverflow";
        case MediaError::Cancelled: return "Cancelled";
        case MediaError::NonMonotonic: return "NonMonotonic";
        case MediaError::NoData: return "NoData";
        case MediaError::BadState: return "BadState";
    }
    return "Unknown";
}
const char* stateName(CaptureState s) noexcept {
    switch (s) {
        case CaptureState::Armed: return "Armed";
        case CaptureState::Recording: return "Recording";
        case CaptureState::Finalizing: return "Finalizing";
        case CaptureState::Completed: return "Completed";
        case CaptureState::Cancelled: return "Cancelled";
        case CaptureState::Failed: return "Failed";
    }
    return "Unknown";
}
}  // namespace cd::media

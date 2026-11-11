// CircuitDrift media: shared enums and error codes for the local capture/export pipeline (P13).
// Owner: cd_media. Threading: plain values, any thread. Gate G07 (native MP4 encoders) is OPEN:
// nothing in this module produces MP4/H.264; Format::Mp4 is reported unavailable with a reason.
#pragma once

namespace cd::media {

enum class Format { Png, Gif, Mp4 };

enum class MediaError {
    Ok = 0,
    InvalidArgument,
    Unavailable,       // encoder/format not available (e.g. MP4 while G07 is open)
    MemoryCap,         // capture plan exceeds the capture memory ceiling
    SinkWriteFailed,   // disk full, permission denied, rename failure
    AudioOverflow,     // audio ring would have overflowed: capture stopped, never silent
    Cancelled,
    NonMonotonic,
    NoData,
    BadState
};

enum class CaptureState { Armed, Recording, Finalizing, Completed, Cancelled, Failed };

const char* errorName(MediaError e) noexcept;
const char* stateName(CaptureState s) noexcept;

}  // namespace cd::media

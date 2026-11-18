#pragma once
#include <cstdint>

// The replay editor's export as it runs, from the frames its encoder takes. ReSkate hooks the
// encoder so that every rendered frame of an export is encoded (Extension/Rendering/replay_export.h);
// its hook is wrapped in one that counts them (hook_interpose.h), its file left as ReSkate has it.
// Each frame is one frame of the export's frame rate further into the replay (Replay Shake times its
// shake by them). A new export begins when a frame comes after none for three seconds.
namespace dingosdk::replay_shake::export_frames {
struct Capture {
    bool running{};          // a frame was encoded in the last two seconds
    std::uint64_t session{}; // which export: counts them up from 1
    std::uint64_t frames{};  // encoded so far in that export
};
Capture capture() noexcept;
// The size the export's video is encoded at, as the engine's H.264 setup was last given it (an MP4
// export): 0 while none has been seen.
struct Size {
    int width{}, height{};
    float fps{};
};
Size size() noexcept;
// How far into the replay the export has got: its frames encoded so far, at its frame rate (60 until the encoder's
// is known); 0 while none runs. While a video is exported the replay's clock (replay_activity's playhead) stands where
// the export began, so its time is that plus this (2026-11-17: focus and shake keys did not play through videos).
inline double seconds_exported() noexcept {
    const auto running = capture();
    if (!running.running) return 0.0;
    const float fps = size().fps;
    return static_cast<double>(running.frames) / (fps > 1.0f ? static_cast<double>(fps) : 60.0);
}
} // namespace dingosdk::replay_shake::export_frames

#pragma once
#include <cstdint>
#include <vector>

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
// When the H.264 setup last ran (GetTickCount64; 0 never): an MP4 export's, just before its first frame. A WebM
// export has none, so size() is then an earlier MP4's.
std::uint64_t configured_ms() noexcept;
// A video being exported, or set up to be (its H.264 setup in the last few seconds): the game's render size and
// window are not to change under it.
bool busy() noexcept;
// How far into the replay the export has got: its frames encoded so far, at its frame rate (60 until the encoder's
// is known); 0 while none runs. While a video is exported the replay's clock (replay_activity's playhead) stands where
// the export began, so its time is that plus this (2026-11-17: focus and shake keys did not play through videos).
inline double seconds_exported() noexcept {
    const auto running = capture();
    if (!running.running) return 0.0;
    const float fps = size().fps;
    return static_cast<double>(running.frames) / (fps > 1.0f ? static_cast<double>(fps) : 60.0);
}
// The replay editor's timeline as it maps to the replay's clock, and where an export of it starts: Replay Plus reads it
// (the replay's chunks, the editor's timeline and trim) and keeps it here while the editor shows it.
struct Timeline {
    struct Chunk {
        double clock_from{}, clock_to{};
        float from{}, to{}; // the share of the timeline it fills
    };
    std::vector<Chunk> chunks;        // none: the timeline is one stretch of the clock from `start`
    double start{}, length{};         // the editor's timeline: its start on the clock, and its length (seconds)
    float trim_from{}, trim_to{1.0f}; // what an export films: the trim, shares of the whole replay (as the chunks')
    float bar_from{}, bar_to{1.0f};   // the part of the whole replay the editor's timeline shows (trimmed: the trim)
    bool known{};
};
void set_timeline(Timeline timeline) noexcept;
// The replay's time, from its clock (replay_activity's playhead) as read now. While a video is exported the clock stands
// where the editor's playhead was when the export began, wherever that was (2026-11-19: 3.57 s into a clip, so keys
// played 3.57 s early; 1.2 took it for the clip's start), and the export runs from the trim's start: so its time is the
// timeline's from there, the frames encoded so far further on, through the replay's chunks (as the timeline was when the
// export began). With no timeline known, on from where the clock stood. Outside an export, the clock. Any thread.
double replay_time(double clock) noexcept;
// The time on the replay's clock a share (0..1) of the whole replay is at, through its chunks (there must be some).
double clock_at_share(const Timeline& timeline, double share) noexcept;
} // namespace dingosdk::replay_shake::export_frames

#include "replay_shake_export.h"
#include "Extension/ReplayPlus/replay_plus.h"
#include "hook_interpose.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/replay_keyframes.h"
#include "replay_shake_build.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

namespace dingosdk::replay_shake::export_frames {
namespace {
namespace native = addr::replay_export;
using Submit = void (*)(std::uintptr_t encoder, std::uintptr_t pixels, std::uintptr_t pitch, std::uintptr_t unused);
std::atomic<Submit> inner{}; // ReSkate's own hook on the encoder's frame submit
// And on the H.264 setup, whose encoder knows the video's size (the frame submit's does not).
using Configure = std::uintptr_t (*)(std::uintptr_t encoder, std::uintptr_t, std::uintptr_t, std::uintptr_t);
std::atomic<Configure> inner_configure{};
std::atomic<int> video_width{}, video_height{};
std::atomic<float> video_fps{};
std::atomic<std::uintptr_t> image{};
std::atomic<std::uint64_t> configured_at{};
// Render thread writes, any thread reads (capture()).
std::atomic<std::uint64_t> session{}, frames{}, last_frame_ms{};
// When the export running now had its first frame, and when the one before it had its last (0: none).
std::atomic<std::uint64_t> export_began_ms{}, previous_export_ended_ms{};
// A pause of a few seconds between frames is the next export. The pause is timed from the end of
// the last frame's encode, so a slow encode (4K) is still the same export.
constexpr std::uint64_t new_export_ms = 3000, running_ms = 2000;

void count() noexcept {
    const auto now = GetTickCount64();
    const auto last = last_frame_ms.load(std::memory_order_relaxed);
    if (now - last > new_export_ms) {
        previous_export_ended_ms.store(last, std::memory_order_relaxed);
        export_began_ms.store(now, std::memory_order_relaxed);
        frames.store(0, std::memory_order_relaxed);
        session.fetch_add(1, std::memory_order_relaxed);
    }
    frames.fetch_add(1, std::memory_order_relaxed);
    last_frame_ms.store(now, std::memory_order_release);
}
// In place of ReSkate's hook, which it calls with the arguments as they came (all four registers).
void submit(std::uintptr_t encoder, std::uintptr_t pixels, std::uintptr_t pitch, std::uintptr_t unused) {
    std::uintptr_t vtable{};
    const bool replay =
        memory::peek(encoder, vtable) && vtable == image.load(std::memory_order_acquire) + native::replay_encoder_vtable;
    if (replay) {
        count();
        // Replay Plus's lens, grade and hood, as the screen shows them (the game films its frame before
        // the overlay draws on it).
        replay_plus::export_frame(encoder, pixels, pitch);
    }
    inner.load(std::memory_order_acquire)(encoder, pixels, pitch, unused); // encodes before it returns
    if (replay) last_frame_ms.store(GetTickCount64(), std::memory_order_release);
}
std::uintptr_t configure(std::uintptr_t encoder, std::uintptr_t a, std::uintptr_t b, std::uintptr_t c) {
    int width{}, height{};
    float fps{};
    if (memory::peek(encoder + native::h264_width, width) && memory::peek(encoder + native::h264_height, height) &&
        memory::peek(encoder + native::h264_fps, fps) && width > 0 && height > 0 && width <= 16384 && height <= 16384) {
        video_width.store(width, std::memory_order_relaxed);
        video_height.store(height, std::memory_order_relaxed);
        video_fps.store(fps, std::memory_order_relaxed);
    }
    configured_at.store(GetTickCount64(), std::memory_order_release);
    return inner_configure.load(std::memory_order_acquire)(encoder, a, b, c);
}
bool is_configure(void* target) noexcept {
    const auto at = reinterpret_cast<std::uintptr_t>(target);
    std::array<unsigned char, native::configure_h264_prefix.size()> bytes{};
    return at > native::configure_h264 && memory::read_bytes(at, bytes.data(), bytes.size()) && bytes == native::configure_h264_prefix;
}
// The encoder's frame submit, by its bytes (ReSkate checks them too before it hooks it).
bool is_submit(void* target) noexcept {
    const auto at = reinterpret_cast<std::uintptr_t>(target);
    std::array<unsigned char, native::submit_frame_prefix.size()> bytes{};
    return at > native::submit_frame && memory::read_bytes(at, bytes.data(), bytes.size()) && bytes == native::submit_frame_prefix;
}

// ---- The replay's time as the game plays its camera keyframes (played()) ----
namespace kf = game::build::v20260929::replay_keyframes;
using SegmentEval = bool (*)(std::uintptr_t camera, double time, std::uintptr_t segment, std::uintptr_t out);
std::atomic<SegmentEval> segment_original{};
std::atomic<bool> played_hooked{};
std::atomic<double> played_time{};
std::atomic<std::uint64_t> played_at{}, played_calls{}, played_thread{};
std::atomic<int> played_streak{};
std::atomic<std::uint64_t> trace_until{}; // research: each call to the log until then (GetTickCount64)
bool segment_eval(std::uintptr_t camera, double time, std::uintptr_t segment, std::uintptr_t out) {
    const auto now = GetTickCount64();
    if (std::isfinite(time)) {
        // (One play a frame, each frame's on one job thread or another, one after the other.)
        played_streak.store(next_streak(played_streak.load(std::memory_order_relaxed), played_time.load(std::memory_order_relaxed), time),
                            std::memory_order_relaxed);
        played_time.store(time, std::memory_order_relaxed);
        played_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        played_at.store(now, std::memory_order_release);
    }
    const auto calls = played_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (now < trace_until.load(std::memory_order_relaxed)) {
        try {
            logging::log(logging::Level::info, logging::Channel::graphics,
                         "Replay Shake played: {:.5f} (call {}, camera {:x}, segment {:x}, thread {}, {} ms; export {} frame {}).", time, calls, camera,
                         segment, GetCurrentThreadId(), now, session.load(std::memory_order_relaxed), frames.load(std::memory_order_relaxed));
        } catch (...) {}
    }
    return segment_original.load(std::memory_order_acquire)(camera, time, segment, out);
}
} // namespace

bool start_played_clock(std::uintptr_t base) noexcept {
    if (played_hooked.load(std::memory_order_acquire)) return true;
    std::array<unsigned char, 32> bytes{};
    if (!memory::peek(base + kf::segment_eval.rva, bytes) || bytes != kf::segment_eval.bytes) {
        logging::write(logging::Level::info, logging::Channel::graphics,
                       "Replay Shake: the game's keyframe player is not the one known: exports are timed by their frames (slow motion not followed).");
        return false;
    }
    auto* const target = reinterpret_cast<void*>(base + kf::segment_eval.rva);
    void* relay{};
    if (dingosdk::hook_prepare(target, reinterpret_cast<void*>(&segment_eval), &relay) != HookOk || !relay) return false;
    segment_original.store(reinterpret_cast<SegmentEval>(relay), std::memory_order_release);
    if (const auto status = dingosdk::hook_enable(target); status != HookOk) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Shake: the keyframe player's hook did not go in ({}).",
                     hook_status_string(status));
        dingosdk::hook_remove(target);
        return false;
    }
    played_hooked.store(true, std::memory_order_release);
    return true;
}
Played played() noexcept {
    Played result;
    result.at = played_at.load(std::memory_order_acquire);
    result.time = played_time.load(std::memory_order_relaxed);
    result.calls = played_calls.load(std::memory_order_relaxed);
    result.streak = played_streak.load(std::memory_order_relaxed);
    result.thread = static_cast<std::uint32_t>(played_thread.load(std::memory_order_relaxed));
    result.hooked = played_hooked.load(std::memory_order_relaxed);
    return result;
}
void trace_played(std::uint32_t milliseconds) noexcept { trace_until.store(GetTickCount64() + milliseconds, std::memory_order_relaxed); }

std::uint64_t configured_ms() noexcept { return configured_at.load(std::memory_order_acquire); }
Size setup_of_this_export() noexcept {
    const auto at = configured_at.load(std::memory_order_acquire);
    const auto began = export_began_ms.load(std::memory_order_relaxed), before = previous_export_ended_ms.load(std::memory_order_relaxed);
    if (!at || !began || at <= before || (began > at && began - at > 10000)) return {};
    return size();
}
bool busy() noexcept {
    const auto at = configured_at.load(std::memory_order_acquire);
    return capture().running || (at && GetTickCount64() - at < 5000);
}
Size size() noexcept {
    return {video_width.load(std::memory_order_relaxed), video_height.load(std::memory_order_relaxed), video_fps.load(std::memory_order_relaxed)};
}

Capture capture() noexcept {
    const auto last = last_frame_ms.load(std::memory_order_acquire);
    Capture result;
    result.running = last && GetTickCount64() - last <= running_ms;
    result.session = session.load(std::memory_order_relaxed);
    result.frames = frames.load(std::memory_order_relaxed);
    return result;
}

namespace {
struct Timing {
    std::mutex mutex;
    Timeline shown;                 // as the editor shows it now
    std::uint64_t session{};        // the export timed below
    Timeline exported;              // the timeline when it began
    double clock{};                 // the clock when it began
    bool played{};                  // timed by the camera keys' plays (played()) since they played in it
    double held{};                  // the last of them
};
Timing& timing() {
    static Timing value;
    return value;
}
} // namespace

void set_timeline(Timeline timeline) noexcept {
    try {
        auto& t = timing();
        std::lock_guard lock(t.mutex);
        t.shown = std::move(timeline);
    } catch (...) {}
}

double replay_time(double clock) noexcept {
    const auto running = capture();
    if (!running.running) return clock;
    const float fps = size().fps;
    const double step = 1.0 / (fps > 1.0f ? static_cast<double>(fps) : 60.0);
    try {
        auto& t = timing();
        std::lock_guard lock(t.mutex);
        if (running.session != t.session) {
            // A new export: the timeline as the editor showed it, and the clock where it stands.
            t.session = running.session;
            t.exported = t.shown;
            t.clock = clock;
            t.played = false;
        }
        // The replay's time as the game plays the camera's keyframes in this export (played()): the time of the frame it
        // draws now, slow motion and all. The frames encoded go by the clip at 1x and run behind the frames drawn, after
        // the few the export holds first (2026-11-20: a key at a quarter speed had the clip's 1.5 s take 6 s of the video,
        // 797 frames, and the frames encoded ran 2 behind the time played at its start). Taken once the plays run on
        // (next_streak: not the export's odd first one), whatever the editor's clock says (it can be another clip's start:
        // 23.29 for one that began at 199.6, 2026-11-20); once taken in this export, the last held through a stall or jump.
        const auto p = played();
        if (p.hooked && p.at && p.at >= export_began_ms.load(std::memory_order_relaxed) && GetTickCount64() - p.at < 1000 && p.streak >= 2) {
            t.played = true;
            t.held = p.time;
            return p.time;
        }
        if (t.played) return t.held;
        // Without keyframes the camera plays none: the clip at 1x, by the frames encoded.
        return time_by_frames(t.exported, t.clock, static_cast<double>(running.frames) * step);
    } catch (...) {
        return clock;
    }
}
} // namespace dingosdk::replay_shake::export_frames

// ReSkate's export hook (hook_interpose.h): it goes in as prepared, wrapped in the count. The relay
// it is given is the encoder's own, so its frames go on to the encoder just as before.
namespace dingosdk::replay_export {
HookStatus WINAPI hook_prepare(void* target, void* replacement, void** original) {
    namespace frames = replay_shake::export_frames;
    if (replacement && frames::is_configure(target)) {
        // The H.264 setup: wrapped too, to note the video's size for Replay Plus's finishing of its frames.
        const auto status = dingosdk::hook_prepare(target, reinterpret_cast<void*>(&frames::configure), original);
        if (status == HookOk) frames::inner_configure.store(reinterpret_cast<frames::Configure>(replacement), std::memory_order_release);
        return status;
    }
    if (!replacement || !frames::is_submit(target)) return dingosdk::hook_prepare(target, replacement, original);
    const auto status = dingosdk::hook_prepare(target, reinterpret_cast<void*>(&frames::submit), original);
    // Published once prepared, and before the hook can be enabled (ReSkate enables it after this
    // returns). A failed prepare leaves a hook already there as it was.
    if (status == HookOk) {
        frames::image.store(reinterpret_cast<std::uintptr_t>(target) - addr::replay_export::submit_frame, std::memory_order_release);
        frames::inner.store(reinterpret_cast<frames::Submit>(replacement), std::memory_order_release);
    }
    return status;
}
} // namespace dingosdk::replay_export

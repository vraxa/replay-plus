#include "replay_shake_export.h"
#include "Extension/ReplayPlus/replay_plus.h"
#include "hook_interpose.h"
#include "Engine/Core/Platform/memory.h"
#include "replay_shake_build.h"
#include <Windows.h>
#include <array>
#include <atomic>
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
// A pause of a few seconds between frames is the next export. The pause is timed from the end of
// the last frame's encode, so a slow encode (4K) is still the same export.
constexpr std::uint64_t new_export_ms = 3000, running_ms = 2000;

void count() noexcept {
    const auto now = GetTickCount64();
    if (now - last_frame_ms.load(std::memory_order_relaxed) > new_export_ms) {
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
} // namespace

std::uint64_t configured_ms() noexcept { return configured_at.load(std::memory_order_acquire); }
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

double clock_at_share(const Timeline& timeline, double share) noexcept {
    if (timeline.chunks.empty()) return timeline.start + share * timeline.length;
    // Through its chunk; before the first, its start; past the last, its end.
    const Timeline::Chunk* last = &timeline.chunks.front();
    for (const auto& c : timeline.chunks) {
        if (share < c.from) return c.clock_from;
        if (share <= c.to) {
            const double span = static_cast<double>(c.to) - c.from;
            const double along = span > 1e-9 ? (share - c.from) / span : 0.0;
            return c.clock_from + along * (c.clock_to - c.clock_from);
        }
        last = &c;
    }
    return last->clock_to;
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
        }
        const double along = static_cast<double>(running.frames) * step;
        const auto& e = t.exported;
        if (!e.known || e.length <= 1e-3) return t.clock + along;
        if (!e.chunks.empty()) {
            // The whole replay's seconds per share (its chunks' footage over the shares they fill), from the trim's start.
            double footage = 0, shares = 0;
            for (const auto& c : e.chunks) {
                footage += c.clock_to - c.clock_from;
                shares += static_cast<double>(c.to) - c.from;
            }
            if (footage > 1e-3 && shares > 1e-6) return clock_at_share(e, static_cast<double>(e.trim_from) + along * shares / footage);
        }
        // One stretch: the timeline's start, the trim's start on it, and on from there.
        const double shown = static_cast<double>(e.bar_to) - e.bar_from;
        const double trim_at = shown > 1e-6 ? (static_cast<double>(e.trim_from) - e.bar_from) / shown : 0.0;
        return e.start + trim_at * e.length + along;
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

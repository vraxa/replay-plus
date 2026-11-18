#include "camera_update_hook.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "replay_shake_build.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>

namespace dingosdk::replay_shake::camera_update {
namespace {
// Skate.exe of Steam build 25414733 (sha256 fbce74d5...), each function found by its bytes (another
// build's are others, and the hook is then unavailable):
// - A camera evaluator's update, (evaluator, update), where update+0 is the frame's time step
//   (float). Called once a frame for each camera that updates, by the evaluators' own update
//   (0x1737a20) and by a camera's update (engine.h camera_vtable, slot +0x88), both on the engine
//   thread. With directors, it blends what they work out (snapshotting the blend for what it hands
//   on), lets its modifiers change that into its state (at [evaluator+0x60]+0x10), sets the camera
//   from the state (apply), copies the state to its own +0x90, fills in what it hands on
//   ([evaluator+0x80]: a byte, 1 once filled in, then the blend's state at +0x10), runs its
//   modifiers again (hand), and hands that on to its listeners ([evaluator+0x10], slot 0, with the
//   evaluator and what it hands on; the object at [evaluator+0x78] reads the copy just before them,
//   0x176ed40). Without directors, it neither sets the camera nor hands anything on (it marks what it
//   hands on empty, its byte 0), but still runs hand.
// - apply, (evaluator, state): sets the camera from the state (0x17ac360 and 0x1707c60: its
//   transform at +0x10 and +0x50, rows right, up, back, position: camera_transform) and its field of
//   view (+0xac). Called by the update only.
// - hand, (evaluator, update): the update's modifiers once the camera is set. Called by the update
//   only, once in each, after it fills in what it hands on (or marks it empty).
// - set_state, (camera, set): the camera's transform (+0x10, and +0x50 with the offset at +0x90 added
//   to its position) from 0x90 bytes the conversion makes of a state; nothing for a NaN. Called by apply,
//   the update itself on a cut, an evaluator's snap (0x1727ea0), and a camera class's own update
//   (0x2c390f0).
// - set_transform, (camera, transform): the camera's transform (+0x10 and +0x50, no offset) from a
//   world matrix; nothing for a NaN. Called by the other camera classes' own updates (0x2c38c10...),
//   the camera views' default camera (0x2f0f740), the free camera (0x2f8fa90) and others.
struct Target {
    const char* name;
    std::uintptr_t rva;
    std::array<unsigned char, 32> bytes;
};
enum Which : std::size_t { update_at, apply_at, hand_at, set_state_at, set_transform_at, target_count };
constexpr std::array<Target, target_count> targets{{
    {"camera update", 0x1728a50, {
        0x4c, 0x8b, 0xdc, 0x48, 0x81, 0xec, 0xd8, 0x02, 0x00, 0x00, 0x48, 0x8b, 0x05, 0x5f, 0xb9, 0xa9,
        0x05, 0x48, 0x33, 0xc4, 0x48, 0x89, 0x84, 0x24, 0xa0, 0x02, 0x00, 0x00, 0x49, 0x89, 0x5b, 0x18}},
    {"camera update's setting of the camera", 0x1725130, {
        0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x78, 0x10, 0x55, 0x48, 0x8d, 0x68, 0xa1,
        0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00, 0x48, 0x8b, 0xda, 0x48, 0x8b, 0xf9, 0x48, 0x8b, 0xcb}},
    {"camera update's hand-on", 0x1727a60, {
        0x40, 0x53, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x58, 0x48, 0x8b, 0x79, 0x38, 0x48, 0x8b, 0xea,
        0x48, 0x8b, 0x59, 0x30, 0x48, 0x8b, 0xf1, 0x48, 0x8b, 0x81, 0x80, 0x00, 0x00, 0x00, 0x48, 0x89}},
    {"setting of a camera from a state", 0x1707c60, {
        0x8b, 0x42, 0x40, 0x25, 0x00, 0x00, 0x80, 0x7f, 0x3d, 0x00, 0x00, 0x80, 0x7f, 0x0f, 0x84, 0xed,
        0x00, 0x00, 0x00, 0x8b, 0x42, 0x44, 0x25, 0x00, 0x00, 0x80, 0x7f, 0x3d, 0x00, 0x00, 0x80, 0x7f}},
    {"setting of a camera from a transform", 0x1707a10, {
        0x48, 0x8b, 0xc4, 0x48, 0x81, 0xec, 0x28, 0x01, 0x00, 0x00, 0xc5, 0xfa, 0x10, 0x0a, 0xc5, 0xfa,
        0x10, 0x52, 0x04, 0xc5, 0xfa, 0x10, 0x5a, 0x08, 0xc5, 0xfa, 0x10, 0x62, 0x10, 0xc5, 0xfa, 0x10}},
}};
using Function = void (*)(std::uintptr_t evaluator, std::uintptr_t argument);

struct State {
    std::mutex mutex; // preparing and switching: client thread
    bool prepared{}, failed{};
    std::array<bool, target_count> enabled{};
    std::array<void*, target_count> target{};
    std::array<std::atomic<Function>, target_count> original{};
    std::atomic<const Callbacks*> callbacks{};
};
// Lives until the process ends: the detours can run at any time.
State& state() {
    static auto* value = new State;
    return *value;
}
struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
};
// The evaluator's camera, should it point back at the evaluator (as camera_observer.cpp checks).
std::uintptr_t camera_of(std::uintptr_t evaluator) noexcept {
    std::uintptr_t camera{}, back{};
    return memory::peek(evaluator + layout::evaluator_camera, camera) && camera &&
            memory::peek(camera + layout::camera_evaluator, back) && back == evaluator
        ? camera : 0;
}
void update_detour(std::uintptr_t evaluator, std::uintptr_t update) {
    auto& s = state();
    const auto* callbacks = s.callbacks.load(std::memory_order_acquire);
    const auto camera = callbacks ? camera_of(evaluator) : 0;
    if (camera && callbacks->before) {
        LastError error;
        callbacks->before(camera);
    }
    s.original[update_at].load(std::memory_order_acquire)(evaluator, update);
    // The same camera still: the update may have changed what the evaluator moves.
    if (camera && callbacks->after && camera_of(evaluator) == camera) {
        LastError error;
        callbacks->after(camera);
    }
}
void apply_detour(std::uintptr_t evaluator, std::uintptr_t from) {
    auto& s = state();
    s.original[apply_at].load(std::memory_order_acquire)(evaluator, from);
    const auto* callbacks = s.callbacks.load(std::memory_order_acquire);
    if (!callbacks || !callbacks->applied) return;
    if (const auto camera = camera_of(evaluator)) {
        std::uintptr_t blender{};
        if (!memory::peek(evaluator + evaluator_blender, blender) || (blender & 7)) blender = 0;
        LastError error;
        callbacks->applied(camera, from, blender);
    }
}
void hand_detour(std::uintptr_t evaluator, std::uintptr_t update) {
    auto& s = state();
    const auto* callbacks = s.callbacks.load(std::memory_order_acquire);
    if (callbacks && callbacks->handing)
        if (const auto camera = camera_of(evaluator)) {
            std::uintptr_t handed{};
            std::uint8_t filled{};
            const auto handed_at = memory::peek(evaluator + evaluator_handed, handed) && handed && memory::peek(handed, filled) &&
                    filled == 1
                ? handed + holder_state : 0;
            LastError error;
            callbacks->handing(camera, evaluator + evaluator_copy, handed_at);
        }
    s.original[hand_at].load(std::memory_order_acquire)(evaluator, update);
}
template <Which which> void set_detour(std::uintptr_t camera, std::uintptr_t from) {
    auto& s = state();
    s.original[which].load(std::memory_order_acquire)(camera, from);
    const auto* callbacks = s.callbacks.load(std::memory_order_acquire);
    if (!camera || !callbacks || !callbacks->written) return;
    LastError error;
    callbacks->written(camera, which == set_state_at);
}
std::array<void*, target_count> detours() noexcept {
    return {reinterpret_cast<void*>(&update_detour), reinterpret_cast<void*>(&apply_detour),
        reinterpret_cast<void*>(&hand_detour), reinterpret_cast<void*>(&set_detour<set_state_at>),
        reinterpret_cast<void*>(&set_detour<set_transform_at>)};
}
// Switches one detour, logging a failure. Under the mutex.
bool set(State& s, std::size_t which, bool on) noexcept {
    if (s.enabled[which] == on) return true;
    const auto status = on ? hook_enable(s.target[which]) : hook_disable(s.target[which]);
    if (status != HookOk) {
        logging::log(logging::Level::warning, logging::Channel::graphics,
            "Could not {} the hook on the game's {} ({}).", on ? "enable" : "disable", targets[which].name, hook_status_string(status));
        return false;
    }
    s.enabled[which] = on;
    return true;
}
} // namespace

bool start(std::uintptr_t base, const Callbacks& callbacks) noexcept {
    auto& s = state();
    try {
        std::lock_guard lock(s.mutex);
        if (s.prepared) return true;
        if (s.failed || !base) return false;
        s.failed = true; // until it is prepared
        for (const auto& target : targets) {
            std::array<unsigned char, 32> actual{};
            if (!memory::read_bytes(base + target.rva, actual.data(), actual.size()) || actual != target.bytes) {
                logging::log(logging::Level::warning, logging::Channel::graphics,
                    "Replay Shake knows the game's {} on game build 25414733 (this is {}), and it is not at 0x{:x} here: it "
                    "shakes the views drawn instead.", target.name, supported_build::steam_build_id, target.rva);
                return false;
            }
        }
        const auto replacement = detours();
        std::array<void*, target_count> relay{};
        for (std::size_t i = 0; i < target_count; ++i) {
            auto* target = reinterpret_cast<void*>(base + targets[i].rva);
            auto status = hook_prepare(target, replacement[i], &relay[i]);
            if (status == HookOk && !relay[i]) {
                (void)hook_remove(target);
                status = HookUnsupportedFunction;
            }
            if (status != HookOk) {
                for (std::size_t j = 0; j < i; ++j) (void)hook_remove(s.target[j]);
                logging::log(logging::Level::warning, logging::Channel::graphics,
                    "Hooking the game's {} failed ({}): Replay Shake shakes the views drawn instead.", targets[i].name,
                    hook_status_string(status));
                return false;
            }
            s.target[i] = target;
        }
        // Published before the detours can ever be enabled (want()).
        for (std::size_t i = 0; i < target_count; ++i)
            s.original[i].store(reinterpret_cast<Function>(relay[i]), std::memory_order_release);
        s.callbacks.store(new Callbacks(callbacks), std::memory_order_release);
        s.prepared = true;
        s.failed = false;
        return true;
    } catch (...) {
        return false;
    }
}

bool available() noexcept { return state().prepared; }

bool want(bool on) noexcept {
    auto& s = state();
    try {
        std::lock_guard lock(s.mutex);
        if (!s.prepared) return !on;
        // On: the update first (it takes the shake off again), then what puts it on; off: the other way
        // round. Should one not switch, those switched in this call go back as they were.
        constexpr std::array<std::size_t, target_count> on_order{update_at, hand_at, set_state_at, set_transform_at, apply_at};
        constexpr std::array<std::size_t, target_count> off_order{apply_at, set_transform_at, set_state_at, hand_at, update_at};
        const auto& order = on ? on_order : off_order;
        const auto was = s.enabled;
        for (std::size_t i = 0; i < target_count; ++i) {
            if (set(s, order[i], on)) continue;
            for (std::size_t j = i; j-- > 0;) (void)set(s, order[j], was[order[j]]);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace dingosdk::replay_shake::camera_update

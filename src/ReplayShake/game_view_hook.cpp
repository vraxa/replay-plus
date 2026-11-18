#include "game_view_hook.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>

namespace dingosdk::replay_shake::game_view {
namespace {
// Skate.exe of Steam build 25414733, each function found by its bytes (another build's are others,
// and the hook is then unavailable):
// - fill, (description, id, settings, camera view, beside, view): fills a view description in for a
//   camera view (0x1240 bytes, the game's own list of the views it draws: its first view at +0, the
//   local player's index of the camera view at +0x1150) from its view settings (field of view, near
//   and far...: 0x3e743a0 and others) and the two transforms (0x90 bytes each, Transform): `beside`
//   copied to the description's +0x1160, `view` set as the first view's transform last of all
//   (0x3e745a0). Called once for each camera view the game draws, each frame, by the game's building
//   of its views (0x3eb88f0) only, which keeps neither pointer.
// - override, (description, context): the replay camera's say on a description just filled in, which
//   the building of the views calls right after each fill through a function the game registers at
//   start (0x4bd4e0). While the replay camera is on (its object's +0x28), it sets the first view's
//   transform anew (0x3e745a0) from what that camera works out (0x4c72d0); otherwise it leaves it.
// - set_transform, (view, transform): a view's transform (+0 to +0x90) and the view marked changed
//   (+0x140), unless it holds that transform already.
// - set_fov, (view, fov): a view's vertical field of view (+0x104, radians), the horizontal one worked
//   out from it and the view's aspect (+0x2b0, from +0x11c), and the view marked changed (+0x140).
struct Target {
    const char* name;
    std::uintptr_t rva;
    std::array<unsigned char, 32> bytes;
};
enum Which : std::size_t { fill_at, override_at, set_transform_at, set_fov_at, target_count };
constexpr std::size_t hooked_count = 2; // fill and override; the setters are only called
constexpr std::array<Target, target_count> targets{{
    {"filling-in of a view it draws", 0x3ef19c0, {
        0x48, 0x8b, 0xc4, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x08, 0x01, 0x00, 0x00,
        0x48, 0x89, 0x58, 0x18, 0x48, 0x8b, 0xf9, 0x48, 0x8b, 0x0d, 0x12, 0x41, 0x4a, 0x03, 0x44, 0x0f}},
    {"replay camera's setting of a view", 0x4b9550, {
        0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48,
        0x8d, 0x6c, 0x24, 0xb0, 0x48, 0x81, 0xec, 0x50, 0x01, 0x00, 0x00, 0x48, 0x83, 0x3d, 0xe5, 0x8b}},
    {"setting of a view's transform", 0x3e745a0, {
        0xc5, 0xf8, 0x10, 0x02, 0xc5, 0xf8, 0xc2, 0x09, 0x00, 0xc5, 0xf8, 0x50, 0xc1, 0x83, 0xe0, 0x07,
        0x3c, 0x07, 0x75, 0x5e, 0xc5, 0xf8, 0x10, 0x42, 0x10, 0xc5, 0xf8, 0xc2, 0x49, 0x10, 0x00, 0xc5}},
    {"setting of a view's field of view", 0x3e743a0, {
        0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0xc5, 0xfa, 0x10, 0x81, 0x04, 0x01, 0x00, 0x00, 0xc5, 0xf8,
        0x2e, 0xc1, 0xc5, 0xf8, 0x28, 0xd1, 0x48, 0x8b, 0xd9, 0x7a, 0x02, 0x74, 0x28, 0xc5, 0xfa, 0x10}},
}};
// Every register argument as it came (the id is a word the game widened; the rest are pointers).
using Fill = void (*)(std::uintptr_t description, std::uintptr_t id, std::uintptr_t settings, std::uintptr_t camera_view,
    std::uintptr_t beside, std::uintptr_t view);
using Override = void (*)(std::uintptr_t description, std::uintptr_t context);
using SetTransform = void (*)(std::uintptr_t view, const Transform* transform);
using SetFov = void (*)(std::uintptr_t view, float fov);

struct State {
    std::mutex mutex; // preparing and switching: client thread
    bool prepared{}, failed{};
    std::array<bool, hooked_count> enabled{};
    std::array<void*, hooked_count> target{};
    std::atomic<Fill> fill{};
    std::atomic<Override> override_original{};
    std::atomic<SetTransform> set_transform{};
    std::atomic<SetFov> set_fov{};
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
void fill_detour(std::uintptr_t description, std::uintptr_t id, std::uintptr_t settings, std::uintptr_t camera_view,
    std::uintptr_t beside, std::uintptr_t view) {
    auto& s = state();
    const auto* callbacks = s.callbacks.load(std::memory_order_acquire);
    // The callback changes copies, which the game then fills the description in from: the game's own
    // transforms stay as they were.
    Transform beside_copy, view_copy;
    bool copied = false;
    if (callbacks && callbacks->building && description && camera_view && memory::peek(beside, beside_copy) &&
        memory::peek(view, view_copy)) {
        LastError error;
        callbacks->building(description, camera_view, beside_copy, view_copy);
        copied = true;
    }
    s.fill.load(std::memory_order_acquire)(description, id, settings, camera_view,
        copied ? reinterpret_cast<std::uintptr_t>(&beside_copy) : beside, copied ? reinterpret_cast<std::uintptr_t>(&view_copy) : view);
    if (callbacks && callbacks->built && description && camera_view) {
        LastError error;
        callbacks->built(description, camera_view);
    }
}
void override_detour(std::uintptr_t description, std::uintptr_t context) {
    auto& s = state();
    s.override_original.load(std::memory_order_acquire)(description, context);
    const auto* callbacks = s.callbacks.load(std::memory_order_acquire);
    if (callbacks && callbacks->overridden && description) {
        LastError error;
        callbacks->overridden(description);
    }
}
// Switches one detour, logging a failure. Under the mutex.
bool set(State& s, std::size_t which, bool on) noexcept {
    if (s.enabled[which] == on) return true;
    const auto status = on ? hook_enable(s.target[which]) : hook_disable(s.target[which]);
    if (status != HookOk) {
        logging::log(logging::Level::warning, logging::Channel::graphics, "Could not {} the hook on the game's {} ({}).",
            on ? "enable" : "disable", targets[which].name, hook_status_string(status));
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
                    "shakes the camera instead.", target.name, supported_build::steam_build_id, target.rva);
                return false;
            }
        }
        const std::array<void*, hooked_count> replacement{reinterpret_cast<void*>(&fill_detour), reinterpret_cast<void*>(&override_detour)};
        std::array<void*, hooked_count> relay{};
        for (std::size_t i = 0; i < hooked_count; ++i) {
            auto* target = reinterpret_cast<void*>(base + targets[i].rva);
            auto status = hook_prepare(target, replacement[i], &relay[i]);
            if (status == HookOk && !relay[i]) {
                (void)hook_remove(target);
                status = HookUnsupportedFunction;
            }
            if (status != HookOk) {
                for (std::size_t j = 0; j < i; ++j) (void)hook_remove(s.target[j]);
                logging::log(logging::Level::warning, logging::Channel::graphics,
                    "Hooking the game's {} failed ({}): Replay Shake shakes the camera instead.", targets[i].name,
                    hook_status_string(status));
                return false;
            }
            s.target[i] = target;
        }
        // Published before the detours can ever be enabled (want()).
        s.fill.store(reinterpret_cast<Fill>(relay[fill_at]), std::memory_order_release);
        s.override_original.store(reinterpret_cast<Override>(relay[override_at]), std::memory_order_release);
        s.set_transform.store(reinterpret_cast<SetTransform>(base + targets[set_transform_at].rva), std::memory_order_release);
        s.set_fov.store(reinterpret_cast<SetFov>(base + targets[set_fov_at].rva), std::memory_order_release);
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
        // On: the replay camera's say first (it only shakes again what the fill shook), then the fill;
        // off: the other way round. Should one not switch, those switched in this call go back as they were.
        constexpr std::array<std::size_t, hooked_count> on_order{override_at, fill_at};
        constexpr std::array<std::size_t, hooked_count> off_order{fill_at, override_at};
        const auto& order = on ? on_order : off_order;
        const auto was = s.enabled;
        for (std::size_t i = 0; i < hooked_count; ++i) {
            if (set(s, order[i], on)) continue;
            for (std::size_t j = i; j-- > 0;) (void)set(s, order[j], was[order[j]]);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void set_transform(std::uintptr_t view, const Transform& transform) noexcept {
    if (const auto setter = state().set_transform.load(std::memory_order_acquire); setter && view) setter(view, &transform);
}
void set_fov(std::uintptr_t view, float fov) noexcept {
    if (const auto setter = state().set_fov.load(std::memory_order_acquire); setter && view) setter(view, fov);
}
} // namespace dingosdk::replay_shake::game_view

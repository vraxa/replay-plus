#include "render_view_hook.h"
#include "hook_interpose.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "replay_shake_build.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <mutex>

namespace dingosdk::render_view {
namespace {
namespace build = addr::skater_render;
using RenderView = void (*)(std::uintptr_t blackboard, std::uintptr_t current, std::uintptr_t previous, std::uint8_t jitter);
constexpr auto client_count = static_cast<std::size_t>(Client::count);

struct State {
    std::mutex mutex; // preparing and switching: client thread
    bool prepared{}, failed{}, enabled{};
    std::uintptr_t base{};
    void* target{};
    std::atomic<RenderView> original{};
    // The skater x-ray's own hook (hook_interpose.h): run in place of the original while it wants it.
    std::atomic<RenderView> inner{};
    std::array<std::atomic<Callback>, client_count> before{}, after{};
    std::atomic<unsigned> wanted{}; // a bit per client
};
// Lives until the process ends: the detour can run on a render thread at any time.
State& state() {
    static auto* value = new State;
    return *value;
}
unsigned bit(Client client) noexcept { return 1U << static_cast<unsigned>(client); }

// A hook must not leave the game a different last error.
struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
};
void detour(std::uintptr_t blackboard, std::uintptr_t current, std::uintptr_t previous, std::uint8_t jitter) {
    auto& s = state();
    const auto wanted = s.wanted.load(std::memory_order_acquire);
    if (wanted) {
        LastError error;
        for (std::size_t i = 0; i < client_count; ++i)
            if (wanted & (1U << i))
                if (const auto callback = s.before[i].load(std::memory_order_acquire)) callback(current, previous, blackboard, jitter);
    }
    // The x-ray's hook calls the original itself, then reads the view.
    const auto inner = (wanted & bit(Client::xray)) ? s.inner.load(std::memory_order_acquire) : nullptr;
    (inner ? inner : s.original.load(std::memory_order_acquire))(blackboard, current, previous, jitter);
    if (wanted) {
        LastError error;
        for (std::size_t i = 0; i < client_count; ++i)
            if (wanted & (1U << i))
                if (const auto callback = s.after[i].load(std::memory_order_acquire)) callback(current, previous, blackboard, jitter);
    }
}

// Under the mutex from here on.
bool prepare(State& s, std::uintptr_t base) noexcept {
    if (s.prepared) return s.base == base;
    if (s.failed || !base) return false;
    const auto& contract = build::render_view_contract;
    std::array<unsigned char, 32> actual{};
    if (!memory::peek(base + contract.rva, actual) || actual != contract.bytes) {
        s.failed = true;
        logging::log(logging::Level::warning, logging::Channel::graphics,
            "The renderer's view set-up at 0x{:x} is not this build's: nothing changes or reads the views it draws.", contract.rva);
        return false;
    }
    auto* target = reinterpret_cast<void*>(base + contract.rva);
    void* relay{};
    auto status = hook_prepare(target, reinterpret_cast<void*>(&detour), &relay);
    if (status == HookOk && !relay) {
        (void)hook_remove(target);
        status = HookUnsupportedFunction;
    }
    if (status != HookOk) {
        s.failed = true;
        logging::log(logging::Level::warning, logging::Channel::graphics,
            "Hooking the renderer's view set-up failed ({}).", hook_status_string(status));
        return false;
    }
    // The relay is published before the detour can ever be enabled (switch_client()).
    s.original.store(reinterpret_cast<RenderView>(relay), std::memory_order_release);
    s.base = base;
    s.target = target;
    s.prepared = true;
    return true;
}
HookStatus switch_client(State& s, Client client, bool on) noexcept {
    if (!s.prepared) return on ? HookNotFound : HookOk; // nothing to switch
    const auto before = s.wanted.load(std::memory_order_acquire);
    const auto next = on ? before | bit(client) : before & ~bit(client);
    const bool enable = next != 0;
    if (enable != s.enabled) {
        // A client switched on is in the mask before the detour can first run.
        if (enable) s.wanted.store(next, std::memory_order_release);
        const auto status = enable ? hook_enable(s.target) : hook_disable(s.target);
        if (status != HookOk) {
            s.wanted.store(before, std::memory_order_release);
            logging::log(logging::Level::warning, logging::Channel::graphics,
                "Could not {} the renderer's view hook ({}).", enable ? "enable" : "disable", hook_status_string(status));
            return status;
        }
        s.enabled = enable;
    }
    s.wanted.store(next, std::memory_order_release);
    return HookOk;
}
// Whether one of the x-ray's hook calls names the view set-up (hook_interpose.h). The first, its
// hook_prepare, finds it by its bytes and prepares the shared detour, should Replay Shake not have.
bool is_view_setup(State& s, void* target) noexcept {
    if (s.prepared) return target == s.target;
    const auto at = reinterpret_cast<std::uintptr_t>(target);
    const auto& contract = build::render_view_contract;
    std::array<unsigned char, 32> actual{};
    return !s.failed && at > contract.rva && memory::peek(at, actual) && actual == contract.bytes && prepare(s, at - contract.rva);
}
} // namespace

bool start(std::uintptr_t base) noexcept {
    auto& s = state();
    try {
        std::lock_guard lock(s.mutex);
        return prepare(s, base);
    } catch (...) {
        return false;
    }
}

void set_before(Client client, Callback callback) noexcept {
    state().before[static_cast<std::size_t>(client)].store(callback, std::memory_order_release);
}
void set_after(Client client, Callback callback) noexcept {
    state().after[static_cast<std::size_t>(client)].store(callback, std::memory_order_release);
}

bool want(Client client, bool on) noexcept {
    auto& s = state();
    try {
        std::lock_guard lock(s.mutex);
        return switch_client(s, client, on) == HookOk;
    } catch (...) {
        return false;
    }
}
bool wanted(Client client) noexcept { return (state().wanted.load(std::memory_order_acquire) & bit(client)) != 0; }
} // namespace dingosdk::render_view

// The skater x-ray's hook calls (Extension/Skater/xray_render.cpp, through hook_interpose.h). Its hook
// for the view set-up is kept and run inside the shared detour, given the original's relay to call;
// every other target is the hook service's.
namespace dingosdk::xray {
namespace {
namespace view = render_view;
template<class Action> HookStatus shared(void* target, Action action, HookStatus (WINAPI* otherwise)(void*)) noexcept {
    auto& s = view::state();
    try {
        std::unique_lock lock(s.mutex);
        if (!view::is_view_setup(s, target)) {
            lock.unlock();
            return otherwise(target);
        }
        return action(s);
    } catch (...) {
        return HookNotInitialized;
    }
}
} // namespace

HookStatus WINAPI hook_prepare(void* target, void* replacement, void** original) {
    auto& s = view::state();
    try {
        std::unique_lock lock(s.mutex);
        if (!view::is_view_setup(s, target)) {
            lock.unlock();
            return dingosdk::hook_prepare(target, replacement, original);
        }
        s.inner.store(reinterpret_cast<view::RenderView>(replacement), std::memory_order_release);
        if (original) *original = reinterpret_cast<void*>(s.original.load(std::memory_order_acquire));
        return HookOk;
    } catch (...) {
        return HookNotInitialized;
    }
}
HookStatus WINAPI hook_enable(void* target) {
    return shared(target, [](view::State& s) { return view::switch_client(s, view::Client::xray, true); },
        &dingosdk::hook_enable);
}
HookStatus WINAPI hook_disable(void* target) {
    return shared(target, [](view::State& s) { return view::switch_client(s, view::Client::xray, false); },
        &dingosdk::hook_disable);
}
HookStatus WINAPI hook_remove(void* target) {
    return shared(target, [](view::State& s) {
        const auto status = view::switch_client(s, view::Client::xray, false);
        s.inner.store(nullptr, std::memory_order_release);
        return status;
    }, &dingosdk::hook_remove);
}
} // namespace dingosdk::xray

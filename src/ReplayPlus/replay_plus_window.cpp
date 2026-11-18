#include "replay_plus_window.h"
#include "replay_plus_link.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <thread>

namespace dingosdk::replay_plus::window {
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT show_message = WM_APP + 41, hide_message = WM_APP + 42, give_back_message = WM_APP + 43, mark_message = WM_APP + 44;
constexpr wchar_t backdrop_class[] = L"ReplayPlusBackdrop";

// The backdrop: a black window of its own, on a thread of its own (with its own messages), so the game's
// threads never wait on it. It goes just behind the game's window, over its monitor; and while it is up
// the taskbar is told the game is full screen (ITaskbarList2::MarkFullscreenWindow), so it keeps out of
// the way when the game is in front.
struct Backdrop {
    std::once_flag started;
    std::atomic<HWND> window{};
    ComPtr<ITaskbarList2> taskbar; // the backdrop's thread's only
    HWND game{};                   // the game's window it last covered for (the backdrop's thread's)
    // Test builds: the taskbar left alone (research: which step takes the game's focus).
    std::atomic<bool> no_taskbar{};
};
Backdrop& backdrop() {
    static auto* value = new Backdrop;
    return *value;
}
void cover(HWND self, HWND game) {
    if (!game || !IsWindow(game)) return;
    MONITORINFO info{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &info)) return;
    const RECT m = info.rcMonitor;
    // Just behind the game's window (inserted after it), over the whole monitor. Never active: should the
    // system make it so anyway, it gives the game its activation back at once (WM_ACTIVATE below).
    auto& b = backdrop();
    b.game = game;
    const HWND before = GetForegroundWindow();
    SetWindowPos(self, game, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    if (b.taskbar && !b.no_taskbar.load(std::memory_order_relaxed)) (void)b.taskbar->MarkFullscreenWindow(game, TRUE);
    const HWND after = GetForegroundWindow();
    if (before == game && after != game) {
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: the backdrop took the foreground from the game (it is given back).");
        SetForegroundWindow(game);
    }
}
LRESULT CALLBACK backdrop_proc(HWND self, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ACTIVATE:
        // Activated after all (the system can, despite WS_EX_NOACTIVATE): the game's activation back, as soon
        // as this message is done with.
        if (LOWORD(w) != WA_INACTIVE) {
            logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: the backdrop was activated ({}): the game gets it back.",
                         LOWORD(w) == WA_CLICKACTIVE ? "by a click" : "by the system");
            PostMessageW(self, give_back_message, 0, 0);
        }
        return 0;
    case give_back_message: {
        const auto game = backdrop().game;
        if (game && IsWindow(game)) SetForegroundWindow(game);
        return 0;
    }
    case mark_message: {
        auto& b = backdrop();
        if (b.taskbar && IsWindow(reinterpret_cast<HWND>(l))) (void)b.taskbar->MarkFullscreenWindow(reinterpret_cast<HWND>(l), w != 0);
        return 0;
    }
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(self, &r);
        FillRect(reinterpret_cast<HDC>(w), &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(self, &paint);
        EndPaint(self, &paint);
        return 0;
    }
    case show_message: cover(self, reinterpret_cast<HWND>(l)); return 0;
    case hide_message: {
        ShowWindow(self, SW_HIDE);
        auto& b = backdrop();
        const auto game = reinterpret_cast<HWND>(l);
        if (b.taskbar && game && IsWindow(game)) (void)b.taskbar->MarkFullscreenWindow(game, FALSE);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(self, message, w, l);
}
void backdrop_thread() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto& b = backdrop();
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&b.taskbar))) && FAILED(b.taskbar->HrInit()))
        b.taskbar.Reset();
    WNDCLASSEXW type{sizeof(WNDCLASSEXW)};
    type.lpfnWndProc = &backdrop_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = backdrop_class;
    RegisterClassExW(&type);
    const HWND self = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, backdrop_class, L"Replay Plus", WS_POPUP, 0, 0, 16, 16, nullptr,
                                      nullptr, type.hInstance, nullptr);
    b.window.store(self, std::memory_order_release);
    MSG message{};
    while (self && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    b.taskbar.Reset();
    if (SUCCEEDED(com)) CoUninitialize();
}
HWND backdrop_window() {
    auto& b = backdrop();
    std::call_once(b.started, [] { std::thread(&backdrop_thread).detach(); });
    return b.window.load(std::memory_order_acquire);
}

// The game's window as Replay Plus has shaped it (game thread only).
struct Shape {
    bool shaped{};
    HWND game{};
    RECT original{}; // where it was, to put it back
    Size size;
    std::uint64_t next_size_try{}, next_cover{};
};
Shape& shape() {
    static Shape value;
    return value;
}
bool same(const RECT& a, const RECT& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }
} // namespace

Size fitted(float aspect, Size monitor) noexcept {
    if (!(aspect > 0.1f) || monitor.width <= 0 || monitor.height <= 0) return {};
    const float screen = static_cast<float>(monitor.width) / static_cast<float>(monitor.height);
    Size s;
    if (aspect <= screen) {
        s.height = monitor.height & ~1;
        s.width = std::min(static_cast<int>(std::lround(static_cast<float>(monitor.height) * aspect * 0.5f)) * 2, monitor.width & ~1);
    } else {
        s.width = monitor.width & ~1;
        s.height = std::min(static_cast<int>(std::lround(static_cast<float>(monitor.width) / aspect * 0.5f)) * 2, monitor.height & ~1);
    }
    return s;
}

Size monitor_size() noexcept {
    const auto game = reinterpret_cast<HWND>(link::game_window());
    if (!game || !IsWindow(game)) return {};
    MONITORINFO info{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &info)) return {};
    return {static_cast<int>(info.rcMonitor.right - info.rcMonitor.left), static_cast<int>(info.rcMonitor.bottom - info.rcMonitor.top)};
}

bool shaped() noexcept { return shape().shaped; }
void test_backdrop(bool show) noexcept {
    const HWND self = backdrop_window();
    const auto game = reinterpret_cast<HWND>(link::game_window());
    if (self && game) PostMessageW(self, show ? show_message : hide_message, 0, reinterpret_cast<LPARAM>(game));
}
void test_taskbar(bool mark) noexcept {
    const HWND self = backdrop_window();
    const auto game = reinterpret_cast<HWND>(link::game_window());
    if (self && game) PostMessageW(self, mark_message, mark ? 1 : 0, reinterpret_cast<LPARAM>(game));
}
void test_no_taskbar(bool none) noexcept { backdrop().no_taskbar.store(none, std::memory_order_relaxed); }

std::string keep(Size size) noexcept {
    try {
        auto& s = shape();
        const auto game = reinterpret_cast<HWND>(link::game_window());
        const auto now = GetTickCount64();
        const HWND self = backdrop_window();
        if (size.width > 0 && size.height > 0 && game && IsWindow(game)) {
            MONITORINFO info{sizeof(MONITORINFO)};
            if (!GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &info)) return {};
            const RECT m = info.rcMonitor;
            const RECT wanted{m.left + (m.right - m.left - size.width) / 2, m.top + (m.bottom - m.top - size.height) / 2, 0, 0};
            RECT target = wanted;
            target.right = target.left + size.width;
            target.bottom = target.top + size.height;
            std::string said;
            if (!s.shaped || s.game != game) {
                GetWindowRect(game, &s.original);
                s.shaped = true;
                s.game = game;
                s.next_size_try = 0;
                s.next_cover = 0;
                said = std::format("Replay Plus: the game's window is {}x{} in the middle of its monitor, black around it.", size.width,
                                   size.height);
            }
            s.size = size;
            RECT now_at{};
            GetWindowRect(game, &now_at);
            if (!same(now_at, target) && now >= s.next_size_try) {
                // Asynchronously: the game's own thread moves its window, and nothing here waits for it.
                SetWindowPos(game, nullptr, target.left, target.top, size.width, size.height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                s.next_size_try = now + 500;
            }
            // The backdrop just behind the game, whenever the game is in front and it is not there.
            if (self && now >= s.next_cover && (!IsWindowVisible(self) || (GetForegroundWindow() == game && GetWindow(game, GW_HWNDNEXT) != self))) {
                PostMessageW(self, show_message, 0, reinterpret_cast<LPARAM>(game));
                s.next_cover = now + 250;
            }
            return said;
        }
        if (!s.shaped) return {};
        s.shaped = false;
        if (s.game && IsWindow(s.game))
            SetWindowPos(s.game, nullptr, s.original.left, s.original.top, s.original.right - s.original.left, s.original.bottom - s.original.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        if (self) PostMessageW(self, hide_message, 0, reinterpret_cast<LPARAM>(s.game));
        return "Replay Plus: the game's window is as it was.";
    } catch (...) {
        return {};
    }
}
} // namespace dingosdk::replay_plus::window

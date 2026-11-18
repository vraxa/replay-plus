#pragma once
#include <cstdint>
#include <string>

// The replay editor's own picture shape (Settings::replay_aspect): the game renders a picture of that
// shape when its swap chain is (RenderDevice.FullscreenWidth / Height, which it takes while it runs), and
// shows it 1:1 when its window is that size too. Replay Plus sizes the window to it, in the middle of
// its monitor, with a black window of its own behind it over the rest of the monitor, and tells the
// taskbar the game is full screen (so it does not show over it); and puts the window back as it was
// after. The game's picture is never stretched: a 4:3 swap chain in a 16:9 window would be.
namespace dingosdk::replay_plus::window {
// The size (even numbers) of a picture of `aspect` (width / height) as big as fits on a monitor of
// this size.
struct Size {
    int width{}, height{};
    bool operator==(const Size&) const = default;
};
Size fitted(float aspect, Size monitor) noexcept;

// The size of the monitor the game's window is on (0, 0: not known; the window is link::game_window()).
Size monitor_size() noexcept;

// Game thread, every tick: the window `size` in the middle of its monitor with the backdrop behind it,
// or (0, 0) as it was. Says what it did, once, when it changes (empty: nothing to say).
std::string keep(Size size) noexcept;
// Whether the window is shaped now (and the backdrop up).
bool shaped() noexcept;
// Research (test builds): the backdrop shown behind the game's window as it is (or hidden), and the taskbar
// told (or not) that the game is full screen, each alone; and whether showing it tells the taskbar.
void test_backdrop(bool show) noexcept;
void test_taskbar(bool mark) noexcept;
void test_no_taskbar(bool none) noexcept;
} // namespace dingosdk::replay_plus::window

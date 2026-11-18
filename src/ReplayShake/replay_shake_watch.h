#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// `replayshake probe`: which of the game's code reads (or writes) a few places, found with the
// processor's data breakpoints; and where in the game's memory copies of a camera are. Diagnostic only:
// nothing the game does changes.
//
// A watch window arms the breakpoints in every thread of the game for a moment (a worker thread
// suspends each thread in turn to set its debug registers), a vectored exception handler counts each
// access (the instruction after it, the calls it came through, the thread, and where in the camera's
// update it came), and the window disarms them again. The accesses watched are only slower meanwhile.
//
// A search reads the game's memory that is in RAM (never what is paged out), on a worker thread of low
// priority, for copies of a camera: the whole transform, or its position alone.
namespace dingosdk::replay_shake::watch {
inline constexpr std::size_t slot_count = 4;
// Eight bytes to watch (aligned to 8; 0 for none), what to call them, and whether writes alone count.
struct Spot {
    std::uintptr_t address{};
    std::string name;
    bool writes{};
};
// What happened besides the accesses, in the window's sequence: the camera's update (on the engine
// thread) and the renderer's set-ups of its main view, each with up to three positions.
enum class Mark : std::uint8_t { access, update_start, camera_set, handing, update_end, main_view };
// Client thread: opens a window of `window_ms` on these spots, `engine_thread` updating the camera, and
// `context` said with what it found. False while another window is open or not yet reported, or when
// nothing could be watched.
bool arm(const std::array<Spot, slot_count>& spots, unsigned window_ms, std::uint32_t engine_thread, std::string context) noexcept;
// Any thread: one mark, kept while a window is open (the camera's update marks also say where in it the
// engine thread is, for its accesses); `at`, an address to say with it (the main view's).
void mark(Mark what, const float* first = nullptr, const float* second = nullptr, const float* third = nullptr,
    std::uintptr_t at = 0) noexcept;
// Whether a window is open, or closed and not yet reported.
bool busy() noexcept;
// A write a window caught (on a place watched for writes only): the instruction after it, rcx and rdx
// at its first (a copy's destination and source, as the game's copies take them), the place, and how
// often.
struct Write {
    std::uint64_t rip{}, rcx{}, rdx{};
    std::uintptr_t place{};
    std::uint32_t count{};
};
// Client thread: once a window has closed, what it found (lines to log), and the window is free again;
// empty until then. `writes`, when given, gets the writes caught.
std::vector<std::string> report(std::uintptr_t image_base, std::vector<Write>* writes = nullptr);

// A copy of the camera found: where, whether the whole transform or its position alone, and the object
// it seems to be in (the nearest vtable of Skate.exe's before it, within 4 KB; 0 none).
struct Copy {
    std::uintptr_t at{};
    bool whole{};
    std::uintptr_t object{}, vtable{}; // vtable: an offset in Skate.exe
};
// Client thread: starts a search for copies of `camera` (a transform: rows right, up, back, position),
// leaving out the ranges in `own` ([begin, end): what Replay Shake keeps itself). False while another
// search runs or waits to be reported.
bool find(const std::array<float, 16>& camera, std::vector<std::pair<std::uintptr_t, std::uintptr_t>> own,
    std::uintptr_t image_base, std::string context) noexcept;
// Client thread: once the search has ended, what it found (lines to log) and the copies; empty lines
// until then.
std::vector<std::string> found(std::uintptr_t image_base, std::vector<Copy>& copies);
// The range the watch keeps its own records in (its marks hold positions), for a search to leave out.
std::pair<std::uintptr_t, std::uintptr_t> own_memory() noexcept;
} // namespace dingosdk::replay_shake::watch

#pragma once
#include "replay_plus_settings.h"
#include "replay_plus_lens.h"
#include "Extension/ReplayShake/replay_shake_model.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// What the three sides of Replay Plus hand each other, with no game access: the runtime (the game
// thread, which owns the settings), the overlay (the render thread: the picture's pass and the
// replay editor's panel) and the export (the encoder's thread). Every hand-over is a snapshot
// published whole, or a queue, so no side ever waits on another for long.
namespace dingosdk::replay_plus::link {
// The clock every time here is on (milliseconds, steady): compare with it, not another.
std::uint64_t now_ms() noexcept;

// ---- The picture: game thread -> render thread and export thread ------------------------------
struct Picture {
    bool on{};          // the replay editor is open (or a test asks) and the settings change the picture
    Settings settings;
    std::string hood_path;  // the hood's picture file (the Mk1's, or the player's own), empty: none
    std::uint64_t hood_serial{}; // changes whenever the hood's picture should be read again
    // The camera light, in the game's view: its near and far planes (metres), which the game's depth is
    // measured between (the view's description, replay_plus.cpp view_built).
    float near_plane{0.06f};
    float far_plane{11000.0f};
    // Autofocus is on (in the replay editor): the render thread reads how far away the middle of the
    // picture is (note_focus_depth), from the game's depth.
    bool autofocus{};
    bool in_replay{};  // the replay editor is open (its button hint shows with the game's UI)
    float fov_auto{150}; // degrees: the field of view Auto picks (settings.fov_mode automatic)
    bool exporting{};  // a video is being filmed (the pass goes on under the game's UI: the video films what it drew)
};
void publish_picture(std::shared_ptr<const Picture> picture) noexcept;
std::shared_ptr<const Picture> picture() noexcept; // never null

// The vertical field of view (radians) the game built the picture's view with, as the replay camera
// left it, for the frame being made (game thread, from the view hook); 0 while not known. And when
// (GetTickCount64) it was last told, so a stale one is not trusted.
void note_fov(float fov) noexcept;
float fov() noexcept;
std::uint64_t fov_at() noexcept;
// How far away (metres) what is in the middle of the picture is, as the render thread last read it
// from the game's depth (autofocus), and when; 0 while not known.
void note_focus_depth(float metres) noexcept;
float focus_depth() noexcept;
std::uint64_t focus_depth_at() noexcept;

// The hood's picture, decoded (a worker thread, when Picture::hood_path changes): 8-bit RGBA with
// straight alpha, for the render thread to upload and the export thread to lay over exported frames.
struct HoodImage {
    std::string path;
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
    std::uint64_t serial{};
    lens::Hole hole; // its see-through middle (the lens is fitted to it)
};
void publish_hood(std::shared_ptr<const HoodImage> image) noexcept;
std::shared_ptr<const HoodImage> hood() noexcept; // null while there is none

// ---- The panel: game thread -> render thread ---------------------------------------------------
struct ShakeView {
    bool available{}, on{};
    replay_shake::Shake fallback;
    std::uint32_t seed{1};
};
struct View {
    std::uint64_t revision{};
    bool available{};    // Replay Plus started on this game build
    bool in_replay{};    // the replay editor is open
    Settings settings;
    std::vector<std::string> presets;    // the presets folder's, by name
    std::vector<std::string> hood_files; // the overlays folder's pictures, by file name
    float fov_degrees{};                 // the replay camera's field of view now (0: not known)
    float fov_auto{};                    // Auto's field of view (degrees)
    float centre_zoom{1};                // how much the fisheye enlarges the middle of the game's picture now
    float resolution_now{};              // Render.ResolutionScale now (0: not read)
    // The render scale Replay Plus would ask in the replay editor (Sharp middle, VIDEO), and the most the
    // card's video memory allows there now (keep_memory_guard; 0: not known).
    float resolution_wanted{}, resolution_limit{};
    std::string status;                  // one line: what Replay Plus is doing
    std::string presets_folder, overlays_folder, import_folder, export_folder;
    ShakeView shake;
    std::string reply;                   // the last command's answer, for the panel's foot
    std::uint64_t reply_serial{};
    // The replay's clock (its time now, while the replay is up) and the focus there.
    bool playhead_ok{};
    double playhead{};
    float focus_distance_now{}, focus_blur_now{};
    bool advanced_edit{}; // the replay editor's Advanced Edit is up (its button hint shows there)
    // The replay editor's timeline (the game's): where it starts on the replay's clock, and how long.
    bool timeline_ok{};
    double timeline_start{}, timeline_length{};
    // The shake at the playhead (its keys, or Replay Shake's own switch), and that switch.
    bool shake_here{}, shake_switch{};
    // R3 is held in the replay editor (its shortcuts' guide shows), and what the last shortcut did (and
    // when, now_ms()).
    bool r3_held{};
    std::string toast;
    std::uint64_t toast_at{};
};
// The panel is worked from the keyboard (or a pad the game reads itself): the game's input is then
// paused whole while it is open, rather than only the pad's buttons kept from it.
std::atomic<bool>& panel_keyboard() noexcept;
// Research (`replayplus trace <frames>`): the render side logs the game's texture transitions for that
// many frames, to find where its picture is finished before its UI is drawn.
std::atomic<int>& trace_frames() noexcept;
// Research (test builds, `replayplus exportdepth latest|matched`): exported frames lit from the latest depth,
// as before 2026-11-17, rather than the depth of the frame each was drawn in; to compare the two.
std::atomic<bool>& export_depth_latest() noexcept;
// The pad's buttons Replay Plus keeps from the game for `ms` from now (the panel open on a pad: only
// its buttons, so the sticks and triggers still work the replay; and after it closes, until what
// closed it is let go). ReSkate's input capture ORs hidden_pad_buttons into what it hides from the
// game's pad reads (Extension/UI/Overlay/input_capture.cpp game_hidden_buttons).
void hide_pad_buttons(std::uint64_t ms) noexcept;
std::uint16_t hidden_pad_buttons() noexcept;

// The game's window, as the overlay last saw it (0: not known). Any thread.
void note_game_window(std::uintptr_t window) noexcept;
std::uintptr_t game_window() noexcept;

// The frame, as the render thread last drew one for Replay Plus: the back buffer's size, and the size the
// game rendered its picture at before its UI (its render scale applied; 0 while not seen); and the GPU the
// game renders on (its adapter's LUID; 0 while not known). For the game thread's video memory guard
// (replay_plus.cpp keep_memory_guard). Any thread.
struct FrameSizes {
    std::uint32_t screen_width{}, screen_height{}, render_width{}, render_height{};
    std::uint64_t at{}; // now_ms() when told (0: never)
};
void note_frame_sizes(std::uint32_t screen_width, std::uint32_t screen_height, std::uint32_t render_width, std::uint32_t render_height) noexcept;
FrameSizes frame_sizes() noexcept;
void note_adapter(std::uint64_t luid) noexcept;
std::uint64_t adapter() noexcept;
// How many frames in a row the pass has drawn under the game's UI (into the game's picture), as the render
// thread last said (0: the last frame it did not). The game films its frame after its last pass, so while
// this runs a video's frames already have the lens, the grade, the hood and the light in them: the export
// leaves them as they are (replay_plus.cpp export_frame) instead of finishing them on the CPU.
void note_drawn_under_ui(bool drawn) noexcept;
std::uint32_t drawn_under_ui() noexcept;
// The render side finishing an exported frame on the GPU (replay_plus_render.cpp overlay::finish_export:
// 8-bit, blue first, in place). False when it could not; the frame is then as it was.
bool finish_export_on_gpu(std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t pitch) noexcept;

// ---- The replay editor's pad: R3 is Replay Plus's button there ----------------------------------
// While on (renewed every tick by the game thread: the replay editor up, the panel closed, the
// shortcuts on; it lapses 300 ms after the last renewal), the game's own pad reads go through
// pad_filter (ReSkate's input capture, Extension/UI/Overlay/input_capture.cpp): R3 never reaches the
// game; while it is held nothing else of the pad's does either but the left stick (R3 with another
// button is Replay Plus's); and what was pressed with it stays kept from the game until it is let go.
// A tap of R3 alone comes back to the game as a short press (inject_pad), so the game's own R3 (Photo)
// still works. ReSkate's own reads (Replay Plus's among them) see the pad as it is.
void renew_pad_layer() noexcept;
void inject_pad(std::uint16_t buttons, std::uint64_t ms) noexcept;
// The game's read of the pad, changed in place; whether anything was changed.
bool pad_filter(std::uint16_t& buttons, std::uint8_t& left_trigger, std::uint8_t& right_trigger, std::int16_t& right_x,
                std::int16_t& right_y) noexcept;
void publish_view(std::shared_ptr<const View> view) noexcept;
std::shared_ptr<const View> view() noexcept; // never null

// ---- Commands: the panel and the menu -> game thread -------------------------------------------
// A console line, "replayplus set lens.zoom 1.2" or "replayshake default intensity 40": run on the
// game thread on its next tick, as typed in the console.
void post(std::string command);
std::vector<std::string> take();

// ---- The panel's own state, shared both ways ------------------------------------------------
// The panel is open (the render thread draws it and reads the pad; the game thread keeps the game's
// input from it meanwhile). Asked open by the game thread (the held combo, `replayplus panel`).
std::atomic<bool>& panel_open() noexcept;
// The game thread asks the panel to open (or close) itself on the render thread's next frame.
void ask_panel(bool open) noexcept;
// Render thread: the request, once.
bool take_panel_request(bool& open) noexcept;
bool panel_requested() noexcept;
// The render thread says when the panel last drew (GetTickCount64), so a panel that stopped being
// drawn (the overlay off) does not hold the game's input.
void note_panel_drawn() noexcept;
std::uint64_t panel_drawn_at() noexcept;

// ---- Research and test captures ------------------------------------------------------------
// `replayplus shot <name>`: the render thread saves the next frame it finishes (after its pass,
// before ReSkate's menu is drawn) as a BMP in the shots folder. Any thread.
void ask_shot(std::string path) noexcept;
bool take_shot(std::string& path) noexcept;
bool shot_asked() noexcept;
// What the render thread last saw, for `replayplus status` and the log.
struct RenderReport {
    std::uint32_t width{}, height{}, format{};
    int transfer{};
    std::uint64_t frames{};  // frames the pass drew
    std::string problem;     // why it drew nothing, if it could not
    float fit{};
};
void report(const RenderReport& report) noexcept;
RenderReport render_report() noexcept;
} // namespace dingosdk::replay_plus::link

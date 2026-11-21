#include "Extension/ReplayPlus/replay_plus.h"
#include "replay_shake.h"
#include "replay_shake_editor.h"
#include "replay_shake_model.h"
#include "replay_shake_settings.h"
#include "replay_shake_view.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Core/Platform/path_text.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/UI/game_view.h"
#include "Extension/UI/Overlay/overlay.h"
#include "replay_shake_build.h"
#include "camera_update_hook.h"
#include "game_view_hook.h"
#include "render_view_hook.h"
#include "replay_shake_export.h"
#include "replay_shake_frames.h"
#include "replay_shake_watch.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/replay_activity.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>

namespace dingosdk::replay_shake {
namespace {
using Clock = std::chrono::steady_clock;
namespace render = addr::skater_render;
constexpr std::uint64_t save_after_ms = 1000;    // a dragged slider saves once it rests
constexpr std::uint64_t save_retry_ms = 10000;   // after a save failed
constexpr std::uint64_t view_every_ms = 100;     // the menu's snapshot, while nothing else changes
constexpr std::uint64_t probe_read_every_ms = 500;
constexpr std::uint64_t lease_ms = 500;          // the renderer shakes only while client ticks keep coming
constexpr std::uint64_t kept_log_every_ms = 5000; // `replayshake probe`: what the renderer kept of the frames before
constexpr double most_ahead = 0.1;               // seconds a frame may run ahead of the last playhead reading
constexpr double most_rate = 8;                  // replay seconds per real second: faster is a scrub
constexpr std::size_t most_settings_bytes = 4 * 1024 * 1024;

double seconds_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double>(to - from).count();
}
std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return result;
}

// What the renderer shakes by, published whole by the game thread.
struct Frame {
    std::vector<Point> points;
    Shake fallback;
    std::uint32_t seed{};
    bool have_playhead{};
    double playhead{};       // seconds, when it last changed...
    Clock::time_point stamp; // ...at this moment...
    std::uint64_t playhead_session{}, playhead_frames{}; // ...and with this export (replay_shake_export.h) this far on
    std::uint64_t playhead_camera_frames{};              // ...and the camera updated this often in it (`source 1`)
    std::uint64_t playhead_build_frames{};               // ...and the game built the main view this often in it (`source 3`)
    double rate{};           // replay seconds per real second, from its last two changes
    double interval{};       // seconds between those two: how far a frame may run ahead of the last
    int export_fps{};
    // Replay Plus's shake keys, and where the replay's clock is read (gate_now).
    std::vector<Gate> gates;
    std::uintptr_t base{};
};
// How much of the shake there is now, by Replay Plus's shake keys at the replay's clock (all of it with
// no keys, or while the replay's clock is not up). While a video is exported, the replay's time as
// export_frames::replay_time has it (from where the export began on the timeline, by the frames encoded).
float gate_now(const Frame& frame) noexcept {
    if (frame.gates.empty() || !frame.base) return 1.0f;
    std::uint8_t lane{};
    double clock{};
    if (!memory::peek(frame.base + addr::replay_activity::playing, lane) || !lane ||
        !memory::peek(frame.base + addr::replay_activity::playhead, clock) || !std::isfinite(clock))
        return 1.0f;
    return gate_at(frame.gates, export_frames::replay_time(clock));
}

struct State {
    std::atomic<bool> ready{};
    std::uintptr_t base{};
    // The game thread's (client tick and console commands); the menu and renderer get copies.
    std::mutex mutex;
    Settings settings;
    std::filesystem::path file; // where the settings are saved
    std::string stale_build;    // the game build the replay cameras were found on, when it is not this one
    bool dirty{}, save_failed{};
    std::uint64_t save_at{};
    Editor editor;              // what was last read of the replay editor
    std::string editor_problem;
    std::optional<std::vector<double>> previous_keyframes;
    scan::Reader reader;
    std::vector<scan::Seen> seen;
    scan::UiReader menus;
    std::vector<Gate> gates; // Replay Plus's shake keys (set_gates)
    scan::GameUi ui;            // the game's menu state, as last read (while looking)
    std::string probe_menus;    // `replayshake probe`: the menu state as last logged
    bool gameplay_held{};       // a replay camera is on show, but no game menu: taken for gameplay, kept still
    bool menus_read{};          // the menu state has been read: should it go missing, no menu is up
    bool level{}, test{}, probe{}, hooked{};
    std::uint64_t hook_retry_at{}; // after the renderer's hook could not be switched
    std::string probe_cameras, probe_read;
    std::uint64_t probe_read_at{};
    std::optional<double> playhead;
    Clock::time_point playhead_at;
    std::uint64_t playhead_session{}, playhead_frames{};
    double rate{}, interval{};
    std::uint64_t revision{}, view_at{};
    // The renderer's.
    std::mutex frame_mutex;
    std::shared_ptr<const Frame> frame;
    // Until when (GetTickCount64) the main view shakes: each client tick renews it, so a game that
    // stops ticking (a load) stops shaking.
    std::atomic<std::uint64_t> live_until{};
    // `replayshake history`: put the cameras the renderer kept of the frames before back as they were
    // shown (carry()). On unless switched off, to compare.
    std::atomic<bool> carry{true};
    // `replayshake copies`: give the shake to exact copies of the main camera in views of other kinds
    // (other_view()). On unless switched off, to compare.
    std::atomic<bool> copies{true};
    // `replayshake passes`: which of each frame's set-ups of the main view shake (a bit each, from the
    // first; the eighth stands for the rest). One that does not keeps the game's camera while the game
    // copies it (shake_view(), restore_view()), to find which pass the shadows are worked out in.
    std::atomic<std::uint32_t> passes{0xff};
    // `replayshake source`: where the shake goes. On the transform the game builds the picture's view
    // from each frame (3, the default wherever the game's building of its views is known:
    // game_view_hook.h): everything the renderer works out from the view (the picture, its shadows,
    // culling, fog, the frame before for motion blur) comes from the shaken view, and no camera of the
    // game's is changed. Or on the camera itself (1, the default wherever only the game's camera
    // update is known: camera_update_hook.h), inside the game's update of it, right after the update
    // sets it for the frame: what the update hands on and everything drawn from the camera come with the
    // shake (gameplay's picture; the replay editor's is not drawn from that camera). Given its own back
    // at the start of the camera's next update, so that what works the camera out never sees the shake;
    // or (2, to tell where the renderer reads the camera) only once that update sets it afresh. Or on
    // the views the renderer draws (0).
    enum class Source : std::uint8_t { views, camera, held, game_views };
    Source source_mode{};
    // `replayshake source 3`: the hook on the game's building of its views is on (builds_hooked; after
    // it could not be switched, a few seconds before another try), and the views built shake (under
    // frame_mutex: the hook's callback reads it).
    bool builds_hooked{}, builds_on{};
    std::uint64_t builds_retry_at{};
    std::uint64_t playhead_build_frames{}; // the main view's builds in the export when the playhead last changed
    // `replayshake probe`: the camera on show (the first local view's) to measure the view built against.
    std::atomic<std::uintptr_t> probe_camera{};
    // The camera shaken last: what the game set it up as and what it was shaken to (its transform, the
    // same before its offset is added to its position, and its field of view).
    struct SourceShake {
        std::uintptr_t camera{};
        Matrix base{}, shown{}, base_unoffset{}, shown_unoffset{};
        float base_fov{}, fov{};
        bool unoffset{}; // the transform before its offset was shaken too
        bool used{};
        bool fresh{}; // put on in the camera's update under way
        // When it was worked out, and for what replay time: a camera the game sets more than once a
        // frame is given the frame's shake each time, the export's frames counted once.
        Clock::time_point at{};
        double time{};
    } source_shake;
    // The camera's state (camera_update_hook.h) the update set it from, the update's copy of it, the
    // state it handed on, and the blender's two (the replay editor draws its picture from those), each as
    // the game had it and as shaken (under the shake above).
    struct StateShake {
        std::uintptr_t at{};
        std::array<float, camera_update::state_size / sizeof(float)> base{}, shown{};
        float base_fov{}, fov{};
        bool used{};
    };
    enum StatePlace : std::size_t { state_set_from, state_copy, state_handed, state_blend, state_blend_kept, state_places };
    std::array<StateShake, state_places> source_states{};
    // Every camera the game set of late (camera_update_hook.h's `written`, any class), newest last: the
    // renderer finds in them which camera its main view is drawn from.
    struct CameraWrite {
        std::uintptr_t camera{}, vtable{}; // its vtable's address
        Matrix transform{};                // as it was left (shaken, should it be the one shaken)
        bool from_state{};
    };
    std::mutex census_mutex;
    std::array<CameraWrite, 48> census{};
    std::size_t census_next{};
    // `replayshake probe`: the cameras set since the last report, and how (under census_mutex).
    struct CameraTally {
        std::uintptr_t camera{}, vtable{};
        std::uint32_t from_state{}, from_transform{};
    };
    std::array<CameraTally, 12> tallies{};
    std::size_t tally_count{};
    std::uint32_t tallies_dropped{};
    // `replayshake follow`: the camera the renderer's main view is drawn from, should it be another
    // than the one on show (the renderer finds it: follow_view()), and its vtable; it is shaken instead,
    // right after the game sets it. 0: the one on show.
    std::atomic<std::uintptr_t> drawn_camera{}, drawn_vtable{};
    std::atomic<bool> follow{true};
    // `replayshake probe`: the main view's camera as last drawn without the shake (render thread), for
    // the client tick to look for (scan_for_view()), and when (GetTickCount64).
    std::mutex unshaken_mutex;
    Matrix unshaken_view{};
    std::uint64_t unshaken_at{};
    std::uintptr_t shown_camera{}; // client tick: the camera on show that counts, as last found
    std::uintptr_t logged_drawn{}; // client tick: the drawn camera last logged
    // For the renderer, under frame_mutex: the camera holds the shake (its views come with it as they
    // are), and what the cameras shaken of late were set up as and shaken to. Or the views drawn shake
    // instead (source 0, or the camera itself cannot be: another game build, no camera of the game's
    // own class on show, its hook not on), as the client tick last found.
    bool source_on{};
    bool views_on{};
    struct Written {
        Matrix base{}, shown{};
    };
    std::array<Written, 4> source_written{};
    std::size_t source_next{};
    // The camera the game's camera update shakes (0 none), and the thread that may: the engine thread.
    std::atomic<std::uintptr_t> camera_target{};
    std::atomic<DWORD> engine_thread{};
    bool camera_hooked{};            // the hook on the game's camera update is on
    std::uint64_t camera_retry_at{}; // after it could not be switched
    // (`replayshake probe`) Shakes put on the camera; on the state it was set from, the update's copy
    // of it and what the update handed on; the camera still shaken when its update ended, or changed by
    // then; the camera and the states given back before the game's next update of it; and updates of
    // it on another thread (left alone).
    std::uint32_t camera_shakes{}, camera_restores{};
    std::array<std::uint32_t, state_places> state_shakes{};
    std::uint32_t state_restores{}, held_at_end{}, changed_at_end{};
    std::uintptr_t other_blender{}; // the vtable of a blender of another class than the one known, last met
    std::atomic<std::uint32_t> camera_elsewhere{};
    // ...and writes of the camera followed on another thread (shaken all the same), and shakes let go
    // because the state was busy (the client tick holding it, while another thread set the camera).
    std::atomic<std::uint32_t> drawn_elsewhere{}, lock_missed{};
    // During an export: which one, the camera's updates in it so far (one a rendered frame, the one
    // encoded), where its replay time began, and the count when the playhead was last read in it.
    std::uint64_t camera_session{}, camera_frames{}, playhead_camera_frames{};
    double camera_export_base{};
    // `replayshake after`: the main view (and its previous view) keep the shake once the game has
    // copied them (on), or are given the game's camera back right after each copy (off), so that what
    // reads the views later sees the game's camera (restore_view()).
    std::atomic<bool> keep{true};
    std::atomic<bool> probe_render{}; // `replayshake probe`, for the render thread
    // Counts the times shaking started: the renderer forgets the frames it shook before.
    std::atomic<std::uint32_t> since{};
    bool was_live{};
    // `replayshake probe`: the renderer traces the view set-ups that come next.
    std::atomic<bool> trace_armed{};
    std::uint64_t kept_logged_at{}, trace_armed_at{};
    // `replayshake probe`: when the next watch window opens (replay_shake_watch.h), and how many have.
    std::uint64_t watch_at{};
    std::uint32_t watch_windows{};
    // The main view (and its previous view) the renderer set up last, for a window to watch.
    std::atomic<std::uintptr_t> main_view_at{}, previous_view_at{};
    // Once in a probe: a search of the game's memory for the main view's camera as drawn without the
    // shake, once it has stood still for a moment (a paused replay); then windows on the copies found,
    // for what writes them.
    bool find_started{}, find_running{};
    Matrix find_candidate{};
    std::uint32_t find_stable{};
    std::vector<watch::Copy> copies_watched;
    // The view whose camera the window open watches for writes (0: none); the view followed up the
    // copies the game makes of its views, should the main view's camera be copied from another; and how
    // many copies up that is.
    std::uintptr_t watched_view{}, upstream_view{};
    std::uint32_t upstream_hops{};
};
// Lives until the process ends: the renderer can call in at any time.
State& state() {
    static auto* value = new State;
    return *value;
}
// How deep this thread is in the state's lock (the client tick, a command): a camera update that
// comes meanwhile (camera_before() and the rest: around_camera()) is let go rather than waiting on itself.
thread_local int holding_state = 0;
struct Holding {
    Holding() noexcept { ++holding_state; }
    ~Holding() { --holding_state; }
    Holding(const Holding&) = delete;
    Holding& operator=(const Holding&) = delete;
};

// The 64-byte places in a view a camera may be kept in (16-byte aligned), for carry().
constexpr std::size_t camera_places = (render::view_size - 64) / 16 + 1;
static_assert(camera_places <= 32, "a place per bit of a std::uint32_t");
// `replayshake probe`: the renderer's views of other kinds (not the main picture's: shadows, the
// sky, reflections...), by kind.
struct OtherKind {
    std::uint32_t kind{}, setups{};
    std::uint32_t copies{}, places{};                   // set-ups holding the frame's camera as the game set it up, and where
    std::uint32_t shown{};                              // ...holding it shaken already
    std::uint32_t previous_copies{}, previous_places{}; // their previous view held the frame before's as the game set it up
    std::uint32_t early{}; // set up before the main view, from the camera that view then came with: no shake known yet
    // From one frame to the next while the main camera stood still (the game's camera, before any
    // copy was given the shake): its own camera stayed, or moved (it follows the shaken one).
    std::uint32_t still_kept{}, still_moved{};
};
// `replayshake probe`: how the renderer's view set-ups came and what carry() found, counted on the
// render thread; the client tick logs them and starts afresh.
struct Kept {
    std::uint32_t setups{};         // view set-ups shaken
    std::uint32_t frames{};         // frames of the game begun (frames::Place::next)
    std::uint32_t shown{};          // a view set up again, showing its frame's camera already
    std::uint32_t again{};          // another view of a frame, given the frame's camera
    std::uint32_t most_views{};     // the most views one frame was set up as
    std::uint32_t still_passes{};   // set-ups held still (`replayshake passes`)
    std::uint32_t released{};       // set-ups given the game's camera back after their copy (`replayshake after 0`)
    std::uint32_t source_seen{}, source_missed{}; // (`replayshake source 1`) set-ups that came with the camera shaken, or as the game set it up
    // (`replayshake source 1`) The main view's set-ups whose camera was another camera's the game set
    // of late, by camera (the first four found), or no camera's.
    struct DrawnFrom {
        std::uintptr_t camera{}, vtable{};
        std::uint32_t setups{};
    };
    std::array<DrawnFrom, 4> drawn_from{};
    std::uint32_t drawn_from_none{}, drawn_from_dropped{};
    std::uint32_t previous_shown{}; // the previous view held the camera the frame before showed...
    std::uint32_t previous_game{};  // ...the game's camera instead, unshaken: put back as shown
    std::uint32_t previous_other{}; // ...neither (a cut, or no previous view)
    std::array<std::uint32_t, 2 * camera_places> copies{}; // other copies put back: view, then previous
    // The frame's camera against the client's (Engine/Game/UI/game_view.h), as each frame begins.
    std::uint32_t client_same{}, client_close{}, client_far{}, client_none{};
    std::array<OtherKind, 12> others{};
    std::size_t other_count{};
    std::uint32_t other_kinds_dropped{};
    OtherKind* other(std::uint32_t kind) noexcept {
        for (std::size_t i = 0; i < other_count; ++i)
            if (others[i].kind == kind) return &others[i];
        if (other_count == others.size()) {
            ++other_kinds_dropped;
            return nullptr;
        }
        others[other_count] = {};
        others[other_count].kind = kind;
        return &others[other_count++];
    }
};
// What carry() found in the previous view's camera.
enum class Previous : std::uint8_t { none, shown, put_back, other };
// `replayshake probe`: the first view set-ups after it is switched on, one by one.
struct Traced {
    std::uint64_t at_us{};
    std::uint32_t thread{}, frame{}, kind{};
    std::uintptr_t view{}, previous{}, blackboard{};
    std::uint8_t jitter{};
    frames::Place place{};
    bool moved{}; // (a frame begun) the game's camera is not where it was the frame before
    Previous previous_state{};
    std::uint32_t copies{}, previous_copies{}; // (another kind) where it held the frame's camera, the frame before's
    bool copied{};                             // ...and was given the shake there (`replayshake copies`)
    std::uint32_t setup{};                     // (the main view) its number in the frame, from 0
    bool still{};                              // ...kept the game's camera for the copy (`replayshake passes`)
    bool released{};                           // ...given the game's camera back after it (`replayshake after 0`)
};
constexpr std::size_t trace_size = 40;
// `replayshake probe`: views of other kinds set up since the main view began its frame.
struct Recent {
    std::uintptr_t view{};
    std::uint64_t at_us{};
    std::uint32_t kind{};
};
// Render thread only (under its mutex).
struct Renderer {
    std::mutex mutex;
    frames::Frames tracker;
    std::uint32_t since{};       // the State::since its frames are of
    Kept kept;
    std::array<Traced, trace_size> trace{};
    std::size_t traced{};
    bool tracing{};
    std::uint32_t trace_frames{}; // frames begun since the trace started
    std::uint64_t trace_from_us{};
    std::array<Recent, 32> recent{};
    std::size_t recent_count{};
    // `replayshake probe`: views of other kinds, as the game set them up when last seen.
    struct Watched {
        std::uintptr_t view{};
        std::uint32_t kind{}, frame{};
        Matrix camera{}, base{}; // its camera, and the main view's as the game set it up then
        bool used{};
    };
    std::array<Watched, 16> watched{};
    std::size_t next_watched{};
    std::array<std::pair<std::uint32_t, std::uint32_t>, 12> kind_counts{}; // (kind, views of it) this frame...
    std::size_t kind_count_used{};
    std::uint32_t kind_counts_frame{};                                     // ...of frame number this
    std::uint64_t session{};
    double export_base{};
    Clock::time_point origin = Clock::now();
    // `replayshake follow`: another camera the main view keeps coming with, as the game set it, while
    // the one shaken does not show (how many set-ups in a row); once following one, the set-ups since
    // without its shake, and whether one came with it; the cameras followed that never brought it.
    struct Follow {
        std::uintptr_t candidate{}, vtable{};
        std::uint32_t streak{};
        std::uint32_t without{};
        bool confirmed{};
        std::array<std::uintptr_t, 8> failed{};
        std::size_t failed_count{};
    } follow;
};
Renderer& renderer() {
    static auto* value = new Renderer;
    return *value;
}
// `replayshake source 3`: the game's building of its views (game_view_hook.h), on the thread that
// builds them, under its own mutex (taken after the state's, never with the frame's or the renderer's held).
struct Builds {
    std::mutex mutex;
    Clock::time_point origin = Clock::now();
    // During an export: which one, the main view's builds in it so far, and where its replay time began.
    std::uint64_t session{}, frames{};
    double export_base{};
    // The frame's shake as last worked out (once a frame: views built within 2 ms of it are the same
    // frame's): when, for what replay time, and by how much.
    Clock::time_point at{};
    double time{};
    Offsets moved{};
    bool used{};
    // `replayshake probe`: counted since the last report.
    struct Counts {
        std::uint32_t filled{};          // view descriptions filled in while the views built shake
        std::uint32_t elsewhere{};       // ...on another thread than the client tick's
        std::uint32_t picture{}, others{}; // ...for the first local player (the picture), and for others...
        std::array<std::uint32_t, 4> other_players{}; // ...whose indices were these (the first four met)
        std::size_t other_player_count{};
        std::uint32_t shaken{}, still{}, unsound{}; // main views given the shake, a still one, or left (not a camera)
        std::uint32_t beside_same{};     // the transform kept beside the view was the view's own: shaken too
        std::uint32_t held{}, not_held{}; // once filled in, the view held the shaken camera, or not
        std::uint32_t zoomed{};
        // The replay camera's say right after (game_view_hook.h): it set the view anew (shaken again), it
        // left it, or it set something that is no camera's (left).
        std::uint32_t replayed{}, not_replayed{}, replay_unsound{};
        float replay_apart{}; // metres: the farthest the replay camera's view was from the game's own as filled in
        std::uint32_t frames{};          // frames the shake was worked out for
        std::uint32_t intervals{};       // ...and the times between two of them measured:
        double shortest_ms{}, longest_ms{};
        // The view's camera as the game filled it in, and as the replay camera set it anew, against the
        // camera on show as it was then.
        struct Against {
            std::uint32_t same{}, within_cm{}, beyond_cm{}, no_camera{};
            float farthest{}; // metres
        } filled_in, replay;
    } counts;
};
Builds& builds() {
    static auto* value = new Builds;
    return *value;
}

bool write_bytes(std::uintptr_t address, const void* source, std::size_t size) noexcept {
    if (address < 0x10000 || !size || size > memory::highest_user_address || address > memory::highest_user_address - size)
        return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(address), source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// A camera worth shaking: rows right, up and back of unit length, square to each other and
// right-handed. A mirrored view (a reflection) is left as it is.
bool sound(const Matrix& m) noexcept {
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (!std::isfinite(m[row * 4 + axis]) || std::abs(m[row * 4 + axis]) > 1e7f) return false;
    for (std::size_t a = 0; a < 3; ++a)
        for (std::size_t b = a; b < 3; ++b) {
            const float dot = m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] + m[a * 4 + 2] * m[b * 4 + 2];
            if (std::abs(dot - (a == b ? 1.0f : 0.0f)) > 0.05f) return false;
        }
    const float determinant = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
        m[2] * (m[4] * m[9] - m[5] * m[8]);
    return determinant > 0.5f;
}
// The replay time a frame shows. During an export, from the playhead read in that export, one
// step of the export's frame rate for every frame encoded since; before the playhead has been read
// in it, from where the export began. Each frame of the video is one step on, whatever the
// encoder's pace. Otherwise the playhead, run on at its pace between two readings of it (no further
// than they are apart); without a playhead, the time since the game started.
double frame_time(const Frame& frame, Renderer& r) {
    const auto now = Clock::now();
    const auto preview = [&] {
        if (!frame.have_playhead) return seconds_between(r.origin, now);
        return frame.playhead + std::clamp(frame.rate * seconds_between(frame.stamp, now), -frame.interval, frame.interval);
    };
    const auto capture = export_frames::capture();
    if (!capture.running) return preview();
    const double step = 1.0 / std::max(frame.export_fps, 1);
    if (frame.have_playhead && frame.playhead_session == capture.session && capture.frames >= frame.playhead_frames)
        return frame.playhead + static_cast<double>(capture.frames - frame.playhead_frames) * step;
    if (capture.session != r.session) {
        r.session = capture.session;
        r.export_base = preview() - static_cast<double>(capture.frames) * step;
    }
    return r.export_base + static_cast<double>(capture.frames) * step;
}
using ViewBytes = std::array<std::byte, render::view_size>;
// Whether the 64 bytes at `at` hold `camera`: the first three lanes of its rows, bit for bit (the
// renderer keeps other values in the fourth lanes).
bool holds(const ViewBytes& data, std::size_t at, const Matrix& camera) noexcept {
    for (std::size_t row = 0; row < 4; ++row)
        if (std::memcmp(data.data() + at + row * 16, camera.data() + row * 4, 3 * sizeof(float)) != 0) return false;
    return true;
}
// Those lanes of `camera` written at `at` in the view at `view`; its fourth lanes stay as they are.
bool put(std::uintptr_t view, std::size_t at, const Matrix& camera) noexcept {
    bool all = true;
    for (std::size_t row = 0; row < 4; ++row) all = write_bytes(view + at + row * 16, camera.data() + row * 4, 3 * sizeof(float)) && all;
    return all;
}
// The renderer tells how the picture moved since the frame before from the camera that frame was
// set up with, which it keeps in the previous view (and in copies elsewhere in the views): motion
// blur, temporal anti-aliasing and the lighting's temporal filters. Should it keep the game's
// camera there, unshaken, the whole shake reads as movement every frame, and the picture smears and
// swims. So every exact copy of a camera as the game set it up, where a shaken one was shown, is put
// back as shown: the frame before's in the previous view, this frame's (else the frame before's)
// elsewhere in the view itself.
Previous carry(Renderer& r, std::uintptr_t view, std::uintptr_t previous) noexcept {
    const auto& now = r.tracker.now();
    const auto& before = r.tracker.before();
    const bool now_moves = frames::moves(now), before_moves = frames::moves(before);
    auto result = Previous::none;
    const auto pass = [&](std::uintptr_t at, bool earlier) {
        ViewBytes data{};
        if (!memory::peek_bytes(at, data.data(), data.size())) {
            if (earlier) {
                ++r.kept.previous_other;
                result = Previous::other;
            }
            return;
        }
        for (std::size_t place = 0; place < camera_places; ++place) {
            const auto offset = place * 16;
            const bool camera_slot = offset == render::view_camera_offset;
            if (earlier && camera_slot) {
                if (before.used && holds(data, offset, before.shown)) {
                    ++r.kept.previous_shown;
                    result = Previous::shown;
                } else if (before_moves && holds(data, offset, before.base)) {
                    put(at, offset, before.shown);
                    float fov{};
                    std::memcpy(&fov, data.data() + render::view_fov_offset, sizeof(fov));
                    if (before.fov != before.base_fov && fov == before.base_fov)
                        (void)write_bytes(at + render::view_fov_offset, &before.fov, sizeof(before.fov));
                    ++r.kept.previous_game;
                    result = Previous::put_back;
                } else {
                    ++r.kept.previous_other;
                    result = Previous::other;
                }
                continue;
            }
            if (camera_slot) continue; // the view's own camera, shaken already
            const Matrix* shown{};
            if (earlier) {
                if (before_moves && holds(data, offset, before.base)) shown = &before.shown;
            } else if (now_moves && holds(data, offset, now.base)) {
                shown = &now.shown;
            } else if (before_moves && !frames::same_camera(before.base, now.base) && holds(data, offset, before.base)) {
                shown = &before.shown;
            }
            if (!shown) continue;
            put(at, offset, *shown);
            ++r.kept.copies[(earlier ? camera_places : 0) + place];
        }
    };
    if (previous && previous != view) pass(previous, true);
    else ++r.kept.previous_other;
    pass(view, false);
    return result;
}
std::uint64_t micros(const Renderer& r) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - r.origin).count());
}
// Under the renderer's mutex, first thing in each view set-up: a new start forgets the frames
// before, and a probe switched on starts its trace.
void begin_setup(State& s, Renderer& r, std::uint64_t now_us) noexcept {
    if (const auto since = s.since.load(std::memory_order_acquire); since != r.since) {
        r.since = since;
        r.tracker.reset();
        r.recent_count = 0;
        r.follow = {};
    }
    if (s.trace_armed.exchange(false, std::memory_order_acq_rel)) {
        r.tracing = true;
        r.traced = 0;
        r.trace_frames = 0;
        r.trace_from_us = now_us;
        r.kept = {}; // counted afresh from here, not since shaking began
    }
}
Traced* next_trace(Renderer& r) noexcept { return r.tracing && r.traced < r.trace.size() ? &r.trace[r.traced++] : nullptr; }
// `replayshake probe`, as a frame begins from `base`: the views of other kinds set up before it with
// that camera (they could not be given its shake), and whether it is the client's camera.
void note_frame(Renderer& r, const Matrix& base, bool moved, std::uint64_t now_us) noexcept {
    if (moved)
        for (std::size_t i = 0; i < r.recent_count; ++i) {
            const auto& recent = r.recent[i];
            ViewBytes data{};
            if (now_us - recent.at_us > frames::longest_frame_us || !memory::peek_bytes(recent.view, data.data(), data.size()))
                continue;
            for (std::size_t place = 0; place < camera_places; ++place)
                if (holds(data, place * 16, base)) {
                    if (auto* kind = r.kept.other(recent.kind)) ++kind->early;
                    break;
                }
        }
    r.recent_count = 0;
    const auto client = latest_game_view(std::chrono::milliseconds(500));
    if (!client) {
        ++r.kept.client_none;
        return;
    }
    Matrix world{};
    std::copy(client->world.begin(), client->world.end(), world.begin());
    if (frames::same_camera(world, base)) {
        ++r.kept.client_same;
        return;
    }
    float most{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t axis = 0; axis < 3; ++axis)
            most = std::max(most, std::abs(world[row * 4 + axis] - base[row * 4 + axis]));
    ++(most < 0.01f ? r.kept.client_close : r.kept.client_far);
}
// `replayshake passes`: what a set-up held still changed, on this thread, to put back once the game
// has copied the view (restore_view()), so the next set-up finds the view as the frame shows it. A
// few deep, should the game set a view up inside another's set-up.
struct Held { // plain, so the thread-local copies need no constructor (zeroed)
    std::uintptr_t view, previous;
    std::uint32_t view_places, previous_places;
    Matrix view_shown, previous_shown;
    float view_fov, previous_fov;
    bool view_fov_changed, previous_fov_changed;
};
thread_local std::array<Held, 4> held_stack;
thread_local std::size_t held_count;
// `replayshake after 0`: a view (and its previous view) to give the game's camera back once the game
// has copied it, on this thread.
struct Released { // plain, as Held
    std::uintptr_t view, previous;
    Matrix view_shown, view_base, previous_shown, previous_base;
    float view_fov, view_base_fov, previous_fov, previous_base_fov;
    bool view_moves, previous_moves;
};
thread_local std::array<Released, 4> released_stack;
thread_local std::size_t released_count;
// The frame's camera as the game set it up, in place of the shaken one, for the copy the game makes
// next: in the view and its other copies, and the frame before's in the previous view. Under the
// renderer's mutex. Whether anything was changed.
bool hold_still(Renderer& r, std::uintptr_t view, std::uintptr_t previous) noexcept {
    const auto& now = r.tracker.now();
    const auto& before = r.tracker.before();
    if (held_count == held_stack.size()) return false;
    Held held{};
    const auto exchange = [](std::uintptr_t at, const Matrix& from, const Matrix& to, float from_fov, float to_fov,
                              std::uint32_t& places, bool& fov_changed) {
        ViewBytes data{};
        if (!memory::peek_bytes(at, data.data(), data.size())) return;
        for (std::size_t place = 0; place < camera_places; ++place)
            if (holds(data, place * 16, from) && put(at, place * 16, to)) places |= 1U << place;
        float fov{};
        std::memcpy(&fov, data.data() + render::view_fov_offset, sizeof(fov));
        if (from_fov != to_fov && fov == from_fov) fov_changed = write_bytes(at + render::view_fov_offset, &to_fov, sizeof(to_fov));
    };
    if (frames::moves(now)) exchange(view, now.shown, now.base, now.fov, now.base_fov, held.view_places, held.view_fov_changed);
    if (frames::moves(before) && previous && previous != view)
        exchange(previous, before.shown, before.base, before.fov, before.base_fov, held.previous_places, held.previous_fov_changed);
    if (!held.view_places && !held.previous_places && !held.view_fov_changed && !held.previous_fov_changed) return false;
    held.view = view;
    held.previous = previous;
    held.view_shown = now.shown;
    held.previous_shown = before.shown;
    held.view_fov = now.fov;
    held.previous_fov = before.fov;
    held_stack[held_count++] = held;
    return true;
}
// Every copy of `from` in the view at `at` replaced by `to`, and its field of view likewise.
void replace_camera(std::uintptr_t at, const Matrix& from, const Matrix& to, float from_fov, float to_fov) noexcept {
    ViewBytes data{};
    if (!at || !memory::peek_bytes(at, data.data(), data.size())) return;
    for (std::size_t place = 0; place < camera_places; ++place)
        if (holds(data, place * 16, from)) (void)put(at, place * 16, to);
    float fov{};
    std::memcpy(&fov, data.data() + render::view_fov_offset, sizeof(fov));
    if (from_fov != to_fov && fov == from_fov) (void)write_bytes(at + render::view_fov_offset, &to_fov, sizeof(to_fov));
}
// What hold_still() changed for the view, put back once the game has copied it.
void restore_held(std::uintptr_t view) noexcept {
    if (!held_count || held_stack[held_count - 1].view != view) return;
    const auto held = held_stack[--held_count];
    const auto back = [](std::uintptr_t at, std::uint32_t places, const Matrix& shown, bool fov_changed, float fov) {
        for (std::size_t place = 0; place < camera_places; ++place)
            if (places & (1U << place)) (void)put(at, place * 16, shown);
        if (fov_changed) (void)write_bytes(at + render::view_fov_offset, &fov, sizeof(fov));
    };
    back(held.view, held.view_places, held.view_shown, held.view_fov_changed, held.view_fov);
    if (held.previous) back(held.previous, held.previous_places, held.previous_shown, held.previous_fov_changed, held.previous_fov);
}
// After the game copied a view (render_view_hook.h): what hold_still() changed for it, put back;
// then, with `replayshake after 0`, the game's camera given back.
void restore_view(std::uintptr_t view, std::uintptr_t, std::uintptr_t, std::uint8_t) noexcept {
    restore_held(view);
    if (!released_count || released_stack[released_count - 1].view != view) return;
    const auto released = released_stack[--released_count];
    if (released.view_moves) replace_camera(released.view, released.view_shown, released.view_base, released.view_fov, released.view_base_fov);
    if (released.previous_moves && released.previous)
        replace_camera(released.previous, released.previous_shown, released.previous_base, released.previous_fov,
            released.previous_base_fov);
}
// How far apart two cameras are: the most any of their rows' first three lanes differ by.
float apart(const Matrix& a, const Matrix& b) noexcept {
    float most{};
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t axis = 0; axis < 3; ++axis) most = std::max(most, std::abs(a[row * 4 + axis] - b[row * 4 + axis]));
    return most;
}
// Which camera the game set of late a view's camera is (State::census): the newest of another camera
// than the one shaken, and whether one of the shaken camera's is too.
struct Found {
    State::CameraWrite other;
    bool target{};
};
Found drawn_from_camera(State& s, const Matrix& camera) noexcept {
    Found found;
    const auto target = s.camera_target.load(std::memory_order_acquire);
    try {
        std::lock_guard lock(s.census_mutex);
        for (std::size_t i = 1; i <= s.census.size(); ++i) { // the newest first
            const auto& write = s.census[(s.census_next + s.census.size() - i) % s.census.size()];
            if (!write.camera || apart(write.transform, camera) >= 1e-4f) continue;
            if (write.camera == target) found.target = true;
            else if (!found.other.camera) found.other = write;
        }
    } catch (...) { /* Nothing found this time. */ }
    return found;
}
constexpr std::uint32_t follow_after = 24;   // main view set-ups in a row (six frames)
constexpr std::uint32_t follow_try_for = 360; // set-ups without the shake for a camera followed to bring it
constexpr std::uint32_t follow_lost_after = 240;
// `replayshake follow`, render thread under its mutex, for each set-up of the main view: should it keep
// coming with the camera shaken as the game set it up, while another camera the game set holds just
// what it comes with, the view is drawn from that one, which is shaken instead (the client tick takes
// it up: State::drawn_camera). Only set-ups that tell count: a still shake, or a view that holds
// neither, says nothing. One followed whose shake never comes is given up until shaking starts afresh;
// one whose shake stops coming, for now.
void follow_view(State& s, Renderer& r, int verdict, const Found& found) noexcept {
    auto& f = r.follow;
    if (const auto drawn = s.drawn_camera.load(std::memory_order_acquire)) {
        if (verdict > 0) {
            f.confirmed = true;
            f.without = 0;
            return;
        }
        if (verdict == 0 || ++f.without < (f.confirmed ? follow_lost_after : follow_try_for)) return;
        if (!f.confirmed && f.failed_count < f.failed.size()) f.failed[f.failed_count++] = drawn;
        s.drawn_camera.store(0, std::memory_order_release);
        f.candidate = 0;
        f.streak = f.without = 0;
        f.confirmed = false;
        return;
    }
    const auto& other = found.other;
    const auto failed_end = f.failed.begin() + static_cast<std::ptrdiff_t>(f.failed_count);
    if (!s.follow.load(std::memory_order_acquire) || verdict >= 0 || !other.camera ||
        std::find(f.failed.begin(), failed_end, other.camera) != failed_end) {
        f.streak = 0;
        return;
    }
    if (other.camera != f.candidate) {
        f.candidate = other.camera;
        f.vtable = other.vtable;
        f.streak = 0;
    }
    if (++f.streak < follow_after) return;
    s.drawn_vtable.store(other.vtable, std::memory_order_release);
    s.drawn_camera.store(other.camera, std::memory_order_release);
    f.streak = f.without = 0;
    f.confirmed = false;
}
// `replayshake probe`: which other camera's a main view set-up came with, or no camera's.
void count_drawn_from(Kept& kept, const Found& found) noexcept {
    if (!found.other.camera) {
        if (!found.target) ++kept.drawn_from_none;
        return;
    }
    for (auto& entry : kept.drawn_from)
        if (entry.camera == found.other.camera || !entry.camera) {
            entry.camera = found.other.camera;
            entry.vtable = found.other.vtable;
            ++entry.setups;
            return;
        }
    ++kept.drawn_from_dropped;
}
// Before the renderer copies a view for its shaders (render_view_hook.h): the main view's camera,
// shaken, one shake for every set-up of a frame (replay_shake_frames.h), and the cameras the
// renderer kept of the frame before as they were shown (carry()). Only what is drawn moves; the
// game's cameras stay where they are.
void shake_view(State& s, std::uintptr_t view, std::uintptr_t previous, std::uintptr_t blackboard, std::uint8_t jitter) {
    std::shared_ptr<const Frame> frame;
    bool source{}, built{}, views{};
    std::array<State::Written, 4> written{};
    {
        std::lock_guard lock(s.frame_mutex);
        frame = s.frame;
        source = s.source_on;
        built = s.builds_on;
        views = s.views_on;
        if (source || built) written = s.source_written;
    }
    if (source || built) {
        // The camera itself (or the view the game builds the picture from: `source 3`) is shaken: the
        // views come with it as they are. Whether the view's camera is nearer the camera shaken of late
        // than what it was set up as, and (the camera itself) which camera the game set of late it is
        // (follow_view()).
        Matrix camera{};
        if (!memory::peek(view + render::view_camera_offset, camera)) return;
        s.main_view_at.store(view, std::memory_order_release);
        s.previous_view_at.store(previous, std::memory_order_release);
        watch::mark(watch::Mark::main_view, camera.data() + 12, nullptr, nullptr, view);
        int verdict = 0; // 1 came with the shake, -1 as the game set it up, 0 cannot tell (a still shake)
        for (const auto& entry : written) {
            const float to_shown = apart(entry.shown, camera), to_base = apart(entry.base, camera);
            if (to_shown < 1e-4f && to_base > 1e-3f) verdict = 1;
            else if (verdict == 0 && to_base < 1e-4f && to_shown > 1e-3f) verdict = -1;
        }
        const auto drawn_from = built ? Found{} : drawn_from_camera(s, camera);
        const bool probe = s.probe_render.load(std::memory_order_acquire);
        if (probe && verdict <= 0) {
            std::lock_guard unshaken_lock(s.unshaken_mutex);
            s.unshaken_view = camera;
            s.unshaken_at = GetTickCount64();
        }
        auto& r = renderer();
        std::lock_guard lock(r.mutex);
        begin_setup(s, r, micros(r));
        if (!built) follow_view(s, r, verdict, drawn_from);
        if (probe) {
            if (verdict) ++(verdict > 0 ? r.kept.source_seen : r.kept.source_missed);
            if (!built) count_drawn_from(r.kept, drawn_from);
        }
        return;
    }
    // The views drawn shake only where the camera itself cannot (State::views_on): not while the client
    // tick has yet to put the shake on the camera, which would jolt the picture as shaking starts.
    if (!views || !frame) return;
    Matrix camera{};
    float fov{};
    if (!memory::peek(view + render::view_camera_offset, camera) || !memory::peek(view + render::view_fov_offset, fov)) return;
    auto& r = renderer();
    std::lock_guard lock(r.mutex);
    const auto now_us = micros(r);
    begin_setup(s, r, now_us);
    const auto placed = r.tracker.place(camera, fov, view, now_us);
    if (!sound(placed.base) || !std::isfinite(placed.base_fov) || placed.base_fov <= 0) return;
    bool moved{};
    switch (placed.place) {
    case frames::Place::shown:
        r.tracker.note(view);
        ++r.kept.shown;
        break;
    case frames::Place::again: {
        const auto shot = r.tracker.shot_of(placed);
        if (!frames::same_camera(shot.camera, camera) && !put(view, render::view_camera_offset, shot.camera)) return;
        if (shot.fov != fov) (void)write_bytes(view + render::view_fov_offset, &shot.fov, sizeof(shot.fov));
        r.tracker.wrote(view, placed, shot);
        ++r.kept.again;
        break;
    }
    case frames::Place::next: {
        const auto& last = r.tracker.now();
        moved = !last.used || !frames::same_camera(last.base, placed.base);
        const auto shake = gated(offsets(sample(frame->points, frame->fallback, frame_time(*frame, r)), frame->seed), gate_now(*frame));
        r.tracker.start(placed.base, placed.base_fov, shake, view, now_us);
        const auto& shot = r.tracker.now();
        if (!frames::same_camera(shot.shown, camera) && !put(view, render::view_camera_offset, shot.shown)) return;
        if (shot.fov != fov) (void)write_bytes(view + render::view_fov_offset, &shot.fov, sizeof(shot.fov));
        ++r.kept.frames;
        ++r.trace_frames;
        if (s.probe_render.load(std::memory_order_acquire)) note_frame(r, placed.base, moved, now_us);
        break;
    }
    }
    ++r.kept.setups;
    r.kept.most_views = std::max(r.kept.most_views, static_cast<std::uint32_t>(r.tracker.now().view_count));
    const auto kept = s.carry.load(std::memory_order_acquire) ? carry(r, view, previous) : Previous::none;
    // `replayshake passes`: this set-up keeps the game's camera while the game copies the view.
    const auto setup = r.tracker.now().setups - 1;
    const bool still = !(s.passes.load(std::memory_order_acquire) & (1U << std::min<std::uint32_t>(setup, 7))) &&
        hold_still(r, view, previous);
    if (still) ++r.kept.still_passes;
    // `replayshake after 0`: once copied, the views are given the game's camera back.
    bool released = false;
    if (!s.keep.load(std::memory_order_acquire) && released_count < released_stack.size()) {
        const auto& now = r.tracker.now();
        const auto& before = r.tracker.before();
        released_stack[released_count++] = {view, previous != view ? previous : 0, now.shown, now.base, before.shown, before.base,
            now.fov, now.base_fov, before.fov, before.base_fov, frames::moves(now), frames::moves(before)};
        r.tracker.returned();
        released = true;
        ++r.kept.released;
    }
    if (auto* traced = next_trace(r))
        *traced = {now_us - r.trace_from_us, GetCurrentThreadId(), r.trace_frames, render::main_view_kind, view, previous,
            blackboard, jitter, placed.place, moved, kept, 0, 0, false, setup, still, released};
}
// `replayshake probe`: whether a view of another kind follows the shake. The n-th view of a kind in a
// frame is taken for the n-th the frame before (should the game make them anew each frame); while the
// main camera stands still from one frame to the next, its camera as the game set it up stays, unless
// the game works it out from the shaken one. Under the renderer's mutex.
void watch(Renderer& r, OtherKind& counted, std::uint32_t kind, const ViewBytes& data) noexcept {
    const auto frame = r.tracker.frame();
    if (r.kind_counts_frame != frame) {
        r.kind_counts_frame = frame;
        r.kind_count_used = 0;
    }
    std::size_t entry = 0;
    while (entry < r.kind_count_used && r.kind_counts[entry].first != kind) ++entry;
    if (entry == r.kind_count_used) {
        if (r.kind_count_used == r.kind_counts.size()) return;
        r.kind_counts[entry] = {kind, 0};
        ++r.kind_count_used;
    }
    const auto nth = r.kind_counts[entry].second++;
    Matrix camera{};
    std::memcpy(camera.data(), data.data() + render::view_camera_offset, sizeof(camera));
    const auto& now = r.tracker.now();
    // The slot of this kind's n-th view: kind and n in `view`, in place of an address.
    const auto key = (std::uintptr_t{kind} << 32) | nth;
    std::size_t slot = 0;
    while (slot < r.watched.size() && !(r.watched[slot].used && r.watched[slot].view == key)) ++slot;
    if (slot < r.watched.size()) {
        const auto& last = r.watched[slot];
        if (last.frame != frame && now.used && frames::same_camera(last.base, now.base))
            ++(frames::same_camera(last.camera, camera) ? counted.still_kept : counted.still_moved);
    } else {
        slot = r.next_watched;
        r.next_watched = (r.next_watched + 1) % r.watched.size();
    }
    r.watched[slot] = {key, kind, frame, camera, now.base, true};
}
// A view of another kind (shadows, the sky, reflections...: not the main picture's). Should it keep
// an exact copy of the main camera as the game set it up this frame, it draws from the main camera:
// the copy is given the frame's shake, or the view would draw this frame from where the camera was
// before it shook (`replayshake copies`). Its previous view likewise, with the frame before's as
// shown (`replayshake history`). Views from any other camera are left as they are.
void other_view(State& s, std::uintptr_t view, std::uintptr_t previous, std::uintptr_t blackboard, std::uint8_t jitter,
                std::uint32_t kind) {
    const bool probing = s.probe_render.load(std::memory_order_acquire);
    bool views{};
    {
        std::lock_guard lock(s.frame_mutex);
        views = s.views_on && !s.source_on;
    }
    const bool copies = views && s.copies.load(std::memory_order_acquire);
    const bool history = views && s.carry.load(std::memory_order_acquire);
    if (!probing && !copies && !history) return;
    auto& r = renderer();
    std::lock_guard lock(r.mutex);
    const auto now_us = micros(r);
    begin_setup(s, r, now_us);
    auto* counted = probing ? r.kept.other(kind) : nullptr;
    if (counted) ++counted->setups;
    if (probing && r.recent_count < r.recent.size()) r.recent[r.recent_count++] = {view, now_us, kind};
    const auto& now = r.tracker.now();
    const auto& before = r.tracker.before();
    std::uint32_t found{}, shown{}, found_before{};
    ViewBytes data{};
    const bool peeked = (frames::moves(now) || probing) && memory::peek_bytes(view, data.data(), data.size());
    if (peeked && counted) watch(r, *counted, kind, data);
    if (peeked && frames::moves(now)) {
        for (std::size_t place = 0; place < camera_places; ++place) {
            if (holds(data, place * 16, now.base)) {
                found |= 1U << place;
                if (copies) put(view, place * 16, now.shown);
            } else if (holds(data, place * 16, now.shown)) {
                shown |= 1U << place;
            }
        }
        float fov{};
        std::memcpy(&fov, data.data() + render::view_fov_offset, sizeof(fov));
        if (copies && (found & (1U << (render::view_camera_offset / 16))) && now.fov != now.base_fov && fov == now.base_fov)
            (void)write_bytes(view + render::view_fov_offset, &now.fov, sizeof(now.fov));
    }
    if (history && frames::moves(before) && previous && previous != view && memory::peek_bytes(previous, data.data(), data.size()))
        for (std::size_t place = 0; place < camera_places; ++place)
            if (holds(data, place * 16, before.base)) {
                found_before |= 1U << place;
                put(previous, place * 16, before.shown);
            }
    if (counted) {
        if (found) ++counted->copies;
        counted->places |= found;
        if (shown) ++counted->shown;
        if (found_before) ++counted->previous_copies;
        counted->previous_places |= found_before;
    }
    if (auto* traced = next_trace(r))
        *traced = {now_us - r.trace_from_us, GetCurrentThreadId(), r.trace_frames, kind, view, previous, blackboard, jitter,
            frames::Place::shown, false, Previous::none, found, found_before, copies, 0, false, false};
}
// Every view the renderer sets up, while the shake is on (render_view_hook.h).
void on_view(std::uintptr_t view, std::uintptr_t previous, std::uintptr_t blackboard, std::uint8_t jitter) noexcept {
    auto& s = state();
    if (GetTickCount64() > s.live_until.load(std::memory_order_acquire)) return;
    try {
        std::uint32_t kind{};
        if (!memory::peek(view - render::view_kind_back, kind)) return;
        if (kind == render::main_view_kind) shake_view(s, view, previous, blackboard, jitter);
        else other_view(s, view, previous, blackboard, jitter, kind);
    } catch (...) { /* A lost shake is never worth the renderer. */ }
}

// The game build this ReSkate runs: the replay editor's cameras are found for one build at a time.
constexpr std::string_view game_build = supported_build::steam_build_id;
// A director gameplay's own camera selects (layout::gameplay_directors).
bool gameplay_director(std::uintptr_t director) {
    const auto& gameplay = layout::gameplay_directors;
    return std::find(gameplay.begin(), gameplay.end(), director) != gameplay.end();
}
// How a camera on show is one of the replay editor's: not at all, by what gameplay's camera has too
// (the vtable all skate.'s cameras have, engine.h camera_vtable, or a director of gameplay's), or by a
// director of the editor's own.
enum class Match : std::uint8_t { none, camera, director };
Match replay_camera(const Settings& settings, const scan::Seen& seen) {
    const auto listed = [&](std::uintptr_t value) {
        return value && std::find(settings.cameras.begin(), settings.cameras.end(), value) != settings.cameras.end();
    };
    if (listed(seen.director)) return gameplay_director(seen.director) ? Match::camera : Match::director;
    return listed(seen.vtable) ? Match::camera : Match::none;
}
const std::vector<std::uintptr_t>& replay_cameras(const Settings& settings) { return settings.cameras; }

// The settings live in a file of their own beside the ReSkate profile (Extension/Profile/
// local_profile.cpp default_path): the profile keeps texts of 4 KB at most, and a mapping and a
// few dozen keyframe shakes are more. Plain JSON, to read, keep or share.
std::filesystem::path settings_file() {
    std::array<wchar_t, 32768> buffer{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return {};
    return std::filesystem::path(buffer.data()) / L"ReSkate" / L"profiles" / L"offline" / L"replay_shake.json";
}
// None saved yet: the defaults. Throws when there is one that cannot be read.
Settings read_settings(const std::filesystem::path& file) {
    std::error_code error;
    if (file.empty() || !std::filesystem::exists(file, error)) return {};
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("it cannot be opened");
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.size() > most_settings_bytes) throw std::runtime_error("it is too big");
    return settings_from_json(Json::parse(text));
}
// Written beside the file, then moved over it: a crash part way never leaves half a file.
bool write_settings(const std::filesystem::path& file, const Settings& settings) {
    if (file.empty()) return false;
    const auto text = to_json(settings).dump(2) + "\n";
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    auto temporary = file;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        if (!output) return false;
    }
    if (MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::filesystem::remove(temporary, error);
    return false;
}

// Under the state's mutex from here on.
void changed(State& s) {
    s.dirty = true;
    s.save_at = GetTickCount64() + save_after_ms;
}
void save_when_due(State& s) {
    if (!s.dirty || GetTickCount64() < s.save_at) return;
    if (write_settings(s.file, s.settings)) {
        s.dirty = false;
        s.save_failed = false;
        return;
    }
    s.save_at = GetTickCount64() + save_retry_ms;
    if (!s.save_failed)
        logging::log(logging::Level::warning, logging::Channel::graphics,
            "Replay Shake could not save its settings to {}; it tries again every {} seconds.", path_utf8(s.file),
            save_retry_ms / 1000);
    s.save_failed = true;
}
// How fast the playhead runs, from its last two changes; still for a quarter of a second is paused.
// Each change also notes how far the export (if one runs) had got, to time its frames from.
void measure(State& s, const std::optional<double>& playhead) {
    const auto now = Clock::now();
    if (!playhead) {
        s.playhead.reset();
        s.rate = s.interval = 0;
        return;
    }
    if (!s.playhead || *playhead != *s.playhead) {
        if (s.playhead) {
            const double elapsed = seconds_between(s.playhead_at, now);
            const bool steady = elapsed > 0 && elapsed < 0.5;
            s.rate = steady ? std::clamp((*playhead - *s.playhead) / elapsed, -most_rate, most_rate) : 0;
            s.interval = steady ? std::min(elapsed, most_ahead) : 0;
        }
        const auto capture = export_frames::capture();
        s.playhead = playhead;
        s.playhead_at = now;
        s.playhead_session = capture.session;
        s.playhead_frames = capture.frames;
        s.playhead_camera_frames = capture.session == s.camera_session ? s.camera_frames : 0;
        {
            auto& b = builds();
            std::lock_guard lock(b.mutex);
            s.playhead_build_frames = capture.session == b.session ? b.frames : 0;
        }
    } else if (seconds_between(s.playhead_at, now) > 0.25) {
        s.rate = 0;
    }
}
void publish_frame(State& s) {
    auto frame = std::make_shared<Frame>();
    frame->points = points(s.settings, s.editor);
    frame->fallback = s.settings.fallback;
    frame->seed = s.settings.seed;
    frame->have_playhead = s.playhead.has_value();
    frame->playhead = s.playhead.value_or(0);
    frame->stamp = s.playhead_at;
    frame->playhead_session = s.playhead_session;
    frame->playhead_frames = s.playhead_frames;
    frame->playhead_camera_frames = s.playhead_camera_frames;
    frame->playhead_build_frames = s.playhead_build_frames;
    frame->rate = s.rate;
    frame->interval = s.interval;
    frame->export_fps = s.settings.export_fps;
    frame->gates = s.gates;
    frame->base = s.base;
    std::lock_guard lock(s.frame_mutex);
    s.frame = std::move(frame);
}
// `replayshake source`: whether a camera is one of the game's evaluated cameras (engine.h's
// camera_vtable: shaken in their update), or the one the renderer was found to draw from, of the class
// found (one that went away is left alone).
bool evaluated_camera(std::uintptr_t camera) noexcept {
    std::uintptr_t vtable{};
    return camera && memory::peek(camera, vtable) && vtable == state().base + addr::engine::camera_vtable;
}
bool game_camera(std::uintptr_t camera) noexcept {
    const auto& s = state();
    std::uintptr_t vtable{};
    return camera && memory::peek(camera, vtable) &&
        (vtable == s.base + addr::engine::camera_vtable ||
            (camera == s.drawn_camera.load(std::memory_order_acquire) && vtable == s.drawn_vtable.load(std::memory_order_acquire)));
}
// A camera's transform before its offset (float[4] at +0x90) is added to its position: the game sets it
// with the transform itself (Skate.exe 0x1707c60, from the camera update: camera_update_hook.h). Shaken
// with it, so that whatever builds the transform from it again keeps the shake.
constexpr std::uintptr_t camera_unoffset = 0x10, camera_offset = 0x90;
// `replayshake source 1`, at the start of the game's next update of the camera (a camera it is
// updating, so one that is there): the camera given its own back where it still holds the shake.
// Whether anything was.
bool unshake_source(const State::SourceShake& shake) noexcept {
    if (!shake.used || !game_camera(shake.camera)) return false;
    Matrix now{};
    float fov{};
    bool any = false;
    if (memory::peek(shake.camera + layout::camera_transform, now) && frames::same_camera(now, shake.shown) &&
        !frames::same_camera(shake.shown, shake.base))
        any = put(shake.camera, layout::camera_transform, shake.base) || any;
    if (shake.unoffset && memory::peek(shake.camera + camera_unoffset, now) && frames::same_camera(now, shake.shown_unoffset) &&
        !frames::same_camera(shake.shown_unoffset, shake.base_unoffset))
        any = put(shake.camera, camera_unoffset, shake.base_unoffset) || any;
    if (shake.fov != shake.base_fov && memory::peek(shake.camera + layout::camera_fov, fov) && fov == shake.fov)
        any = write_bytes(shake.camera + layout::camera_fov, &shake.base_fov, sizeof(shake.base_fov)) || any;
    return any;
}
// Whether two cameras turn the same way: the first three lanes of the rows right, up and back.
bool same_turn(const Matrix& a, const Matrix& b) noexcept {
    for (std::size_t row = 0; row < 3; ++row)
        if (std::memcmp(a.data() + row * 4, b.data() + row * 4, 3 * sizeof(float)) != 0) return false;
    return true;
}
// A state of the camera (camera_update_hook.h) as it is at a place: its eye, offset, point looked at
// and way up, and its field of view.
struct StateNow {
    std::array<float, camera_update::state_size / sizeof(float)> values{};
    float fov{};
};
bool read_state(std::uintptr_t at, StateNow& now) noexcept {
    return at && memory::peek(at + camera_update::state_eye, now.values) && memory::peek(at + camera_update::state_fov, now.fov);
}
// Whether a state still holds what it was shaken to, to the bit.
bool still_shaken(const State::StateShake& record, const StateNow& now) noexcept {
    return record.used && std::memcmp(now.values.data(), record.shown.data(), sizeof(now.values)) == 0 &&
        std::memcmp(&now.fov, &record.fov, sizeof(now.fov)) == 0;
}
// `replayshake source 1`, at the start of the game's next update of the camera: a state given its own
// back where it still holds the shake (the update writes each afresh, but what works the camera out may
// read them first). Whether it was.
bool unshake_state(const State::StateShake& record) noexcept {
    StateNow now;
    if (!read_state(record.at, now) || !still_shaken(record, now)) return false;
    bool any = false;
    if (std::memcmp(record.shown.data(), record.base.data(), sizeof(record.base)) != 0)
        any = write_bytes(record.at + camera_update::state_eye, record.base.data(), sizeof(record.base)) || any;
    if (std::memcmp(&record.fov, &record.base_fov, sizeof(record.fov)) != 0)
        any = write_bytes(record.at + camera_update::state_fov, &record.base_fov, sizeof(record.base_fov)) || any;
    return any;
}
// The camera's shake turning a direction of the world: its parts along the rows right, up and back of
// the camera as the game set it up, along the shaken camera's instead.
std::array<float, 3> turned(const State::SourceShake& shake, const float* direction) noexcept {
    std::array<float, 3> result{};
    for (std::size_t row = 0; row < 3; ++row) {
        float along = 0;
        for (std::size_t axis = 0; axis < 3; ++axis) along += direction[axis] * shake.base[row * 4 + axis];
        for (std::size_t axis = 0; axis < 3; ++axis) result[axis] += along * shake.shown[row * 4 + axis];
    }
    return result;
}
// Whether the game would set a camera up from this way up and look by its fallback (Skate.exe
// 0x17ac360): a look too short to tell its direction by, or a way up within 0.9999 of the look (the
// way up's length as it is), which the game swaps the axes of. Neither turns with the camera. Told
// with a little to spare (0.9998: the game rounds its own way), since taking it for one costs nothing.
bool by_fallback(const float* way_up, const std::array<float, 3>& look) noexcept {
    const float length_squared = look[0] * look[0] + look[1] * look[1] + look[2] * look[2];
    if (!(length_squared >= 1e-6f)) return true;
    const float along = (way_up[0] * look[0] + way_up[1] * look[1] + way_up[2] * look[2]) / std::sqrt(length_squared);
    return !(std::abs(along) <= 0.9998f);
}
// `replayshake source 1`, in the camera's update once the shake is on the camera: the same shake put on
// a state of it (camera_update_hook.h), as the game would have set the camera from it shaken. Its eye
// moves as the camera did, the point looked at and the way up turn with the camera about the eye, and
// its field of view zooms as the camera's did. One that holds what `same_as` was shaken to (the
// update's copy of the state it set the camera from) is taken as that; one that still holds the shake
// (the game did not write it afresh) is shaken from what was under it, never on top. Whether it was.
bool shake_state(State& s, State::StatePlace place, std::uintptr_t at, const State::StateShake* same_as) noexcept {
    const auto& shake = s.source_shake;
    auto& record = s.source_states[place];
    StateNow now;
    if (!read_state(at, now)) {
        record = {};
        return false;
    }
    if (same_as && still_shaken(*same_as, now)) {
        record = *same_as;
        record.at = at;
        return true;
    }
    if (record.at != at) record = {};
    const bool ours = still_shaken(record, now);
    const auto base = ours ? record.base : now.values;
    const float base_fov = ours ? record.base_fov : now.fov;
    constexpr std::size_t eye = camera_update::state_eye / sizeof(float), target = camera_update::state_target / sizeof(float),
                          up = camera_update::state_up / sizeof(float);
    for (const std::size_t at_row : {eye, target, up})
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (!std::isfinite(base[at_row + axis]) || std::abs(base[at_row + axis]) > 1e7f) {
                record = {};
                return false;
            }
    auto shown = base;
    if (!frames::same_camera(shake.shown, shake.base)) {
        std::array<float, 3> aim{};
        for (std::size_t axis = 0; axis < 3; ++axis) aim[axis] = base[target + axis] - base[eye + axis];
        const auto turned_aim = turned(shake, aim.data());
        auto turned_up = turned(shake, base.data() + up);
        // Looking (nearly) along the way up, as from straight above, the game sets the camera up from
        // another way up than this one: the shaken camera's own up row is given instead, from which
        // the game sets it up just as shown.
        if (by_fallback(base.data() + up, aim) || by_fallback(turned_up.data(), turned_aim))
            for (std::size_t axis = 0; axis < 3; ++axis) turned_up[axis] = shake.shown[4 + axis];
        for (std::size_t axis = 0; axis < 3; ++axis) {
            shown[eye + axis] = base[eye + axis] + (shake.shown[12 + axis] - shake.base[12 + axis]);
            shown[target + axis] = shown[eye + axis] + turned_aim[axis];
            shown[up + axis] = turned_up[axis];
        }
    }
    const float shown_fov = shake.fov != shake.base_fov && shake.base_fov > 0 && base_fov > 1 && base_fov < 175
        ? std::clamp(base_fov * (shake.fov / shake.base_fov), 1.0f, 175.0f) : base_fov;
    if (std::memcmp(shown.data(), now.values.data(), sizeof(shown)) != 0 &&
        !write_bytes(at + camera_update::state_eye, shown.data(), sizeof(shown))) {
        record = {};
        return false;
    }
    if (std::memcmp(&shown_fov, &now.fov, sizeof(shown_fov)) != 0)
        (void)write_bytes(at + camera_update::state_fov, &shown_fov, sizeof(shown_fov));
    record = {at, base, shown, base_fov, shown_fov, true};
    return true;
}
// `replayshake source 1`: the replay time a camera update shows. During an export, one step of the
// export's frame rate on for every update of the camera in it (each is one rendered frame, the one
// the encoder takes), counted here rather than read from the encoder's count, which the engine thread
// runs a frame or two ahead of; from the playhead read in that export, or from where the export began.
// Otherwise as the views'.
double camera_time(State& s, const Frame& frame, Renderer& r) {
    const auto capture = export_frames::capture();
    if (!capture.running) return frame_time(frame, r);
    if (capture.session != s.camera_session) {
        s.camera_session = capture.session;
        s.camera_frames = 0;
        s.camera_export_base = frame_time(frame, r);
    } else {
        ++s.camera_frames;
    }
    const double step = 1.0 / std::max(frame.export_fps, 1);
    if (frame.have_playhead && frame.playhead_session == capture.session && s.camera_frames >= frame.playhead_camera_frames)
        return frame.playhead + static_cast<double>(s.camera_frames - frame.playhead_camera_frames) * step;
    return s.camera_export_base + static_cast<double>(s.camera_frames) * step;
}
// `replayshake source 1`, right after the game set the camera (in its update, or for a camera of
// another class the renderer was found to draw from, in the setter): the shake put on it. Engine
// thread, under the state's mutex. Should the camera still hold the shake (the game did not set it up
// this time), it is shaken from what was under it, never on top. The field of view (zoom) is the
// evaluated cameras' only: another class may keep it elsewhere.
bool shake_camera(State& s, std::uintptr_t camera) {
    auto& shake = s.source_shake;
    if (shake.camera != camera) { // another camera's: the game's updates of it set it afresh
        shake = {};
        s.source_states = {};
    }
    if (!game_camera(camera)) return false;
    const bool engine = evaluated_camera(camera);
    Matrix now{}, unoffset{};
    float fov{};
    if (!memory::peek(camera + layout::camera_transform, now) || (engine && !memory::peek(camera + layout::camera_fov, fov)) ||
        !sound(now) || !std::isfinite(fov))
        return false;
    // Each part the game did not set up afresh (it still holds the shake) is shaken from what was under it.
    const bool ours = shake.used && frames::same_camera(now, shake.shown);
    const auto base = ours ? shake.base : now;
    const auto base_fov = shake.used && fov == shake.fov ? shake.base_fov : fov;
    // The transform before the offset turns as the transform does (the game keeps them turned alike).
    const bool unoffset_known = memory::peek(camera + camera_unoffset, unoffset) &&
        (ours ? shake.unoffset && frames::same_camera(unoffset, shake.shown_unoffset) : same_turn(unoffset, now));
    const auto base_unoffset = ours ? shake.base_unoffset : unoffset;
    std::shared_ptr<const Frame> frame;
    {
        std::lock_guard lock(s.frame_mutex);
        frame = s.frame;
    }
    if (!frame) return false;
    // Set again within the same frame (a camera the game sets more than once a frame): the frame's
    // shake again, the export's frame counted once.
    const auto at = Clock::now();
    double time = shake.time;
    if (!shake.used || at - shake.at >= std::chrono::milliseconds(2)) {
        auto& r = renderer();
        std::lock_guard lock(r.mutex);
        time = camera_time(s, *frame, r);
    }
    const auto moved = gated(offsets(sample(frame->points, frame->fallback, time), frame->seed), gate_now(*frame));
    auto shown = still(moved) ? base : shaken(base, moved);
    auto shown_unoffset = base_unoffset;
    for (std::size_t i = 0; i < 12; ++i) shown_unoffset[i] = shown[i]; // rows right, up, back (and their fourth lanes)
    for (std::size_t axis = 0; axis < 3; ++axis)
        shown_unoffset[12 + axis] = base_unoffset[12 + axis] + (shown[12 + axis] - base[12 + axis]);
    // Where the game's position is the one before the offset plus the offset, to the bit (as its update
    // sets it), the shaken one is made so too: on a cut the update builds the transform again from the
    // one before the offset (Skate.exe 0x1729202), which then gives just what was shown.
    std::array<float, 4> offset{};
    if (unoffset_known && memory::peek(camera + camera_offset, offset)) {
        bool exact = true;
        for (std::size_t axis = 0; axis < 3; ++axis) exact = exact && base[12 + axis] == base_unoffset[12 + axis] + offset[axis];
        if (exact)
            for (std::size_t axis = 0; axis < 3; ++axis) shown[12 + axis] = shown_unoffset[12 + axis] + offset[axis];
    }
    // The camera's field of view is in degrees (the views' in radians): zoom is a percentage of either.
    const auto shown_fov = !engine || moved[channel::zoom] == 0 || base_fov <= 1 || base_fov >= 175
        ? base_fov : std::clamp(base_fov * (1.0f + moved[channel::zoom] / 100.0f), 1.0f, 175.0f);
    if (!frames::same_camera(shown, now) && !put(camera, layout::camera_transform, shown)) return false;
    if (unoffset_known && !frames::same_camera(shown_unoffset, unoffset)) (void)put(camera, camera_unoffset, shown_unoffset);
    if (engine && shown_fov != fov) (void)write_bytes(camera + layout::camera_fov, &shown_fov, sizeof(shown_fov));
    shake = {camera, base, shown, base_unoffset, shown_unoffset, base_fov, shown_fov, unoffset_known, true, false, at, time};
    return true;
}
// In the game's update of a camera (camera_update_hook.h), engine thread: should it be the camera to
// shake, the shake comes off at the start (what works the camera out never sees it) and goes on again
// as soon as the update has set the camera (everything handed on and drawn from it does). Let go
// meanwhile, should this thread already hold the state (inside the client tick or a command), or
// another thread update the camera.
// A camera of another class (one the renderer was found to draw from) may be set on another thread
// (`any_thread`): the state's lock is all that is needed there.
template <class Work> void around_camera(std::uintptr_t camera, Work work, bool any_thread = false) noexcept {
    auto& s = state();
    if (!camera || camera != s.camera_target.load(std::memory_order_acquire) || holding_state) return;
    if (!any_thread && GetCurrentThreadId() != s.engine_thread.load(std::memory_order_acquire)) {
        s.camera_elsewhere.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try {
        std::unique_lock lock(s.mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            s.lock_missed.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (s.source_mode == State::Source::views) return;
        const Holding holding;
        work(s);
    } catch (...) { /* The shake waits for the next update. */ }
}
// `replayshake probe`'s watch (replay_shake_watch.h): where the update of the camera shaken has got to.
bool watched(std::uintptr_t camera) noexcept { return camera && camera == state().camera_target.load(std::memory_order_acquire); }
void camera_before(std::uintptr_t camera) noexcept {
    if (watched(camera)) watch::mark(watch::Mark::update_start);
    around_camera(camera, [camera](State& s) {
        auto& shake = s.source_shake;
        shake.fresh = false;
        if (!shake.used || shake.camera != camera) return;
        // `source 2`: left on until the update sets the camera afresh.
        if (s.source_mode == State::Source::held) return;
        if (unshake_source(shake)) ++s.camera_restores;
        for (const auto& record : s.source_states)
            if (unshake_state(record)) ++s.state_restores;
        s.source_states = {};
        shake.used = false;
    });
}
// `replayshake probe`: one more write of a camera, from a state or a transform. Under census_mutex.
void tally(State& s, std::uintptr_t camera, std::uintptr_t vtable, bool from_state) noexcept {
    for (std::size_t i = 0; i < s.tally_count; ++i)
        if (s.tallies[i].camera == camera) {
            ++(from_state ? s.tallies[i].from_state : s.tallies[i].from_transform);
            return;
        }
    if (s.tally_count == s.tallies.size()) {
        ++s.tallies_dropped;
        return;
    }
    auto& added = s.tallies[s.tally_count++];
    added = {camera, vtable, from_state ? 1U : 0U, from_state ? 0U : 1U};
}
// A camera's transform as it is now, noted for the renderer to find its main view's camera among
// (State::census); `tallied`, counted for the probe too.
void note_write(State& s, std::uintptr_t camera, std::uintptr_t vtable, bool from_state, bool tallied) noexcept {
    Matrix now{};
    if (!memory::peek(camera + layout::camera_transform, now)) return;
    const bool probe = tallied && s.probe_render.load(std::memory_order_acquire);
    try {
        std::lock_guard lock(s.census_mutex);
        s.census[s.census_next] = {camera, vtable, now, from_state};
        s.census_next = (s.census_next + 1) % s.census.size();
        if (probe) tally(s, camera, vtable, from_state);
    } catch (...) { /* Not noted this time. */ }
}
// The frame's shake put on the camera, in its update. Only while client ticks keep coming (a load, or
// a tick ReSkate skips, stops it): the game's next update of the camera then sets it afresh, and it
// stays so. Whether it was.
bool put_on_camera(State& s, std::uintptr_t camera) {
    if (GetTickCount64() > s.live_until.load(std::memory_order_acquire) || !shake_camera(s, camera)) return false;
    s.source_shake.fresh = true;
    ++s.camera_shakes;
    std::lock_guard frame_lock(s.frame_mutex);
    s.source_written[s.source_next] = {s.source_shake.base, s.source_shake.shown};
    s.source_next = (s.source_next + 1) % s.source_written.size();
    return true;
}
void camera_applied(std::uintptr_t camera, std::uintptr_t state_at, std::uintptr_t blender) noexcept {
    // For the watch: the camera as the game set it and as shaken, and the blend's position (its eye and
    // offset), before it is shaken.
    const bool marking = watched(camera);
    std::array<std::array<float, 3>, 3> marked{};
    std::array<bool, 3> have{};
    if (marking && blender) {
        std::array<float, 8> blend{};
        if (memory::peek(blender + camera_update::blend_state, blend)) {
            for (std::size_t axis = 0; axis < 3; ++axis) marked[2][axis] = blend[axis] + blend[4 + axis];
            have[2] = true;
        }
    }
    around_camera(camera, [camera, state_at, blender, marking, &marked, &have](State& s) {
        if (!put_on_camera(s, camera)) return;
        if (marking) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                marked[0][axis] = s.source_shake.base[12 + axis];
                marked[1][axis] = s.source_shake.shown[12 + axis];
            }
            have[0] = have[1] = true;
        }
        if (shake_state(s, State::state_set_from, state_at, nullptr)) ++s.state_shakes[State::state_set_from];
        // The blender's own states, which the replay editor's picture is drawn from: a blender of the
        // class known only (another is noted for the probe), and neither the same place as the state
        // just shaken (that one would be shaken twice).
        std::uintptr_t blender_vtable{};
        if (blender && memory::peek(blender, blender_vtable)) {
            const auto known = std::find_if(camera_update::blender_vtables.begin(), camera_update::blender_vtables.end(),
                [&](std::uintptr_t vtable) { return blender_vtable == s.base + vtable; });
            if (known != camera_update::blender_vtables.end()) {
                for (const auto& [place, offset] : {std::pair{State::state_blend, camera_update::blend_state},
                         std::pair{State::state_blend_kept, camera_update::blend_kept}})
                    if (blender + offset != state_at && shake_state(s, place, blender + offset, nullptr)) ++s.state_shakes[place];
            } else {
                s.other_blender = blender_vtable;
            }
        }
        // The census holds the camera as the game set it (the setter's write, just before): as shaken too.
        note_write(s, camera, s.base + addr::engine::camera_vtable, true, false);
    });
    if (marking)
        watch::mark(watch::Mark::camera_set, have[0] ? marked[0].data() : nullptr, have[1] ? marked[1].data() : nullptr,
            have[2] ? marked[2].data() : nullptr);
}
void camera_handing(std::uintptr_t camera, std::uintptr_t copy, std::uintptr_t handed) noexcept {
    if (watched(camera)) watch::mark(watch::Mark::handing);
    around_camera(camera, [camera, copy, handed](State& s) {
        // An update that did not set the camera (no director moves it: it hands nothing on) leaves it
        // as the start of the update gave it back: it is shaken here instead, so that it shakes on.
        if (!s.source_shake.fresh || s.source_shake.camera != camera) {
            (void)put_on_camera(s, camera);
            return;
        }
        const auto set_from = s.source_states[State::state_set_from];
        if (shake_state(s, State::state_copy, copy, &set_from)) ++s.state_shakes[State::state_copy];
        if (handed && shake_state(s, State::state_handed, handed, &set_from)) ++s.state_shakes[State::state_handed];
    });
}
// `replayshake probe`: whether the camera still held the shake when its update ended.
void camera_after(std::uintptr_t camera) noexcept {
    around_camera(camera, [camera](State& s) {
        const auto& shake = s.source_shake;
        Matrix now{};
        if (!shake.fresh || shake.camera != camera || frames::same_camera(shake.shown, shake.base) ||
            !memory::peek(camera + layout::camera_transform, now))
            return;
        ++(frames::same_camera(now, shake.shown) ? s.held_at_end : s.changed_at_end);
    });
    if (watched(camera)) watch::mark(watch::Mark::update_end);
}
// Right after the game set any camera's transform (camera_update_hook.h). The camera the renderer was
// found to draw from, of a class the game does not evaluate, is shaken here (an evaluated one is shaken
// in its update); then the write is noted as it was left.
void camera_written(std::uintptr_t camera, bool from_state) noexcept {
    auto& s = state();
    std::uintptr_t vtable{};
    if (!memory::peek(camera, vtable)) return;
    if (camera == s.camera_target.load(std::memory_order_acquire) && vtable != s.base + addr::engine::camera_vtable) {
        if (GetCurrentThreadId() != s.engine_thread.load(std::memory_order_acquire))
            s.drawn_elsewhere.fetch_add(1, std::memory_order_relaxed);
        around_camera(camera, [camera](State& locked) { (void)put_on_camera(locked, camera); }, true);
    }
    note_write(s, camera, vtable, from_state, true);
}
// `replayshake source 3`: the replay time the views built show. During an export, one step of the
// export's frame rate on for every frame the main view is built in (each is one rendered frame, the one
// the encoder takes), counted here rather than read from the encoder's count, which the game's thread
// runs a frame or two ahead of; from the playhead read in that export, or from where the export began.
// Otherwise the playhead, run on at its pace between two readings of it, as the renderer's (frame_time()).
// Under the builds' mutex.
double build_time(Builds& b, const Frame& frame) {
    const auto now = Clock::now();
    const auto preview = [&] {
        if (!frame.have_playhead) return seconds_between(b.origin, now);
        return frame.playhead + std::clamp(frame.rate * seconds_between(frame.stamp, now), -frame.interval, frame.interval);
    };
    const auto capture = export_frames::capture();
    if (!capture.running) return preview();
    if (capture.session != b.session) {
        b.session = capture.session;
        b.frames = 0;
        b.export_base = preview();
    } else {
        ++b.frames;
    }
    const double step = 1.0 / std::max(frame.export_fps, 1);
    if (frame.have_playhead && frame.playhead_session == capture.session && b.frames >= frame.playhead_build_frames)
        return frame.playhead + static_cast<double>(b.frames - frame.playhead_build_frames) * step;
    return b.export_base + static_cast<double>(b.frames) * step;
}
// `replayshake source 3`: the view description shaken last on this thread (until the next is filled in),
// its camera as the game built it and as its view is to hold it once filled in, and the frame's shake
// (its zoom goes on once it is filled in).
struct PendingBuild { // plain, so the thread-local copy needs no constructor (zeroed)
    std::uintptr_t description;
    Matrix base, shown;
    Offsets moved;
};
thread_local PendingBuild pending_build;
// How far apart two cameras' positions are (metres).
float distance_between(const Matrix& a, const Matrix& b) noexcept {
    float squared = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) squared += (a[12 + axis] - b[12 + axis]) * (a[12 + axis] - b[12 + axis]);
    return std::sqrt(squared);
}
// `replayshake probe`, under the builds' mutex: where the camera on show is against a view's camera.
void measure_view(State& s, Builds::Counts::Against& against, const Matrix& view) noexcept {
    Matrix shown_camera{};
    const auto camera = s.probe_camera.load(std::memory_order_acquire);
    if (!camera || !memory::peek(camera + layout::camera_transform, shown_camera)) {
        ++against.no_camera;
    } else if (apart(shown_camera, view) < 1e-4f) {
        ++against.same;
    } else {
        const float distance = distance_between(shown_camera, view);
        ++(distance < 0.01f ? against.within_cm : against.beyond_cm);
        if (std::isfinite(distance)) against.farthest = std::max(against.farthest, distance);
    }
}
// `replayshake source 3`, as the game fills a view description in (game_view_hook.h), on the thread that
// builds its views: the first local player's view is given the frame's shake, worked out once a frame,
// on both its transforms (before and after the offset: they turn alike), and so is the transform kept
// beside it should it be the view's own. Only while client ticks keep coming (a load stops it) and the
// views built shake; a transform that is no camera's (not orthonormal) is left as it is.
void view_building(std::uintptr_t description, std::uintptr_t camera_view, game_view::Transform& beside,
    game_view::Transform& view) noexcept {
    pending_build.description = 0;
    auto& s = state();
    if (GetTickCount64() > s.live_until.load(std::memory_order_acquire)) return;
    try {
        std::shared_ptr<const Frame> frame;
        bool on{};
        {
            std::lock_guard lock(s.frame_mutex);
            frame = s.frame;
            on = s.builds_on;
        }
        if (!on || !frame) return;
        std::uint32_t player{};
        const bool picture = memory::peek(camera_view + game_view::camera_view_player, player) && player == 0;
        const bool probe = s.probe_render.load(std::memory_order_acquire);
        auto& b = builds();
        std::lock_guard lock(b.mutex);
        auto& counts = b.counts;
        ++counts.filled;
        if (GetCurrentThreadId() != s.engine_thread.load(std::memory_order_acquire)) ++counts.elsewhere;
        if (!picture) {
            ++counts.others;
            const auto players_end = counts.other_players.begin() + static_cast<std::ptrdiff_t>(counts.other_player_count);
            if (counts.other_player_count < counts.other_players.size() &&
                std::find(counts.other_players.begin(), players_end, player) == players_end)
                counts.other_players[counts.other_player_count++] = player;
            return;
        }
        ++counts.picture;
        if (!sound(view.world) || !sound(view.before_offset)) {
            ++counts.unsound;
            return;
        }
        const auto at = Clock::now();
        if (!b.used || at - b.at >= std::chrono::milliseconds(2)) {
            if (b.used) {
                const double between = std::chrono::duration<double, std::milli>(at - b.at).count();
                counts.shortest_ms = counts.intervals ? std::min(counts.shortest_ms, between) : between;
                counts.longest_ms = counts.intervals ? std::max(counts.longest_ms, between) : between;
                ++counts.intervals;
            }
            b.time = build_time(b, *frame);
            b.moved = gated(offsets(sample(frame->points, frame->fallback, b.time), frame->seed), gate_now(*frame));
            b.at = at;
            b.used = true;
            ++counts.frames;
        }
        const Matrix base = view.world;
        if (probe) measure_view(s, counts.filled_in, base);
        if (still(b.moved)) {
            ++counts.still;
            return;
        }
        const bool beside_same = std::memcmp(&beside, &view, sizeof(view)) == 0;
        view.world = shaken(view.world, b.moved);
        view.before_offset = shaken(view.before_offset, b.moved);
        if (beside_same) {
            beside = view;
            ++counts.beside_same;
        }
        ++counts.shaken;
        pending_build = {description, base, view.world, b.moved};
        std::lock_guard frame_lock(s.frame_mutex);
        s.source_written[s.source_next] = {base, view.world};
        s.source_next = (s.source_next + 1) % s.source_written.size();
    } catch (...) { /* The view is built as the game has it this time. */ }
}
// `replayshake source 3`, once the game has filled the description shaken in: its view zoomed (the game's
// own setter, on a view with no projection of its own), and, for the probe, whether it holds the shake.
// The picture's description (the first local player's view) as Replay Plus last saw it filled in:
// the replay camera's say on it comes right after, on the same thread.
std::uintptr_t plus_description{};
void view_built(std::uintptr_t description, std::uintptr_t camera_view) noexcept {
    std::uint32_t player{1};
    if (memory::peek(camera_view + game_view::camera_view_player, player) && player == 0) {
        plus_description = description;
        replay_plus::view_built(description, false);
    }
    if (!pending_build.description || pending_build.description != description) return;
    const auto& pending = pending_build;
    bool zoomed_now = false;
    if (pending.moved[channel::zoom] != 0) {
        float fov{};
        std::uint8_t own_projection{};
        if (memory::peek(description + game_view::description_fov, fov) &&
            memory::peek(description + game_view::description_own_projection, own_projection) && !own_projection &&
            std::isfinite(fov) && fov > 0.02f && fov < 3.0f) {
            game_view::set_fov(description, zoomed(fov, pending.moved));
            zoomed_now = true;
        }
    }
    auto& s = state();
    if (!s.probe_render.load(std::memory_order_acquire)) return;
    Matrix now{};
    const bool held = memory::peek(description + game_view::description_camera, now) && frames::same_camera(now, pending.shown);
    try {
        auto& b = builds();
        std::lock_guard lock(b.mutex);
        ++(held ? b.counts.held : b.counts.not_held);
        if (zoomed_now) ++b.counts.zoomed;
    } catch (...) { /* Not counted this time. */ }
}
// `replayshake source 3`, right after the replay camera has had its say on the description shaken
// (game_view_hook.h): should it have set the view anew (in the replay editor, from the replay's own
// camera, which trails the camera the editor moves), that is the picture's camera now, and it is given the
// frame's shake in turn, through the game's own setter. Left as the fill made it, it shakes already.
void view_overridden(std::uintptr_t description) noexcept {
    if (description && description == plus_description) {
        plus_description = 0;
        replay_plus::view_built(description, true);
    }
    if (!pending_build.description || pending_build.description != description) return;
    const auto pending = pending_build;
    pending_build.description = 0;
    auto& s = state();
    try {
        game_view::Transform now{};
        const bool read = memory::peek(description + game_view::description_transform, now);
        const bool replayed = read && !frames::same_camera(now.world, pending.shown);
        const bool soundly = replayed && sound(now.world) && sound(now.before_offset);
        game_view::Transform shaken_now = now;
        if (soundly) {
            shaken_now.world = shaken(now.world, pending.moved);
            shaken_now.before_offset = shaken(now.before_offset, pending.moved);
            game_view::set_transform(description, shaken_now);
            std::lock_guard frame_lock(s.frame_mutex);
            s.source_written[s.source_next] = {now.world, shaken_now.world};
            s.source_next = (s.source_next + 1) % s.source_written.size();
        }
        if (!s.probe_render.load(std::memory_order_acquire)) return;
        auto& b = builds();
        std::lock_guard lock(b.mutex);
        auto& counts = b.counts;
        if (!replayed) {
            ++counts.not_replayed;
            return;
        }
        if (!soundly) {
            ++counts.replay_unsound;
            return;
        }
        ++counts.replayed;
        if (const float distance = distance_between(now.world, pending.base); std::isfinite(distance))
            counts.replay_apart = std::max(counts.replay_apart, distance);
        measure_view(s, counts.replay, now.world);
    } catch (...) { /* The view stays as the replay camera set it this time. */ }
}
// `replayshake source 3`, client tick: the hook on the game's building of its views on while the views
// built shake. Asked every tick (nothing to switch costs nothing); a failure waits a few seconds, and
// meanwhile the camera (or the views drawn) shakes instead.
void set_builds(State& s, bool on) {
    if (GetTickCount64() < s.builds_retry_at) return;
    if (game_view::want(on)) {
        s.builds_hooked = on;
    } else {
        s.builds_hooked = false;
        s.builds_retry_at = GetTickCount64() + 5000;
    }
}
// `replayshake source 1`, client tick: the camera the game's camera update shakes (0 none), with the
// hook on while there is one. While the hook is not on (it could not be switched), the views drawn
// shake instead.
void set_camera(State& s, std::uintptr_t camera) {
    s.engine_thread.store(GetCurrentThreadId(), std::memory_order_release);
    // A camera no longer shaken is left alone (it may be gone): should it still be there, the game's
    // next update of it sets it afresh.
    if (s.source_shake.camera != camera) {
        s.source_shake = {};
        s.source_states = {};
    }
    // Asked every tick (nothing to switch costs nothing), so that a switch that failed partway is
    // finished or undone the next time; a failure waits a few seconds, and meanwhile the views drawn
    // shake instead.
    const bool wanted = camera != 0;
    if (GetTickCount64() >= s.camera_retry_at) {
        if (camera_update::want(wanted)) {
            s.camera_hooked = wanted;
        } else {
            s.camera_hooked = false;
            s.camera_retry_at = GetTickCount64() + 5000;
        }
    }
    const auto target = s.camera_hooked ? camera : 0;
    s.camera_target.store(target, std::memory_order_release);
    std::lock_guard frame_lock(s.frame_mutex);
    s.source_on = target != 0;
}
// `replayshake probe`: the renderer traces the next view set-ups (log_kept() logs them).
void arm_trace(State& s) {
    s.trace_armed.store(true, std::memory_order_release);
    s.trace_armed_at = GetTickCount64();
}
// The renderer's hook is on only while there is something to shake: switched off, it costs nothing.
// Switching it suspends the game's threads, so a failure waits a few seconds before another try.
void set_live(State& s, bool live) {
    if (live && !s.was_live) {
        s.since.fetch_add(1, std::memory_order_acq_rel);
        if (s.probe) arm_trace(s);
        auto& b = builds(); // the views built start afresh too
        std::lock_guard lock(b.mutex);
        b.used = false;
    }
    s.was_live = live;
    s.live_until.store(live ? GetTickCount64() + lease_ms : 0, std::memory_order_release);
    if (live == s.hooked || GetTickCount64() < s.hook_retry_at) return;
    if (render_view::want(render_view::Client::replay_shake, live)) s.hooked = live;
    else s.hook_retry_at = GetTickCount64() + 5000;
}

// The game's menu state, in words.
std::string menus_text(const scan::GameUi& ui) {
    if (!ui.found) return "not read (Replay Shake cannot tell the replay editor from gameplay)";
    return std::format("a game menu {}, the game's UI {}", ui.in_menu ? "is up" : "is not up", ui.ui_hidden ? "hidden" : "shown");
}
std::string status_text(const State& s) {
    const auto& settings = s.settings;
    if (!settings.enabled) return "Off: Replay Plus's panel switches it on (SHAKE), in the replay editor.";
    if (s.test) {
        return silent(settings.fallback) ? "Testing: every view shakes with the default shake, which is at 0%: turn it up to see it."
                                         : "Testing: every view in the level shakes with the default shake.";
    }
    if (s.gameplay_held)
        return "Gameplay, by the look of it (a replay camera is on show, but no game menu is up and no director but "
               "gameplay's moves it): kept still. Shake gameplay too, if this is the replay editor.";
    if (!s.editor.open) return replay_cameras(settings).empty() ? "Waiting for the replay editor (its camera is not known yet)."
                                                                : "Waiting for the replay editor.";
    if (export_frames::capture().running)
        return std::format("Exporting: each frame shakes one step of {} frames a second on.", settings.export_fps);
    if (!s.editor.playhead)
        return s.editor.keyframes || !settings.keys.empty()
            ? "Shaking the replay with the default shake: keyframe shakes need the playhead mapped (README.md, step 3)."
            : "Shaking the replay with the default shake.";
    if (s.editor.keyframes) {
        const auto count = s.editor.keyframes->size();
        return count ? std::format("Shaking the replay by its {} keyframe{}.", count, count == 1 ? "" : "s")
                     : "Shaking the replay with the default shake (it has no keyframes).";
    }
    if (!settings.keys.empty())
        return std::format("Shaking the replay by {} shake key{}.", settings.keys.size(), settings.keys.size() == 1 ? "" : "s");
    return "Shaking the replay with the default shake.";
}
std::string next_step_text(const State& s) {
    if (replay_cameras(s.settings).empty() && !s.stale_build.empty())
        return std::format("The game build changed since the replay editor's camera was found (on build {}, now {}): find it "
                           "again. Turn on Probe, open the replay editor, and add the camera that turns up (README.md, step 2).",
            s.stale_build, game_build);
    if (replay_cameras(s.settings).empty())
        return "Next: find the replay editor's camera. Turn on Probe, open the replay editor, and add the camera that turns up "
               "(README.md, step 2).";
    // Only the vtable every camera has: gameplay is told apart by the game's menu state alone.
    const auto& cameras = s.settings.cameras;
    if (!s.settings.gameplay && cameras.size() == 1 && cameras.front() == addr::engine::camera_vtable && s.probe)
        return std::format("{} is every camera's vtable, gameplay's too: Replay Shake keeps gameplay still by the game's menu "
                           "state. With Probe on, the replay editor's camera may show a director of its own: add that one "
                           "(README.md, step 2).", hex(addr::engine::camera_vtable));
    if (!s.settings.mapping.root)
        return "Optional: map the replay editor's playhead and keyframes for a shake per keyframe (README.md, step 3).";
    if (!s.editor_problem.empty() && (s.editor.open || s.probe)) return "The editor mapping: " + s.editor_problem;
    return {};
}
void publish_view(State& s) {
    auto view = std::make_shared<View>();
    view->revision = ++s.revision;
    view->available = s.ready.load(std::memory_order_acquire);
    view->enabled = s.settings.enabled;
    view->test = s.test;
    view->probe = s.probe;
    view->gameplay = s.settings.gameplay;
    view->gameplay_held = s.gameplay_held;
    view->editor_open = s.editor.open;
    view->every_camera = addr::engine::camera_vtable;
    view->gameplay_directors.assign(layout::gameplay_directors.begin(), layout::gameplay_directors.end());
    if (s.probe) view->menu_state = menus_text(s.ui);
    view->shaking = GetTickCount64() <= s.live_until.load(std::memory_order_acquire);
    view->exporting = export_frames::capture().running;
    view->keyframe_mode = s.editor.keyframes.has_value();
    view->playhead = s.editor.playhead;
    view->status = status_text(s);
    view->next_step = next_step_text(s);
    view->mapping = describe(s.settings.mapping);
    view->editor_problem = s.editor_problem;
    view->cameras = replay_cameras(s.settings);
    for (const auto& seen : s.seen) {
        const SeenCamera camera{seen.vtable, seen.director};
        if ((camera.vtable || camera.director) &&
            std::find(view->cameras_seen.begin(), view->cameras_seen.end(), camera) == view->cameras_seen.end())
            view->cameras_seen.push_back(camera);
    }
    view->fallback = s.settings.fallback;
    view->seed = s.settings.seed;
    view->export_fps = s.settings.export_fps;
    const auto playhead = s.editor.playhead;
    const auto current = [&](std::size_t index, const std::vector<double>& times) {
        return playhead && *playhead >= times[index] && (index + 1 == times.size() || *playhead < times[index + 1]);
    };
    if (s.editor.keyframes) {
        const auto& times = *s.editor.keyframes;
        for (std::size_t i = 0; i < times.size(); ++i) {
            Row row;
            row.number = static_cast<int>(i + 1);
            row.time = times[i];
            row.shake = s.settings.fallback;
            for (const auto& key : s.settings.keys)
                if (std::abs(key.time - times[i]) <= key_tolerance) {
                    row.own = true;
                    row.shake = key.shake;
                    row.blend = key.blend;
                }
            row.current = current(i, times);
            row.selected = s.editor.selected && *s.editor.selected == static_cast<int>(i);
            view->rows.push_back(row);
        }
    } else {
        std::vector<double> times;
        for (const auto& key : s.settings.keys) times.push_back(key.time);
        for (std::size_t i = 0; i < s.settings.keys.size(); ++i) {
            const auto& key = s.settings.keys[i];
            Row row;
            row.number = static_cast<int>(i + 1);
            row.time = key.time;
            row.own = true;
            row.shake = key.shake;
            row.blend = key.blend;
            row.current = current(i, times);
            view->rows.push_back(row);
        }
    }
    s.view_at = GetTickCount64();
    publish(std::move(view));
}
// `replayshake probe 1`: the cameras on show as they change, and what the mapping reads.
void log_probe(State& s) {
    std::string cameras;
    for (const auto& seen : s.seen) {
        if (seen.other_view) {
            cameras += std::format(" [view {}: not a camera view, vtable {}]", seen.index,
                seen.view_vtable ? hex(seen.view_vtable) : std::string("outside Skate.exe"));
            continue;
        }
        std::string camera = seen.vtable ? "camera vtable " + hex(seen.vtable)
                                         : seen.camera ? std::string("a camera outside Skate.exe") : std::string("no camera");
        if (seen.director) camera += std::format(", director {} (of {})", hex(seen.director), seen.directors);
        else if (seen.camera) camera += ", no director read";
        const auto match = replay_camera(s.settings, seen);
        if (match != Match::none) camera += match == Match::director ? ", a replay camera by its director" : ", a replay camera";
        cameras += std::format(" [view {}{}{}: {}]", seen.index, seen.ids[0] == 0 && seen.ids[1] == 0 ? ", local" : "",
            seen.enabled ? "" : ", off", camera);
    }
    if (cameras != s.probe_cameras) {
        s.probe_cameras = cameras;
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Shake probe: cameras on show:{}",
            cameras.empty() ? std::string(" none (no camera views)") : cameras);
    }
    if (const auto menus = menus_text(s.ui) + (s.gameplay_held ? "; a replay camera on show is taken for gameplay and kept still" : "");
        menus != s.probe_menus) {
        s.probe_menus = menus;
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Shake probe: the game's menu state: {}.", menus);
    }
    if (!s.settings.mapping.root || GetTickCount64() - s.probe_read_at < probe_read_every_ms) return;
    std::string read = "playhead " + (s.editor.playhead ? seconds(*s.editor.playhead) : std::string("-"));
    if (s.editor.keyframes) {
        read += std::format(", {} keyframe(s):", s.editor.keyframes->size());
        for (std::size_t i = 0; i < s.editor.keyframes->size() && i < 12; ++i) read += " " + seconds((*s.editor.keyframes)[i]);
    }
    if (s.editor.selected) read += std::format(", selected {}", *s.editor.selected + 1);
    if (!s.editor_problem.empty()) read += " (" + s.editor_problem + ")";
    if (read != s.probe_read) {
        s.probe_read = read;
        s.probe_read_at = GetTickCount64();
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Shake probe: the editor mapping reads {}", read);
    }
}
// Short names for the addresses and threads of a trace, in the order they first come: "v1", "v2"...
class Names {
public:
    explicit Names(std::string_view letter) : letter_(letter) {}
    std::string operator()(std::uint64_t value) {
        auto found = std::find(seen_.begin(), seen_.end(), value);
        if (found == seen_.end()) {
            seen_.push_back(value);
            found = seen_.end() - 1;
        }
        return std::format("{}{}", letter_, found - seen_.begin() + 1);
    }
    // "v1 = 0x1234, v2 = ..." (threads in decimal).
    std::string legend(bool decimal = false) const {
        std::string text;
        for (std::size_t i = 0; i < seen_.size(); ++i)
            text += std::format("{}{}{} = {}", text.empty() ? "" : ", ", letter_, i + 1,
                decimal ? std::to_string(seen_[i]) : hex(seen_[i]));
        return text;
    }

private:
    std::string_view letter_;
    std::vector<std::uint64_t> seen_;
};
// `replayshake passes`, as typed: "1111", the first set-up of a frame first (the fourth stands for the rest).
std::string passes_text(std::uint32_t passes) {
    std::string text;
    for (std::uint32_t bit = 0; bit < 4; ++bit) text += (passes & (1U << bit)) ? '1' : '0';
    return text;
}
// " +0x0 +0x40": the places a mask has, as offsets in a view.
std::string places_text(std::uint32_t mask) {
    std::string text;
    for (std::size_t place = 0; place < camera_places; ++place)
        if (mask & (1U << place)) text += std::format(" +0x{:x}", place * 16);
    return text;
}
// `replayshake probe 1`: the view set-ups the renderer traced, once it has them all (or five seconds on).
void log_trace(State& s, Renderer& r) {
    std::array<Traced, trace_size> trace{};
    std::size_t count{};
    {
        std::lock_guard lock(r.mutex);
        if (!r.tracing || (r.traced < r.trace.size() && GetTickCount64() - s.trace_armed_at < kept_log_every_ms)) return;
        trace = r.trace;
        count = r.traced;
        r.tracing = false;
    }
    if (!count) return;
    Names threads("t"), views("v"), previous("p"), blackboards("b");
    std::vector<std::string> lines;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& t = trace[i];
        std::string what;
        if (t.kind != render::main_view_kind) {
            what = std::format("kind {} view (not the main picture's): ", t.kind);
            what += t.copies ? std::string(t.copied ? "the main camera, as the game set it up, given the frame's shake at"
                                                    : "the main camera, as the game set it up (copies off: left), at") +
                    places_text(t.copies)
                             : std::string("no copy of the main camera as the game set it up");
            if (t.previous_copies) what += "; previous view: the frame before's put back as shown at" + places_text(t.previous_copies);
        } else {
            constexpr std::array<const char*, 4> previous_names{"not looked at (history off, or none)", "as shown",
                "put back as shown", "neither"};
            switch (t.place) {
            case frames::Place::next:
                what = std::format("frame {} begins ({})", t.frame, t.moved ? "the game's camera moved" : "the camera is where it was");
                break;
            case frames::Place::shown: what = std::format("frame {}, the view set up again, its shake on already", t.frame); break;
            case frames::Place::again: what = std::format("frame {}, another view, given the frame's shake", t.frame); break;
            }
            what += std::format("; previous view: {}; set-up {} of the frame{}{}", previous_names[static_cast<std::size_t>(t.previous_state)],
                t.setup + 1, t.still ? ", held still for the copy (passes)" : "", t.released ? ", the game's camera given back after (after 0)" : "");
        }
        lines.push_back(std::format("\n  {:2}. +{:.3f} ms {} {} {} {} jitter {}: {}", i + 1, static_cast<double>(t.at_us) / 1000.0,
            threads(t.thread), views(t.view), t.previous ? previous(t.previous) : std::string("p-"), blackboards(t.blackboard),
            t.jitter, what));
    }
    // In parts the log takes whole (it keeps 8 KB of a message).
    constexpr std::size_t per_message = 16;
    for (std::size_t first = 0; first < lines.size(); first += per_message) {
        std::string text;
        for (std::size_t i = first; i < std::min(lines.size(), first + per_message); ++i) text += lines[i];
        logging::log(logging::Level::info, logging::Channel::graphics,
            "Replay Shake probe: the renderer's first {} view set-ups since probing, as they came (t thread, v view, p previous "
            "view, b blackboard; part {} of {}):{}", count, first / per_message + 1, (lines.size() + per_message - 1) / per_message,
            text);
    }
    logging::log(logging::Level::info, logging::Channel::graphics, "Replay Shake probe: in those set-ups, {}; {}; {}; threads {}",
        views.legend(), previous.legend(), blackboards.legend(), threads.legend(true));
}
// The camera's transform the game sets from a state (Skate.exe 0x17ac360, camera_update_hook.h): rows
// right, up and back from the eye's look at the point looked at and the way up (with the game's own
// fallbacks), the position the eye plus the offset. False when it cannot be told.
bool transform_of_state(const float* state, Matrix& out) noexcept {
    using Vector = std::array<float, 3>;
    const auto dot = [](const Vector& a, const Vector& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto cross = [](const Vector& a, const Vector& b) {
        return Vector{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const Vector look{state[8] - state[0], state[9] - state[1], state[10] - state[2]};
    const float look_squared = dot(look, look);
    Vector dir{0, 0, 1};
    if (look_squared >= 1e-6f) {
        const float scale = 1.0f / std::sqrt(look_squared);
        dir = {look[0] * scale, look[1] * scale, look[2] * scale};
    }
    Vector way_up{state[12], state[13], state[14]};
    if (!(std::abs(dot(way_up, dir)) <= 0.9999f)) way_up = {way_up[1], way_up[2], way_up[0]};
    const auto side = cross(way_up, dir);
    const float side_length = std::sqrt(dot(side, side));
    if (!(side_length > 1e-12f) || !std::isfinite(side_length)) return false;
    const Vector r0{side[0] / side_length, side[1] / side_length, side[2] / side_length};
    const auto up = cross(dir, r0);
    out = {-r0[0], -r0[1], -r0[2], 0, up[0], up[1], up[2], 0, -dir[0], -dir[1], -dir[2], 0,
        state[0] + state[4], state[1] + state[5], state[2] + state[6], 1};
    for (const float value : out)
        if (!std::isfinite(value)) return false;
    return true;
}
// `replayshake probe`: where in an object (`size` bytes at `at`) a view's camera is, as a transform or
// as a state the game would set it from; and, given `pointers`, the pointers it holds.
void look_in(std::string& found, std::string_view name, std::uintptr_t at, std::size_t size, const Matrix& view,
    std::vector<std::uintptr_t>* pointers) noexcept {
    if (!at || (at & 7)) return;
    std::array<float, 0x500 / sizeof(float)> data{};
    size = std::min(size, sizeof(data));
    std::size_t read = 0; // what could be read, from the start
    for (; read < size; read += 0x40)
        if (!memory::peek_bytes(at + read, reinterpret_cast<std::byte*>(data.data()) + read, std::min<std::size_t>(0x40, size - read)))
            break;
    read = std::min(read, size);
    try {
        for (std::size_t offset = 0; offset + 64 <= read; offset += 16) {
            Matrix m{};
            std::memcpy(m.data(), reinterpret_cast<const std::byte*>(data.data()) + offset, sizeof(m));
            Matrix converted{};
            if (sound(m) && apart(m, view) < 1e-4f) found += std::format(" {} 0x{:x}+0x{:x} (a transform);", name, at, offset);
            else if (transform_of_state(m.data(), converted) && apart(converted, view) < 1e-4f)
                found += std::format(" {} 0x{:x}+0x{:x} (a state);", name, at, offset);
        }
        if (pointers)
            for (std::size_t offset = 0; offset + 8 <= read && pointers->size() < 64; offset += 8) {
                std::uintptr_t value{};
                std::memcpy(&value, reinterpret_cast<const std::byte*>(data.data()) + offset, sizeof(value));
                if (value >= 0x10000 && value < memory::highest_user_address && !(value & 7) &&
                    std::find(pointers->begin(), pointers->end(), value) == pointers->end())
                    pointers->push_back(value);
            }
    } catch (...) { /* What was found so far. */ }
}
// `replayshake probe`, client tick: where the main view's camera, as last drawn without the shake, is to
// be found around the camera on show (its evaluator, state, blender, what it hands on, its entries and
// their directors, and what those point at) and in the camera views (and their own cameras). Inside an
// evaluator (camera_update_hook.cpp): its state at [+0x60]+0x10, blender at [+0x68], what it hands on
// at [+0x80].
void scan_for_view(State& s) {
    Matrix view{};
    {
        std::lock_guard lock(s.unshaken_mutex);
        if (!s.unshaken_at || GetTickCount64() - s.unshaken_at > 1000) return;
        view = s.unshaken_view;
    }
    std::string found;
    const auto camera = s.shown_camera;
    look_in(found, "the camera on show", camera, 0x190, view, nullptr);
    std::uintptr_t evaluator{};
    if (camera && memory::peek(camera + layout::camera_evaluator, evaluator) && evaluator) {
        std::vector<std::uintptr_t> pointers;
        look_in(found, "its evaluator", evaluator, 0x180, view, &pointers);
        constexpr std::array<std::pair<std::uintptr_t, std::string_view>, 3> parts{{
            {0x60, "its state"}, {0x68, "its blender"}, {0x80, "what it hands on"}}};
        for (const auto& [offset, name] : parts) {
            std::uintptr_t part{};
            if (memory::peek(evaluator + offset, part)) look_in(found, name, part, 0x500, view, nullptr);
        }
        std::uintptr_t list{}, begin{}, end{};
        if (memory::peek(evaluator + layout::evaluator_entries, list) && list && memory::peek(list + layout::entries_begin, begin) &&
            memory::peek(list + layout::entries_end, end) && end > begin && (end - begin) % layout::entry_stride == 0 &&
            (end - begin) / layout::entry_stride <= layout::most_entries)
            for (std::size_t i = 0; i < (end - begin) / layout::entry_stride; ++i) {
                std::uintptr_t entry{}, director{};
                if (!memory::peek(begin + i * layout::entry_stride, entry) || !entry) continue;
                look_in(found, std::format("entry {}", i + 1), entry, 0xc0, view, &pointers);
                if (memory::peek(entry + 0x60, director))
                    look_in(found, std::format("entry {}'s director", i + 1), director, 0x300, view, &pointers);
            }
        for (const auto pointer : pointers)
            if (pointer != evaluator) look_in(found, "what they point at", pointer, 0x200, view, nullptr);
    }
    for (const auto& seen : s.seen) {
        if (!seen.view) continue;
        look_in(found, std::format("view {}", seen.index), seen.view, 0x500, view, nullptr);
        std::uintptr_t own{};
        if (!seen.other_view && memory::peek(seen.view + 0x2c8, own))
            look_in(found, std::format("view {}'s own camera", seen.index), own, 0x190, view, nullptr);
    }
    logging::log(logging::Level::info, logging::Channel::graphics,
        "Replay Shake probe (scan): the main view's camera, as last drawn without the shake, is found at:{}",
        found.empty() ? std::string(" nowhere looked (around the camera on show, and the camera views)") : found);
}
// `replayshake probe 1`, while shaking: how the renderer set its frames up, and what it kept of the
// frames before (carry()).
void log_kept(State& s) {
    auto& r = renderer();
    log_trace(s, r);
    if (GetTickCount64() - s.kept_logged_at < kept_log_every_ms) return;
    s.kept_logged_at = GetTickCount64();
    Kept kept;
    {
        std::lock_guard lock(r.mutex);
        kept = r.kept;
        r.kept = {};
    }
    // `replayshake source 3`: what the game's building of its views did, and whether the views drawn came with it.
    Builds::Counts built{};
    {
        auto& b = builds();
        std::lock_guard lock(b.mutex);
        built = b.counts;
        b.counts = {};
    }
    std::string other_players;
    for (std::size_t i = 0; i < built.other_player_count; ++i)
        other_players += std::format("{}{}", other_players.empty() ? " (local player " : ", ", built.other_players[i]);
    if (!other_players.empty()) other_players += ")";
    if (s.builds_hooked || built.filled)
        logging::log(logging::Level::info, logging::Channel::graphics,
            "Replay Shake probe (source 3): the game filled in {} view(s) while they shook ({} on another thread than the "
            "client tick's), {} of them the first local player's (the picture's) and {} others'{}. The picture's view: given "
            "the shake {}x, a still shake {}x, no camera's transform {}x (left); the transform kept beside it was its own {}x "
            "(shaken too); once filled in it held the shaken camera {}x, not {}x; zoomed {}x. Then the replay camera set it "
            "anew {}x (given the shake again; up to {:.2f} m from the game's view as filled in), left it {}x, set it to no "
            "camera's transform {}x (left). The shake was worked out for {} frame(s), {:.1f} to {:.1f} ms apart. Against the "
            "camera on show then, the view as filled in was the same {}x, within 1 cm {}x, further {}x (up to {:.2f} m), no "
            "camera on show {}x; as the replay camera set it, the same {}x, within 1 cm {}x, further {}x (up to {:.2f} m), no "
            "camera on show {}x. The main view's set-ups came with the shaken view {}x and as the game built it {}x (a still "
            "shake tells neither).",
            built.filled, built.elsewhere, built.picture, built.others, other_players, built.shaken, built.still,
            built.unsound, built.beside_same, built.held, built.not_held, built.zoomed, built.replayed, built.replay_apart,
            built.not_replayed, built.replay_unsound, built.frames, built.shortest_ms, built.longest_ms, built.filled_in.same,
            built.filled_in.within_cm, built.filled_in.beyond_cm, built.filled_in.farthest, built.filled_in.no_camera,
            built.replay.same, built.replay.within_cm, built.replay.beyond_cm, built.replay.farthest, built.replay.no_camera,
            kept.source_seen, kept.source_missed);
    // `replayshake source 1`: what was done to the camera itself, and whether the views came with it.
    const auto elsewhere = s.camera_elsewhere.exchange(0, std::memory_order_acq_rel);
    if (s.camera_target.load(std::memory_order_acquire) || s.camera_shakes || s.camera_restores || elsewhere)
        logging::log(logging::Level::info, logging::Channel::graphics,
            "Replay Shake probe (source {}): the shake put on the camera in the game's update of it, as soon as the update set "
            "it, {}x (on the state it was set from {}x, the update's copy of that {}x, what the update hands on {}x, the "
            "blender's blend {}x and the blend as taken {}x{}); the camera still shaken when the update ended {}x, changed by "
            "then {}x; the shake taken off at the start of its next update {}x (the states {}x); {} update(s) of it on another "
            "thread (left alone); the main view's set-ups came with the shaken camera {}x and as the game set it up {}x (a still "
            "shake tells neither).",
            s.source_mode == State::Source::held ? 2 : 1, s.camera_shakes, s.state_shakes[State::state_set_from],
            s.state_shakes[State::state_copy], s.state_shakes[State::state_handed], s.state_shakes[State::state_blend],
            s.state_shakes[State::state_blend_kept],
            s.other_blender ? std::format("; a blender of another class met, vtable 0x{:x}: left alone", s.other_blender - s.base)
                            : std::string(),
            s.held_at_end, s.changed_at_end, s.camera_restores, s.state_restores, elsewhere, kept.source_seen,
            kept.source_missed);
    s.other_blender = 0;
    s.camera_shakes = s.camera_restores = s.state_restores = s.held_at_end = s.changed_at_end = 0;
    s.state_shakes = {};
    // `replayshake follow`: which cameras the main view came with, and every camera the game set.
    if (s.camera_target.load(std::memory_order_acquire) || kept.drawn_from[0].camera || kept.drawn_from_none) {
        std::string drawn_from;
        for (const auto& entry : kept.drawn_from)
            if (entry.camera)
                drawn_from += std::format("{} 0x{:x} (vtable 0x{:x}) {}x", drawn_from.empty() ? "" : ",", entry.camera,
                    entry.vtable - s.base, entry.setups);
        if (kept.drawn_from_dropped) drawn_from += std::format(", others {}x", kept.drawn_from_dropped);
        std::string set;
        {
            std::lock_guard lock(s.census_mutex);
            for (std::size_t i = 0; i < s.tally_count; ++i) {
                const auto& entry = s.tallies[i];
                set += std::format("{} 0x{:x} (vtable 0x{:x}, from a state {}x, a transform {}x)", set.empty() ? "" : ",", entry.camera,
                    entry.vtable - s.base, entry.from_state, entry.from_transform);
            }
            if (s.tallies_dropped) set += std::format(", and {} write(s) of further cameras", s.tallies_dropped);
            s.tally_count = 0;
            s.tallies_dropped = 0;
        }
        const auto drawn = s.drawn_camera.load(std::memory_order_acquire);
        logging::log(logging::Level::info, logging::Channel::graphics,
            "Replay Shake probe (follow): the main view's set-ups came with another camera the game set:{} ; with no camera's it "
            "set {}x. Shaken: {} ({} write(s) of it on another thread, {} shake(s) let go while the state was busy). The "
            "cameras the game set:{}.",
            drawn_from.empty() ? std::string(" none") : drawn_from, kept.drawn_from_none,
            drawn ? std::format("0x{:x}, the one the main view is drawn from", drawn)
                  : std::format("the camera on show, 0x{:x}", s.camera_target.load(std::memory_order_acquire)),
            s.drawn_elsewhere.exchange(0, std::memory_order_acq_rel), s.lock_missed.exchange(0, std::memory_order_acq_rel),
            set.empty() ? std::string(" none") : set);
        scan_for_view(s);
    }
    if (!kept.setups) return;
    std::string copies;
    for (std::size_t i = 0; i < kept.copies.size(); ++i)
        if (kept.copies[i])
            copies += std::format(" {}+0x{:x} {}x", i < camera_places ? "view" : "previous", (i % camera_places) * 16, kept.copies[i]);
    const auto history = s.carry.load(std::memory_order_acquire)
        ? std::format("The previous view held the frame before's camera as shown {}x, as the game set it up (put back as "
                      "shown) {}x, neither {}x; other copies put back:{}.", kept.previous_shown, kept.previous_game,
              kept.previous_other, copies.empty() ? std::string(" none") : copies)
        : std::string("History off: the frames before are left as the game set them up.");
    std::string others;
    for (std::size_t i = 0; i < kept.other_count; ++i) {
        const auto& other = kept.others[i];
        others += std::format(" kind {}: {} set-up(s)", other.kind, other.setups);
        if (other.copies) others += std::format(", {} with the main camera as the game set it up at{}", other.copies, places_text(other.places));
        if (other.shown) others += std::format(", {} with it shaken already", other.shown);
        if (other.previous_copies)
            others += std::format(", {} with the frame before's in the previous view at{}", other.previous_copies,
                places_text(other.previous_places));
        if (other.early) others += std::format(", {} set up from the frame's camera before the main view (left unshaken)", other.early);
        if (other.still_kept || other.still_moved)
            others += std::format(", while the main camera stood still its own stayed {}x and moved {}x", other.still_kept,
                other.still_moved);
        others += ";";
    }
    if (kept.other_kinds_dropped) others += std::format(" and {} set-up(s) of further kinds;", kept.other_kinds_dropped);
    logging::log(logging::Level::info, logging::Channel::graphics,
        "Replay Shake probe: {} view set-up(s) shaken in {} frame(s) ({:.1f} a frame, up to {} view(s) each): {} found the "
        "frame's shake on already (the same view set up again), {} were another view given it, {} held still (passes {}), {} given "
        "the game's camera back after their copy; {} came with the camera shaken on the camera itself, {} not (source). {} "
        "The frame's camera against "
        "the client's: the same {}x, within 1 cm {}x, further {}x, no client camera {}x. Views of other kinds{}:{}",
        kept.setups, kept.frames, kept.frames ? static_cast<double>(kept.setups) / kept.frames : 0.0, kept.most_views,
        kept.shown, kept.again, kept.still_passes, passes_text(s.passes.load(std::memory_order_acquire)), kept.released,
        kept.source_seen, kept.source_missed, history,
        kept.client_same, kept.client_close, kept.client_far, kept.client_none,
        s.copies.load(std::memory_order_acquire) ? "" : " (copies off: left as the game set them up)",
        others.empty() ? std::string(" none") : others);
}
// `replayshake probe 1`, client tick, while a camera is shaken in its update: what of the game's reads
// that camera's places (replay_shake_watch.h), in windows of half a second every four seconds, the
// places taking turns: its transform each time, then the transform before its offset and the
// blender's two states; or the state the update sets it from, its copy and what it hands on; or the
// main view the renderer set up last, and its previous view. Once in a probe, as soon as the main
// view's camera, drawn without the shake, has stood still for half a second (a paused replay), the
// game's memory is searched for copies of it; then the copies found take a turn too, for what writes
// them. Each window's report, and the search's, is logged once done.
constexpr unsigned watch_window_ms = 500;
constexpr std::uint64_t watch_every_ms = 4000;
constexpr std::uint32_t find_after_ticks = 30;
constexpr std::size_t most_copies_watched = watch::slot_count;
std::pair<std::uintptr_t, std::uintptr_t> range_of(const void* at, std::size_t size) noexcept {
    const auto begin = reinterpret_cast<std::uintptr_t>(at);
    return {begin, begin + size};
}
// The copies to watch for what writes them: those in an object of the game's only, whole ones first.
std::vector<watch::Copy> copies_to_watch(std::vector<watch::Copy> copies) {
    std::erase_if(copies, [](const watch::Copy& copy) { return !copy.object; });
    std::stable_sort(copies.begin(), copies.end(), [](const watch::Copy& a, const watch::Copy& b) { return a.whole && !b.whole; });
    if (copies.size() > most_copies_watched) copies.resize(most_copies_watched);
    return copies;
}
void search_for_view(State& s) {
    if (s.find_started) return;
    Matrix view{};
    std::uint64_t at{};
    {
        std::lock_guard lock(s.unshaken_mutex);
        view = s.unshaken_view;
        at = s.unshaken_at;
    }
    if (!at || GetTickCount64() - at > 500 || !sound(view)) {
        s.find_stable = 0;
        return;
    }
    if (std::memcmp(view.data(), s.find_candidate.data(), sizeof(view)) != 0) {
        s.find_candidate = view;
        s.find_stable = 0;
        return;
    }
    if (++s.find_stable < find_after_ticks || watch::busy()) return;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> own{range_of(&s, sizeof(s)), range_of(&renderer(), sizeof(Renderer)),
        watch::own_memory()};
    const auto camera = s.camera_target.load(std::memory_order_acquire);
    if (watch::find(view, std::move(own), s.base, std::format("the main view's camera as drawn without the shake, the camera 0x{:x} "
            "shaken; {}", camera, menus_text(s.ui)))) {
        s.find_started = s.find_running = true;
        logging::log(logging::Level::info, logging::Channel::graphics,
            "Replay Shake probe (find): searching the game's memory for the main view's camera as drawn without the shake ...");
    }
}
// Skate.exe of build 25414733: a render view's assignment (0x3e5d430: the view at rcx made the one at
// rdx, field by field, its camera at +0x40), which the renderer copies its views with.
constexpr std::uintptr_t view_copy_begin = 0x3e5d430, view_copy_end = 0x3e5d760;
constexpr std::uint32_t most_copies_followed = 8;
// After a window on a view's camera (writes only): should the game have copied it from another view,
// that one is watched next, right away; until a view whose camera something else writes (what makes
// it: the window's report names it), or too many copies up.
void follow_copies(State& s, const std::vector<watch::Write>& writes) {
    const auto view = s.watched_view;
    s.watched_view = 0;
    if (!view) return;
    std::uintptr_t source{};
    for (const auto& write : writes)
        if (write.rip >= s.base + view_copy_begin && write.rip < s.base + view_copy_end && write.rcx == view && write.rdx &&
            write.rdx != view)
            source = static_cast<std::uintptr_t>(write.rdx);
    if (source && s.upstream_hops < most_copies_followed) {
        ++s.upstream_hops;
        s.upstream_view = source;
        s.watch_at = GetTickCount64() + 250;
        logging::log(logging::Level::info, logging::Channel::graphics,
            "Replay Shake probe (watch): the camera of view 0x{:x} is copied from view 0x{:x} ({} cop{} up from the main "
            "view): what writes that one is watched next.",
            view, source, s.upstream_hops, s.upstream_hops == 1 ? "y" : "ies");
        return;
    }
    if (s.upstream_view)
        logging::log(logging::Level::info, logging::Channel::graphics,
            "Replay Shake probe (watch): {} cop{} up from the main view, view 0x{:x} is {}: the window just logged names what "
            "writes its camera.",
            s.upstream_hops, s.upstream_hops == 1 ? "y" : "ies", view,
            writes.empty() ? "written by nothing seen (another each frame?)" : "not copied from another view");
    s.upstream_view = 0;
    s.upstream_hops = 0;
}
void watch_camera(State& s) {
    std::vector<watch::Write> writes;
    const auto reported = watch::report(s.base, &writes);
    for (const auto& line : reported) logging::log(logging::Level::info, logging::Channel::graphics, "{}", line);
    if (!reported.empty()) follow_copies(s, writes);
    std::vector<watch::Copy> copies;
    const auto found = watch::found(s.base, copies);
    for (const auto& line : found) logging::log(logging::Level::info, logging::Channel::graphics, "{}", line);
    if (!found.empty()) {
        s.find_running = false;
        s.copies_watched = copies_to_watch(std::move(copies));
    }
    const auto camera = s.camera_target.load(std::memory_order_acquire);
    if (!camera) return;
    search_for_view(s);
    const auto now = GetTickCount64();
    if (s.find_running || watch::busy() || now < s.watch_at) return;
    std::uintptr_t evaluator{}, blender{}, holder{}, handed{};
    if (!memory::peek(camera + layout::camera_evaluator, evaluator) || !evaluator) return;
    if (!memory::peek(evaluator + camera_update::evaluator_blender, blender)) blender = 0;
    if (!memory::peek(evaluator + camera_update::evaluator_state, holder)) holder = 0;
    if (!memory::peek(evaluator + camera_update::evaluator_handed, handed)) handed = 0;
    constexpr std::uintptr_t position = 0x30; // a transform's position row
    std::array<watch::Spot, watch::slot_count> spots;
    // By turns: the main view's camera (what writes it), the camera's places as the game works it out,
    // the main view again, then the state's places (or the copies a search found).
    const auto main_view = s.main_view_at.load(std::memory_order_acquire);
    const auto turn = s.watch_windows % 4;
    const auto view = s.upstream_view ? s.upstream_view : (turn == 0 || turn == 2) ? main_view : 0;
    if (view) {
        // Writes only: what fills the view's camera in, each frame (the view stays where it is).
        constexpr std::array<const char*, 4> rows{"its first row", "its second row", "its third row", "its position"};
        for (std::size_t row = 0; row < rows.size(); ++row)
            spots[row] = {view + render::view_camera_offset + row * 0x10,
                std::format("{} 0x{:x}: its camera, {}", s.upstream_view ? "the view copied from" : "the main view", view, rows[row]),
                true};
    } else if (turn == 1 || (turn != 3 && !main_view)) {
        spots[0] = {camera + layout::camera_transform + position, "the camera's transform (its position)"};
        spots[1] = {camera + camera_unoffset + position, "its transform before the offset (its position)"};
        spots[2] = {blender ? blender + camera_update::blend_state : 0, "its blender's blend (its eye)"};
        spots[3] = {blender ? blender + camera_update::blend_kept : 0, "the blend as taken (its eye)"};
    } else if (s.copies_watched.empty()) {
        spots[0] = {camera + layout::camera_transform + position, "the camera's transform (its position)"};
        spots[1] = {holder ? holder + camera_update::holder_state : 0, "the state its update sets it from (its eye)"};
        spots[2] = {evaluator + camera_update::evaluator_copy, "the update's copy of that (its eye)"};
        spots[3] = {handed ? handed + camera_update::holder_state : 0, "what the update hands on (its eye)"};
    } else {
        // The copies of the camera a search found in the game's objects: writes only (whole: their
        // position row; aligned to 8).
        for (std::size_t i = 0; i < s.copies_watched.size(); ++i) {
            const auto& copy = s.copies_watched[i];
            const auto at = (copy.whole ? copy.at + position : copy.at) & ~static_cast<std::uintptr_t>(7);
            spots[i] = {at, std::format("{} copy at 0x{:x} (in an object at 0x{:x}, vtable Skate.exe+0x{:x})",
                                copy.whole ? "a whole" : "a position's", copy.at, copy.object, copy.vtable),
                true};
        }
    }
    std::string director;
    for (const auto& seen : s.seen)
        if (seen.camera == camera && seen.director) director = std::format(", director 0x{:x}", seen.director);
    const auto context = std::format("the camera 0x{:x}{}; {}", camera, director, menus_text(s.ui));
    if (watch::arm(spots, watch_window_ms, s.engine_thread.load(std::memory_order_acquire), context)) {
        s.watched_view = view;
        if (!s.upstream_view) ++s.watch_windows; // a view followed up its copies takes no turn
        s.watch_at = now + watch_every_ms;
    } else {
        s.upstream_view = 0;
        s.upstream_hops = 0;
        ++s.watch_windows; // a turn that cannot be watched (no main view yet, say) gives way to the next
        s.watch_at = now + watch_every_ms / 4;
    }
}
std::optional<bool> switch_word(const std::vector<std::string>& words) {
    const auto word = words.empty() ? std::string() : lower(words[0]);
    if (word == "1" || word == "on" || word == "true") return true;
    if (word == "0" || word == "off" || word == "false") return false;
    return std::nullopt;
}
} // namespace

bool start(std::uintptr_t base) noexcept {
    auto& s = state();
    if (s.ready.load(std::memory_order_acquire)) return s.base == base;
    if (!render_view::start(base)) {
        logging::write(logging::Level::warning, logging::Channel::graphics,
            "Replay Shake is unavailable: the renderer's view hook could not be prepared on this game build.");
        return false;
    }
    render_view::set_before(render_view::Client::replay_shake, &on_view);
    render_view::set_after(render_view::Client::replay_shake, &restore_view);
    // The shake goes on the camera itself wherever the game's camera update is known (`source 1`).
    camera_update::Callbacks callbacks;
    callbacks.before = &camera_before;
    callbacks.applied = &camera_applied;
    callbacks.handing = &camera_handing;
    callbacks.after = &camera_after;
    callbacks.written = &camera_written;
    const bool camera = camera_update::start(base, callbacks);
    // And on the view the game builds the picture from, before all that, wherever that is known (`source 3`).
    game_view::Callbacks build_callbacks;
    build_callbacks.building = &view_building;
    build_callbacks.built = &view_built;
    build_callbacks.overridden = &view_overridden;
    const bool built = game_view::start(base, build_callbacks);
    // And the replay's time as the game plays its camera keyframes, which exports are timed by (slow motion and all).
    const bool played = export_frames::start_played_clock(base);
    try {
        std::lock_guard lock(s.mutex);
        s.base = base;
        s.source_mode = built ? State::Source::game_views : camera ? State::Source::camera : State::Source::views;
        s.file = settings_file();
        try {
            s.settings = read_settings(s.file);
        } catch (const std::exception& error) {
            logging::log(logging::Level::warning, logging::Channel::graphics,
                "Replay Shake's settings in {} could not be read ({}); it starts from its defaults.", path_utf8(s.file), error.what());
        }
        // The replay editor's camera (its director) is known for game build 25414733, the one ReSkate 2.1.4
        // runs (found with step 2): on a first start there it is there already, so the shake shows in the
        // editor with nothing to set up. Step 2 still adds others, and it is kept like one found there.
        if (s.settings.cameras.empty() && s.settings.cameras_build.empty() && game_build == "25414733") {
            s.settings.cameras.push_back(0x6255760);
            logging::log(logging::Level::info, logging::Channel::graphics,
                "Replay Shake: the replay editor's camera for game build {} is known (0x6255760).", game_build);
        }
        // Cameras found on another game build are other cameras: find them again (no build said: this one).
        if (s.settings.cameras_build.empty()) s.settings.cameras_build = game_build;
        if (!s.settings.cameras.empty() && s.settings.cameras_build != game_build) {
            s.stale_build = s.settings.cameras_build;
            s.settings.cameras.clear();
            logging::log(logging::Level::warning, logging::Channel::graphics,
                "Replay Shake: the replay editor's camera was found on game build {}, and this is {}: find it again "
                "(Extension/ReplayShake/README.md, step 2).", s.stale_build, game_build);
        }
        s.ready.store(true, std::memory_order_release);
        logging::log(logging::Level::info, logging::Channel::graphics,
            "Replay Shake ready ({}): default {}, {} keyframe shake(s), {} replay camera(s), editor mapping {}; it shakes {}; exports are timed {}.",
            s.settings.enabled ? "on" : "off", describe(s.settings.fallback), s.settings.keys.size(),
            replay_cameras(s.settings).size(), s.settings.mapping.root ? "set" : "not set",
            built ? "the view the game builds the picture from"
                  : camera ? "the camera itself, as the game works it out" : "the views drawn",
            played ? "by the replay's time as its keyframes play (slow motion followed)" : "by their frames");
        publish_view(s);
    } catch (...) {
        return false;
    }
    if (!replay_plus::start(base))
        logging::write(logging::Level::warning, logging::Channel::graphics, "Replay Plus could not start; Replay Shake works on its own.");
    return true;
}

void tick(std::uintptr_t base, std::uint32_t client_state) noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire) || base != s.base) return;
    bool editor_open{};
    struct Then {
        std::uintptr_t base;
        std::uint32_t client_state;
        bool& editor_open;
        // Replay Plus's tick, once Replay Shake's lock is let go (it sends Replay Shake verbs of its own).
        ~Then() { replay_plus::tick(base, client_state, editor_open); }
    } then{base, client_state, editor_open};
    try {
        std::lock_guard lock(s.mutex);
        const Holding holding;
        const bool level = client_state == 13 || client_state == 21;
        const bool was_open = s.editor.open;
        // In a level the replay editor is always looked for: Replay Plus needs it (its panel switches the shake
        // back on from there), whether the shake is on or not.
        const bool looking = level;
        s.seen = looking ? scan::cameras_on_show(base) : std::vector<scan::Seen>{};
        s.ui = looking ? s.menus.read(base) : scan::GameUi{};
        // A replay camera in a view that is switched off is not drawn. One known by what gameplay's
        // camera has too (its vtable, or a director of gameplay's: replay_camera()) may be gameplay's:
        // unless gameplay shakes too, it counts while a game menu is up or the game hides its UI, as in
        // the replay editor, while an export runs, or while something other than gameplay's own camera
        // moves it (a director read and not gameplay's).
        auto match = Match::none;
        bool other_director = false;
        for (const auto& seen : s.seen) {
            if (!seen.enabled) continue;
            const auto found = replay_camera(s.settings, seen);
            match = std::max(match, found);
            other_director = other_director || (found != Match::none && seen.director && !gameplay_director(seen.director));
        }
        // Never read (another game build?): it cannot tell, and the camera counts. Read before and
        // missing now (a load, the UI model changing): no menu, until it is read again.
        s.menus_read = s.menus_read || s.ui.found;
        const bool menus = (s.ui.found ? s.ui.in_menu || s.ui.ui_hidden : !s.menus_read) || export_frames::capture().running ||
            other_director;
        s.gameplay_held = match == Match::camera && !s.settings.gameplay && !menus;
        Editor next;
        // And only while a replay is up (the replay activity's lane: the replay editor, its dialogs and its exports).
        // Other screens that show a camera of the same kind with the game's UI hidden are not the replay editor:
        // ReSkate's park editor (its props), the hub's skater view just after a map loads.
        std::uint8_t lane{};
        const bool replay_up = memory::peek(base + addr::replay_activity::playing, lane) && lane != 0;
        next.open = match != Match::none && !s.gameplay_held && replay_up;
        s.editor_problem.clear();
        if (s.settings.mapping.root && looking && (next.open || s.probe))
            s.editor_problem = s.reader.read(base, s.settings.mapping, next);
        // Shakes stay on their keyframes as the editor moves them, within one stay in the editor:
        // the next replay opened may be another one.
        if (next.open && next.keyframes) {
            if (s.previous_keyframes && *s.previous_keyframes != *next.keyframes) {
                const auto before = s.settings.keys;
                follow(s.settings.keys, *s.previous_keyframes, *next.keyframes);
                if (s.settings.keys != before) {
                    std::stable_sort(s.settings.keys.begin(), s.settings.keys.end(),
                        [](const Key& a, const Key& b) { return a.time < b.time; });
                    changed(s);
                }
            }
            s.previous_keyframes = next.keyframes;
        } else {
            s.previous_keyframes.reset();
        }
        measure(s, next.playhead);
        s.editor = std::move(next);
        s.level = level;
        if (s.probe) {
            log_probe(s);
            log_kept(s);
            watch_camera(s);
        }
        const bool live = s.settings.enabled && level && (s.editor.open || s.test);
        set_live(s, live);
        publish_frame(s);
        // `replayshake source 3`: the views the game builds shake, its hook on while they do.
        const bool want_builds = live && s.source_mode == State::Source::game_views && game_view::available();
        // Replay Plus reads (and fixes) the picture's field of view through the same hook.
        const bool plus_builds = replay_plus::wants_views(level && s.editor.open) && game_view::available();
        set_builds(s, want_builds || plus_builds);
        const bool builds_shake = want_builds && s.builds_hooked;
        // `replayshake source 1` (and 3 while its hook is not on): the camera on show that counts (the
        // local view's while testing), of the game's own camera class, whose update the hook knows
        // (camera_update_hook.h). The probe measures the views built against it.
        std::uintptr_t counting_camera{};
        for (const auto& seen : s.seen)
            if (seen.enabled && seen.camera && seen.vtable == addr::engine::camera_vtable &&
                (s.test ? seen.index == 0 : replay_camera(s.settings, seen) != Match::none)) {
                counting_camera = seen.camera;
                break;
            }
        s.probe_camera.store(s.probe ? counting_camera : 0, std::memory_order_release);
        const auto source_camera =
            live && !builds_shake && s.source_mode != State::Source::views && camera_update::available() ? counting_camera : 0;
        // `replayshake follow`: should the renderer draw its main view from another camera than the one
        // on show (follow_view()), that one is shaken instead, while the one on show stays.
        if (source_camera != s.shown_camera || !s.follow.load(std::memory_order_acquire)) s.drawn_camera.store(0, std::memory_order_release);
        s.shown_camera = source_camera;
        const auto drawn = source_camera ? s.drawn_camera.load(std::memory_order_acquire) : 0;
        if (drawn != s.logged_drawn) {
            if (drawn)
                logging::log(logging::Level::info, logging::Channel::graphics,
                    "Replay Shake: the renderer draws its main view from camera 0x{:x} (vtable 0x{:x}), not from the one on show "
                    "(0x{:x}): it shakes that one, right after the game sets it.", drawn,
                    s.drawn_vtable.load(std::memory_order_acquire) - base, source_camera);
            else
                logging::log(logging::Level::info, logging::Channel::graphics,
                    "Replay Shake: it shakes the camera on show again{}.", source_camera ? "" : " (none now)");
            s.logged_drawn = drawn;
        }
        set_camera(s, drawn ? drawn : source_camera);
        // The views drawn shake instead where neither the views built nor the camera itself can (`source
        // 0`, another game build, no camera of the game's own class on show, or the hooks not on).
        {
            std::lock_guard frame_lock(s.frame_mutex);
            s.builds_on = builds_shake;
            s.views_on = live && !builds_shake &&
                (s.source_mode == State::Source::views || !camera_update::available() || !s.source_on);
        }
        save_when_due(s);
        if (s.editor.open != was_open || GetTickCount64() - s.view_at >= view_every_ms) publish_view(s);
        editor_open = level && s.editor.open;
    } catch (...) { /* The shake waits for the next tick. */ }
}

std::string run_command(std::string_view verb, const std::vector<std::string>& words) {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire)) return "Replay Shake is unavailable on this game build.";
    try {
        std::lock_guard lock(s.mutex);
        const Holding holding;
        const auto name = lower(verb);
        std::string reply;
        if (name == "status") {
            const auto next = next_step_text(s);
            reply = status_text(s) + (next.empty() ? std::string() : " " + next);
        } else if (name == "test") {
            const auto on = switch_word(words);
            if (!on) return "test 0|1";
            s.test = *on;
            reply = s.test ? "Test on: every view in the level shakes with the default shake (`replayshake test 0` ends it)."
                           : "Test off: only the replay editor shakes.";
        } else if (name == "history") {
            const auto on = switch_word(words);
            if (!on) return "history 0|1";
            s.carry.store(*on, std::memory_order_release);
            reply = *on ? "History on: the renderer's record of the frame before shows the shake as it was shown (motion blur and "
                          "the temporal effects see only the shake's movement)."
                        : "History off, to compare: the renderer's record of the frame before is left as the game set it up.";
        } else if (name == "passes") {
            if (!words.empty()) {
                const auto& digits = words[0];
                if (digits.empty() || digits.size() > 4 || digits.find_first_not_of("01") != std::string::npos)
                    return "passes <up to 4 digits, 1 shakes and 0 holds still>: the main view's set-ups in each frame, the first "
                           "first; the last digit also stands for any set-ups after it. passes 1111 shakes them all.";
                std::uint32_t mask{};
                for (std::size_t i = 0; i < 8; ++i)
                    if ((i < digits.size() ? digits[i] : digits.back()) == '1') mask |= 1U << i;
                s.passes.store(mask, std::memory_order_release);
            }
            const auto mask = s.passes.load(std::memory_order_acquire);
            std::string held_still;
            for (std::uint32_t i = 0; i < 4; ++i)
                if (!(mask & (1U << i))) held_still += std::format("{}{}", held_still.empty() ? "" : ", ", i + 1);
            reply = held_still.empty()
                ? std::string("Passes 1111: every set-up of the main view shakes.")
                : std::format("Passes {}: set-up {} of each frame keeps the game's camera while the game copies it; the rest "
                              "shake. Watch the picture and the far shadows.", passes_text(mask), held_still);
        } else if (name == "source") {
            const auto word = words.empty() ? std::string() : lower(words[0]);
            const auto mode = word == "3" || word == "game" || word == "built" ? std::optional(State::Source::game_views)
                            : word == "1" || word == "camera" || word == "on" ? std::optional(State::Source::camera)
                            : word == "2" || word == "held" ? std::optional(State::Source::held)
                            : word == "0" || word == "views" || word == "off" ? std::optional(State::Source::views)
                            : std::nullopt;
            if (!mode)
                return "source 0|1|2|3: 3 shakes the view the game builds the picture from (the default), 1 the camera itself, "
                       "0 the views the renderer draws, 2 the camera and keeps the shake on it through the game's next update "
                       "of it (to compare).";
            if (*mode == State::Source::game_views && !game_view::available())
                return std::format("Source 3 needs the game's building of its views, which Replay Shake knows on game build "
                                   "25414733 only (this is {}).", game_build);
            if ((*mode == State::Source::camera || *mode == State::Source::held) && !camera_update::available())
                return std::format("Source {} needs the game's camera update, which Replay Shake knows on game build 25414733 "
                                   "only (this is {}): the views drawn shake instead.", word, game_build);
            if (s.source_mode != *mode) s.since.fetch_add(1, std::memory_order_acq_rel); // the renderer starts its frames afresh
            s.source_mode = *mode;
            reply = *mode == State::Source::game_views
                ? "Source 3: the shake is put on the transform the game builds the picture's view from each frame, so the "
                  "picture, its shadows, fog and motion blur all come from the shaken view, and the camera the editor moves "
                  "never sees the shake (the default)."
                : *mode == State::Source::camera
                ? "Source 1, to compare: the shake is put on the camera itself, as soon as the game sets it for each frame, so "
                  "its shadows, fog and motion blur come from the shaken camera too (gameplay's picture is drawn from it; the "
                  "replay editor's is not)."
                : *mode == State::Source::held
                ? "Source 2, to compare: the shake is put on the camera itself as with 1, and kept on it until the game's next "
                  "update of it sets it afresh (what moves the camera may then follow the shake)."
                : "Source 0, to compare: the shake is put on the views the renderer draws (the shadows may jump against it).";
        } else if (name == "follow") {
            const auto on = switch_word(words);
            if (!on) return "follow 0|1";
            s.follow.store(*on, std::memory_order_release);
            if (!*on) s.drawn_camera.store(0, std::memory_order_release);
            s.since.fetch_add(1, std::memory_order_acq_rel); // the renderer looks afresh
            reply = *on ? "Follow on: should the picture be drawn from another camera than the one on show (as in the replay "
                          "editor), that camera is shaken instead (the default)."
                        : "Follow off, to compare: only the camera on show is shaken.";
        } else if (name == "after") {
            const auto on = switch_word(words);
            if (!on) return "after 0|1";
            s.keep.store(*on, std::memory_order_release);
            reply = *on ? "After 1: the main view keeps the shaken camera once the game has copied it (as usual)."
                        : "After 0, to compare: right after the game copies the main view, it is given the game's camera back, so "
                          "whatever reads the view later sees the camera unshaken. Watch the picture and the far shadows.";
        } else if (name == "copies") {
            const auto on = switch_word(words);
            if (!on) return "copies 0|1";
            s.copies.store(*on, std::memory_order_release);
            reply = *on ? "Copies on: views of other kinds that copy the main camera (shadows, the sky...) draw from the shaken camera."
                        : "Copies off, to compare: views of other kinds keep the main camera as the game set it up.";
        } else if (name == "probe") {
            const auto on = switch_word(words);
            if (!on) return "probe 0|1";
            s.probe = *on;
            s.probe_render.store(*on, std::memory_order_release);
            if (*on) arm_trace(s);
            s.camera_shakes = s.camera_restores = s.state_restores = s.held_at_end = s.changed_at_end = 0;
            s.state_shakes = {};
            {
                std::lock_guard census_lock(s.census_mutex);
                s.tally_count = 0;
                s.tallies_dropped = 0;
            }
            s.camera_elsewhere.store(0, std::memory_order_release);
            s.drawn_elsewhere.store(0, std::memory_order_release);
            s.lock_missed.store(0, std::memory_order_release);
            {
                auto& b = builds();
                std::lock_guard builds_lock(b.mutex);
                b.counts = {};
            }
            s.probe_cameras.clear();
            s.probe_read.clear();
            // Each probe searches once, and watches from its first turn.
            s.find_started = false;
            s.find_stable = 0;
            s.copies_watched.clear();
            s.watch_windows = 0;
            s.watch_at = 0;
            s.watched_view = s.upstream_view = 0;
            s.upstream_hops = 0;
            reply = s.probe ? "Probe on: the cameras on show and what the editor mapping reads go to ReSkate.log, and the "
                              "REPLAY SHAKE page lists the cameras on show."
                            : "Probe off.";
        } else {
            const auto mapping = s.settings.mapping;
            const auto cameras = s.settings.cameras;
            auto result = command(s.settings, verb, words, s.editor);
            if (s.settings.cameras != cameras) {
                s.settings.cameras_build = game_build;
                s.stale_build.clear();
            }
            if (result.changed) changed(s);
            if (!(s.settings.mapping == mapping)) {
                s.reader.forget();
                s.previous_keyframes.reset();
            }
            reply = std::move(result.text);
        }
        set_live(s, s.settings.enabled && s.level && (s.editor.open || s.test));
        publish_frame(s);
        publish_view(s);
        return reply;
    } catch (const std::exception& error) {
        return std::string("Replay Shake: ") + error.what();
    }
}

void set_gates(std::vector<Gate> gates) noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire)) return;
    try {
        std::lock_guard lock(s.mutex);
        if (gates == s.gates) return;
        s.gates = std::move(gates);
        publish_frame(s);
    } catch (...) {}
}

PanelState panel_state() noexcept {
    auto& s = state();
    PanelState out;
    if (!s.ready.load(std::memory_order_acquire)) return out;
    try {
        std::lock_guard lock(s.mutex);
        out.available = true;
        out.on = s.settings.enabled;
        out.fallback = s.settings.fallback;
        out.seed = s.settings.seed;
        out.ui_found = s.ui.found;
        out.in_menu = s.ui.in_menu;
        out.ui_hidden = s.ui.ui_hidden;
        out.advanced_edit = s.ui.advanced_edit;
        out.timeline = s.ui.timeline;
        out.playhead_ok = s.ui.playhead_ok;
        out.timeline_length = s.ui.timeline_length;
        out.playhead = s.ui.playhead;
        out.timeline_from = s.ui.timeline_from;
        out.timeline_to = s.ui.timeline_to;
        out.timeline_at = s.ui.timeline_at;
    } catch (...) {}
    return out;
}

Json preset_shake() {
    auto& s = state();
    Json json = Json::object();
    if (!s.ready.load(std::memory_order_acquire)) return json;
    std::lock_guard lock(s.mutex);
    json["enabled"] = s.settings.enabled;
    json["default"] = shake_to_json(s.settings.fallback);
    json["seed"] = s.settings.seed;
    return json;
}

std::string apply_preset_shake(const Json& json) {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire) || !json.is_object()) return {};
    try {
        std::lock_guard lock(s.mutex);
        const auto before = to_json(s.settings).dump();
        if (json.contains("enabled") && json.at("enabled").is_boolean()) s.settings.enabled = json.at("enabled").get<bool>();
        if (json.contains("default")) s.settings.fallback = shake_from_json(json.at("default"), s.settings.fallback);
        if (json.contains("seed") && json.at("seed").is_number()) {
            const auto seed = json.at("seed").get<double>();
            if (seed >= 0 && seed <= 4294967295.0) s.settings.seed = static_cast<std::uint32_t>(seed);
        }
        if (before != to_json(s.settings).dump()) {
            changed(s);
            publish_view(s);
        }
        return s.settings.enabled ? "Replay Shake: " + describe(s.settings.fallback) + "." : std::string("Replay Shake off.");
    } catch (...) { return {}; }
}

bool exporting() noexcept { return export_frames::capture().running; }
} // namespace dingosdk::replay_shake

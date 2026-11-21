#include "replay_plus.h"
#include "replay_plus_depth.h"
#include "replay_plus_lens.h"
#include "replay_plus_light.h"
#include "replay_plus_link.h"
#include "replay_plus_native.h"
#include "replay_plus_settings.h"
#include "replay_plus_window.h"
#include "Extension/Localization/text_overrides.h"
#include "Extension/ReplayKeyframes/replay_keyframes.h"
#include "Extension/ReplayShake/game_view_hook.h"
#include "Extension/ReplayShake/replay_shake.h"
#include "Extension/ReplayShake/replay_shake_export.h"
#include "Extension/ReplayShake/replay_shake_settings.h"
#include "Extension/Settings/local_native_settings.h"
#include "Extension/Settings/named_settings.h"
#include "Extension/UI/Overlay/overlay.h"
#include "Extension/UI/Overlay/scripted_pad.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/light_tables.h"
#include "Engine/Game/Build/20260929/local_native_settings.h"
#include "Engine/Game/Build/20260929/named_settings.h"
#include "Engine/Game/Build/20260929/replay_activity.h"
#include "Engine/Game/Build/20260929/replay_export.h"
#include "Engine/Game/Build/20260929/replay_keyframes.h"
#include <Windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <charconv>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace dingosdk::runtime::detail {
void execute_console_command(const std::string& command); // Runtime/console_bridge.cpp
}

namespace dingosdk::replay_plus {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t save_after_ms = 1500, view_every_ms = 250, hold_every_ms = 250;
constexpr std::uint64_t panel_hold_ms = 450;

std::string path_text(const fs::path& path) {
    const auto wide = path.wstring();
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr, nullptr);
    return out;
}
fs::path utf8_path(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return fs::path(wide);
}
fs::path local_app_data() {
    std::array<wchar_t, 32768> buffer{};
    const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return {};
    return fs::path(buffer.data());
}
std::string read_text(const fs::path& file, std::size_t most) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("it cannot be opened");
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.size() > most) throw std::runtime_error("it is too big");
    return text;
}
// Written beside the file, then moved over it: a crash part way never leaves half a file.
bool write_text(const fs::path& file, const std::string& text) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    auto temporary = file;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        if (!output) return false;
    }
    if (MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    fs::remove(temporary, error);
    return false;
}
std::string number_text(float value) { return std::format("{}", value); }

// One of the game's own camera keyframes (Engine/Game/Build/20260929/replay_keyframes.h: a framing key's node), as
// Keep keys keeps it between visits to the replay editor: its session, its time on the replay's clock and its value,
// byte for byte (0x120 bytes of numbers: the camera's transform, where the target was, the editor's settings).
constexpr std::size_t camera_key_bytes = 0x120;
struct CameraKey {
    std::int32_t session{};
    double time{};
    std::array<std::uint8_t, camera_key_bytes> value{};
    bool operator==(const CameraKey&) const = default;
};

// A setting of the game's held while Replay Plus wants it, then given back.
struct Held {
    std::string wanted;
    bool changed{}; // Replay Plus changed it (and gives it back)
    std::uint64_t failed_at{};
    std::string last_error;
};

struct State {
    std::recursive_mutex mutex;
    std::atomic<bool> ready{};
    std::uintptr_t base{};
    Settings settings;
    fs::path file, folder, presets, overlays, shots, imports, exports;
    std::set<std::string> import_tried; // import files that could not be moved out of the import folder (path and time)
    bool dirty{};
    std::uint64_t save_at{};
    bool save_failed{};
    // The game.
    bool level{}, in_replay{}, was_in_replay{};
    std::map<std::string, Held, std::less<>> held; // by setting name
    std::uint64_t held_at{};
    float resolution_now{};
    // The panel, and R3's shortcuts in the replay editor.
    std::uint32_t pad_before{};      // the pad's buttons last tick (ReSkate's own read: the pad as it is)
    bool r3_down{}, r3_used{};       // R3 held, and something pressed with it this time
    std::uint64_t r3_at{};
    std::array<std::uint64_t, action_count> repeat_at{};     // focus / blur held on the pad: when they start moving
    std::array<std::uint64_t, action_count> key_repeat_at{}; // likewise on the keyboard
    std::array<bool, action_count> key_was{};                // each shortcut's key down last tick
    std::map<std::string, bool> preset_key_was;              // and each preset's
    std::uint64_t key_tick_at{};
    std::uint64_t pad_tick_at{};
    std::string toast;               // what the last shortcut did (the guide shows it)
    std::uint64_t toast_at{};
    std::vector<replay_shake::Gate> gates_sent; // the shake keys as Replay Shake has them
    // Auto's field of view (degrees), for the settings and screen it was worked out for.
    float fov_auto{150};
    std::uint64_t fov_auto_revision{}, fov_auto_hood{};
    float fov_auto_aspect{};
    // The replay editor's picture shape: the game's render size (pixels) and its window (as the game's threads see
    // the screen: the same on an unscaled screen, link::shape_pixels) as Replay Plus holds them, and what is wrong when
    // it cannot (the game not in borderless full screen).
    window::Size shape, shape_window;
    std::string shape_problem;
    bool resolution_checked{};
    // Sharp middle's render scale (0: none), and since when a lower one would do.
    float sharp_scale{};
    std::uint64_t sharp_lower_since{};
    // The texture pool for this card (KB; 0: the game's own), worked out once (texture_pool_for_card).
    std::uint32_t texture_pool_kb{};
    bool texture_pool_checked{};
    // The video memory guard (keep_memory_guard): the raised render scale asked of the game in the replay
    // editor, and what it goes by.
    struct MemoryGuard {
        float asked{};               // the scale asked now (0: nothing raised yet)
        float limit{};               // the most the card's memory allows, as last worked out (0: not known)
        std::uint64_t checked_at{}, changed_at{}, raise_since{};
        double bytes_per_pixel{600}; // what a pixel of the game's render targets takes (learnt from each change)
        bool learning{};
        bool lowered{};              // lowered for the memory in this visit to the editor: not raised again until the next
        std::uint64_t usage_before{};
        double pixels_before{};
        std::uint32_t screen_width{}, screen_height{}; // the screen it was asked for
        std::uint64_t usage{}, budget{};               // the game's video memory as last read
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
        std::uint64_t adapter_luid{};
    } memory;
    bool f11_down{};
    // A combo of the player's own (`replayplus bind`), held.
    std::uint64_t combo_down_at{};
    bool combo_down{}, combo_consumed{}, combo_armed{};
    bool timeline_ok{};
    float bar_from{}, bar_to{1.0f}; // the part of the whole replay the editor's timeline shows (link::timeline_share)
    double timeline_start{}, timeline_length{};
    bool timeline_published{}; // replay_shake_export.h set_timeline
    std::vector<link::Chunk> chunks; // the replay's footage, as the game strings it (replay_chunks)
    bool input_held{};
    bool pad_held{}; // the panel was open on a pad: its buttons were kept from the game
    std::uint64_t release_wait_until{};
    // What the panel shows.
    std::uint64_t revision{1}, published_revision{}, view_at{};
    std::vector<std::string> preset_names, hood_files;
    std::uint64_t lists_at{};
    std::string reply;
    std::uint64_t reply_serial{};
    // The hood picture: the one asked for, and the one being decoded.
    std::string hood_wanted;
    std::uint64_t hood_serial{};
    std::atomic<bool> hood_busy{};
    // From the view hook (the views' thread).
    std::atomic<float> fixed_fov{0.0f}; // radians; 0 none
    std::atomic<float> near_plane{0.06f}, far_plane{11000.0f}; // the picture's view's, as last read
    bool f10_down{};
    // The replay's clock, and the focus at it.
    bool playhead_ok{};
    double playhead{};
    FocusAt focus_now{3.0f, 1.0f};
    std::uint64_t focus_tick_at{};
    double playhead_shown{-1}; // as the panel was last given it
    std::atomic<bool> views_wanted{};
    std::array<std::atomic<float>, 24> probe_floats{};
    // The picture's camera as the replay left it (view_built), its world transform (rows right, up, back,
    // position), and when it was seen (GetTickCount64).
    std::array<std::atomic<float>, 16> camera{};
    std::atomic<std::uint64_t> camera_at{};
    // The camera light as a light of the game's (replay_plus_light.h): which way its transform's axes go from the
    // camera's (0: as the camera's rows; 1: right and back turned round, its forward the camera's), how far above the
    // lens it sits (m), how many lumens 100% brightness is; and whether it is made now.
    int light_axes{0};
    // 100% brightness: a skater 2 to 3 m away as lit as the light drawn over the picture had them, at night (2026-11-19);
    // the game's own lamps are 10 000 to 25 000 000 in the same units.
    float light_above{0.1f}, light_lumens{12000.0f};
    bool game_light_made{};
    bool light_on_here{}; // keyed: whether the keys have the light on at the playhead
    // The game's keyframe functions are the ones known (this game build): Keep keys can put its camera keyframes
    // back, and the skater's autofocus can read where the replay has the skater.
    bool keyframes_checked{}, keyframes_ok{};
    // Keep keys: the game's camera keyframes as last seen in the replay editor (this run of the game), when they were
    // first seen gone (they count as deleted once gone a while with the editor still open), and this visit's putting
    // back: done (or nothing to do), and not before when.
    std::vector<CameraKey> camera_keys;
    std::uint64_t camera_keys_gone_at{};
    bool camera_keys_done{};
    std::uint64_t camera_keys_from{};
    // The skater's autofocus: how far away the skater is along the camera (metres), and when that was worked out.
    float skater_depth{};
    std::uint64_t skater_at{};
    // Exported videos to the player's folder: an export running (Replay Shake counts its frames), and since when on
    // the files' clock (the video it writes is newer).
    bool exporting_seen{};
    fs::file_time_type export_began{};
    std::atomic<bool> picking_folder{};
    // The export watch (watch_exports): each step of an export said in the log as it comes, and where one stops.
    struct ExportWatch {
        bool filming{};                       // the game is drawing a video's frames (link::video_frames_seen)
        std::uint64_t since{}, last_drawn{};  // link::now_ms
        std::uint64_t session{}, frames{};    // export_frames' session and frames as it began: its encoder taking a frame moves one
        std::uint64_t encoded_at{}, progress_at{};
        bool said_stuck{};
        std::uint32_t width{}, height{};
        fs::file_time_type began{};           // on the files' clock
    } watch;
    bool title_set{}; // the replay editor's title is Replay Plus's (keep_title)
    // Exports.
    std::atomic<bool> export_on{};
    std::vector<std::uint8_t> export_scratch;
    std::mutex export_mutex;
    std::uint64_t export_frames{}, export_session_at{};
    bool export_logged{}, export_gpu_said{};
    std::uint64_t export_unsized{}; // frames of this export that went out as the game made them, their size not known
#if defined(REPLAY_PLUS_TEST_INBOX)
    std::uint64_t inbox_at{}, inbox_wait_until{};
    std::vector<std::string> inbox;
    std::atomic<std::uint64_t> skater_probe_until{};
    std::uint64_t skater_probe_next{};
#endif
};
State& state() {
    static auto* value = new State;
    return *value;
}

void changed(State& s) {
    s.dirty = true;
    s.save_at = GetTickCount64() + save_after_ms;
    ++s.revision;
}
void save_when_due(State& s) {
    if (!s.dirty || GetTickCount64() < s.save_at) return;
    if (write_text(s.file, to_json(s.settings).dump(2) + "\n")) {
        s.dirty = false;
        s.save_failed = false;
        return;
    }
    s.save_at = GetTickCount64() + 5000;
    if (!s.save_failed)
        logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Plus could not save its settings to {}; it tries again.",
                     path_text(s.file));
    s.save_failed = true;
}
void say(State& s, std::string text) {
    s.reply = std::move(text);
    ++s.reply_serial;
    ++s.revision;
}
void say_toast(State& s, std::string text, bool log); // (below; its default comes with its declaration further on)

// ---- The game's camera keyframes, and where the replay has the skater ----------------------------
// The keyframe manager (replay_keyframes.h manager) keeps the replay editor's sessions (an EASTL map by id), each with
// its framing keys (a map by time). Every frame it also keeps, for the replay's camera, where the replay has the
// skater: the body, the upper body and the board, vec4s from +0x90 (read 2026-11-18 on build 25414733: the editor's
// Target: Body and the keyframes it makes take theirs from there). Not the skater standing where play stopped: the
// replay draws one of its own.
namespace kf = game::build::v20260929::replay_keyframes;
constexpr std::uintptr_t manager_skater_body = 0x90;
constexpr std::size_t session_framing_keys = 0x60; // a session's node: the anchor of its framing keys' map
constexpr std::size_t map_size_after_anchor = 0x20;
// The game's own add of a framing key (Skate.exe 0x6065b0): at a time on the replay's clock (a double; snapped to its frames and
// kept within the replay), with a camera (0 orbit, 1 free), a target (0 the body) and the editor's numbers, filled in
// from the replay as it is now; false when the session has a key at that time already. It makes the key's node (0x150
// bytes, the value at +0x30) and puts it in the map. Then the session's segments are rebuilt (kf::session_rebuild).
using AddFramingKey = bool (*)(void* manager, int session, double time, int camera, int target, float fov, float tilt, float roll,
                               float x_offset, float y_offset, float speed);
using RebuildSession = void (*)(void* manager, int session);
constexpr game::build::Fingerprint add_framing_key{0x6065b0, {
    0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x89,0x54,0x24,0x10,0x57,0x48,
    0x83,0xec,0x70,0x48,0x8b,0x41,0x20,0x48,0x8b,0xf9,0x48,0x83,0xc1,0x10,0x41,0x8b}};
// The value's filler it calls (0x6087f0), which takes the target from the manager's +0x90, +0xa0 or +0xb0.
constexpr game::build::Fingerprint fill_framing_key{0x6087f0, {
    0xc5,0xfa,0x11,0x5c,0x24,0x20,0x48,0x81,0xec,0x18,0x01,0x00,0x00,0x4c,0x8b,0xd1,
    0x45,0x8b,0xc8,0x45,0x85,0xc0,0x74,0x2d,0x41,0x83,0xe9,0x01,0x74,0x1d,0x41,0x83}};
// Where the add puts a key asked for at a time (Skate.exe 0x624d20, which the add calls): snapped to the replay's frames
// and kept within the part of the replay the editor shows (its trim). Right after the editor opens, that part is
// still empty: every time lands on its first frame.
using KeyTime = double (*)(void* manager, double time);
constexpr game::build::Fingerprint key_time{0x624d20, {
    0x40,0x53,0x48,0x83,0xec,0x60,0xc5,0xf8,0x28,0xd1,0xc5,0xfb,0x10,0x89,0xd0,0x00,
    0x00,0x00,0xc5,0xf3,0x59,0x05,0x46,0xdf,0xa4,0x05,0xc5,0xf8,0x29,0x74,0x24,0x50}};
// In a key's value: the time it keeps of itself (double), its camera (int), field of view and playback speed.
constexpr std::size_t value_time = 0xf8, value_camera = 0x108, value_fov = 0x104, value_speed = 0x114;

std::uintptr_t peek_pointer(std::uintptr_t address) {
    std::uintptr_t value{};
    return memory::peek(address, value) ? value : 0;
}
// The next node of an EASTL map (right at +0, left at +8, parent at +16), as the game steps through one.
std::uintptr_t map_next(std::uintptr_t node) {
    if (const auto right = peek_pointer(node)) {
        node = right;
        for (int guard = 0; guard < 64; ++guard) {
            const auto left = peek_pointer(node + 8);
            if (!left) break;
            node = left;
        }
        return node;
    }
    auto parent = peek_pointer(node + 16);
    for (int guard = 0; parent && node == peek_pointer(parent) && guard < 64; ++guard) {
        node = parent;
        parent = peek_pointer(parent + 16);
    }
    if (peek_pointer(node) != parent) node = parent;
    return node;
}
bool code_matches(std::uintptr_t base, const game::build::Fingerprint& f) noexcept {
    std::array<unsigned char, 32> bytes{};
    return memory::peek_bytes(base + f.rva, bytes.data(), bytes.size()) && bytes == f.bytes;
}
bool keyframes_known(State& s) {
    if (!s.keyframes_checked) {
        s.keyframes_checked = true;
        // (The rebuild is ReSkate's own keyframe smoothing's to hook, which checked it first: its first bytes are then a jump.)
        s.keyframes_ok = code_matches(s.base, add_framing_key) && code_matches(s.base, fill_framing_key) && code_matches(s.base, key_time) &&
                         (replay_keyframes::available() || code_matches(s.base, kf::session_rebuild));
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: {}.",
                     s.keyframes_ok ? "the game's camera keyframes can be kept between visits to the replay editor, and the skater found for the autofocus"
                                    : "the game's keyframe functions are not the ones known on this game build: Keep keys keeps only Replay Plus's own "
                                      "keys, and the skater's autofocus focuses on the middle instead");
    }
    return s.keyframes_ok;
}
std::uintptr_t keyframe_manager(State& s) { return keyframes_known(s) ? peek_pointer(s.base + kf::manager) : 0; }
// Every framing key of every session, as the manager has them now. False when it has no session (the editor is not
// up, or closing) or the walk went wrong.
bool read_camera_keys(std::uintptr_t manager, std::vector<CameraKey>& out) {
    out.clear();
    const auto anchor = manager + kf::manager_sessions;
    std::uint64_t sessions{};
    if (!memory::peek(anchor + map_size_after_anchor, sessions) || !sessions || sessions > 64) return false;
    int n = 0;
    for (auto node = peek_pointer(anchor + 8); node && node != anchor && n < 64; node = map_next(node), ++n) {
        CameraKey key;
        if (!memory::peek(node + 0x20, key.session)) return false;
        const auto keys = node + session_framing_keys;
        int k = 0;
        for (auto at = peek_pointer(keys + 8); at && at != keys && k < 512; at = map_next(at), ++k) {
            if (!memory::peek(at + kf::framing_key_time, key.time) || !std::isfinite(key.time) ||
                !memory::peek_bytes(at + kf::framing_key_value, key.value.data(), key.value.size()))
                return false;
            out.push_back(key);
        }
        if (k >= 512) return false;
    }
    return n < 64;
}
// The node of a session's framing key nearest a time (within a frame or so), or 0.
std::uintptr_t camera_key_node(std::uintptr_t manager, std::int32_t session, double time) {
    const auto anchor = manager + kf::manager_sessions;
    int n = 0;
    for (auto node = peek_pointer(anchor + 8); node && node != anchor && n < 64; node = map_next(node), ++n) {
        std::int32_t id{};
        if (!memory::peek(node + 0x20, id) || id != session) continue;
        const auto keys = node + session_framing_keys;
        std::uintptr_t best{};
        double gap = 1.0 / 40.0;
        int k = 0;
        for (auto at = peek_pointer(keys + 8); at && at != keys && k < 512; at = map_next(at), ++k) {
            double at_time{};
            if (memory::peek(at + kf::framing_key_time, at_time) && std::abs(at_time - time) < gap) {
                gap = std::abs(at_time - time);
                best = at;
            }
        }
        return best;
    }
    return 0;
}
// The replay's clock from its first frame to its last: the footage the replay editor has now (the replay activity's
// client, its replay at +0x568: +0x70 and +0x78, as the game's add of a key keeps a key's time within them).
bool replay_span(State& s, double& from, double& to) {
    const auto client = peek_pointer(s.base + addr::replay_activity::client);
    const auto replay = client ? peek_pointer(client + addr::replay_activity::manager) : 0;
    return replay && memory::peek(replay + 0x70, from) && memory::peek(replay + 0x78, to) && std::isfinite(from) && std::isfinite(to) && to > from;
}
// The replay's chunks (its replay at +0xa0..+0xa8: 0x18 bytes each, the clock from and to as doubles, then the
// timeline's share from and to as floats), as the game maps the timeline to the clock (Skate.exe 0x610090).
void read_chunks(State& s) {
    std::vector<link::Chunk> chunks;
    const auto client = peek_pointer(s.base + addr::replay_activity::client);
    const auto replay = client ? peek_pointer(client + addr::replay_activity::manager) : 0;
    const auto first = replay ? peek_pointer(replay + 0xa0) : 0, last = replay ? peek_pointer(replay + 0xa8) : 0;
    if (first && last > first && (last - first) % 0x18 == 0 && (last - first) / 0x18 <= 256) {
        for (auto at = first; at < last; at += 0x18) {
            link::Chunk c;
            if (!memory::peek(at, c.clock_from) || !memory::peek(at + 8, c.clock_to) || !memory::peek(at + 0x10, c.from) ||
                !memory::peek(at + 0x14, c.to) || !std::isfinite(c.clock_from) || !std::isfinite(c.clock_to) || !std::isfinite(c.from) ||
                !std::isfinite(c.to) || c.clock_to < c.clock_from || c.to < c.from) {
                chunks.clear();
                break;
            }
            chunks.push_back(c);
        }
    }
    if (chunks != s.chunks) {
        s.chunks = std::move(chunks);
        ++s.revision;
    }
}
// Where a time on the replay's clock is on the editor's timeline, in seconds from its start (as the editor shows it).
double timeline_seconds(const State& s, double clock) {
    if (!s.timeline_ok) return clock;
    const float share = link::timeline_share(s.chunks, s.timeline_start, s.timeline_length, clock, s.bar_from, s.bar_to);
    return share >= 0 ? share * s.timeline_length : clock - s.timeline_start;
}
// The rows of a frame in a buffer of its own: a private allocation that starts at the frame (the game's readback for the
// encoder), its size the frame's rows (rounded up to a page); else 0.
std::uint32_t own_rows(std::uintptr_t pixels, std::uintptr_t pitch) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (!pixels || pitch < 256 || VirtualQuery(reinterpret_cast<const void*>(pixels), &info, sizeof info) != sizeof info) return 0;
    if (reinterpret_cast<std::uintptr_t>(info.AllocationBase) != pixels || info.State != MEM_COMMIT || info.Type != MEM_PRIVATE) return 0;
    const std::uintptr_t bytes = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize - pixels;
    const std::uintptr_t rows = bytes / pitch;
    return rows <= 8192 ? static_cast<std::uint32_t>(rows) : 0u;
}
// How many rows (of `row_bytes`, `pitch` apart) the memory from `pixels` on holds, up to `wanted`: committed, writable
// and unguarded, region by region (VirtualQuery). An exported frame is never read or written past its buffer.
std::uint32_t rows_held(std::uintptr_t pixels, std::uintptr_t pitch, std::uint32_t row_bytes, std::uint32_t wanted) noexcept {
    if (!pixels || !pitch || !row_bytes || !wanted) return 0;
    const std::uintptr_t need = pixels + (static_cast<std::uintptr_t>(wanted) - 1) * pitch + row_bytes;
    constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    std::uintptr_t at = pixels;
    while (at < need) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof info) != sizeof info) break;
        if (info.State != MEM_COMMIT || !(info.Protect & writable) || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) break;
        const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (end <= at) break;
        at = end;
    }
    if (at >= need) return wanted;
    if (at < pixels + row_bytes) return 0;
    return static_cast<std::uint32_t>((at - pixels - row_bytes) / pitch + 1);
}
bool poke_bytes(std::uintptr_t address, const void* source, std::size_t size) noexcept {
    if (!source || !size || address < 0x10000 || address > memory::highest_user_address - size) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(address), source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_add(std::uintptr_t function, std::uintptr_t manager, std::int32_t session, double time, std::int32_t camera, float fov, float speed) noexcept {
    __try {
        (void)reinterpret_cast<AddFramingKey>(function)(reinterpret_cast<void*>(manager), session, time, camera, 0, fov, 0, 0, 0, 0, speed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_key_time(std::uintptr_t function, std::uintptr_t manager, double time, double& out) noexcept {
    __try {
        out = reinterpret_cast<KeyTime>(function)(reinterpret_cast<void*>(manager), time);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_rebuild(std::uintptr_t function, std::uintptr_t manager, std::int32_t session) noexcept {
    __try {
        reinterpret_cast<RebuildSession>(function)(reinterpret_cast<void*>(manager), session);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Keep keys: the camera keyframes of the last visit put back where they were (game thread, in the replay editor,
// its sessions still without keys). Each is made by the game's own add (its node, its place in the map), then given
// its kept value byte for byte: the camera, the target as it was, the editor's settings. Only those on footage the
// replay still has (its clock keeps a moment's time while that moment is in the replay), within what the editor
// shows. False while the editor's range is not up yet (try again in a moment).
bool put_back_camera_keys(State& s, std::uintptr_t manager) {
    double from{}, to{}, first{}, last{};
    const auto at = s.base + key_time.rva;
    if (!replay_span(s, from, to) || !call_key_time(at, manager, -1.0e7, first) || !call_key_time(at, manager, 1.0e7, last) ||
        !std::isfinite(first) || !std::isfinite(last) || last - first < 0.25)
        return false;
    from = std::max(from, first);
    to = std::min(to, last);
    const auto add = s.base + add_framing_key.rva, rebuild = s.base + kf::session_rebuild.rva;
    std::set<std::int32_t> sessions;
    std::size_t put{}, gone{}, failed{};
    for (const auto& key : s.camera_keys) {
        double lands{};
        if (key.time < from - 1e-3 || key.time > to + 1e-3 || !call_key_time(at, manager, key.time, lands) ||
            std::abs(lands - key.time) > 1.0 / 40.0) {
            ++gone;
            continue;
        }
        std::int32_t camera{};
        float fov{}, speed{};
        std::memcpy(&camera, key.value.data() + value_camera, sizeof(camera));
        std::memcpy(&fov, key.value.data() + value_fov, sizeof(fov));
        std::memcpy(&speed, key.value.data() + value_speed, sizeof(speed));
        if (camera < 0 || camera > 16) camera = 0;
        if (!std::isfinite(fov) || fov < 1 || fov > 179) fov = 65;
        if (!std::isfinite(speed) || speed <= 0 || speed > 100) speed = 1;
        const auto node = call_add(add, manager, key.session, key.time, camera, fov, speed)
                              ? camera_key_node(manager, key.session, key.time)
                              : 0;
        double node_time{};
        auto value = key.value;
        if (!node || !memory::peek(node + kf::framing_key_time, node_time)) {
            ++failed;
            continue;
        }
        std::memcpy(value.data() + value_time, &node_time, sizeof(node_time));
        if (!poke_bytes(node + kf::framing_key_value, value.data(), value.size())) {
            ++failed;
            continue;
        }
        sessions.insert(key.session);
        ++put;
    }
    for (const auto session : sessions) (void)call_rebuild(rebuild, manager, session);
    logging::log(logging::Level::info, logging::Channel::graphics,
                 "Replay Plus: Keep keys put back {} camera keyframe(s) of {} ({} on footage no longer in the replay or outside its trim, {} not made); the "
                 "editor shows {:.3f} to {:.3f}.",
                 put, s.camera_keys.size(), gone, failed, from, to);
    if (put)
        say_toast(s, std::format("Put back {} camera keyframe{} from last time.", put, put == 1 ? "" : "s"), false);
    else if (gone)
        say_toast(s, "Last time's camera keyframes were on footage that is no longer in the replay.", false);
    return true;
}
// Keep keys, every tick: in the replay editor, the camera keyframes of the last visit put back once its session is up
// and still empty, then followed as they change (kept when the editor closes: its sessions go with it).
void keep_camera_keys(State& s) {
    if (!s.in_replay || !s.settings.keep_keys) return;
    const auto manager = keyframe_manager(s);
    if (!manager) return;
    const auto now = GetTickCount64();
    std::vector<CameraKey> keys;
    const bool up = read_camera_keys(manager, keys);
    if (!s.camera_keys_done) {
        // Once the editor has its session and its range (Advanced Edit sets it up: the keys come back as it opens),
        // into a session with no keys of its own; tried a few times a second meanwhile.
        if (!up || !s.timeline_ok || now < s.camera_keys_from) return;
        s.camera_keys_from = now + 250;
        if (!keys.empty() || s.camera_keys.empty() || put_back_camera_keys(s, manager)) s.camera_keys_done = true;
        return;
    }
    if (!up) return; // closing: what was last seen is kept
    if (!keys.empty()) {
        s.camera_keys = std::move(keys);
        s.camera_keys_gone_at = 0;
    } else if (!s.camera_keys.empty()) {
        // All gone with the editor still up (Delete All Keyframes): gone for good once it stays so a while.
        if (!s.camera_keys_gone_at) s.camera_keys_gone_at = now;
        else if (now - s.camera_keys_gone_at > 1500) {
            s.camera_keys.clear();
            s.camera_keys_gone_at = 0;
        }
    }
}
// How far in front of the picture's camera the replay has the skater's body (metres along its view); false when not
// known (an unknown game build, no camera seen lately, a place that makes no sense).
bool skater_depth(State& s, float& depth) {
    const auto manager = keyframe_manager(s);
    std::array<float, 4> body{};
    if (!manager || !memory::peek(manager + manager_skater_body, body)) return false;
    const auto seen = s.camera_at.load(std::memory_order_acquire);
    if (!seen || GetTickCount64() - seen > 1000) return false;
    std::array<float, 16> c{};
    for (std::size_t i = 0; i < c.size(); ++i) c[i] = s.camera[i].load(std::memory_order_relaxed);
    const float dx = body[0] - c[12], dy = body[1] - c[13], dz = body[2] - c[14];
    const float apart = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(apart) || apart > 500 || (body[0] == 0 && body[1] == 0 && body[2] == 0)) return false;
    depth = -(dx * c[8] + dy * c[9] + dz * c[10]);
    return std::isfinite(depth) && depth > 0.1f;
}

// ---- Exported videos to a folder of the player's own -------------------------------------------
// The game writes its videos (MP4) to its own Videos folder, under the local app data it is given (ReSkate's, else
// Windows'). Once an export is done and the game has let the file go, it is moved to the folder the player chose (on a
// thread of its own: to another drive it is a copy).
std::vector<fs::path> game_video_folders() {
    std::vector<fs::path> folders;
    const auto local = local_app_data();
    if (local.empty()) return folders;
    folders.push_back(local / L"ReSkate" / L"Game" / L"Skate" / L"data" / L"Videos");
    folders.push_back(local / L"Skate" / L"data" / L"Videos");
    return folders;
}
fs::path game_video_folder() {
    std::error_code error;
    for (const auto& folder : game_video_folders())
        if (fs::is_directory(folder, error)) return folder;
    const auto folders = game_video_folders();
    return folders.empty() ? fs::path() : folders.front();
}
fs::path video_folder(const Settings& v) { return v.video_folder.empty() ? game_video_folder() : utf8_path(v.video_folder); }
// A name in the folder not taken yet: "name.mp4", else "name (2).mp4", ...
fs::path free_name(const fs::path& folder, const fs::path& name) {
    std::error_code error;
    auto target = folder / name;
    for (int n = 2; fs::exists(target, error) && n < 1000; ++n)
        target = folder / (name.stem().wstring() + std::format(L" ({})", n) + name.extension().wstring());
    return target;
}
void move_videos(State& s, fs::path to, fs::file_time_type since) {
    std::thread([&s, to = std::move(to), since] {
      // (Nothing may leave a thread of its own: the game would close.)
      try {
        std::set<fs::path> moved;
        const auto deadline = GetTickCount64() + 120000;
        std::error_code error;
        fs::create_directories(to, error);
        while (GetTickCount64() < deadline) {
            bool waiting = false;
            for (const auto& folder : game_video_folders()) {
                if (!fs::is_directory(folder, error)) continue;
                std::error_code walk;
                for (auto entry = fs::directory_iterator(folder, walk); !walk && entry != fs::directory_iterator(); entry.increment(walk)) {
                    const auto& item = *entry;
                    if (!item.is_regular_file(error)) continue;
                    auto extension = item.path().extension().wstring();
                    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                    if (extension != L".mp4" || moved.contains(item.path()) || fs::last_write_time(item.path(), error) < since) continue;
                    // Still the game's while it cannot be opened alone (it is writing or finishing it).
                    const HANDLE file = CreateFileW(item.path().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file == INVALID_HANDLE_VALUE) {
                        waiting = true;
                        continue;
                    }
                    CloseHandle(file);
                    const auto target = free_name(to, item.path().filename());
                    moved.insert(item.path());
                    const bool ok = MoveFileExW(item.path().c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) != FALSE;
                    const auto why = ok ? DWORD{} : GetLastError();
                    try {
                        std::lock_guard lock(s.mutex);
                        if (ok) {
                            logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: the video {} went to {}.", path_text(item.path()),
                                         path_text(target));
                            say_toast(s, std::format("Video saved to {}.", path_text(to)), false);
                        } else {
                            logging::log(logging::Level::warning, logging::Channel::graphics,
                                         "Replay Plus: the video {} could not be moved to {} (error {}): it stays where the game put it.",
                                         path_text(item.path()), path_text(target), why);
                            say_toast(s, std::format("The video could not be moved to {} (error {}): it is in the game's Videos folder.",
                                                     path_text(to), why), false);
                        }
                    } catch (...) {}
                }
            }
            error.clear();
            if (!waiting && !moved.empty()) return;
            Sleep(500);
        }
      } catch (...) {}
    }).detach();
}
// Every tick: an export ending sends its video to the player's folder, when one is set.
void follow_exports(State& s) {
    const bool exporting = replay_shake::exporting();
    if (exporting && !s.exporting_seen) s.export_began = fs::file_time_type::clock::now() - std::chrono::seconds(5);
    if (!exporting && s.exporting_seen && !s.settings.video_folder.empty()) move_videos(s, utf8_path(s.settings.video_folder), s.export_began);
    s.exporting_seen = exporting;
}
// The newest video written since `since` in the game's Videos folders or the player's own (where follow_exports moves
// it): its name and size, for the log; empty when there is none.
std::string newest_video(const State& s, fs::file_time_type since) {
    std::error_code error;
    auto folders = game_video_folders();
    if (!s.settings.video_folder.empty()) folders.push_back(utf8_path(s.settings.video_folder));
    fs::path best;
    fs::file_time_type best_at{};
    std::uintmax_t bytes{};
    for (const auto& folder : folders) {
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_regular_file(error)) continue;
            const auto at = it->last_write_time(error);
            if (error || at < since || (!best.empty() && at <= best_at)) continue;
            best = it->path();
            best_at = at;
            bytes = it->file_size(error);
        }
        error.clear();
    }
    return best.empty() ? std::string() : std::format("{} ({:.1f} MB)", path_text(best), static_cast<double>(bytes) / 1048576.0);
}
// Every tick: an export's steps said in the log as they come, and where one stops (a player's exports never got going,
// three times, and the log said nothing of where, 2026-11-20). The game filming a video (its video frames drawn, each
// frame: link::video_frames_seen), its encoder taking the first frame and how soon, how fast it goes, and the export
// done (its frames, its seconds, the video written); or where it stopped: filming 4 s with no frame encoded, or
// filming over before any was. (Ten video frames drawn first: a photo copies one or two the same way.)
void watch_exports(State& s) {
    auto& w = s.watch;
    const auto now = link::now_ms();
    std::uint64_t drawn = 0;
    std::uint32_t width = 0, height = 0;
    for (const auto& v : link::video_frames_seen())
        if (v.times >= (w.filming ? 1u : 10u) && v.last_ms > drawn) drawn = v.last_ms, width = v.width, height = v.height;
    const bool drawing = drawn && now >= drawn && now - drawn < 1000;
    const auto running = replay_shake::export_frames::capture();
    if (!w.filming) {
        if (!drawing) return;
        w = {};
        w.filming = true;
        w.since = now;
        w.last_drawn = drawn;
        w.session = running.session;
        w.frames = running.frames;
        w.width = width;
        w.height = height;
        w.began = fs::file_time_type::clock::now() - std::chrono::seconds(2);
#if defined(REPLAY_PLUS_TEST_INBOX)
        // (Test builds: the camera keys' plays in an export's first seconds, to the log.)
        replay_shake::export_frames::trace_played(8000);
#endif
        // An MP4's H.264 setup runs just before its first frame is drawn; a WebM has none.
        const auto setup_at = replay_shake::export_frames::configured_ms();
        const auto setup = replay_shake::export_frames::size();
        const bool mp4 = setup_at && GetTickCount64() - setup_at < 10000;
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus export: the game is filming a video, its frames {}x{}{}.", width,
                     height, mp4 ? std::format(" (an MP4: its H.264 setup ran for {}x{} at {:.0f} fps)", setup.width, setup.height, setup.fps)
                                 : std::string(" (no H.264 setup lately: a WebM)"));
        return;
    }
    if (drawing) w.last_drawn = drawn;
    // Its encoder took this export's first frame: a new session, or (one right after another export) more frames.
    const bool encoding = running.session != w.session || (running.running && running.frames > w.frames);
    if (encoding && !w.encoded_at) {
        w.encoded_at = w.progress_at = now;
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus export: its encoder is taking the frames ({} ms after the filming was seen).",
                     now - w.since);
    }
    // (Not once its encoder has taken frames: when it finishes, `encoding` goes false while the game still shows the
    // last frames, and this said "stuck" after every export, 2026-11-20.)
    if (!w.encoded_at && !encoding && !w.said_stuck && now - w.since > 4000) {
        w.said_stuck = true;
        logging::log(logging::Level::warning, logging::Channel::graphics,
                     "Replay Plus export: the game has been filming for 4 s ({}x{} frames) and its encoder has taken no frame yet: the export is stuck "
                     "before it encodes.",
                     w.width, w.height);
    }
    if (encoding && running.running && now - w.progress_at >= 10000) {
        w.progress_at = now;
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus export: {} frames encoded so far ({:.1f} a second).", running.frames,
                     static_cast<double>(running.frames) * 1000.0 / static_cast<double>(std::max<std::uint64_t>(now - w.encoded_at, 1)));
    }
    if (drawing || now - w.last_drawn <= 3000 || running.running) return;
    // Filming is over.
    if (w.encoded_at) {
        const double seconds = static_cast<double>(w.last_drawn - w.encoded_at) / 1000.0;
        const auto video = newest_video(s, w.began);
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus export: done: {} frames encoded in {:.1f} s ({:.1f} a second); {}.",
                     running.frames, seconds, seconds > 0 ? static_cast<double>(running.frames) / seconds : 0.0,
                     video.empty() ? "no new video found in the Videos folder" : "the video: " + video);
    } else {
        logging::log(logging::Level::warning, logging::Channel::graphics,
                     "Replay Plus export: the game stopped filming after {:.1f} s and its encoder never took a frame: no video was made.",
                     static_cast<double>(w.last_drawn - w.since) / 1000.0);
    }
    w = {};
}
// The replay editor's title is Replay Plus's: the game's "INSTANT REPLAY" (its text ID_REPLAY_INSTANTREPLAY, found in a
// dump of the editor's UI, 2026-11-20), through ReSkate's text overrides as a plugin's text goes in, once. (Left so
// while Replay Plus is switched off too: the store has no removal, and putting the game's words back would mean
// English ones over every other language's.)
void keep_title(State& s) {
    if (s.title_set) return;
    s.title_set = true;
    if (const auto why = text_overrides::set("Replay Plus", {{"ID_REPLAY_INSTANTREPLAY", "REPLAY PLUS"}}); !why.empty())
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: the replay editor's title stays the game's: {}.", why);
}
// The Windows folder picker (its own thread, with its own message loop); the folder chosen comes back to the game
// thread as a console line. The game's window is given the front again after.
void pick_video_folder(State& s) {
    if (s.picking_folder.exchange(true)) return;
    const auto start_in = video_folder(s.settings);
    std::thread([&s, start_in] {
        std::string chosen;
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
            {
                Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
                if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                    DWORD options{};
                    (void)dialog->GetOptions(&options);
                    (void)dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
                    (void)dialog->SetTitle(L"Replay Plus: where your videos go");
                    (void)dialog->SetOkButtonLabel(L"Use this folder");
                    Microsoft::WRL::ComPtr<IShellItem> folder;
                    std::error_code error;
                    if (!start_in.empty() && fs::is_directory(start_in, error) &&
                        SUCCEEDED(SHCreateItemFromParsingName(start_in.c_str(), nullptr, IID_PPV_ARGS(&folder))))
                        (void)dialog->SetFolder(folder.Get());
                    if (SUCCEEDED(dialog->Show(nullptr))) {
                        Microsoft::WRL::ComPtr<IShellItem> item;
                        PWSTR path{};
                        if (SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                            chosen = path_text(fs::path(path));
                            CoTaskMemFree(path);
                        }
                    }
                }
            }
            CoUninitialize();
        }
        if (const auto window = reinterpret_cast<HWND>(link::game_window())) SetForegroundWindow(window);
        if (!chosen.empty()) link::post("replayplus videofolder " + chosen);
        s.picking_folder.store(false);
    }).detach();
}
std::string video_folder_verb(State& s, const std::vector<std::string>& words) {
    std::string rest;
    for (std::size_t i = 0; i < words.size(); ++i) rest += (i ? " " : "") + words[i];
    const auto what = lower(rest);
    if (what.empty()) {
        return s.settings.video_folder.empty() ? std::format("Videos stay where the game puts them: {}.", path_text(game_video_folder()))
                                               : std::format("Videos go to {} once each is exported.", s.settings.video_folder);
    }
    if (what == "pick" || what == "choose") {
        pick_video_folder(s);
        return "The folder picker is open (over the game).";
    }
    if (what == "open") {
        const auto folder = video_folder(s.settings);
        std::error_code error;
        fs::create_directories(folder, error);
        ShellExecuteW(nullptr, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return std::format("{} opens in File Explorer.", path_text(folder));
    }
    if (what == "default" || what == "game" || what == "reset") {
        if (s.settings.video_folder.empty()) return "Videos already stay where the game puts them.";
        s.settings.video_folder.clear();
        changed(s);
        return std::format("Videos stay where the game puts them again: {}.", path_text(game_video_folder()));
    }
    auto folder = utf8_path(rest);
    std::error_code error;
    if (folder.is_relative()) return "That is not a whole path (it needs a drive, like D:\\Videos).";
    fs::create_directories(folder, error);
    if (!fs::is_directory(folder, error)) return std::format("{} is not a folder that can be made or used.", rest);
    folder = fs::weakly_canonical(folder, error);
    s.settings.video_folder = path_text(folder);
    changed(s);
    return std::format("Videos go to {} from now on (each once it is exported).", s.settings.video_folder);
}

// ---- Folders, presets and overlays -------------------------------------------------------
std::vector<std::string> files_in(const fs::path& folder, std::initializer_list<std::wstring_view> extensions) {
    std::vector<std::string> names;
    std::error_code error;
    if (!fs::is_directory(folder, error)) return names;
    for (const auto& item : fs::directory_iterator(folder, error)) {
        if (!item.is_regular_file(error)) continue;
        auto extension = item.path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) continue;
        names.push_back(path_text(item.path().filename()));
        if (names.size() >= 200) break;
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return names;
}
void say_toast(State& s, std::string text, bool log = true);
std::vector<std::string> preset_names_in(const fs::path& folder) {
    std::vector<std::string> names;
    for (auto name : files_in(folder, {L".json"})) {
        name.resize(name.size() - 5);
        names.push_back(std::move(name));
    }
    return names;
}
// Preset files put in the import folder: each that reads as a preset is added to the presets (with a number
// after its name should there be one of that name), and the file itself moved into Import\Imported; one that
// does not read, into Import\Not presets. Files changed in the last two seconds wait (still being written).
void import_presets(State& s) {
    std::error_code error;
    if (!fs::is_directory(s.imports, error)) return;
    std::vector<fs::path> files;
    for (const auto& item : fs::directory_iterator(s.imports, error)) {
        if (!item.is_regular_file(error) || lower(path_text(item.path().extension())) != ".json") continue;
        const auto at = item.last_write_time(error);
        if (error || fs::file_time_type::clock::now() - at < std::chrono::seconds(2)) continue;
        if (s.import_tried.contains(path_text(item.path()) + std::format("|{}", at.time_since_epoch().count()))) continue;
        files.push_back(item.path());
        if (files.size() >= 50) break;
    }
    if (files.empty()) return;
    auto names = preset_names_in(s.presets);
    // Out of the way, under a number should a file of that name be there already.
    const auto park = [&](const fs::path& file, const wchar_t* folder) {
        std::error_code e;
        const auto into = s.imports / folder;
        fs::create_directories(into, e);
        auto to = into / file.filename();
        for (int n = 2; fs::exists(to, e) && n < 1000; ++n) to = into / (file.stem().wstring() + std::format(L" {}", n) + file.extension().wstring());
        fs::rename(file, to, e);
        if (e) {
            const auto at = fs::last_write_time(file, e);
            s.import_tried.insert(path_text(file) + std::format("|{}", at.time_since_epoch().count()));
        }
    };
    for (const auto& file : files) {
        Preset preset;
        std::string why;
        bool read = false;
        try {
            read = preset_from_json(Json::parse(read_text(file, 256 * 1024)), preset, why);
        } catch (const std::exception& e) {
            why = e.what();
        }
        if (!read) {
            park(file, L"Not presets");
            logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: {} does not read as a preset. {}", path_text(file), why);
            say_toast(s, std::format("{} is not a Replay Plus preset: moved to Import\\Not presets.", path_text(file.filename())), false);
            continue;
        }
        auto name = clean_preset_name(preset.name);
        if (name.empty()) name = clean_preset_name(path_text(file.stem()));
        name = unused_preset_name(name, names);
        preset.name = name;
        const auto to = s.presets / utf8_path(preset_file_name(name));
        if (!write_text(to, preset_json(preset).dump(2) + "\n")) {
            logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Plus: could not add {} to the presets ({}).", path_text(file), path_text(to));
            continue;
        }
        names.push_back(name);
        park(file, L"Imported");
        say_toast(s, std::format("Preset {} imported.", name));
    }
}
void refresh_lists(State& s, bool now) {
    if (!now && GetTickCount64() - s.lists_at < 3000) return;
    s.lists_at = GetTickCount64();
    import_presets(s);
    std::vector<std::string> presets;
    for (auto name : files_in(s.presets, {L".json"})) {
        name.resize(name.size() - 5);
        presets.push_back(std::move(name));
    }
    auto hoods = files_in(s.overlays, {L".png", L".jpg", L".jpeg", L".bmp", L".webp"});
    if (presets != s.preset_names || hoods != s.hood_files) {
        s.preset_names = std::move(presets);
        s.hood_files = std::move(hoods);
        ++s.revision;
    }
}
std::optional<Preset> read_preset(State& s, std::string_view name, std::string& why) {
    const auto file = s.presets / utf8_path(preset_file_name(name));
    std::error_code error;
    if (!fs::exists(file, error)) {
        why = std::format("there is no preset called {}", name);
        return std::nullopt;
    }
    try {
        Preset preset;
        if (!preset_from_json(Json::parse(read_text(file, 256 * 1024)), preset, why)) return std::nullopt;
        return preset;
    } catch (const std::exception& e) {
        why = e.what();
        return std::nullopt;
    }
}
// The first time: the folders and the shipped presets.
void set_up_folders(State& s) {
    std::error_code error;
    fs::create_directories(s.presets, error);
    fs::create_directories(s.overlays, error);
    fs::create_directories(s.imports, error);
    fs::create_directories(s.exports, error);
    // The built-in presets: written once (the marker says which version), and when this version's are
    // newer: then each of the older ones that is still as Replay Plus wrote it (no newer than the
    // marker) is written over; one deleted stays deleted, one saved over stays as it is.
    const auto marker = s.folder / L"presets made.txt";
    int made = 0;
    if (fs::exists(marker, error)) {
        made = 1;
        try {
            const auto text = read_text(marker, 4096);
            if (const auto at = text.find("version "); at != std::string::npos) made = std::max(1, std::atoi(text.c_str() + at + 8));
        } catch (...) {}
    }
    if (made < built_in_presets_version) {
        const auto older = older_built_in_names();
        const auto written = made ? fs::last_write_time(marker, error) : fs::file_time_type{};
        // Built-ins this version no longer has, still as Replay Plus wrote them: moved into "Older built-ins"
        // (kept, out of the list). One saved over is the player's own and stays.
        if (made)
            for (const auto& name : retired_built_in_names()) {
                std::error_code e;
                const auto file = s.presets / utf8_path(preset_file_name(name));
                if (!fs::exists(file, e) || e) continue;
                const auto at = fs::last_write_time(file, e);
                if (e || at > written + std::chrono::seconds(10)) continue;
                const auto folder = s.presets / L"Older built-ins";
                fs::create_directories(folder, e);
                const auto to = folder / file.filename();
                if (fs::exists(to, e)) continue;
                fs::rename(file, to, e);
                if (!e)
                    logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: the built-in preset {} is retired: kept in {}.", name,
                                 path_text(folder));
            }
        for (const auto& preset : built_in_presets()) {
            const auto file = s.presets / utf8_path(preset_file_name(preset.name));
            const bool there = fs::exists(file, error);
            bool write = !there && (made == 0 || std::find(older.begin(), older.end(), preset.name) == older.end());
            if (there && made > 0) {
                const auto at = fs::last_write_time(file, error);
                write = !error && at <= written + std::chrono::seconds(10);
            }
            if (write) {
                write_text(file, preset_json(preset).dump(2) + "\n");
                if (made) logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: the built-in preset {} is the new one.", preset.name);
            }
        }
        write_text(marker, std::format("Replay Plus wrote its own presets here (version {}). Delete a preset and it stays deleted.\n",
                                       built_in_presets_version));
    }
}

// ---- The hood's picture (decoded on a worker thread) ---------------------------------------
std::string hood_path_for(const State& s) {
    if (!s.settings.enabled) return {};
    if (s.settings.hood == Hood::picture && !s.settings.hood_file.empty()) return path_text(s.overlays / utf8_path(s.settings.hood_file));
    return {};
}
std::shared_ptr<link::HoodImage> decode_picture(const std::string& path) {
    auto image = std::make_shared<link::HoodImage>();
    image->path = path;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        UINT width{}, height{};
        const auto wide = utf8_path(path).wstring();
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width && height && width <= 8192 &&
            height <= 8192 && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            image->rgba.resize(std::size_t{width} * height * 4);
            if (SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image->rgba.size()), image->rgba.data()))) {
                image->width = width;
                image->height = height;
                // Its see-through middle, which the lens is fitted to.
                image->hole = lens::measure_hole(image->rgba.data(), width, height);
            } else {
                image->rgba.clear();
            }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return image;
}
void keep_hood(State& s) {
    const auto wanted = hood_path_for(s);
    if (wanted == s.hood_wanted || s.hood_busy.load(std::memory_order_acquire)) return;
    s.hood_wanted = wanted;
    const auto serial = ++s.hood_serial;
    if (wanted.empty()) {
        link::publish_hood(nullptr);
        return;
    }
    s.hood_busy.store(true, std::memory_order_release);
    std::thread([wanted, serial] {
        auto image = decode_picture(wanted);
        image->serial = serial;
        if (image->width)
            logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: overlay {} ({}x{}; {}).", wanted, image->width, image->height,
                         image->hole.known ? std::format("see-through to {:.3f} across, {:.3f} along its diagonal, {:.3f} up", image->hole.radius[0],
                                                         image->hole.radius[lens::hole_steps / 2], image->hole.radius[lens::hole_steps])
                                           : std::string("nothing see-through in its middle"));
        else
            logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Plus: the overlay picture {} could not be read.", wanted);
        link::publish_hood(image->width ? std::move(image) : nullptr);
        state().hood_busy.store(false, std::memory_order_release);
    }).detach();
}

// ---- The field of view, and what the fisheye makes of it --------------------------------------
// The picture's aspect (the swap chain's, as the render thread last saw it; 16:9 until then).
float screen_aspect() {
    const auto report = link::render_report();
    return report.width && report.height ? static_cast<float>(report.width) / static_cast<float>(report.height) : 16.0f / 9.0f;
}
// Auto's field of view, worked out again when the settings or the screen change.
void keep_fov_auto(State& s) {
    const float aspect = screen_aspect();
    // (Again too once the overlay's picture is read: the lens is fitted to what it leaves showing.)
    const auto hood = link::hood();
    const std::uint64_t hood_serial = hood ? hood->serial : 0;
    if (s.fov_auto_revision == s.revision && s.fov_auto_aspect == aspect && s.fov_auto_hood == hood_serial) return;
    s.fov_auto_revision = s.revision;
    s.fov_auto_aspect = aspect;
    s.fov_auto_hood = hood_serial;
    const float degrees = lens::auto_fov(s.settings, aspect, s.settings.fov_widest, hood ? &hood->hole : nullptr) * 180.0f / lens::pi;
    if (std::abs(degrees - s.fov_auto) > 0.05f) {
        s.fov_auto = degrees;
        ++s.revision;
        s.fov_auto_revision = s.revision;
    }
}
// The field of view the picture is made with now: the one the game built it with, while it is told; else
// the one Replay Plus sets (fixed, Auto), else the editor's usual.
float fov_now(const State& s) {
    const float told = link::fov();
    if (told > 0.02f && link::now_ms() - link::fov_at() < 1000) return told;
    const auto& v = s.settings;
    return (v.fov_mode == FovMode::fixed ? v.fov : v.fov_mode == FovMode::automatic ? s.fov_auto : 65.0f) * lens::pi / 180.0f;
}
// How much the fisheye enlarges the middle of the game's picture now (1 without one).
float centre_zoom_now(const State& s) {
    if (!lens_on(s.settings)) return 1.0f;
    const auto hood = link::hood();
    const auto frame = lens::frame_for(s.settings, screen_aspect(), fov_now(s), hood ? &hood->hole : nullptr);
    return lens::centre_zoom(frame, lens::fit(frame));
}

// ---- Sharp middle: the replay editor renders finer by the fisheye's enlargement ---------------------
// The scale that gives the middle as much of the game's detail as at 120 degrees (an enlargement of
// sharp_zoom), in quarters, up to sharp_most. Raised at once; lowered only once lower has done for two
// seconds (the editor's own field of view keys move it, and each change has the game remake its targets).
void keep_sharp_scale(State& s) {
    const auto& v = s.settings;
    // (Held while a video is exported: a zoom keyed into the clip would have raised it part way through.)
    if (replay_shake::export_frames::busy()) return;
    float wanted = 0;
    if (v.enabled && s.in_replay && s.timeline_ok && v.sharp_middle && lens_on(v)) {
        const float zoom = centre_zoom_now(s);
        wanted = std::clamp(std::ceil(zoom / sharp_zoom * 4.0f - 0.05f) / 4.0f, 1.0f, sharp_most);
        if (wanted <= 1.0f) wanted = 0;
    }
    const auto now = GetTickCount64();
    if (wanted > s.sharp_scale) {
        s.sharp_scale = wanted;
        s.sharp_lower_since = 0;
        ++s.revision;
    } else if (wanted < s.sharp_scale) {
        if (!s.sharp_lower_since) s.sharp_lower_since = now;
        if (now - s.sharp_lower_since >= 2000 || !s.in_replay) {
            s.sharp_scale = wanted;
            s.sharp_lower_since = 0;
            ++s.revision;
        }
    } else {
        s.sharp_lower_since = 0;
    }
}

// The game's texture streaming pool for the card it renders on (KB; 0: the game's own). The game's is
// 1441040 KB (1.4 GB) on every card. Cards of more than 8 GB get more, 3/8 of what they have past 8 GB, up to
// 2.5 GB (16 GB cards): it is video memory Sharp middle cannot then have. (The game takes it in 32 bits as
// bytes with its headroom, as the mesh pool: well under 4 GB.)
std::uint32_t texture_pool_for_card() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;
    std::uint64_t most = 0;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) most = std::max<std::uint64_t>(most, desc.DedicatedVideoMemory);
        adapter.Reset();
    }
    const std::uint64_t mb = most / (1024 * 1024);
    if (mb <= 8704) return 0;
    const std::uint64_t pool_mb = std::clamp<std::uint64_t>((mb - 8192) * 3 / 8 + 1407, 1408, 2560);
    logging::log(logging::Level::info, logging::Channel::graphics,
                 "Replay Plus: the game's texture pool is {} MB on this card ({} MB of video memory; the game's own 1407 MB), from the next map loaded.",
                 pool_mb, mb);
    return static_cast<std::uint32_t>(pool_mb * 1024);
}

// The render scale Replay Plus wants of the game now, before the video memory guard (0: none): the
// player's own (VIDEO), and in the replay editor at least Sharp middle's.
float scale_wanted(const State& s) {
    const auto& v = s.settings;
    if (!v.enabled) return 0.0f;
    float scale = v.resolution && s.level ? (s.in_replay ? v.replay_scale : v.gameplay_scale) : 0.0f;
    if (s.in_replay && s.sharp_scale > 0) scale = std::max(scale, s.sharp_scale);
    return scale;
}

// ---- Video memory: a render scale above 100% only as far as the card's memory goes -------------------
// Above 100% every render target of the game's is larger: Sharp middle's 200% is three times the pixels
// in 4:3 and four in 16:9. In a big map that took the game past the video memory Windows lets it use (its
// DXGI budget: about 15 GB of a 16 GB card while the game is in front, less behind other windows), and
// Windows moved part of the game's memory out to system memory, where the GPU reads it over PCIe. It
// stayed there after the editor: the frames tanked and stuttered outside until the game was restarted
// (San Andreas ReSkate Edition, 2026-11-17: 3.8 GB moved out, the GPU 95% busy). So in the replay editor
// the scale is raised only once the picture's shape is in place (raised before, the game rendered a moment
// at it on the old, larger screen: 5120x2880 for 4:3's 3840x2880), only as far as keeps the game's use,
// with its larger targets, a margin under its budget, and lowered at once when its use nears the budget
// anyway (the replay's camera moving on to more of the map, the game going behind another window).
std::optional<std::pair<std::uint64_t, std::uint64_t>> video_memory(State::MemoryGuard& m) {
    const auto luid = link::adapter();
    if (!luid) return std::nullopt;
    if (!m.adapter || m.adapter_luid != luid) {
        m.adapter.Reset();
        m.adapter_luid = 0;
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return std::nullopt;
        const LUID id{static_cast<DWORD>(luid & 0xffffffffu), static_cast<LONG>(luid >> 32)};
        if (FAILED(factory->EnumAdapterByLuid(id, IID_PPV_ARGS(&m.adapter)))) {
            m.adapter.Reset();
            return std::nullopt;
        }
        m.adapter_luid = luid;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(m.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) || !info.Budget) return std::nullopt;
    return std::pair{info.CurrentUsage, info.Budget};
}
void keep_memory_guard(State& s) {
    auto& m = s.memory;
    // (The scale stays as it is while a video is exported: the game would remake its targets under the encoder.)
    if (replay_shake::export_frames::busy()) return;
    const float wanted = s.in_replay ? scale_wanted(s) : 0.0f;
    const auto now = GetTickCount64();
    const auto set = [&](float asked, std::uint64_t usage, double pixels_now, const char* why) {
        if (asked == m.asked) return;
        m.asked = asked;
        m.changed_at = now;
        m.raise_since = 0;
        m.learning = asked > 0 && pixels_now > 0;
        m.usage_before = usage;
        m.pixels_before = pixels_now;
        ++s.revision;
        if (asked > 0)
            logging::log(logging::Level::info, logging::Channel::graphics,
                         "Replay Plus: render scale {:.0f}% ({:.0f}% wanted), {}: the game uses {:.2f} GB of the {:.2f} GB of video memory "
                         "Windows gives it (its targets take about {:.0f} bytes a pixel).",
                         asked * 100, wanted * 100, why, static_cast<double>(m.usage) / 1073741824.0, static_cast<double>(m.budget) / 1073741824.0,
                         m.bytes_per_pixel);
    };
    if (wanted <= 1.0f) {
        if (m.asked != 0 || m.limit != 0) {
            m.asked = m.limit = 0;
            m.learning = false;
            m.changed_at = m.raise_since = 0;
            ++s.revision;
        }
        m.lowered = false;
        return;
    }
    if (now - m.checked_at < 250) return;
    m.checked_at = now;
    const auto sizes = link::frame_sizes();
    const bool told = sizes.screen_width && sizes.screen_height && sizes.at && link::now_ms() - sizes.at < 1000;
    const bool shaped = s.shape.width == 0 || (static_cast<int>(sizes.screen_width) == s.shape.width &&
                                               static_cast<int>(sizes.screen_height) == s.shape.height);
    if (!told || !shaped || sizes.screen_width != m.screen_width || sizes.screen_height != m.screen_height) {
        // Not drawn yet, the window not yet its shape, or a new screen (the shape changed): nothing raised
        // until the screen is the one the scale is for.
        if (told) {
            m.screen_width = sizes.screen_width;
            m.screen_height = sizes.screen_height;
        }
        if (m.asked != 0) {
            m.asked = 0;
            m.learning = false;
            m.changed_at = now;
            ++s.revision;
        }
        m.lowered = false;
        return;
    }
    // The PC's own memory left (its commit limit: RAM and page file), which the game spills into past the card's: with
    // too little, the game's own scale (link::commit_room).
    MEMORYSTATUSEX pc{};
    pc.dwLength = sizeof(pc);
    const std::uint64_t commit_left = GlobalMemoryStatusEx(&pc) ? pc.ullAvailPageFile : 0;
    const auto room = link::commit_room(commit_left);
    if (room == link::CommitRoom::none) {
        if (m.asked != 0) {
            m.asked = 0;
            m.lowered = true;
            m.changed_at = now;
            m.raise_since = 0;
            m.learning = false;
            ++s.revision;
            logging::log(logging::Level::info, logging::Channel::graphics,
                         "Replay Plus: render scale back to the game's own: the PC has only {:.1f} GB of its memory left (RAM and page file), "
                         "which the game's larger targets would spill into.",
                         static_cast<double>(commit_left) / 1073741824.0);
        }
        return;
    }
    const auto memory = video_memory(m);
    const double base = static_cast<double>(sizes.screen_width) * sizes.screen_height; // the picture's pixels at 100%
    if (!memory) {
        // The driver tells no budget: as wanted (not raised while the PC's memory is short).
        m.limit = 0;
        if (room == link::CommitRoom::plenty || wanted < m.asked) set(wanted, 0, 0, "the card's memory not known");
        return;
    }
    m.usage = memory->first;
    m.budget = memory->second;
    // The scale the game is set to now (Render.ResolutionScale, as read back; until read, what was asked). Not
    // its picture's size: under a dialog (the editor's Exit Replay) that picture is the screen's size.
    const double now_scale = s.resolution_now > 0.1f ? s.resolution_now : m.asked > 0 ? m.asked : 1.0;
    // After a change: until the game renders at it (its picture before its UI that size; five seconds at most)
    // and its memory has settled.
    if (m.changed_at) {
        const double drawn = sizes.render_width ? static_cast<double>(sizes.render_width) / sizes.screen_width : now_scale;
        const bool rendered = std::abs(drawn - now_scale) < 0.04;
        if (now - m.changed_at < 1500 || (!rendered && now - m.changed_at < 5000)) return;
        if (m.learning) {
            // What a pixel of the game's targets took, from this change (others streaming meanwhile blur it:
            // half the old figure is kept, and never under 300 bytes).
            m.learning = false;
            const double pixels = base * now_scale * now_scale - m.pixels_before;
            if (std::abs(pixels) > base * 0.3) {
                const double each = (static_cast<double>(m.usage) - static_cast<double>(m.usage_before)) / pixels;
                if (each > 50 && each < 5000) m.bytes_per_pixel = std::clamp(0.5 * m.bytes_per_pixel + 0.5 * each, 300.0, 2000.0);
            }
        }
    }
    // The highest scale (in quarters) that keeps the game's use, with its larger targets, a margin under its
    // budget; to go higher again, a wider one (so it does not go up and down as the map streams).
    const double budget = static_cast<double>(m.budget), usage = static_cast<double>(m.usage);
    const auto most_for = [&](double margin) {
        const double square = now_scale * now_scale + (budget - margin - usage) / (m.bytes_per_pixel * base);
        return square > 1.0 ? std::max(1.0f, static_cast<float>(std::floor(std::sqrt(square) * 4.0 + 1e-6) / 4.0)) : 1.0f;
    };
    // The margin: 1.5 GB or 12% of the budget, on cards of 12.5 GB or less 2.5 GB or a fifth (link::memory_margin).
    // Leaving the editor (Exit Replay) the game itself takes 1 to 2.5 GB more for a moment, more at a higher scale
    // (measured at 150%, 2026-11-17), and 4.5 GB on a 12 GB laptop card putting back the 4:3 window (2026-11-19). And
    // no higher than the card's ceiling (link::scale_ceiling: 150% on such a card).
    const double margin = link::memory_margin(m.budget);
    const float ceiling = link::scale_ceiling(m.budget);
    const float capped = std::min(wanted, ceiling);
    const float down = std::min(capped, most_for(margin)), up = std::min(capped, most_for(margin + 0.05 * budget));
    m.limit = std::min(ceiling, most_for(margin));
    const double pixels_now = base * now_scale * now_scale;
    if (m.asked == 0) {
        // (Not raised while the PC's memory is short, nor again in a visit it was lowered in.)
        if (room == link::CommitRoom::plenty && !m.lowered) set(down, m.usage, pixels_now, "first");
        return;
    }
    if (down < m.asked) {
        set(down, m.usage, pixels_now, "lowered");
        m.lowered = true;
        return;
    }
    if (up > m.asked && !m.lowered && room == link::CommitRoom::plenty) {
        // Higher only once it has done for three seconds: each change has the game remake its targets. And not
        // again once lowered in this visit: the budget Windows gives the game moves with what else is on the screen,
        // and the game was raised and lowered by turns every few seconds, remaking its targets each time, a flash
        // on the screen (16:9 at 175%, its budget going between 15.1 and 12.6 GB, 2026-11-18).
        if (!m.raise_since) m.raise_since = now;
        if (now - m.raise_since >= 3000) set(up, m.usage, pixels_now, "raised");
        return;
    }
    m.raise_since = 0;
}

// ---- The replay editor's picture shape (replay_plus_window.h) -----------------------------------
// The game renders at its swap chain's size, which it takes from RenderDevice.FullscreenWidth / Height
// while it runs (borderless full screen: its window stays the monitor's, so the picture would be
// stretched; Replay Plus sizes the window too). A record of the game's own size is kept beside the
// settings while the shape is held, so a game that closes meanwhile (or stops) gets its own back when
// Replay Plus next starts.
fs::path resolution_record(const State& s) { return s.folder / L"game resolution while shaped.txt"; }
// The picture shape wanted now: its window, as the game's threads see the screen, and the picture's size in pixels
// (the two differ only on a screen Windows scales: link::shape_pixels). Both 0 x 0: none.
struct Shaped {
    window::Size window, pixels;
    int scale_percent{100}; // the screen's scaling, as worked out from its two sizes
};
Shaped wanted_shape(State& s) {
    s.shape_problem.clear();
    const auto& v = s.settings;
    // Only once the replay editor's own timeline shows (its UI read): the camera it goes by also shows for
    // a moment as the game loads a level and opens its menu, where reshaping the window took the game's
    // focus (it then held its menu and took no input until clicked).
    if (!v.enabled || !s.in_replay || !s.timeline_ok || v.replay_aspect == Aspect::screen) return {};
    const auto full = named_setting_value("RenderDevice.Fullscreen"), borderless = named_setting_value("RenderDevice.WindowedBorderless");
    if (!full || !borderless) return {};
    if (*full != "1" || *borderless != "1") {
        s.shape_problem = "The picture shape needs the game in borderless full screen (its own display setting).";
        return {};
    }
    const auto seen = window::monitor_size(), pixels = window::monitor_pixels();
    Shaped shaped;
    shaped.window = window::fitted(aspect_ratios[static_cast<std::size_t>(v.replay_aspect)], seen);
    const auto in_pixels = link::shape_pixels({shaped.window.width, shaped.window.height}, {seen.width, seen.height}, {pixels.width, pixels.height});
    shaped.pixels = {in_pixels.width, in_pixels.height};
    if (seen.height > 0 && pixels.height > 0) shaped.scale_percent = static_cast<int>(std::lround(100.0 * pixels.height / seen.height));
    return shaped;
}
void keep_shape(State& s) {
    // (Not while a video is exported: the game would remake its swap chain under the encoder.)
    if (replay_shake::export_frames::busy()) return;
    const auto shaped = wanted_shape(s);
    const auto& shape = shaped.pixels;
    std::error_code error;
    const auto record = resolution_record(s);
    if (shape.width > 0 && !fs::exists(record, error)) {
        const auto w = named_setting_value("RenderDevice.FullscreenWidth"), h = named_setting_value("RenderDevice.FullscreenHeight");
        if (w && h) write_text(record, *w + " " + *h + "\n");
    }
    if (!(shape == s.shape) || !(shaped.window == s.shape_window)) {
        s.shape = shape;
        s.shape_window = shaped.window;
        ++s.revision;
    }
    if (auto said = window::keep(shaped.window); !said.empty()) {
        if (shaped.window.width > 0 && !(shaped.pixels == shaped.window))
            said += std::format(" Its picture is {}x{} pixels: Windows scales this screen {}%.", shaped.pixels.width, shaped.pixels.height,
                                shaped.scale_percent);
        logging::log(logging::Level::info, logging::Channel::graphics, "{}", said);
    }
    // Given back (the settings let go by hold_settings): the record goes.
    if (shape.width == 0 && !window::shaped() && !s.held.contains("RenderDevice.FullscreenWidth")) fs::remove(record, error);
}
// Once, as Replay Plus starts in a level: a record left behind (the game closed while shaped) puts the
// game's own render size back.
void check_resolution(State& s) {
    if (s.resolution_checked || !s.level) return;
    s.resolution_checked = true;
    std::error_code error;
    const auto record = resolution_record(s);
    if (!fs::exists(record, error)) return;
    try {
        const auto text = read_text(record, 256);
        int w{}, h{};
        const auto space = text.find(' ');
        const bool read = space != std::string::npos &&
                          std::from_chars(text.data(), text.data() + space, w).ec == std::errc{} &&
                          std::from_chars(text.data() + space + 1, text.data() + text.size(), h).ec == std::errc{};
        if (read && w >= 640 && h >= 480) {
            const auto now_w = named_setting_value("RenderDevice.FullscreenWidth"), now_h = named_setting_value("RenderDevice.FullscreenHeight");
            if (now_w && now_h && (*now_w != std::to_string(w) || *now_h != std::to_string(h))) {
                (void)change_named_setting("RenderDevice.FullscreenWidth", std::to_string(w), false);
                (void)change_named_setting("RenderDevice.FullscreenHeight", std::to_string(h), false);
                logging::log(logging::Level::info, logging::Channel::graphics,
                             "Replay Plus: the game's render size is {}x{} again (it was left at {}x{} when the game last closed).", w, h, *now_w, *now_h);
            }
        }
    } catch (...) {}
    fs::remove(record, error);
}

// ---- The game's settings, held while asked ----------------------------------------------------
// What Replay Plus wants of the game's named settings now (empty: nothing).
std::map<std::string, std::string, std::less<>> wanted_settings(const State& s) {
    std::map<std::string, std::string, std::less<>> want;
    const auto& v = s.settings;
    if (!v.enabled) return want;
    if (s.in_replay) {
        if (v.game_look && (v.game_barrel != 0 || v.game_barrel_edge != 0 || v.game_barrel_stretch != 0)) {
            // The game's own barrel distortion (GAME), as set there.
            want["DingoVideoFilterSettings.BarrelDistortionEnable"] = "true";
            want["DingoVideoFilterSettings.BarrelDistortionGain"] = number_text(v.game_barrel);
            want["DingoVideoFilterSettings.BarrelDistortionCubicGain"] = number_text(v.game_barrel_edge);
            want["DingoVideoFilterSettings.BarrelDistortionStretch"] = number_text(v.game_barrel_stretch);
        } else if (lens_on(v)) {
            // The fisheye is the whole lens: the game's barrel on top would bend it twice.
            want["DingoVideoFilterSettings.BarrelDistortionEnable"] = "false";
        }
        if (v.focus != Focus::off) {
            // The game's own depth of field, focused where Replay Plus says (in the picture and its exports).
            // An Int32 to the game: -1 not forced (its own), 0 forced off, 1 forced on ("true" is refused).
            // Its physical focus blurs only what is nearer than the focus, so its sprite ranges are given too
            // (focus_ranges), which blur what is behind it as well, shrunk by what the fisheye enlarges.
            const auto r = focus_ranges(s.focus_now, v.focus_depth, centre_zoom_now(s));
            want["PostProcess.ForceDofEnable"] = "1";
            want["PostProcess.ForceDofFocusDistance"] = number_text(s.focus_now.distance);
            want["PostProcess.ForceDofBlurFactor"] = number_text(s.focus_now.blur);
            want["PostProcess.ForceSpriteDofNearStart"] = number_text(r.near_start);
            want["PostProcess.ForceSpriteDofNearEnd"] = number_text(r.near_end);
            want["PostProcess.ForceSpriteDofFarStart"] = number_text(r.far_start);
            want["PostProcess.ForceSpriteDofFarEnd"] = number_text(r.far_end);
            want["PostProcess.ForceSpriteDofBlurMax"] = number_text(r.blur_max);
        }
    }
    // The game's own effects (GAME): in the replay editor, and while skating too when asked. Only what differs
    // from the game's own value is held.
    if (v.game_look && (s.in_replay || (v.game_skating && s.level))) {
        want["PostProcess.ForceEVCompensationEnable"] = "true";
        want["PostProcess.ForceEVCompensation"] = number_text(v.game_exposure);
        want["DingoVideoFilterSettings.FilmGrainEnable"] = v.game_grain > 0 ? "true" : "false";
        want["DingoVideoFilterSettings.ChromaticAberrationEnable"] = v.game_fringe > 0 ? "true" : "false";
        if (v.game_fringe > 0) want["DingoVideoFilterSettings.ChromaticAberrationScaleMultiply"] = number_text(v.game_fringe * 4.0f);
        if (!v.game_motion) {
            want["WorldRender.MotionBlurEnable"] = "false";
        } else {
            if (std::abs(v.game_shutter - 250.0f) > 0.5f) want["WorldRender.MotionBlurFixedShutterTime"] = number_text(1.0f / std::max(v.game_shutter, 1.0f));
            if (v.game_motion_amount != 1.0f) want["WorldRender.MotionBlurScale"] = number_text(v.game_motion_amount);
        }
        if (v.game_bloom <= 0.0f) {
            want["PostProcess.BloomEnable"] = "false";
        } else if (v.game_bloom != 1.0f) {
            for (const auto* axis : {"x", "y", "z"}) want[std::string("PostProcess.ForceBloomScale.") + axis] = number_text(v.game_bloom);
        }
        if (!v.game_flares) want["WorldRender.LensFlaresEnable"] = "false";
        if (v.game_dirt) want["WorldRender.LensDirtEnable"] = "true";
        if (!v.game_ao) want["DingoShaderSettings.AOMode"] = "0";
        if (!v.game_contact_shadows) want["WorldRender.ScreenSpaceContactShadowsEnable"] = "false";
        if (!v.game_fog) {
            want["WorldRender.SkyHeightFogEnable"] = "false";
            want["WorldRender.SkyDepthFogEnable"] = "false";
        }
        // (Not PostProcess.VignetteEnable: the game turns that back on now and then, and held off it flashed between
        // the two, 2026-11-17. Its vignette forced to no colour at all instead, which the game leaves alone.)
        if (!v.game_vignette)
            for (const auto* part : {"x", "y", "z", "w"}) want[std::string("PostProcess.ForceVignetteColor.") + part] = "0";
        if (!v.game_local_exposure) want["PostProcess.LocalExposureEnable"] = "false";
        if (v.game_detail != 1.0f) want["Mesh.GlobalLodScale"] = number_text(v.game_detail);
        if (v.game_shadow_distance != 1.75f) want["Mesh.ShadowDistanceScale"] = number_text(v.game_shadow_distance);
        if (v.game_view_distance != 20000.0f) want["Render.ViewDistance"] = number_text(v.game_view_distance);
    }
    // The texture pool (VIDEO): held always, so it is in place as the next map loads (the game makes the pool
    // anew from it then: 2.1 GB more of video memory with 3 GB in San Andreas, 2026-11-17).
    if (v.bigger_textures && s.texture_pool_kb > 0) want["TextureStreaming.PoolSize"] = std::to_string(s.texture_pool_kb);
    // The render scale: the player's own (VIDEO), and in the replay editor at least Sharp middle's; above
    // 100% in the replay editor, what the video memory guard asks (keep_memory_guard: nothing raised until
    // the picture's shape is in place, and only as far as the card's memory goes).
    float scale = scale_wanted(s);
    if (scale > 1.0f && s.in_replay) scale = s.memory.asked;
    if (scale > 0) {
        want["Render.ResolutionScale"] = number_text(scale);
        want["Render.DynamicResolutionScaleEnable"] = "false";
    }
    if (s.shape.width > 0) {
        want["RenderDevice.FullscreenWidth"] = std::to_string(s.shape.width);
        want["RenderDevice.FullscreenHeight"] = std::to_string(s.shape.height);
    }
    return want;
}
bool same_value(std::string_view now, std::string_view wanted) {
    const auto a = lower(now), b = lower(wanted);
    if (a == b) return true;
    const auto as_bool = [](const std::string& t) -> int { return t == "true" || t == "1" ? 1 : t == "false" || t == "0" ? 0 : -1; };
    if (as_bool(a) >= 0 && as_bool(a) == as_bool(b)) return true;
    char* end{};
    const double x = std::strtod(a.c_str(), &end);
    if (end == a.c_str() || *end) return false;
    const double y = std::strtod(b.c_str(), &end);
    if (end == b.c_str() || *end) return false;
    return std::abs(x - y) <= 1e-4 * std::max(1.0, std::abs(y));
}
void hold_settings(State& s, bool force) {
    const auto now = GetTickCount64();
    if (!force && now - s.held_at < hold_every_ms) return;
    s.held_at = now;
    const auto want = wanted_settings(s);
    // Given back: what is no longer wanted.
    for (auto it = s.held.begin(); it != s.held.end();) {
        if (want.contains(it->first)) { ++it; continue; }
        if (it->second.changed) {
            const auto result = change_named_setting(it->first, {}, true);
            if (result.starts_with("error"))
                logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: {} left as it is ({}).", it->first, result);
        }
        it = s.held.erase(it);
    }
    // Held: what is, set again should the game have changed it.
    for (const auto& [name, value] : want) {
        auto& held = s.held[name];
        held.wanted = value;
        if (held.failed_at && now - held.failed_at < 5000) continue;
        const auto current = named_setting_value(name);
        if (current && same_value(*current, value)) continue;
        const auto result = change_named_setting(name, value, false);
        if (result.starts_with("error")) {
            held.failed_at = now;
            if (held.last_error != result)
                logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Plus: could not set {} to {}: {}", name, value, result);
            held.last_error = result;
        } else {
            held.changed = true;
            held.failed_at = 0;
        }
    }
    if (const auto scale = named_setting_value("Render.ResolutionScale")) {
        char* end{};
        const float value = std::strtof(scale->c_str(), &end);
        if (end != scale->c_str() && value != s.resolution_now) {
            s.resolution_now = value;
            ++s.revision;
        }
    }
}

// ---- The game's own profile options (the replay editor's effects), for research and presets ----
std::uintptr_t option_group(const State& s, unsigned location) {
    std::uintptr_t manager{}, begin{}, end{}, group{};
    if (!memory::peek(s.base + addr::local_native_settings::profile_settings_manager, manager) || !manager ||
        !memory::peek(manager, begin) || !memory::peek(manager + 8, end) || !begin || end < begin || (end - begin) / 8 <= location)
        return 0;
    return memory::peek(begin + std::uintptr_t{location} * 8, group) ? group : 0;
}
std::string option_text(const profile_runtime::NativeSettingValue* value) {
    if (!value) return "(none)";
    const auto json = profile_runtime::native_setting_json(value);
    return json ? json->dump() : std::string("(unreadable)");
}
std::string native_verb(State& s, const std::vector<std::string>& words) {
    if (words.empty()) return "native <key> [value]: the game's own profile option (ReplayEffect_Exposure_S3, sk84_camera_light_radius...).";
    auto& functions = profile_runtime::native_settings();
    if (!functions.get) return "The game's profile options are not reachable yet.";
    const auto& key = words[0];
    for (unsigned location = 0; location < 3; ++location) {
        const auto group = option_group(s, location);
        if (!group) continue;
        const auto* value = functions.get(group, key.c_str());
        if (!value) continue;
        profile_runtime::NativeSettingValue native{};
        if (!memory::peek(reinterpret_cast<std::uintptr_t>(value), native) || !native.data) continue;
        if (words.size() < 2) return std::format("{} (group {}) = {}", key, location, option_text(value));
        const auto type = native.type - s.base;
        bool ok = false;
        if (type == addr::named_settings::native_float32) {
            const float v = std::strtof(words[1].c_str(), nullptr);
            const profile_runtime::NativeSettingValue next{native.type, &v};
            ok = profile_runtime::native_setting_set(group, key.c_str(), native.type, &next);
        } else if (type == addr::named_settings::native_bool) {
            const bool v = words[1] == "1" || lower(words[1]) == "true" || lower(words[1]) == "on";
            const profile_runtime::NativeSettingValue next{native.type, &v};
            ok = profile_runtime::native_setting_set(group, key.c_str(), native.type, &next);
        } else if (type == addr::named_settings::native_int32 || type == addr::named_settings::native_uint32) {
            const std::int32_t v = static_cast<std::int32_t>(std::strtol(words[1].c_str(), nullptr, 10));
            const profile_runtime::NativeSettingValue next{native.type, &v};
            ok = profile_runtime::native_setting_set(group, key.c_str(), native.type, &next);
        } else {
            return std::format("{}: a type Replay Plus does not set (0x{:x}).", key, type);
        }
        return std::format("{} (group {}) {} = {}", key, location, ok ? "set" : "NOT set", option_text(functions.get(group, key.c_str())));
    }
    return std::format("{}: no such option in the game's three groups.", key);
}

// ---- The panel's combo: tapped it switches the shake, held it opens the panel -------------------
// F10 opens the panel (and closes it) from the keyboard, in the replay editor.
void switch_shake(State& s); // (R3's shortcuts, below)
void do_action(State& s, Action action);                          // (R3's shortcuts, below)
void step_focus(State& s, Action action, double steps, bool first); // likewise
std::string preset_verb(State& s, const std::vector<std::string>& words);
// The keyboard's: each shortcut's key (F10 the menu, F11 the shake, as they came; the rest as the player sets them), and
// presets' keys. Focus and blur keys held go on moving, as on the pad.
void read_key(State& s) {
    const auto& v = s.settings;
    // (While the panel has the pointer, ReSkate's overlay counts as open: it is the panel's own.)
    const bool others = overlay::interface_open() && !link::panel_has_pointer();
    const bool open = link::panel_open().load(std::memory_order_acquire);
    // While the panel waits for a key to bind, every key is the panel's: those down then count as held (no press).
    const bool capturing = link::panel_capturing().load(std::memory_order_acquire);
    const bool ctrl = overlay::key_down(VK_CONTROL), shift = overlay::key_down(VK_SHIFT), alt = overlay::key_down(VK_MENU);
    const auto held = [&](std::uint32_t key) {
        if (!(key & 0xff)) return false;
        return overlay::key_down(static_cast<int>(key & 0xff)) && ctrl == ((key & key_ctrl) != 0) && shift == ((key & key_shift) != 0) &&
               alt == ((key & key_alt) != 0);
    };
    const auto now = link::now_ms();
    const double dt = s.key_tick_at ? std::clamp(static_cast<double>(now - s.key_tick_at) / 1000.0, 0.0, 0.1) : 0.0;
    s.key_tick_at = now;
    // The menu's key opens it and closes it.
    constexpr auto menu = static_cast<std::size_t>(Action::panel);
    const bool menu_down = held(v.binds[menu].key);
    if (menu_down && !s.key_was[menu] && !capturing && s.level && s.in_replay && !others) link::ask_panel(!open, true);
    s.key_was[menu] = menu_down;
    // The rest with the menu closed (and nothing else of ReSkate's open).
    const bool free = s.level && s.in_replay && v.enabled && !open && !overlay::interface_open() && !capturing;
    for (std::size_t a = 0; a < action_count; ++a) {
        if (a == menu) continue;
        const auto action = static_cast<Action>(a);
        const bool down = held(v.binds[a].key);
        const bool pressed = down && !s.key_was[a];
        s.key_was[a] = down;
        if (!free || !down) {
            s.key_repeat_at[a] = 0;
            continue;
        }
        const bool moves = action == Action::focus_further || action == Action::focus_nearer || action == Action::blur_less || action == Action::blur_more;
        if (!moves) {
            if (pressed) do_action(s, action);
            continue;
        }
        double steps = 0;
        if (pressed) {
            steps = 1;
            s.key_repeat_at[a] = now + 300;
        } else if (s.key_repeat_at[a] && now >= s.key_repeat_at[a]) {
            const double long_held = static_cast<double>(now - s.key_repeat_at[a]) / 1000.0;
            steps = (12.0 + 24.0 * std::min(long_held, 2.0)) * dt;
        }
        if (steps > 0) step_focus(s, action, steps, pressed);
    }
    // Presets by their keys.
    for (const auto& p : v.preset_binds) {
        const bool down = held(p.bind.key);
        auto& was = s.preset_key_was[p.preset];
        const bool pressed = down && !was;
        was = down;
        if (free && pressed) {
            const auto reply = preset_verb(s, {"load", p.preset});
            say_toast(s, reply, false);
            return; // (the preset's binds are the same ones: the rest next tick)
        }
    }
}
// The replay's clock (Engine/Game/Build/<build>/replay_activity.h: its time, while the replay lane is
// up), and the focus at it: fixed, from the keys, or following the middle of the picture.
void read_focus(State& s) {
    std::uint8_t lane{};
    double playhead{};
    s.playhead_ok = memory::peek(s.base + addr::replay_activity::playing, lane) && lane &&
                    memory::peek(s.base + addr::replay_activity::playhead, playhead) && std::isfinite(playhead);
    if (s.playhead_ok) s.playhead = playhead;
    // While a video is exported: the replay's time as the export has it (replay_shake_export.h replay_time: the clock
    // stands where the editor's playhead was; the export runs from the trim's start).
    if (s.playhead_ok) s.playhead = replay_shake::export_frames::replay_time(playhead);
#if defined(REPLAY_PLUS_TEST_INBOX)
    // (Test builds: while a video is exported, the replay's clock against the frames encoded: each tick for the first 20
    // frames, then every 15th.)
    if (s.playhead_ok) {
        const auto running = replay_shake::export_frames::capture();
        static std::uint64_t traced_session{}, traced_frames{};
        if (running.running && (running.session != traced_session || running.frames < 20 || running.frames >= traced_frames + 15)) {
            traced_session = running.session;
            traced_frames = running.frames;
            const auto light = light_at(s.settings, s.playhead);
            logging::write(logging::Level::info, logging::Channel::graphics,
                           std::format("Replay Plus export clock: {} ms, frame {}, clock {:.4f}, time {:.4f}, light {} {:.2f} warmth {:.2f}, camera at {:.2f} {:.2f} {:.2f}",
                                       GetTickCount64(), running.frames, playhead, s.playhead, light.on ? "on" : "off", light.power, light.warmth,
                                       s.camera[12].load(std::memory_order_relaxed), s.camera[13].load(std::memory_order_relaxed),
                                       s.camera[14].load(std::memory_order_relaxed)));
        }
    }
#endif
    const auto now = link::now_ms();
    const double dt = s.focus_tick_at ? std::clamp(static_cast<double>(now - s.focus_tick_at) / 1000.0, 0.0, 0.25) : 0.0;
    s.focus_tick_at = now;
    const auto& v = s.settings;
    const auto before = s.focus_now;
    switch (v.focus) {
    case Focus::off:
    case Focus::fixed: s.focus_now = {v.focus_distance, v.focus_blur}; break;
    case Focus::keys:
        s.focus_now = s.playhead_ok ? focus_at(v, s.playhead) : FocusAt{v.focus_distance, v.focus_blur};
        // The panel shows (and steps from) the focus at the playhead; a change there makes a key.
        s.settings.focus_distance = s.focus_now.distance;
        s.settings.focus_blur = s.focus_now.blur;
        break;
    case Focus::autofocus: {
        float target = link::focus_depth();
        bool known = target > 0.05f && link::now_ms() - link::focus_depth_at() < 1000;
        // Skater autofocus: how far away the replay has the skater (else, until it does, what is in the middle).
        if (float depth{}; v.focus_skater && skater_depth(s, depth)) {
            target = depth;
            known = true;
            s.skater_depth = depth;
            s.skater_at = GetTickCount64();
        }
        s.focus_now.blur = v.focus_blur;
        if (known) {
            // Eased towards it in how near it looks (a focus ring), as fast as Follow speed says.
            const double rate = 0.6 + 14.0 * static_cast<double>(v.focus_speed) * v.focus_speed;
            const double k = 1.0 - std::exp(-dt * rate);
            const double from = 1.0 / std::max(s.focus_now.distance, 0.05f), to = 1.0 / std::clamp(target, 0.2f, 200.0f);
            s.focus_now.distance = static_cast<float>(1.0 / (from + (to - from) * k));
        }
        break;
    }
    case Focus::count: break;
    }
    if (before.distance != s.focus_now.distance || before.blur != s.focus_now.blur) ++s.revision;
    // The panel shows the playhead (its focus keys go there): kept fresh while it is open.
    const double shown = s.playhead_ok ? s.playhead : -1.0;
    if (std::abs(shown - s.playhead_shown) >= 1.0 / 60.0 && link::panel_open().load(std::memory_order_acquire)) {
        s.playhead_shown = shown;
        ++s.revision;
    }
}
// The pad as ReSkate reads it for itself; in test builds with ReSkate's scripted pad's buttons too
// (the test inbox's `replayplus pad`: the game reads that pad, ReSkate's own reads do not).
void read_pad(ControllerInput& input, bool allow_menu) {
    DingoSDKOverlayReadControllerInput(&input, allow_menu);
#if defined(REPLAY_PLUS_TEST_INBOX)
    if (WORD scripted{}; overlay::scripted_buttons(scripted) && scripted) {
        input.available = true;
        input.buttons |= scripted;
    }
#endif
}
// ---- R3's shortcuts in the replay editor (replay_plus_link.h pad_filter) ------------------------
bool layer_button_ok(std::uint32_t bits) noexcept { return layer_button(bits); }
constexpr std::uint32_t pad_up = 0x0001, pad_down = 0x0002, pad_left = 0x0004, pad_right = 0x0008, pad_view = 0x0020, pad_r3 = 0x0080,
                        pad_lb = 0x0100, pad_rb = 0x0200, pad_lt = 0x10000, pad_rt = 0x20000;
void say_toast(State& s, std::string text, bool log) {
    if (log) logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: {}", text);
    s.toast = std::move(text);
    s.toast_at = link::now_ms();
    ++s.revision;
}
// The focus keyed at the playhead: from Off, Fixed or Auto the depth of field turns Keyframed (a first key
// there holding the focus as it was).
void key_focus(State& s, float distance, float blur, std::string_view what, bool log = true) {
    if (!s.playhead_ok) {
        say_toast(s, "Focus keys go on the replay's time: play or scrub the replay first.");
        return;
    }
    if (s.settings.focus != Focus::keys) {
        s.settings.focus = Focus::keys;
        if (s.settings.focus_keys.empty()) set_focus_key(s.settings, s.playhead, s.focus_now.distance, s.focus_now.blur);
    }
    set_focus_key(s.settings, s.playhead, distance, blur);
    s.focus_now = focus_at(s.settings, s.playhead);
    s.settings.focus_distance = s.focus_now.distance;
    s.settings.focus_blur = s.focus_now.blur;
    changed(s);
    say_toast(s, std::format("{} at {:.2f} s: {:.2f} m, blur {:.0f}%.", what, timeline_seconds(s, s.playhead), distance, blur * 100), log);
}
// Replay Shake's own switch: the shake on or off for the whole replay (its keys, if any, then apply).
void switch_shake(State& s) {
    const auto shake = replay_shake::panel_state();
    if (!shake.available) return;
    const bool on = !shake.on;
    (void)replay_shake::run_command(on ? "on" : "off", {});
    ++s.revision;
    say_toast(s, on ? (s.settings.shake_keys.empty() ? std::string("Shake on.") : std::format("Shake on (its {} key{} apply).",
                                                                                            s.settings.shake_keys.size(),
                                                                                            s.settings.shake_keys.size() == 1 ? "" : "s"))
                    : std::string("Shake off."));
}
// The light switched at the playhead (keyed): the key there turned the other way, or a new key there that turns it the
// other way from how the keys have it.
void switch_light_key(State& s) {
    if (!s.playhead_ok) {
        say_toast(s, "Light keys go on the replay's time: play or scrub the replay first.");
        return;
    }
    auto& v = s.settings;
    if (v.light_keys.empty()) {
        // The first key: on from here (it is as this key before it too, so the light starts off with an off key at 0).
        set_light_key(v, 0.0, {false, v.light_power, v.light_range, v.light_cone, v.light_warmth});
        set_light_key(v, s.playhead, {true, v.light_power, v.light_range, v.light_cone, v.light_warmth});
    } else {
        LightAt here = light_at(v, s.playhead);
        here.on = !here.on;
        set_light_key(v, s.playhead, here);
    }
    changed(s);
    const bool on = light_at(v, s.playhead).on;
    say_toast(s, std::format("Camera light {} from {:.2f} s ({} light key{}).", on ? "on" : "off", timeline_seconds(s, s.playhead), v.light_keys.size(),
                             v.light_keys.size() == 1 ? "" : "s"));
}
// The shake switched at the playhead (a shake key, or the key there taken away).
void key_shake(State& s) {
    if (!s.playhead_ok) {
        say_toast(s, "Shake keys go on the replay's time: play or scrub the replay first.");
        return;
    }
    const auto shake = replay_shake::panel_state();
    const bool on = toggle_shake_key(s.settings, s.playhead, shake.on);
    // The keys switch the shake on and off: Replay Shake itself must be on for them to.
    if (!s.settings.shake_keys.empty() && !shake.on) (void)replay_shake::run_command("on", {});
    changed(s);
    say_toast(s, std::format("Shake {} from {:.2f} s ({} shake key{}).", on ? "on" : "off", timeline_seconds(s, s.playhead), s.settings.shake_keys.size(),
                             s.settings.shake_keys.size() == 1 ? "" : "s"));
}
void delete_keys_here(State& s) {
    if (!s.playhead_ok) return;
    constexpr double near_enough = 0.15;
    const bool focus = remove_focus_key(s.settings, s.playhead, near_enough);
    const bool shake = remove_shake_key(s.settings, s.playhead, near_enough);
    const bool light = remove_light_key(s.settings, s.playhead, near_enough);
    if (!focus && !shake && !light) {
        say_toast(s, "No focus, shake or light key at the playhead.");
        return;
    }
    if (light && !focus && !shake) {
        changed(s);
        say_toast(s, std::format("Deleted the light key at the playhead ({} left).", s.settings.light_keys.size()));
        return;
    }
    if (focus) {
        s.focus_now = focus_at(s.settings, s.playhead);
        s.settings.focus_distance = s.focus_now.distance;
        s.settings.focus_blur = s.focus_now.blur;
    }
    changed(s);
    say_toast(s, std::format("Deleted the {} key{} at the playhead.", focus && shake ? "focus and shake" : focus ? "focus" : "shake",
                             focus && shake ? "s" : ""));
}
void read_shortcuts(State& s) {
    const bool open = link::panel_open().load(std::memory_order_acquire);
    const bool layer = s.level && s.in_replay && !open && s.settings.enabled && s.settings.pad_shortcuts && !overlay::interface_open();
    ControllerInput input;
    if (layer) read_pad(input, true);
    const std::uint32_t buttons = layer && input.available ? input.buttons : 0;
    const auto now = link::now_ms();
    if (layer) link::renew_pad_layer();
    const std::uint32_t pressed = buttons & ~s.pad_before;
    s.pad_before = buttons;
    // The shortcut button: R3 unless the player picked another (CONTROLS).
    const std::uint32_t layer_button = layer_button_ok(s.settings.pad_layer) ? s.settings.pad_layer : default_pad_layer;
    link::pad_layer_button().store(static_cast<std::uint16_t>(layer_button), std::memory_order_relaxed);
    const bool r3 = (buttons & layer_button) != 0;
    if (!layer || !r3) {
        if (s.r3_down) {
            // A tap of R3 alone is the game's own (Photo): given back to it as a short press.
            if (layer && !s.r3_used && now - s.r3_at < 600) link::inject_pad(static_cast<std::uint16_t>(layer_button), 120);
            s.r3_down = false;
            ++s.revision;
        }
        return;
    }
    if (pressed & layer_button || !s.r3_down) {
        s.r3_down = true;
        s.r3_used = (pressed & layer_button) == 0; // held already as the layer came on: not a tap
        s.r3_at = now;
        s.repeat_at = {};
        ++s.revision;
    }
    const auto used = [&] { s.r3_used = true; };
    // Each shortcut on its button (with the shortcut button held), as the player set them (CONTROLS).
    const auto& v = s.settings;
    const auto on = [&](std::uint32_t bits) { return bits && (buttons & bits) == bits; };
    for (std::size_t a = 0; a < action_count; ++a) {
        const auto action = static_cast<Action>(a);
        const bool moves = action == Action::focus_further || action == Action::focus_nearer || action == Action::blur_less || action == Action::blur_more;
        if (moves || !on(v.binds[a].pad) || !(pressed & v.binds[a].pad)) continue;
        used();
        do_action(s, action);
        if (action == Action::panel) return;
    }
    // Presets by their buttons.
    for (const auto& p : v.preset_binds)
        if (on(p.bind.pad) && (pressed & p.bind.pad)) {
            used();
            const auto reply = preset_verb(s, {"load", p.preset});
            say_toast(s, reply, false);
            return;
        }
    // Held, these move: a step at once, then on their own from a moment later, quicker the longer they
    // are held (by the time between ticks, so as fast at any tick rate): focus further / nearer (5% a
    // step), blur less / more (5% a step).
    const double dt = s.pad_tick_at ? std::clamp(static_cast<double>(now - s.pad_tick_at) / 1000.0, 0.0, 0.1) : 0.0;
    s.pad_tick_at = now;
    for (const auto action : {Action::focus_further, Action::focus_nearer, Action::blur_less, Action::blur_more}) {
        const auto a = static_cast<std::size_t>(action);
        const auto bits = v.binds[a].pad;
        if (!on(bits)) {
            s.repeat_at[a] = 0;
            continue;
        }
        double steps = 0;
        const bool first = (pressed & bits) != 0;
        if (first) {
            steps = 1;
            s.repeat_at[a] = now + 300;
        } else if (s.repeat_at[a] && now >= s.repeat_at[a]) {
            const double held = static_cast<double>(now - s.repeat_at[a]) / 1000.0;
            steps = (12.0 + 24.0 * std::min(held, 2.0)) * dt;
        }
        if (steps <= 0) continue;
        used();
        step_focus(s, action, steps, first);
    }
}
// A shortcut done (from its button or its key).
void do_action(State& s, Action action) {
    switch (action) {
    case Action::panel:
        link::ask_panel(true);
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: the panel opens (its shortcut).");
        return;
    case Action::light:
        // The camera light on or off; keyed, a key switching it from the playhead.
        if (s.settings.light && s.settings.light_keyed && s.playhead_ok) {
            switch_light_key(s);
        } else {
            s.settings.light = !s.settings.light;
            changed(s);
            say_toast(s, s.settings.light ? std::format("Camera light on ({:.0f}%).", s.settings.light_power * 100) : std::string("Camera light off."));
        }
        return;
    case Action::shake: switch_shake(s); return;
    case Action::shake_key: key_shake(s); return;
    case Action::delete_keys: delete_keys_here(s); return;
    case Action::focus_further:
    case Action::focus_nearer:
    case Action::blur_less:
    case Action::blur_more: step_focus(s, action, 1, true); return;
    case Action::count: return;
    }
}
// Focus further / nearer (5% a step), blur less / more (5% a step), as a focus key at the playhead.
void step_focus(State& s, Action action, double steps, bool first) {
    auto here = s.playhead_ok ? focus_at(s.settings, s.playhead) : s.focus_now;
    if (s.settings.focus != Focus::keys) here = s.focus_now;
    const float factor = static_cast<float>(std::pow(1.05, steps));
    if (action == Action::focus_further) here.distance = std::min(here.distance * factor, 200.0f);
    if (action == Action::focus_nearer) here.distance = std::max(here.distance / factor, 0.2f);
    if (action == Action::blur_less) here.blur = std::max(here.blur - 0.05f * static_cast<float>(steps), 0.0f);
    if (action == Action::blur_more) here.blur = std::min(here.blur + 0.05f * static_cast<float>(steps), 4.0f);
    key_focus(s, here.distance, here.blur, action == Action::focus_further || action == Action::focus_nearer ? "Focus" : "Blur", first);
}
// The shake keys to Replay Shake (as gates on the replay's clock), when they change.
void send_gates(State& s) {
    std::vector<replay_shake::Gate> gates;
    if (s.settings.enabled)
        for (const auto& key : s.settings.shake_keys) gates.push_back({key.time, key.on});
    if (gates == s.gates_sent) return;
    s.gates_sent = gates;
    replay_shake::set_gates(std::move(gates));
}
// The replay editor's timeline: where it starts on the replay's clock (the playhead's share of it, read
// with the clock), kept while it is read.
void read_timeline(State& s, const replay_shake::PanelState& shake) {
    if (!s.in_replay) {
        s.timeline_ok = false;
        if (!s.chunks.empty()) s.chunks.clear();
        if (s.timeline_published) {
            replay_shake::export_frames::set_timeline({});
            s.timeline_published = false;
        }
        return;
    }
    read_chunks(s);
    if (!shake.timeline || !shake.playhead_ok || shake.timeline_length <= 0) return;
    // Trimmed, the editor's timeline is the trimmed part (Advanced Edit's own tabs, the export's) or the whole replay (its
    // Trim tab): whichever puts the playhead where the timeline has it. The trim and the chunks are shares of the whole.
    const float trim_from = std::clamp(shake.timeline_from, 0.0f, 1.0f), trim_to = std::clamp(shake.timeline_to, 0.0f, 1.0f);
    float bar_from = 0.0f, bar_to = 1.0f;
    if (trim_to - trim_from > 1e-4f && (trim_from > 1e-4f || trim_to < 1.0f - 1e-4f) && !s.chunks.empty()) {
        const float whole = link::timeline_share(s.chunks, 0.0, 0.0, shake.playhead);
        if (whole >= 0) {
            const float as_trimmed = (whole - trim_from) / (trim_to - trim_from);
            if (std::abs(as_trimmed - shake.timeline_at) < std::abs(whole - shake.timeline_at)) {
                bar_from = trim_from;
                bar_to = trim_to;
            }
        }
    }
    if (bar_from != s.bar_from || bar_to != s.bar_to) {
        s.bar_from = bar_from;
        s.bar_to = bar_to;
        ++s.revision;
    }
    // What exports are timed by (replay_shake_export.h replay_time): kept fresh while the editor shows its timeline, and
    // not while a video is exported (what was shown when it began times it).
    if (!replay_shake::export_frames::capture().running) {
        replay_shake::export_frames::Timeline shown;
        for (const auto& c : s.chunks) shown.chunks.push_back({c.clock_from, c.clock_to, c.from, c.to});
        shown.start = shake.playhead - static_cast<double>(shake.timeline_at) * shake.timeline_length;
        shown.length = shake.timeline_length;
        shown.trim_from = trim_from;
        shown.trim_to = std::max(trim_to, trim_from);
        shown.bar_from = bar_from;
        shown.bar_to = bar_to;
        shown.known = true;
        replay_shake::export_frames::set_timeline(std::move(shown));
        s.timeline_published = true;
    }
    const double start = shake.playhead - static_cast<double>(shake.timeline_at) * shake.timeline_length;
    if (!s.timeline_ok || std::abs(start - s.timeline_start) > 1e-3 || std::abs(shake.timeline_length - s.timeline_length) > 1e-3) {
        if (!s.timeline_ok || std::abs(start - s.timeline_start) > 0.05 || std::abs(shake.timeline_length - s.timeline_length) > 0.05)
            logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: the replay's timeline is {:.3f} s long, from {:.3f} on its clock.",
                         shake.timeline_length, start);
        s.timeline_ok = true;
        s.timeline_start = start;
        s.timeline_length = shake.timeline_length;
        ++s.revision;
    }
}
// A combo of the player's own (`replayplus bind`), held for a moment, also opens the panel (R3 + RB
// always does).
void read_combo(State& s) {
    const auto combo = s.settings.panel_bind;
    if (!combo) return;
    const bool open = link::panel_open().load(std::memory_order_acquire);
    const bool allowed = s.level && s.in_replay && !open && combo && valid_controller_combo(combo) && !overlay::interface_open();
    ControllerInput input;
    if (allowed) read_pad(input, false);
    const bool held = allowed && input.available && (input.buttons & combo) == combo;
    const auto now = GetTickCount64();
    if (!allowed) {
        s.combo_down = false;
        s.combo_consumed = false;
        // Pressed already as it comes back (the panel closing on it): counts only once let go.
        s.combo_armed = false;
        return;
    }
    if (!held && !(input.buttons & combo)) s.combo_armed = true;
    if (held && s.combo_armed && !s.combo_down) {
        s.combo_down = true;
        s.combo_consumed = false;
        s.combo_down_at = now;
    }
    if (s.combo_down && held && !s.combo_consumed && now - s.combo_down_at >= panel_hold_ms) {
        s.combo_consumed = true;
        link::ask_panel(true);
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: the panel opens ({} held).", controller_combo_label(combo));
    }
    if (s.combo_down && !held) {
        s.combo_down = false;
        s.combo_armed = false;
    }
}
// While the panel is open, the game gets none of the controller (or the keyboard): it renews the
// overlay's half-second pause on the game's input every tick. Once closed, the pause ends when what
// closed it is let go, so the B that closed it is not also the editor's Back.
void hold_input(State& s) {
    const bool open = link::panel_open().load(std::memory_order_acquire) && link::now_ms() - link::panel_drawn_at() < 1000;
    // From a pad, the panel keeps only the pad's buttons from the game (replay_plus_panel.cpp:
    // hidden_buttons): its sticks and triggers still work the replay. From the keyboard, all of it.
    if (open && !link::panel_keyboard().load(std::memory_order_acquire)) {
        if (s.input_held) overlay::pause_game_input(false);
        s.input_held = false;
        s.pad_held = true;
        s.release_wait_until = GetTickCount64() + 1500;
        return;
    }
    if (open) {
        overlay::pause_game_input(true);
        s.input_held = true;
        s.pad_held = false;
        s.release_wait_until = GetTickCount64() + 1500;
        return;
    }
    if (s.pad_held) {
        // Closed from a pad: its buttons stay kept from the game until they are let go (at most 1.5 s).
        ControllerInput pad;
        read_pad(pad, true);
        if (pad.available && (pad.buttons & 0xf3ff) && GetTickCount64() < s.release_wait_until) link::hide_pad_buttons(120);
        else s.pad_held = false;
    }
    if (!s.input_held) return;
    ControllerInput input;
    DingoSDKOverlayReadControllerInput(&input, true);
    const bool pressed = input.available && input.buttons;
    if (pressed && GetTickCount64() < s.release_wait_until) {
        overlay::pause_game_input(true);
        return;
    }
    overlay::pause_game_input(false);
    s.input_held = false;
}

// ---- Publishing -------------------------------------------------------------------------------
void publish(State& s) {
    auto picture = std::make_shared<link::Picture>();
    picture->settings = s.settings;
    picture->on = s.settings.enabled && s.in_replay && wants_picture(s.settings);
    picture->hood_path = s.hood_wanted;
    picture->hood_serial = s.hood_serial;
    picture->near_plane = s.near_plane.load(std::memory_order_relaxed);
    picture->far_plane = s.far_plane.load(std::memory_order_relaxed);
    picture->autofocus = s.settings.enabled && s.in_replay && s.settings.focus == Focus::autofocus;
    picture->in_replay = s.in_replay;
    picture->exporting = replay_shake::exporting();
    picture->fov_auto = s.fov_auto;
    link::publish_picture(std::move(picture));
    s.views_wanted.store(s.settings.enabled && s.in_replay && wants_views(s.settings), std::memory_order_release);
    const float fixed = s.settings.fov_mode == FovMode::fixed ? s.settings.fov : s.settings.fov_mode == FovMode::automatic ? s.fov_auto : 0.0f;
    s.fixed_fov.store(s.settings.enabled && s.in_replay ? fixed * lens::pi / 180.0f : 0.0f, std::memory_order_release);
    s.export_on.store(s.settings.enabled && wants_picture(s.settings), std::memory_order_release);
    const auto now = GetTickCount64();
    if (s.revision == s.published_revision && now - s.view_at < view_every_ms) return;
    s.view_at = now;
    s.published_revision = s.revision;
    auto view = std::make_shared<link::View>();
    view->revision = s.revision;
    view->available = true;
    view->in_replay = s.in_replay;
    view->settings = s.settings;
    view->presets = s.preset_names;
    view->hood_files = s.hood_files;
    const float fov = link::fov();
    view->fov_degrees = fov > 0 && link::now_ms() - link::fov_at() < 1500 ? fov * 180.0f / lens::pi : 0.0f;
    view->fov_auto = s.fov_auto;
    view->centre_zoom = centre_zoom_now(s);
    view->resolution_now = s.resolution_now;
    view->resolution_wanted = s.in_replay ? scale_wanted(s) : 0.0f;
    view->resolution_limit = s.in_replay ? s.memory.limit : 0.0f;
    view->presets_folder = path_text(s.presets);
    view->overlays_folder = path_text(s.overlays);
    view->import_folder = path_text(s.imports);
    view->export_folder = path_text(s.exports);
    const auto shake = replay_shake::panel_state();
    view->shake = {shake.available, shake.on, shake.fallback, shake.seed};
    view->reply = s.reply;
    view->reply_serial = s.reply_serial;
    view->playhead_ok = s.playhead_ok;
    view->playhead = s.playhead;
    view->focus_distance_now = s.focus_now.distance;
    view->focus_blur_now = s.focus_now.blur;
    view->advanced_edit = shake.advanced_edit;
    view->timeline_ok = s.timeline_ok;
    view->timeline_start = s.timeline_start;
    view->chunks = s.chunks;
    view->timeline_length = s.timeline_length;
    view->bar_from = s.bar_from;
    view->bar_to = s.bar_to;
    view->shake_switch = shake.on;
    view->shake_here = s.playhead_ok ? shake.on && shake_on_at(s.settings, s.playhead, true) : shake.on;
    view->r3_held = s.r3_down;
    view->light_on_here = s.light_on_here;
    view->toast = s.toast;
    view->toast_at = s.toast_at;
    view->video_folder = path_text(video_folder(s.settings));
    view->video_folder_custom = !s.settings.video_folder.empty();
    view->skater_focus_available = s.keyframes_ok;
    view->skater_distance = s.skater_at && GetTickCount64() - s.skater_at < 1000 ? s.skater_depth : 0.0f;
    view->camera_keys_kept = s.camera_keys.size();
    view->status = !s.settings.enabled ? "Off." : s.in_replay ? "In the replay editor." : s.level ? "Skating: opens in the replay editor." : "Waiting for a level.";
    link::publish_view(std::move(view));
}

// ---- Presets ---------------------------------------------------------------------------------
std::string preset_verb(State& s, const std::vector<std::string>& words) {
    const auto rest = [&](std::size_t from) {
        std::string text;
        for (std::size_t i = from; i < words.size(); ++i) text += (i > from ? " " : "") + words[i];
        return text;
    };
    const auto what = words.empty() ? std::string("list") : lower(words[0]);
    refresh_lists(s, true);
    if (what == "list") {
        if (s.preset_names.empty()) return std::format("No presets in {}.", path_text(s.presets));
        std::string text = std::format("{} preset(s) in {}:", s.preset_names.size(), path_text(s.presets));
        for (const auto& name : s.preset_names) text += "\n  " + name;
        return text;
    }
    if (what == "load") {
        const auto name = rest(1);
        std::string why;
        const auto preset = read_preset(s, name, why);
        if (!preset) return "Not loaded: " + why + ".";
        s.settings = apply_preset(s.settings, *preset);
        std::string shake_reply;
        if (!preset->shake.is_null()) shake_reply = replay_shake::apply_preset_shake(preset->shake);
        changed(s);
        hold_settings(s, true);
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: preset {} loaded.", preset->name);
        return std::format("Preset {} loaded.{}", preset->name, shake_reply.empty() ? "" : " " + shake_reply);
    }
    const auto taken = [&](std::string_view name) {
        return std::any_of(s.preset_names.begin(), s.preset_names.end(), [&](const std::string& t) { return same_preset_file(t, name); });
    };
    const auto save_as = [&](const std::string& name) {
        Preset preset;
        preset.name = name;
        preset.settings = s.settings;
        preset.shake = replay_shake::preset_shake();
        const auto file = s.presets / utf8_path(preset_file_name(name));
        if (!write_text(file, preset_json(preset).dump(2) + "\n")) return "Could not write " + path_text(file) + ".";
        s.settings.preset = name;
        changed(s);
        refresh_lists(s, true);
        return std::format("Saved as {}.", name);
    };
    if (what == "save") {
        auto name = clean_preset_name(rest(1));
        if (name.empty()) name = s.settings.preset.empty() ? std::string("My preset") : s.settings.preset;
        return save_as(name);
    }
    if (what == "new") {
        const auto name = clean_preset_name(rest(1));
        if (name.empty()) return "A new preset needs a name: letters, digits, spaces, - and _.";
        if (taken(name)) return std::format("There is a preset called {} already.", name);
        return save_as(name);
    }
    if (what == "rename") {
        const auto text = rest(1);
        const auto bar = text.find('|');
        if (bar == std::string::npos) return "preset rename <name>|<new name>";
        const auto from = clean_preset_name(text.substr(0, bar)), to = clean_preset_name(text.substr(bar + 1));
        if (to.empty()) return "A preset's name: letters, digits, spaces, - and _.";
        std::string why;
        auto preset = read_preset(s, from, why);
        if (!preset) return "Not renamed: " + why + ".";
        if (!same_preset_file(from, to) && taken(to)) return std::format("There is a preset called {} already.", to);
        preset->name = to;
        const auto old_file = s.presets / utf8_path(preset_file_name(from)), new_file = s.presets / utf8_path(preset_file_name(to));
        std::error_code error;
        if (!same_preset_file(from, to)) {
            if (!write_text(new_file, preset_json(*preset).dump(2) + "\n")) return "Could not write " + path_text(new_file) + ".";
            fs::remove(old_file, error);
        } else {
            // Only its letters' case: the file renamed, its name rewritten.
            fs::rename(old_file, new_file, error);
            (void)write_text(new_file, preset_json(*preset).dump(2) + "\n");
        }
        if (same_preset_file(s.settings.preset, from)) {
            s.settings.preset = to;
            changed(s);
        }
        refresh_lists(s, true);
        return std::format("Renamed {} to {}.", from, to);
    }
    if (what == "delete") {
        const auto name = rest(1);
        const auto file = s.presets / utf8_path(preset_file_name(name));
        std::error_code error;
        if (!fs::exists(file, error)) return std::format("There is no preset called {}.", name);
        // Into the Recycle Bin, so a preset deleted by mistake can be had back; removed outright if that cannot be.
        std::wstring from = file.wstring();
        from.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        const bool recycled = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted && !fs::exists(file, error);
        if (!recycled && !fs::remove(file, error)) return std::format("Could not delete {}.", path_text(file));
        if (same_preset_file(s.settings.preset, name)) {
            s.settings.preset.clear();
            changed(s);
        }
        refresh_lists(s, true);
        return recycled ? std::format("Preset {} deleted (it is in the Recycle Bin).", name) : std::format("Preset {} deleted.", name);
    }
    if (what == "export") {
        const auto name = rest(1);
        std::string why;
        auto preset = read_preset(s, name, why);
        if (!preset) return "Not exported: " + why + ".";
        preset->name = name;
        std::error_code error;
        fs::create_directories(s.exports, error);
        const auto file = s.exports / utf8_path(preset_file_name(name));
        if (!write_text(file, preset_json(*preset).dump(2) + "\n")) return "Could not write " + path_text(file) + ".";
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: preset {} exported to {}.", name, path_text(file));
        return std::format("{} is in the export folder: {}. Send that file; it goes in the import folder of whoever gets it.", name,
                           path_text(file));
    }
    if (what == "open" || what == "folder") {
        const auto which = lower(rest(1));
        const auto folder = which == "import" ? s.imports : which == "export" ? s.exports : which == "overlays" ? s.overlays : s.presets;
        std::error_code error;
        fs::create_directories(folder, error);
        // On a thread of its own (the shell may take a moment); File Explorer opens over the game.
        std::thread([folder] {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (SUCCEEDED(com)) CoUninitialize();
        }).detach();
        return std::format("{} opened in File Explorer.", path_text(folder));
    }
    return "preset list | load <name> | save [name] | new <name> | rename <name>|<new name> | export <name> | delete <name> | "
           "open [presets|import|export|overlays]";
}

// ---- Research --------------------------------------------------------------------------------
std::string probe_text(State& s) {
    const auto ui = replay_shake::panel_state();
    std::string text = std::format("Replay Plus probe: level {}, replay editor {}, field of view {:.2f} deg ({} ms old), near {:.3f} far {:.0f}. "
                                   "Game UI: read {}, menu {}, hidden {}.", s.level, s.in_replay, link::fov() * 180.0f / lens::pi,
                                   link::now_ms() - link::fov_at(), s.near_plane.load(), s.far_plane.load(), ui.ui_found, ui.in_menu, ui.ui_hidden);
    std::uint8_t shown{};
    double playhead{};
    const bool read_shown = memory::peek(s.base + addr::replay_activity::playing, shown);
    const bool read_playhead = memory::peek(s.base + addr::replay_activity::playhead, playhead);
    text += std::format(" Replay lane up: {}, playhead {:.3f}.", read_shown ? std::to_string(shown) : "?", read_playhead ? playhead : -1.0);
    const auto free = depth::ui_gap();
    text += std::format(" Advanced Edit: {}. Hint: the game's UI shows {:.2f}, its prompts' row free {:.3f} to {:.3f}.", ui.advanced_edit,
                        depth::ui_shown(), free.from, free.to);
    const auto bar = depth::timeline_bar();
    text += std::format(" Timeline bar: found {} at {:.4f}.", bar.found, bar.y);
    text += std::format(" Timeline: read {}, length {:.4f} s, trim {:.4f} to {:.4f}, playhead at {:.4f} (clock {} {:.4f}, so it starts at {:.4f}).",
                        ui.timeline, ui.timeline_length, ui.timeline_from, ui.timeline_to, ui.timeline_at, ui.playhead_ok, ui.playhead,
                        ui.playhead - ui.timeline_at * ui.timeline_length);
    text += std::format(" Chunks ({}):", s.chunks.size());
    for (const auto& c : s.chunks) text += std::format(" [{:.3f}..{:.3f} at {:.4f}..{:.4f}]", c.clock_from, c.clock_to, c.from, c.to);
    text += " View floats from +0x100:";
    for (std::size_t i = 0; i < s.probe_floats.size(); ++i) text += std::format(" {:.5g}", s.probe_floats[i].load(std::memory_order_relaxed));
    std::uintptr_t renderer{};
    if (memory::peek(s.base + addr::light_tables::renderer, renderer) && renderer) {
        text += " Lights issued:";
        for (const auto& kind : addr::light_tables::kinds) {
            std::uint32_t issued{};
            memory::peek(renderer + kind.manager + addr::light_tables::allocator + addr::light_tables::issued, issued);
            text += std::format(" {} {}", kind.name, issued);
        }
    }
    const auto report = link::render_report();
    text += std::format(" Pass: {} frames, {}x{} format {} transfer {}, fit {:.3f}.", report.frames, report.width, report.height, report.format,
                        report.transfer, report.fit);
    for (const auto& [name, held] : s.held) text += std::format(" Held {}={}{}", name, held.wanted, held.changed ? "" : " (as the game had it)");
    return text;
}
std::string pad_verb(const std::vector<std::string>& words) {
    if (words.empty()) return "pad <buttons, as bind takes them: Back, A, LB+RB..., or none> [milliseconds, 150] [lx|ly|rx|ry|lt|rt value]...";
    // "none": the sticks or triggers alone.
    const auto combo = lower(words[0]) == "none" ? std::optional<std::uint32_t>(0u) : replay_shake::parse_binding(words[0]);
    if (!combo || (*combo != 0 && !valid_controller_combo(*combo)))
        return "pad: no such buttons (LB, RB, A, B, X, Y, Back, Start, Up, Down, Left, Right, L3, R3, LT, RT, none).";
    const int ms = words.size() > 1 ? std::clamp(std::atoi(words[1].c_str()), 16, 20000) : 150;
    XINPUT_GAMEPAD pad{};
    pad.wButtons = static_cast<WORD>(*combo & 0xffff);
    if (*combo & 0x10000) pad.bLeftTrigger = 255;
    if (*combo & 0x20000) pad.bRightTrigger = 255;
    // Sticks: "stickx 0.5" style words after the time (lx, ly, rx, ry), for moving the editor's camera.
    for (std::size_t i = 2; i + 1 < words.size(); i += 2) {
        const auto axis = lower(words[i]);
        const auto value = static_cast<SHORT>(std::clamp(std::atof(words[i + 1].c_str()), -1.0, 1.0) * 32767.0);
        if (axis == "lx") pad.sThumbLX = value;
        else if (axis == "ly") pad.sThumbLY = value;
        else if (axis == "rx") pad.sThumbRX = value;
        else if (axis == "ry") pad.sThumbRY = value;
        else if (axis == "lt") pad.bLeftTrigger = static_cast<BYTE>(std::clamp(std::atof(words[i + 1].c_str()), 0.0, 1.0) * 255.0);
        else if (axis == "rt") pad.bRightTrigger = static_cast<BYTE>(std::clamp(std::atof(words[i + 1].c_str()), 0.0, 1.0) * 255.0);
    }
    const auto ticks = static_cast<std::uint32_t>(std::max(1, ms * 60 / 1000));
    overlay::wake_controller_reads();
    overlay::play_pad({{0, pad}, {ticks, XINPUT_GAMEPAD{}}});
    return std::format("Pad: {} for {} ms ({} game ticks).", controller_combo_label(*combo), ms, ticks);
}

#if defined(REPLAY_PLUS_TEST_INBOX)
// Test builds: lines written to the inbox file are run as console lines, one a tick; "wait <ms>"
// pauses them. What each says goes to the log and to the outbox beside it.
void run_inbox(State& s) {
    const auto now = GetTickCount64();
    const fs::path folder = fs::path(REPLAY_PLUS_TEST_INBOX);
    if (now - s.inbox_at >= 200) {
        s.inbox_at = now;
        const auto file = folder / L"inbox.txt";
        std::error_code error;
        if (fs::exists(file, error)) {
            try {
                const auto text = read_text(file, 1 << 20);
                fs::remove(file, error);
                std::size_t at = 0;
                while (at <= text.size()) {
                    auto end = text.find('\n', at);
                    if (end == std::string::npos) end = text.size();
                    auto line = text.substr(at, end - at);
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                    if (!line.empty() && line[0] != '#') s.inbox.push_back(line);
                    at = end + 1;
                }
            } catch (...) {}
        }
    }
    if (s.inbox.empty() || now < s.inbox_wait_until) return;
    const auto line = s.inbox.front();
    s.inbox.erase(s.inbox.begin());
    std::string reply;
    const auto lowered = lower(line);
    if (lowered.starts_with("wait ")) {
        s.inbox_wait_until = now + static_cast<std::uint64_t>(std::max(0, std::atoi(line.c_str() + 5)));
        reply = "waiting";
    } else {
        std::vector<std::string> words;
        std::size_t at = 0;
        while (at < line.size()) {
            const auto next = line.find(' ', at);
            const auto word = line.substr(at, next == std::string::npos ? std::string::npos : next - at);
            if (!word.empty()) words.push_back(word);
            if (next == std::string::npos) break;
            at = next + 1;
        }
        if (!words.empty() && lower(words[0]) == "replayplus") {
            const auto verb = words.size() > 1 ? words[1] : std::string("status");
            reply = run_command(verb, std::vector<std::string>(words.begin() + std::min<std::ptrdiff_t>(2, static_cast<std::ptrdiff_t>(words.size())), words.end()));
        } else if (!words.empty() && lower(words[0]) == "replayshake") {
            const auto verb = words.size() > 1 ? words[1] : std::string("status");
            reply = replay_shake::run_command(verb, std::vector<std::string>(words.begin() + std::min<std::ptrdiff_t>(2, static_cast<std::ptrdiff_t>(words.size())), words.end()));
        } else {
            runtime::detail::execute_console_command(line);
            reply = "(sent to the console; its answer is in the log)";
        }
    }
    logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus test: {} -> {}", line, reply);
    std::ofstream out(folder / L"outbox.txt", std::ios::app | std::ios::binary);
    out << "> " << line << "\n" << reply << "\n";
}
#endif
} // namespace

bool start(std::uintptr_t base) noexcept {
    auto& s = state();
    if (s.ready.load(std::memory_order_acquire)) return s.base == base;
    try {
        // The game's own faces and button glyphs for the panel, read from its data in the background while it starts.
        native::prepare();
#if defined(REPLAY_PLUS_TEST_INBOX)
        light::watch_makes(base);
#endif
        std::lock_guard lock(s.mutex);
        s.base = base;
        const auto local = local_app_data();
        s.file = local / L"ReSkate" / L"profiles" / L"offline" / L"replay_plus.json";
        s.folder = local / L"ReSkate" / L"Replay Plus";
        s.presets = s.folder / L"Presets";
        s.overlays = s.folder / L"Overlays";
        s.shots = s.folder / L"Shots";
        s.imports = s.folder / L"Import";
        s.exports = s.folder / L"Export";
        std::error_code error;
        if (fs::exists(s.file, error)) {
            try {
                s.settings = settings_from_json(Json::parse(read_text(s.file, 256 * 1024)));
            } catch (const std::exception& e) {
                logging::log(logging::Level::warning, logging::Channel::graphics,
                             "Replay Plus's settings in {} could not be read ({}); it starts from its defaults.", path_text(s.file), e.what());
            }
        }
        // Focus and shake keys are on the replay's clock, and the replay goes with the game: those of an earlier run
        // would land on whatever footage has their times now.
        if (!s.settings.focus_keys.empty() || !s.settings.shake_keys.empty() || !s.settings.light_keys.empty()) {
            logging::log(logging::Level::info, logging::Channel::graphics,
                         "Replay Plus: {} focus key(s), {} shake key(s) and {} light key(s) from the last time the game ran are cleared (their footage "
                         "went with it).",
                         s.settings.focus_keys.size(), s.settings.shake_keys.size(), s.settings.light_keys.size());
            s.settings.focus_keys.clear();
            s.settings.shake_keys.clear();
            s.settings.light_keys.clear();
            s.dirty = true;
            s.save_at = GetTickCount64() + save_after_ms;
        }
        set_up_folders(s);
        refresh_lists(s, true);
        s.ready.store(true, std::memory_order_release);
        logging::log(logging::Level::info, logging::Channel::graphics,
                     "Replay Plus ready ({}): lens {}, {} preset(s) in {}, overlays in {}. Hold RB + R3 (or press F10) in the replay editor for the panel.",
                     s.settings.enabled ? "on" : "off", lens_on(s.settings) ? "on" : "off", s.preset_names.size(),
                     path_text(s.presets), path_text(s.overlays));
        publish(s);
        return true;
    } catch (...) {
        return false;
    }
}

#if defined(REPLAY_PLUS_TEST_INBOX)
void probe_skater(State& s); // (research, below)
#endif
// The camera light as a light of the game's (replay_plus_light.h): on the replay camera (a little above its lens, as a
// light on a camera sits), pointing where it looks, as bright, wide and warm as the LIGHT tab says. Only in the replay
// editor (exports too), and while the camera's pose is fresh; else taken away. When the game's light renderer is not
// the one known there is no light (it was drawn over the picture then, until 1.3.2: that light is gone).
void keep_light(State& s) {
    const auto& v = s.settings;
    light::Wanted w;
    const auto seen = s.camera_at.load(std::memory_order_acquire);
    const bool fresh = seen && GetTickCount64() - seen < 1500;
    const bool where = s.in_replay;
    // Keyed: as the keys have it at the playhead (and the panel shows, and steps from, the light there).
    const bool keyed = v.light_keyed && !v.light_keys.empty() && s.playhead_ok;
    const LightAt at = keyed ? light_at(v, s.playhead) : LightAt{v.light, v.light_power, v.light_range, v.light_cone, v.light_warmth};
    if (keyed && (at.power != v.light_power || at.range != v.light_range || at.cone != v.light_cone || at.warmth != v.light_warmth)) {
        s.settings.light_power = at.power;
        s.settings.light_range = at.range;
        s.settings.light_cone = at.cone;
        s.settings.light_warmth = at.warmth;
        ++s.revision;
    }
    if (keyed && at.on != s.light_on_here) {
        s.light_on_here = at.on;
        ++s.revision;
    }
    w.on = where && v.enabled && at.on && at.power > 0 && fresh;
    if (w.on) {
        std::array<float, 16> c{};
        for (std::size_t i = 0; i < c.size(); ++i) c[i] = s.camera[i].load(std::memory_order_relaxed);
        // The camera's rows: right, up, back, position.
        const float turn = s.light_axes == 1 ? -1.0f : 1.0f;
        for (std::size_t i = 0; i < 3; ++i) {
            w.transform[i] = c[i] * turn;
            w.transform[4 + i] = c[4 + i];
            w.transform[8 + i] = c[8 + i] * turn;
            w.transform[12 + i] = c[12 + i] + c[4 + i] * s.light_above;
        }
        w.transform[15] = 1.0f;
        w.lumens = s.light_lumens * std::clamp(at.power, 0.0f, 8.0f);
        w.range = std::max(at.range, 0.5f);
        w.outer = std::clamp(at.cone, 1.0f, 179.0f);
        w.inner = w.outer * (1.0f - 0.9f * std::clamp(v.light_soft, 0.0f, 1.0f));
        // Its colour as the picture's light had it: a cold LED to a warm tungsten bulb, as bright whatever its colour.
        const float k = std::clamp(at.warmth, -1.0f, 1.0f);
        w.colour = k >= 0 ? std::array<float, 3>{1.0f, 1.0f - 0.18f * k, 1.0f - 0.42f * k} : std::array<float, 3>{1.0f + 0.12f * k, 1.0f + 0.05f * k, 1.0f};
        const float y = 0.2126f * w.colour[0] + 0.7152f * w.colour[1] + 0.0722f * w.colour[2];
        for (auto& c3 : w.colour) c3 /= y;
        w.shadows = v.light_shadows;
    }
    (void)light::update(s.base, w);
    const bool made = w.on && light::exists();
    if (made != s.game_light_made) {
        s.game_light_made = made;
        ++s.revision;
    }
}
void tick(std::uintptr_t base, std::uint32_t client_state, bool editor_open) noexcept {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire) || base != s.base) return;
    try {
        std::lock_guard lock(s.mutex);
        s.level = client_state == 13 || client_state == 21;
        s.in_replay = s.level && editor_open;
        for (const auto& line : link::take()) {
            // The panel's and the menu's commands, as console lines.
            std::vector<std::string> words;
            std::size_t at = 0;
            while (at < line.size()) {
                auto next = line.find(' ', at);
                // (A name of several words is the verb's rest.)
                const auto word = line.substr(at, next == std::string::npos ? std::string::npos : next - at);
                if (!word.empty()) words.push_back(word);
                if (next == std::string::npos) break;
                at = next + 1;
            }
            if (words.size() < 2) continue;
            const std::vector<std::string> rest(words.begin() + 2, words.end());
            std::string reply;
            if (lower(words[0]) == "replayplus") reply = run_command(words[1], rest);
            else if (lower(words[0]) == "replayshake") reply = replay_shake::run_command(words[1], rest);
            if (!reply.empty()) say(s, reply);
        }
#if defined(REPLAY_PLUS_TEST_INBOX)
        run_inbox(s);
#endif
        if (s.in_replay != s.was_in_replay) {
            logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: {} the replay editor.", s.in_replay ? "in" : "out of");
            s.was_in_replay = s.in_replay;
            ++s.revision;
            hold_settings(s, true);
            if (!s.in_replay && link::panel_open().load(std::memory_order_acquire)) link::ask_panel(false);
            // Keep keys: each visit puts back the last one's camera keyframes (once its session is up); without it,
            // leaving the editor takes Replay Plus's own keys with it, as the game does its keyframes.
            s.camera_keys_done = false;
            s.camera_keys_gone_at = 0;
            s.camera_keys_from = GetTickCount64() + 400;
            if (s.in_replay) (void)keyframes_known(s);
            if (!s.in_replay && !s.settings.keep_keys) {
                s.camera_keys.clear();
                if (!s.settings.focus_keys.empty() || !s.settings.shake_keys.empty() || !s.settings.light_keys.empty()) {
                    s.settings.focus_keys.clear();
                    s.settings.shake_keys.clear();
                    s.settings.light_keys.clear();
                    changed(s);
                }
            }
        }
        keep_camera_keys(s);
        follow_exports(s);
        watch_exports(s);
        keep_title(s);
        keep_fov_auto(s);
        if (!s.texture_pool_checked) {
            s.texture_pool_checked = true;
            s.texture_pool_kb = texture_pool_for_card();
        }
        check_resolution(s);
        keep_shape(s);
        keep_sharp_scale(s);
        keep_memory_guard(s);
        read_focus(s);
#if defined(REPLAY_PLUS_TEST_INBOX)
        probe_skater(s);
#endif
        // Every tick while the focus moves (keys, autofocus); otherwise a few times a second.
        hold_settings(s, s.settings.focus == Focus::keys || s.settings.focus == Focus::autofocus);
        const auto shake = replay_shake::panel_state();
        read_timeline(s, shake);
        read_shortcuts(s);
        read_combo(s);
        send_gates(s);
        read_key(s);
        keep_light(s);
        hold_input(s);
        keep_hood(s);
        refresh_lists(s, false);
        publish(s);
        save_when_due(s);
    } catch (...) { /* the next tick tries again */ }
}

#if defined(REPLAY_PLUS_TEST_INBOX)
// ---- Research (test builds): where the skater is in a replay, and the game's own camera keyframes ----
std::string game_keys_text(std::uintptr_t base, bool values) {
    namespace kf = game::build::v20260929::replay_keyframes;
    const auto manager = peek_pointer(base + kf::manager);
    if (!manager) return "No keyframe manager.";
    const auto anchor = manager + kf::manager_sessions;
    std::uint64_t sessions{};
    (void)memory::peek(anchor + 0x20, sessions);
    std::string text = std::format("Keyframe manager 0x{:x}: {} session(s); at +0x40 0x{:x}.", manager, sessions, peek_pointer(manager + 0x40));
    text += " Manager +0x80..0x100:";
    for (std::size_t i = 0x80; i < 0x100; i += 4) {
        float f{};
        (void)memory::peek(manager + i, f);
        text += std::format(" {:.5g}", f);
    }
    if (const auto client = peek_pointer(base + addr::replay_activity::client)) {
        const auto replay = peek_pointer(client + addr::replay_activity::manager);
        double from{}, to{};
        (void)memory::peek(replay + 0x70, from);
        (void)memory::peek(replay + 0x78, to);
        text += std::format(" Replay 0x{:x}: clock {:.4f} to {:.4f}; chunks 0x{:x}..0x{:x}.", replay, from, to, peek_pointer(replay + 0xa0), peek_pointer(replay + 0xa8));
    }
    int n = 0;
    for (auto node = peek_pointer(anchor + 8); node && node != anchor && n < 32; node = map_next(node), ++n) {
        std::int32_t id{};
        (void)memory::peek(node + 0x20, id);
        const auto keys = node + 0x60;
        std::uint64_t count{};
        (void)memory::peek(keys + 0x20, count);
        text += std::format("\n  session {} (node 0x{:x}): {} framing key(s); node", id, node, count);
        for (std::size_t i = 0x20; i < 0x160; i += 8) text += std::format(" {:x}", peek_pointer(node + i));
        int k = 0;
        for (auto key = peek_pointer(keys + 8); key && key != keys && k < 64; key = map_next(key), ++k) {
            double time{};
            (void)memory::peek(key + kf::framing_key_time, time);
            text += std::format("\n    key at {:.4f} (node 0x{:x}):", time, key);
            if (values) {
                for (std::size_t i = 0; i < 0x120; i += 4) {
                    float f{};
                    (void)memory::peek(key + kf::framing_key_value + i, f);
                    text += std::format(" {:.5g}", f);
                }
                text += " | hex:";
                for (std::size_t i = 0; i < 0x120; i += 8) text += std::format(" {:x}", peek_pointer(key + kf::framing_key_value + i));
            }
        }
    }
    return text;
}
void probe_skater(State& s) {
    const auto now = GetTickCount64();
    const auto until = s.skater_probe_until.load(std::memory_order_acquire);
    if (!until) return;
    if (now > until) {
        s.skater_probe_until.store(0, std::memory_order_release);
        logging::log(logging::Level::info, logging::Channel::graphics, "Skater probe: done.");
        return;
    }
    if (now < s.skater_probe_next) return;
    s.skater_probe_next = now + 250;
    std::array<float, 16> cam{};
    for (std::size_t i = 0; i < cam.size(); ++i) cam[i] = s.camera[i].load(std::memory_order_relaxed);
    std::array<float, 12> m{};
    const auto manager = keyframe_manager(s);
    if (manager) (void)memory::peek(manager + manager_skater_body, m);
    float depth{};
    const bool known = skater_depth(s, depth);
    logging::log(logging::Level::info, logging::Channel::graphics,
                 "Skater probe: playhead {} {:.3f}; camera ({:.3f}, {:.3f}, {:.3f}); body ({:.3f}, {:.3f}, {:.3f}) board ({:.3f}, {:.3f}, {:.3f}); depth {} {:.3f}; focus {:.3f}.",
                 s.playhead_ok, s.playhead, cam[12], cam[13], cam[14], m[0], m[1], m[2], m[8], m[9], m[10], known, depth, s.focus_now.distance);
}
// ---- Research (test builds): the game's window, and pad presses -------------------------------
HWND game_window() {
    struct Search {
        DWORD process{};
        HWND best{};
        long area{};
    } search{GetCurrentProcessId()};
    EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(data);
            DWORD process{};
            GetWindowThreadProcessId(window, &process);
            if (process != s.process || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
            RECT r{};
            GetWindowRect(window, &r);
            const long area = (r.right - r.left) * (r.bottom - r.top);
            if (area > s.area) s.area = area, s.best = window;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.best;
}
std::string window_text(HWND window) {
    if (!window) return "No game window.";
    RECT outer{}, inner{};
    GetWindowRect(window, &outer);
    GetClientRect(window, &inner);
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    wchar_t name[128]{};
    GetClassNameW(window, name, 128);
    char title[128]{};
    GetWindowTextA(window, title, 128);
    return std::format("Window \"{}\" ({}): at {},{} to {},{} ({}x{}), client {}x{}, style 0x{:x} ex 0x{:x}; monitor {},{} to {},{}.", title,
                       path_text(fs::path(name)), outer.left, outer.top, outer.right, outer.bottom, outer.right - outer.left,
                       outer.bottom - outer.top, inner.right, inner.bottom, static_cast<unsigned long>(GetWindowLongPtrW(window, GWL_STYLE)),
                       static_cast<unsigned long>(GetWindowLongPtrW(window, GWL_EXSTYLE)), monitor.rcMonitor.left, monitor.rcMonitor.top,
                       monitor.rcMonitor.right, monitor.rcMonitor.bottom);
}
std::string window_verb(const std::vector<std::string>& words) {
    static RECT original{};
    static bool saved = false;
    const HWND window = game_window();
    if (!window) return "No game window.";
    if (words.empty()) return window_text(window);
    if (lower(words[0]) == "restore") {
        if (!saved) return "Nothing to restore.";
        SetWindowPos(window, nullptr, original.left, original.top, original.right - original.left, original.bottom - original.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return window_text(window);
    }
    if (words.size() < 2) return "window [w h | restore]";
    const int w = std::atoi(words[0].c_str()), h = std::atoi(words[1].c_str());
    if (w < 320 || h < 240) return "window [w h | restore]";
    if (!saved) {
        GetWindowRect(window, &original);
        saved = true;
    }
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT want{0, 0, w, h};
    AdjustWindowRectEx(&want, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)));
    const int ow = want.right - want.left, oh = want.bottom - want.top;
    const int x = monitor.rcMonitor.left + (monitor.rcMonitor.right - monitor.rcMonitor.left - ow) / 2;
    const int y = monitor.rcMonitor.top + (monitor.rcMonitor.bottom - monitor.rcMonitor.top - oh) / 2;
    SetWindowPos(window, nullptr, x, y, ow, oh, SWP_NOZORDER | SWP_NOACTIVATE);
    return window_text(window);
}
std::uint32_t pad_bits(std::string_view names) {
    static const std::map<std::string, std::uint32_t, std::less<>> bits{
        {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}, {"menu", 0x0010}, {"start", 0x0010}, {"view", 0x0020},
        {"back", 0x0020}, {"l3", 0x0040}, {"r3", 0x0080}, {"lb", 0x0100}, {"rb", 0x0200}, {"a", 0x1000}, {"b", 0x2000},
        {"x", 0x4000}, {"y", 0x8000}, {"lt", 0x10000}, {"rt", 0x20000}};
    std::uint32_t value = 0;
    std::size_t at = 0;
    const auto text = lower(names);
    while (at <= text.size()) {
        auto end = text.find('+', at);
        if (end == std::string::npos) end = text.size();
        const auto it = bits.find(std::string_view(text).substr(at, end - at));
        if (it == bits.end()) return 0;
        value |= it->second;
        at = end + 1;
    }
    return value;
}
#endif

std::string run_command(std::string_view verb, const std::vector<std::string>& words) {
    auto& s = state();
    if (!s.ready.load(std::memory_order_acquire)) return "Replay Plus is unavailable.";
    try {
        std::lock_guard lock(s.mutex);
        const auto name = lower(verb);
        if (name == "help") {
            return "replayplus status | on | off | set <key> <value> | get <key> | step <key> <n> | reset [all|tab|key] | list [tab] | "
                   "panel [open|close] | preset list|load|save|new|rename|export|delete|open | focus list|key|delete|clear | shake list|key|clear | "
                   "light list|key|off|switch|delete|clear | bind <0x..>|default | shot <name> [clean] | probe | white [auto|<value>] | "
                   "gamelight [lumens <at 100%>] [above <m>]";
        }
        if (name == "status") {
            const auto report = link::render_report();
            return std::format("Replay Plus {}: {}; lens {}, fov {}, bars {}, overlay {}, light {}, resolution {} (now {}). Pass {} frames at {}x{}.",
                               s.settings.enabled ? "on" : "off", s.in_replay ? "in the replay editor" : s.level ? "skating" : "no level",
                               lens_on(s.settings) ? std::format("{:.0f}% circle {:.3f} edge {:.1f}%", s.settings.curve * 100, s.settings.circle,
                                                                 s.settings.compress * 100)
                                                   : std::string("off"),
                               s.settings.fov_mode == FovMode::fixed       ? std::format("{:.0f}", s.settings.fov)
                               : s.settings.fov_mode == FovMode::automatic ? std::format("auto ({:.1f})", s.fov_auto)
                                                                            : std::string("the editor's"),
                               bars_names[static_cast<std::size_t>(s.settings.bars)], hood_names[static_cast<std::size_t>(s.settings.hood)],
                               s.settings.light ? (s.game_light_made ? "on (a light of the game's: " + light::status() + ")" : std::string("on")) : std::string("off"),
                               s.sharp_scale > 0 ? std::format("sharp middle {:.0f}%", s.sharp_scale * 100) : s.settings.resolution ? "own" : "the game's",
                               s.resolution_now, report.frames, report.width,
                               report.height) +
                   (s.memory.budget ? std::format(" Video memory: the game uses {:.2f} of {:.2f} GB; raised scale asked {:.0f}%, the most it allows {:.0f}% "
                                                  "(about {:.0f} bytes a pixel).",
                                                  static_cast<double>(s.memory.usage) / 1073741824.0, static_cast<double>(s.memory.budget) / 1073741824.0,
                                                  s.memory.asked * 100, s.memory.limit * 100, s.memory.bytes_per_pixel)
                                    : std::string());
        }
        if (name == "panel") {
            const auto what = words.empty() ? std::string("open") : lower(words[0]);
            link::ask_panel(what != "close");
            return what == "close" ? "The panel closes." : "The panel opens.";
        }
        if (name == "preset" || name == "presets") return preset_verb(s, words.empty() && name == "presets" ? std::vector<std::string>{"list"} : words);
        if (name == "shot") {
            const auto file = (s.shots / utf8_path(words.empty() ? std::string("shot") : words[0])).replace_extension(L".bmp");
            const bool clean = words.size() > 1 && lower(words[1]) == "clean";
            link::ask_shot(path_text(file) + (clean ? "|clean" : ""));
            return std::format("The next frame goes to {}{}.", path_text(file), clean ? " (before ReSkate's menu and panel)" : "");
        }
        if (name == "trace") {
            const int frames = words.empty() ? 2 : std::clamp(std::atoi(words[0].c_str()), 1, 10);
            link::trace_frames().store(frames);
            return std::format("The game's texture transitions for {} frame(s) go to the log.", frames);
        }
        if (name == "videofolder" || name == "videos") return video_folder_verb(s, words);
        if (name == "keys") {
            std::size_t sessions{};
            std::vector<CameraKey> now;
            const auto manager = keyframe_manager(s);
            if (manager && read_camera_keys(manager, now)) sessions = 1;
            return std::format("Keep keys {}: {} camera keyframe(s) kept{}; {} focus key(s), {} shake key(s).{}", s.settings.keep_keys ? "on" : "off",
                               s.camera_keys.size(), sessions ? std::format(" ({} in the editor now)", now.size()) : std::string(),
                               s.settings.focus_keys.size(), s.settings.shake_keys.size(),
                               keyframes_known(s) ? "" : " (The game's keyframes are not the ones known on this build: only Replay Plus's keys are kept.)");
        }
        if (name == "probe") {
            const auto text = probe_text(s);
            logging::write(logging::Level::info, logging::Channel::graphics, text);
            return text;
        }
        if (name == "named") {
            if (words.empty()) return "named <Setting.Name> [value|restore]";
            if (words.size() == 1) {
                const auto value = named_setting_value(words[0]);
                return std::format("{} = {}", words[0], value ? *value : std::string("(not read)"));
            }
            return change_named_setting(words[0], lower(words[1]) == "restore" ? std::string() : words[1], lower(words[1]) == "restore");
        }
        if (name == "gamelight") {
            // `gamelight [axes 0|1] [above <m>] [lumens <at 100%>] [marks <B, hex>] [flag <n>]`: the real light's set-up, to try in
            // the game (marks and flag: what it is made with; made again).
            for (std::size_t i = 0; i + 1 < words.size(); i += 2) {
                const auto what = lower(words[i]);
                const float v = static_cast<float>(std::atof(words[i + 1].c_str()));
                if (what == "axes") s.light_axes = std::clamp(static_cast<int>(v), 0, 1);
                else if (what == "above") s.light_above = std::clamp(v, -1.0f, 2.0f);
                else if (what == "lumens") s.light_lumens = std::clamp(v, 1.0f, 1.0e6f);
                else if (what == "marks") light::research(s.base, static_cast<std::uint32_t>(std::strtoul(words[i + 1].c_str(), nullptr, 16)), -1);
                else if (what == "flag") light::research(s.base, 0xce5fffffu, static_cast<int>(v));
            }
            return std::format("Camera light (real): axes {}, {:.2f} m above the lens, {:.0f} lm at 100%; {}.", s.light_axes, s.light_above, s.light_lumens,
                               light::status());
        }
        if (name == "white") {
            // The white of the game's picture under its UI (the editor's picture is drawn into it), measured from the
            // screen; `white <value>` fixes it instead (to compare), `white auto` measures again.
            const auto what = words.empty() ? std::string() : lower(words[0]);
            if (what == "dump") {
                // Research: the next measurement's blocks (the picture's and the screen's means) to a CSV in the shots folder.
                const auto file = (s.shots / utf8_path(words.size() > 1 ? words[1] : std::string("white"))).replace_extension(L".csv");
                link::ask_white_dump(path_text(file));
                return "The next measurement's blocks go to " + path_text(file) + ".";
            }
            if (what == "auto") {
                link::white_fixed().store(0.0f);
            } else if (!what.empty()) {
                const float value = std::strtof(what.c_str(), nullptr);
                if (!(value >= 0.05f && value <= 100.0f)) return "white [auto | <the picture's white, e.g. 2.16>]";
                link::white_fixed().store(value);
            }
            link::ask_white();
            std::string game;
            for (const char* setting : {"Render.DisplayMappingEnable", "Render.DisplayMappingVersion", "Render.DisplayMappingSdrPeakLuma",
                                        "Render.DisplayMappingHdr10PeakLuma", "Render.WorldBrightnessAdjustment", "Render.OverlayBrightnessAdjustment",
                                        "Render.HdrOverlayPlaneNits"}) {
                const auto value = named_setting_value(setting);
                game += std::format(" {}={}", setting, value ? *value : std::string("?"));
            }
            const float fixed = link::white_fixed().load();
            const auto detail = link::white_detail();
            const float measured = link::scene_white_measured();
            const auto how = fixed > 0.0f ? std::string("fixed")
                           : measured > 0.0f ? std::format("measured {:.3f}", measured)
                                             : std::string("not measured yet");
            const auto used = link::scene_white_curve()
                                  ? std::format("Under the game's UI its display mapping is undone, the picture taken at {:.3f} ({}; it stays at 1, "
                                                "the game as it comes, unless measured more than 10% off)",
                                                link::scene_white(), how)
                                  : std::format("Under the game's UI the picture's white is {:.3f} ({})", link::scene_white(), how);
            const auto text = std::format("{}. The game's display mapping:{}. {}", used, game,
                                          detail.empty() ? std::string("The next measurement's details go to the log.")
                                                         : "Last asked for: " + detail + " (the next goes to the log)");
            logging::write(logging::Level::info, logging::Channel::graphics, text);
            return text;
        }
        if (name == "native") return native_verb(s, words);
#if defined(REPLAY_PLUS_TEST_INBOX)
        if (name == "dmprobe") {
            // Research: known picture values in the white measurement's blocks (replay_plus.hlsl probe_value), to
            // measure the game's display mapping with `white dump`. Set 1 and on; off.
            const int set = words.empty() || lower(words[0]) == "off" ? 0 : std::clamp(std::atoi(words[0].c_str()), 0, 1000);
            link::display_probe().store(set);
            return set ? std::format("The probe's set {} goes under the game's UI.", set) : std::string("The probe is off.");
        }
        if (name == "window") return window_verb(words);
        if (name == "skaterprobe") {
            const int seconds = words.empty() ? 10 : std::clamp(std::atoi(words[0].c_str()), 1, 120);
            s.skater_probe_next = 0;
            s.skater_probe_until.store(GetTickCount64() + static_cast<std::uint64_t>(seconds) * 1000, std::memory_order_release);
            return std::format("Where the skater is goes to the log every 250 ms for {} s.", seconds);
        }
        if (name == "gamekeys") {
            const auto text = game_keys_text(s.base, !words.empty() && lower(words[0]) == "values");
            logging::write(logging::Level::info, logging::Channel::graphics, text);
            return text;
        }
        if (name == "playedtrace") {
            // Research: every play of the camera's keyframes (the replay's time) to the log for some seconds.
            const int seconds = words.empty() ? 5 : std::clamp(std::atoi(words[0].c_str()), 1, 120);
            replay_shake::export_frames::trace_played(static_cast<std::uint32_t>(seconds) * 1000);
            const auto p = replay_shake::export_frames::played();
            return std::format("Each play of the camera keys goes to the log for {} s (hooked {}, {} plays so far, the last {:.4f}, {} ms ago).", seconds,
                               p.hooked, p.calls, p.time, p.at ? GetTickCount64() - p.at : 0);
        }
        if (name == "testlight") {
            // Research: testlight <clock on> [<clock off>]: light keys at those times on the replay's clock (off a second
            // before, on at the first, off at the second), the light at its brightest, wide and far, to see where they land.
            if (words.empty()) return "testlight <clock on> [<clock off>]";
            const double on = std::atof(words[0].c_str()), off = words.size() > 1 ? std::atof(words[1].c_str()) : 0.0;
            s.settings.light_keys.clear();
            const LightAt dark{false, 8.0f, 50.0f, 120.0f, 0.0f}, lit{true, 8.0f, 50.0f, 120.0f, 0.0f};
            set_light_key(s.settings, on - 1.0, dark);
            set_light_key(s.settings, on, lit);
            if (off > on) set_light_key(s.settings, off, dark);
            s.settings.light = true;
            s.settings.light_keyed = true;
            changed(s);
            return std::format("Light keys: off at {:.3f}, on at {:.3f}{} ({} keys).", on - 1.0, on, off > on ? std::format(", off at {:.3f}", off) : "",
                               s.settings.light_keys.size());
        }
        if (name == "slowkeys") {
            // Research: slowkeys <speed> [from] [gap]: three camera keys (Orbit, the body) into the editor's first session, at the
            // editor's start + from (1x), + from + gap (<speed>) and + from + 3 gaps (1x).
            const auto manager = keyframe_manager(s);
            std::vector<CameraKey> keys;
            if (!manager || !read_camera_keys(manager, keys)) return "No keyframe session (open Advanced Edit first).";
            const auto anchor = manager + kf::manager_sessions;
            const auto first = peek_pointer(anchor + 8);
            std::int32_t session{};
            if (!first || first == anchor || !memory::peek(first + 0x20, session)) return "No keyframe session (open Advanced Edit first).";
            const float speed = words.empty() ? 0.25f : static_cast<float>(std::clamp(std::atof(words[0].c_str()), 0.05, 4.0));
            const double from = words.size() > 1 ? std::atof(words[1].c_str()) : 0.5, gap = words.size() > 2 ? std::atof(words[2].c_str()) : 1.0;
            double start{}, lands{};
            const auto at = s.base + key_time.rva;
            if (!call_key_time(at, manager, -1.0e7, start)) return "The editor's range is not up yet.";
            const auto add = s.base + add_framing_key.rva;
            std::string made;
            const double times[3]{start + from, start + from + gap, start + from + 3 * gap};
            const float speeds[3]{1.0f, speed, 1.0f};
            for (int i = 0; i < 3; ++i) {
                const bool ok = call_add(add, manager, session, times[i], 0, 65.0f, speeds[i]) && call_key_time(at, manager, times[i], lands);
                made += std::format("{}{:.3f} at {:.2f}x{}", i ? ", " : "", lands, speeds[i], ok ? "" : " (failed)");
            }
            (void)call_rebuild(s.base + kf::session_rebuild.rva, manager, session);
            (void)read_camera_keys(manager, keys);
            std::string now;
            for (const auto& k : keys) {
                float v{};
                std::memcpy(&v, k.value.data() + value_speed, sizeof(v));
                now += std::format(" [{} {:.3f} {:.2f}x]", k.session, k.time, v);
            }
            const auto text = std::format("Session {}: keys {}; the editor starts at {:.3f}. Its keys now:{}", session, made, start, now);
            logging::write(logging::Level::info, logging::Channel::graphics, text);
            return text;
        }
        if (name == "backdrop") {
            // Research: backdrop show | hide (the black window alone), mark | unmark (the taskbar alone), notaskbar on | off.
            const auto what = words.empty() ? std::string() : lower(words[0]);
            if (what == "show" || what == "hide") window::test_backdrop(what == "show");
            else if (what == "mark" || what == "unmark") window::test_taskbar(what == "mark");
            else if (what == "notaskbar") window::test_no_taskbar(words.size() > 1 && lower(words[1]) == "on");
            else return "backdrop show | hide | mark | unmark | notaskbar on|off";
            return "backdrop " + what;
        }
        if (name == "pad") {
            // A press on ReSkate's scripted pad (the game reads it as player 0; Replay Plus's own reads see it too in
            // test builds): pad <A+B... | none> [ms] [lx|ly|rx|ry|lt|rt value]... (sticks for the editor's free camera).
            if (words.empty()) return "pad <buttons, e.g. R3 or R3+RB, or none> [ms] [lx|ly|rx|ry|lt|rt value]...";
            const bool none = lower(words[0]) == "none";
            const auto buttons = none ? 0u : pad_bits(words[0]);
            if (!none && !buttons) return "No such buttons: " + words[0];
            const int ms = words.size() > 1 ? std::clamp(std::atoi(words[1].c_str()), 16, 20000) : 150;
            XINPUT_GAMEPAD pressed{};
            pressed.wButtons = static_cast<WORD>(buttons & 0xffff);
            if (buttons & 0x10000) pressed.bLeftTrigger = 255;
            if (buttons & 0x20000) pressed.bRightTrigger = 255;
            for (std::size_t i = 2; i + 1 < words.size(); i += 2) {
                const auto axis = lower(words[i]);
                const double v = std::clamp(std::atof(words[i + 1].c_str()), -1.0, 1.0);
                const auto stick = static_cast<SHORT>(v * 32767.0);
                const auto trigger = static_cast<BYTE>(std::clamp(v, 0.0, 1.0) * 255.0);
                if (axis == "lx") pressed.sThumbLX = stick;
                else if (axis == "ly") pressed.sThumbLY = stick;
                else if (axis == "rx") pressed.sThumbRX = stick;
                else if (axis == "ry") pressed.sThumbRY = stick;
                else if (axis == "lt") pressed.bLeftTrigger = trigger;
                else if (axis == "rt") pressed.bRightTrigger = trigger;
            }
            overlay::wake_controller_reads();
            overlay::play_pad({{0, pressed}, {static_cast<std::uint32_t>(std::max(1, ms * 60 / 1000)), XINPUT_GAMEPAD{}}});
            return std::format("The scripted pad presses 0x{:x} for {} ms (sticks {} {} / {} {}, triggers {} {}).", buttons, ms, pressed.sThumbLX,
                               pressed.sThumbLY, pressed.sThumbRX, pressed.sThumbRY, pressed.bLeftTrigger, pressed.bRightTrigger);
        }
#endif
        if (name == "shake") {
            // The shake's keys along the replay: list, key (the shake switched at the playhead), clear.
            const auto what = words.empty() ? std::string("list") : lower(words[0]);
            if (what == "list") {
                if (s.settings.shake_keys.empty()) return "No shake keys: Replay Shake's own switch holds for the whole replay.";
                std::string text = std::format("{} shake key(s):", s.settings.shake_keys.size());
                for (const auto& key : s.settings.shake_keys)
                    text += std::format("\n  {:.2f} s: {}", timeline_seconds(s, key.time), key.on ? "on" : "off");
                return text;
            }
            if (what == "clear") {
                const auto count = s.settings.shake_keys.size();
                s.settings.shake_keys.clear();
                changed(s);
                return std::format("{} shake key(s) removed.", count);
            }
            if (what == "key") {
                key_shake(s);
                return s.toast;
            }
            return "shake list | key | clear";
        }
        if (name == "light") {
            // `light list | key | off | switch | delete | clear`: the camera light's keys (at the playhead).
            const auto what = words.empty() ? std::string("list") : lower(words[0]);
            if (what == "list") {
                if (s.settings.light_keys.empty()) return "No light keys.";
                std::string text = std::format("{} light key(s){}:", s.settings.light_keys.size(), s.settings.light_keyed ? "" : " (Light keys is off)");
                for (const auto& key : s.settings.light_keys)
                    text += std::format("\n  {:.2f} s: {}, {:.0f}%, {:.1f} m, {:.0f} deg, colour {:.2f}{}", timeline_seconds(s, key.time), key.on ? "on" : "off",
                                        key.power * 100, key.range, key.cone, key.warmth,
                                        s.playhead_ok && std::abs(key.time - s.playhead) <= 1.0 / 30.0 ? " (here)" : "");
                return text;
            }
            if (what == "clear") {
                const auto count = s.settings.light_keys.size();
                s.settings.light_keys.clear();
                changed(s);
                return std::format("{} light key(s) removed.", count);
            }
            if (!s.playhead_ok) return "Light keys go on the replay's time: open a replay first.";
            if (what == "key" || what == "add" || what == "on" || what == "off") {
                LightAt here = light_at(s.settings, s.playhead);
                here.on = what != "off";
                if (!s.settings.light_keyed || s.settings.light_keys.empty())
                    here = {what != "off", s.settings.light_power, s.settings.light_range, s.settings.light_cone, s.settings.light_warmth};
                set_light_key(s.settings, s.playhead, here);
                s.settings.light = true;
                s.settings.light_keyed = true;
                changed(s);
                return std::format("Light key at {:.2f} s: {} ({} key(s)).", timeline_seconds(s, s.playhead), here.on ? "on" : "off", s.settings.light_keys.size());
            }
            if (what == "switch") {
                switch_light_key(s);
                return s.toast;
            }
            if (what == "delete" || what == "remove") {
                if (!remove_light_key(s.settings, s.playhead, 0.25)) return "No light key here.";
                changed(s);
                return std::format("Light key removed ({} left).", s.settings.light_keys.size());
            }
            return "light list | key | off | switch | delete | clear";
        }
        if (name == "focus") {
            const auto what = words.empty() ? std::string("list") : lower(words[0]);
            if (what == "list") {
                if (s.settings.focus_keys.empty()) return "No focus keys.";
                std::string text = std::format("{} focus key(s):", s.settings.focus_keys.size());
                for (const auto& key : s.settings.focus_keys)
                    text += std::format("\n  {:.2f} s: {:.2f} m, blur {:.0f}%{}", timeline_seconds(s, key.time), key.distance, key.blur * 100,
                                        s.playhead_ok && std::abs(key.time - s.playhead) <= 1.0 / 30.0 ? " (here)" : "");
                return text;
            }
            if (what == "clear") {
                const auto count = s.settings.focus_keys.size();
                s.settings.focus_keys.clear();
                changed(s);
                return std::format("{} focus key(s) removed.", count);
            }
            if (!s.playhead_ok) return "Focus keys go on the replay's time: open a replay first.";
            if (what == "key" || what == "add") {
                float distance = s.focus_now.distance, blur = s.focus_now.blur;
                if (words.size() > 1) distance = std::strtof(words[1].c_str(), nullptr);
                if (words.size() > 2) blur = std::strtof(words[2].c_str(), nullptr) / (words[2].find('%') != std::string::npos ? 100.0f : 1.0f);
                set_focus_key(s.settings, s.playhead, distance, blur);
                if (s.settings.focus != Focus::keys) s.settings.focus = Focus::keys;
                changed(s);
                return std::format("Focus key at {:.2f} s: {:.2f} m ({} key(s)).", timeline_seconds(s, s.playhead), std::clamp(distance, 0.2f, 200.0f), s.settings.focus_keys.size());
            }
            if (what == "delete" || what == "remove") {
                if (!remove_focus_key(s.settings, s.playhead, 0.25)) return "No focus key here.";
                changed(s);
                return std::format("Focus key removed ({} left).", s.settings.focus_keys.size());
            }
            return "focus list | key [metres] [blur] | delete | clear";
        }
        if (name == "pad") return pad_verb(words);
        if (name == "hood" && !words.empty()) {
            // The overlays folder's pictures, by name (frame.file), or the next one.
            refresh_lists(s, true);
            if (s.hood_files.empty()) return std::format("No pictures in {}.", path_text(s.overlays));
            // (A name of several words is all of them: "Mk1 hood.png".)
            std::string pick = words[0];
            for (std::size_t i = 1; i < words.size(); ++i) pick += " " + words[i];
            if (lower(pick) != "next" && lower(pick) != "previous") {
                const auto it = std::find_if(s.hood_files.begin(), s.hood_files.end(), [&](const std::string& name) { return lower(name) == lower(pick); });
                if (it == s.hood_files.end()) return std::format("No picture called {} in {}.", pick, path_text(s.overlays));
                pick = *it;
            }
            if (lower(pick) == "next" || lower(pick) == "previous") {
                const auto it = std::find(s.hood_files.begin(), s.hood_files.end(), s.settings.hood_file);
                std::ptrdiff_t index = it == s.hood_files.end() ? -1 : it - s.hood_files.begin();
                const auto count = static_cast<std::ptrdiff_t>(s.hood_files.size());
                index = lower(pick) == "next" ? (index + 1) % count : (index - 1 + count) % count;
                pick = s.hood_files[static_cast<std::size_t>(index)];
            }
            s.settings.hood_file = pick;
            s.settings.hood = Hood::picture;
            changed(s);
            return std::format("Overlay: {}.", pick);
        }
        const std::span<const std::string> rest(words.data(), words.size());
        auto reply = command(s.settings, verb, rest);
        // Keyframed focus: a change of distance or blur is a key at the playhead (made, or moved).
        if (reply.changed && name == "set" && !words.empty() && s.settings.focus == Focus::keys && s.playhead_ok &&
            (lower(words[0]) == "focus.distance" || lower(words[0]) == "focus.blur")) {
            set_focus_key(s.settings, s.playhead, s.settings.focus_distance, s.settings.focus_blur);
            reply.text += std::format(" (key at {:.2f} s)", timeline_seconds(s, s.playhead));
        }
        // Keyed light: likewise a change of its brightness, reach, beam or colour (on, as the keys have it there).
        if (reply.changed && name == "set" && !words.empty() && s.settings.light_keyed && s.playhead_ok &&
            (lower(words[0]) == "light.power" || lower(words[0]) == "light.range" || lower(words[0]) == "light.cone" ||
             lower(words[0]) == "light.warmth")) {
            const bool on = s.settings.light_keys.empty() || light_at(s.settings, s.playhead).on;
            set_light_key(s.settings, s.playhead,
                          {on, s.settings.light_power, s.settings.light_range, s.settings.light_cone, s.settings.light_warmth});
            reply.text += std::format(" (light key at {:.2f} s)", timeline_seconds(s, s.playhead));
        }
        if (reply.changed) {
            s.settings = clamped(s.settings);
            changed(s);
            hold_settings(s, true);
        }
        return reply.text;
    } catch (const std::exception& e) {
        return std::string("error: ") + e.what();
    } catch (...) {
        return "error: Replay Plus could not do that.";
    }
}

bool wants_views(bool editor_open) noexcept {
    auto& s = state();
#if defined(REPLAY_PLUS_TEST_INBOX)
    if (editor_open && s.ready.load(std::memory_order_acquire) && s.skater_probe_until.load(std::memory_order_acquire) > GetTickCount64()) return true;
#endif
    return editor_open && s.ready.load(std::memory_order_acquire) && s.views_wanted.load(std::memory_order_acquire);
}

void view_built(std::uintptr_t description, bool replayed) noexcept {
    auto& s = state();
    try {
        float fov{};
        std::uint8_t own_projection{};
        if (!memory::peek(description + replay_shake::game_view::description_fov, fov) ||
            !memory::peek(description + replay_shake::game_view::description_own_projection, own_projection) || own_projection ||
            !std::isfinite(fov) || fov < 0.02f || fov > 3.1f)
            return;
        const float fixed = s.fixed_fov.load(std::memory_order_acquire);
        if (fixed > 0.02f && std::abs(fixed - fov) > 1e-5f) {
            replay_shake::game_view::set_fov(description, std::clamp(fixed, 0.1f, 3.0f));
            if (!memory::peek(description + replay_shake::game_view::description_fov, fov)) return;
        }
        if (replayed) {
            link::note_fov(fov);
            // The camera the picture is drawn from (the skater's autofocus measures how far away the skater is along it).
            std::array<float, 16> camera{};
            if (memory::peek(description + replay_shake::game_view::description_camera, camera) &&
                std::all_of(camera.begin(), camera.end(), [](float v) { return std::isfinite(v); })) {
                for (std::size_t i = 0; i < camera.size(); ++i) s.camera[i].store(camera[i], std::memory_order_relaxed);
                s.camera_at.store(GetTickCount64(), std::memory_order_release);
#if defined(REPLAY_PLUS_TEST_INBOX)
                // (Test builds: while a video is exported, each replayed view the game builds, for its first frames and
                // then every 30th: which camera the light takes.)
                const auto running = replay_shake::export_frames::capture();
                if (running.running && (running.frames < 12 || running.frames % 30 == 0))
                    logging::write(logging::Level::info, logging::Channel::graphics,
                                   std::format("Replay Plus export view: {} ms, frame {}, camera at {:.2f} {:.2f} {:.2f}, looking {:.2f} {:.2f} {:.2f}, fov {:.3f}",
                                               GetTickCount64(), running.frames, camera[12], camera[13], camera[14], -camera[8], -camera[9],
                                               -camera[10], fov));
#endif
            }
            // Its near and far planes (after the vertical field of view, its two centre offsets and another
            // angle: 0.06 and 11000 on game build 25414733), which the camera light measures the depth by.
            float near_plane{}, far_plane{};
            if (memory::peek(description + 0x114, near_plane) && memory::peek(description + 0x118, far_plane) && near_plane > 0.001f &&
                near_plane < 5.0f && far_plane > 10.0f && far_plane < 1e7f) {
                s.near_plane.store(near_plane, std::memory_order_relaxed);
                s.far_plane.store(far_plane, std::memory_order_relaxed);
            }
            // For the probe: what follows the field of view in the view (its projection's other numbers).
            for (std::size_t i = 0; i < s.probe_floats.size(); ++i) {
                float value{};
                if (memory::peek(description + 0x100 + i * 4, value)) s.probe_floats[i].store(value, std::memory_order_relaxed);
            }
        }
    } catch (...) {}
}

void export_frame(std::uintptr_t encoder, std::uintptr_t pixels, std::uintptr_t pitch) noexcept {
    auto& s = state();
    try {
        if (!s.ready.load(std::memory_order_acquire) || !pixels || !pitch) return;
        const auto now = GetTickCount64();
        std::lock_guard lock(s.export_mutex);
        if (now - s.export_session_at > 3000) {
            s.export_frames = 0;
            s.export_logged = false;
            s.export_gpu_said = false;
            s.export_unsized = 0;
        }
        s.export_session_at = now;
        ++s.export_frames;
        // The frame's size: the game's video frame's, as the render side saw it go to be copied out for the encoder,
        // checked against this export's H.264 setup when it had one (an MP4); the buffer the frame came in is only a
        // bound (link::export_frame_size). Rows padded to 256 bytes as a readback's are; the submit's encoder holds no
        // size of the frame's. (Sized by its buffer, as 1.3 did, an MP4 had every sixth frame go out without the lens and
        // a WebM three frames in six finished as one tall picture written over each other: the game reads its frames back
        // into six buffers in turn and not every one is a buffer of its own, 2026-11-19. A size kept from an earlier
        // export, before that, read and wrote 1920 rows of a 1072-row frame, a WebM after an MP4 of another shape, and the
        // game crashed.)
        const auto setup = replay_shake::export_frames::setup_of_this_export();
        const auto own = own_rows(pixels, pitch);
        const auto sized = link::export_frame_size(pitch, link::video_frames_seen(), link::now_ms(), setup.width, setup.height, own);
        const int width = sized.width, height = sized.height;
        const char* sized_by = sized.by;
        // And all its rows in its buffer, or it goes out as the game made it.
        const int rows = width > 0 && height > 0 ? static_cast<int>(rows_held(pixels, pitch, static_cast<std::uint32_t>(width) * 4u,
                                                                               static_cast<std::uint32_t>(std::clamp(height, 0, 8192))))
                                                 : 0;
        const bool short_buffer = rows < height;
        const bool plausible = !short_buffer && width >= 64 && height >= 64 && width <= 8192 && height <= 8192 &&
                               static_cast<std::uintptr_t>(width) * 4 <= pitch && pitch <= static_cast<std::uintptr_t>(width) * 4 + 4096;
        if (!plausible && s.export_on.load(std::memory_order_acquire)) {
            // Said for the first such frame of an export, and every 300th after: they go out without the lens.
            if (++s.export_unsized == 1 || s.export_unsized % 300 == 0)
                logging::log(logging::Level::info, logging::Channel::graphics,
                             "Replay Plus export: frame {} goes out as the game made it ({} so far): its size from {}{}.", s.export_frames,
                             s.export_unsized, sized_by, short_buffer && height > 0 ? ", but its buffer holds fewer rows" : "");
        }
#if defined(REPLAY_PLUS_TEST_INBOX)
        if (s.export_frames <= 12) {
            // (Test builds: the buffer each of an export's first frames came in, and the size it was given.)
            MEMORY_BASIC_INFORMATION block{};
            std::string where = "?";
            if (VirtualQuery(reinterpret_cast<const void*>(pixels), &block, sizeof block) == sizeof block)
                where = std::format("{} bytes into a block at 0x{:x} (type 0x{:x}), {:.2f} rows from it to its region's end",
                                    pixels - reinterpret_cast<std::uintptr_t>(block.AllocationBase), reinterpret_cast<std::uintptr_t>(block.AllocationBase),
                                    block.Type,
                                    static_cast<double>(reinterpret_cast<std::uintptr_t>(block.BaseAddress) + block.RegionSize - pixels) /
                                        static_cast<double>(pitch));
            std::string seen;
            for (const auto& v : link::video_frames_seen())
                seen += std::format(" {}x{} ({} times, {} ms ago)", v.width, v.height, v.times, link::now_ms() - v.last_ms);
            logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus export frame {}: 0x{:x}, {}; sized {}x{} by {}; video frames seen:{}.",
                         s.export_frames, pixels, where, width, height, sized_by, seen.empty() ? " none" : seen);
        }
        if (!s.export_logged) {
            // (Test builds: where the frame's size might be. The encoder's ints that look like a size, and the buffer's memory
            // region from the frame on.)
            std::string found;
            for (std::uintptr_t off = 0; off < 0x600; off += 4) {
                int a{}, b{};
                if (!memory::peek(encoder + off, a) || !memory::peek(encoder + off + 4, b)) continue;
                if (a >= 320 && a <= 8192 && b >= 180 && b <= 8192 && (static_cast<std::uintptr_t>(a) * 4 <= pitch && pitch < static_cast<std::uintptr_t>(a) * 4 + 256))
                    found += std::format(" +0x{:x}={}x{}", off, a, b);
            }
            MEMORY_BASIC_INFORMATION info{};
            std::string region = "?";
            if (VirtualQuery(reinterpret_cast<const void*>(pixels), &info, sizeof info) == sizeof info) {
                const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
                region = std::format("{} bytes from the frame to its region's end ({:.2f} rows), the region {} bytes from 0x{:x} (allocation 0x{:x}), type 0x{:x}",
                                     end - pixels, static_cast<double>(end - pixels) / static_cast<double>(pitch), info.RegionSize,
                                     reinterpret_cast<std::uintptr_t>(info.BaseAddress), reinterpret_cast<std::uintptr_t>(info.AllocationBase), info.Type);
            }
            logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus export research: encoder sizes{}; buffer: {}.",
                         found.empty() ? " none" : found, region);
        }
#endif
        if (!s.export_logged) {
            s.export_logged = true;
            std::uintptr_t vtable{};
            memory::peek(encoder, vtable);
            std::array<std::uint8_t, 16> first{};
            memory::peek(pixels, first);
            std::string bytes;
            for (const auto b : first) bytes += std::format("{:02x} ", b);
            logging::log(logging::Level::info, logging::Channel::graphics,
                         "Replay Plus export: encoder vtable +0x{:x}, size {}x{} (from {}; this export's H.264 setup {}), pitch {}, its buffer {}, "
                         "holds {} rows{}, first bytes {}; finishing {}.",
                         vtable - s.base, width, height, sized_by,
                         setup.width > 0 ? std::format("{}x{}", setup.width, setup.height) : std::string("none"), pitch,
                         own ? std::format("starts a block of {} rows", own) : std::string("does not start a block of its own"), rows,
                         short_buffer ? " (fewer than its size: not finished)" : "", bytes, plausible && s.export_on.load() ? "on" : "off");
        }
        if (!plausible || !s.export_on.load(std::memory_order_acquire)) return;
        // The pass has been drawing under the game's UI for a few frames: this frame (read back a frame or two
        // after it was drawn) already has everything in it.
        const auto under_ui = link::drawn_under_ui();
        if (under_ui >= 4) {
            if (!s.export_gpu_said) {
                s.export_gpu_said = true;
                logging::log(logging::Level::info, logging::Channel::graphics,
                             "Replay Plus export: the video's frames come with the pass drawn under the game's UI (lens, grade, hood, light).");
            }
            return;
        }
        if (s.export_gpu_said || s.export_frames <= 1) {
            s.export_gpu_said = false;
            logging::log(logging::Level::info, logging::Channel::graphics,
                         "Replay Plus export: the game films apart from what it shows; this export's frames are finished apart (on the GPU when it can).");
        }
        const auto picture = link::picture();
        if (!picture) return;
        float fov = link::fov();
        if (!(fov > 0.02f) || link::now_ms() - link::fov_at() > 2000)
            fov = (picture->settings.fov_mode == FovMode::fixed ? picture->settings.fov
                   : picture->settings.fov_mode == FovMode::automatic ? picture->fov_auto : 65.0f) * lens::pi / 180.0f;
        const auto hood = link::hood();
        auto finish = lens::finish_for(picture->settings, static_cast<float>(width) / static_cast<float>(height), fov, hood ? &hood->hole : nullptr);
        if (hood && hood->width) finish.texture = {hood->rgba.data(), hood->width, hood->height};
        if (!lens::changes(finish)) return;
        // On the GPU, as the screen has it (the camera light too); on the CPU only if that cannot be.
        if (link::finish_export_on_gpu(reinterpret_cast<std::uint8_t*>(pixels), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                                   static_cast<std::uint32_t>(pitch)))
            return;
        lens::Picture frame{reinterpret_cast<std::uint8_t*>(pixels), static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                            static_cast<std::uint32_t>(pitch), true};
        const unsigned threads = std::max(2u, std::thread::hardware_concurrency() > 2 ? std::thread::hardware_concurrency() - 2 : 2u);
        lens::finish(finish, frame, s.export_scratch, threads);
    } catch (...) { /* the frame goes out as the game made it */ }
}
} // namespace dingosdk::replay_plus

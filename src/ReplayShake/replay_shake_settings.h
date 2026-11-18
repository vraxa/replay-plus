#pragma once
#include "replay_shake_model.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Game/Input/controller_bindings.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Replay Shake's settings and its `replayshake` console verbs, with no game access: what is saved
// with the profile, and what each verb does to it given what the client tick last read of the
// replay editor. The runtime (replay_shake.h) owns the one live copy and calls these on the game
// thread; the tests call them directly.
namespace dingosdk::replay_shake {
// How a number the replay editor shows is stored in the game's UI model.
enum class Format : std::uint8_t { f32, f64, i32, u32, i64, count };
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Format::count)> format_names{
    "f32", "f64", "i32", "u32", "i64"};
std::optional<Format> format_named(std::string_view text) noexcept;
std::string_view name(Format format) noexcept;
std::size_t size(Format format) noexcept;
// The number stored in `bytes` (little-endian, size(format) of them).
double decode(Format format, const void* bytes) noexcept;

// A number in the replay editor's UI model: the fields to follow from where it starts, how it is
// stored, and what it is multiplied by to give seconds.
struct Reading {
    std::vector<std::uint32_t> path; // field name hashes, as `ui dump` prints them
    Format format = Format::f32;
    double scale = 1;
    bool operator==(const Reading&) const = default;
};
// Where the replay editor keeps what Replay Shake reads, in the game's UI model. Not known in code:
// each player finds it with the game's own `ui dump` (README.md) and sets it with `replayshake map`.
struct Mapping {
    std::uint32_t root{};                 // schema hash of the root view model every path starts from
    Reading playhead;                     // the replay time on show
    std::vector<std::uint32_t> keyframes; // an array: one element per keyframe
    Reading keyframe_time;                // from each element (no path: the element itself) its time
    Reading selected;                     // the index, from 0, of the keyframe being edited (-1 none)
    bool operator==(const Mapping&) const = default;
};

// Handheld at intensity 0: nothing shakes until it is turned up or a keyframe is given a shake.
Shake default_shake() noexcept;
// A keyframe given its own shake starts as the default, turned up to full if the default is off.
Shake new_key_shake(const Shake& fallback) noexcept;

struct Settings {
    bool enabled = true;
    Shake fallback = default_shake(); // keyframes without their own shake; the whole replay without keyframes
    std::uint32_t seed = 1;           // which noise: another seed, another shake with the same settings
    std::vector<Key> keys;            // sorted by time
    int export_fps = 60;              // frames a second of an export: the game offers 24, 30 and 60
    // The replay editor's cameras, as vtable offsets in Skate.exe: of a camera, or of a camera's
    // director (what moves it, replay_shake_editor.h)...
    std::vector<std::uintptr_t> cameras;
    std::string cameras_build; // ...of this game build (Steam build id): another build moves them
    // Shake gameplay too. Off, a replay camera on show shakes only while a game menu is up or the
    // game hides its UI, as in the replay editor (skate.'s cameras share one vtable, gameplay's too).
    bool gameplay = false;
    // (No controller combo of its own: Replay Plus's panel, in the replay editor, switches it.)
    Mapping mapping;
};
inline constexpr int max_export_fps = 240;
inline constexpr std::size_t max_cameras = 16, max_path = 16;

Json to_json(const Settings& settings);
// Whatever it can read; anything missing or out of range stays at its default.
Settings settings_from_json(const Json& json);

// What the client tick last read of the replay editor.
struct Editor {
    bool open{};                                  // a replay camera is on show
    std::optional<double> playhead;               // seconds
    std::optional<std::vector<double>> keyframes; // seconds, as the editor lists them (none: not readable)
    std::optional<int> selected;                  // index into keyframes
};
// The timeline to shake by. Keyframes and keys sit on the replay's timeline, so without its
// playhead there is none: the default throughout. With it, one point per keyframe while the
// editor's keyframes can be read (none there: the default throughout), otherwise the free keys.
std::vector<Point> points(const Settings& settings, const Editor& editor);

struct Reply {
    std::string text;
    bool changed{}; // the settings changed: save them
};
// One `replayshake` verb and the words after it.
Reply command(Settings& settings, std::string_view verb, std::span<const std::string> words, const Editor& editor);
// The verbs command() takes, for the console's completion and help.
inline constexpr std::array<std::string_view, 16> verbs{"on", "off", "toggle", "default", "key", "add", "remove", "clear",
    "clean", "seed", "fps", "camera", "map", "presets", "list", "gameplay"};
// A controller combo by its buttons' names joined with +, "LB+RB" (Xbox's names or PlayStation's, any
// case: LB RB LT RT L3 R3 A B X Y Up Down Left Right Start Back, L1 R1 L2 R2 Cross Circle Square
// Triangle Options Share Create), or a binding as ReSkate keeps it (a number, as its own `bind`
// commands take it); "0" is none. Nothing when it is neither, or no combo ReSkate can hold.
std::optional<std::uint32_t> parse_binding(std::string_view text);

// A shake as replay_shake.json keeps it, and back from it, for Replay Plus's presets
// (Extension/ReplayPlus): one that names a preset and leaves out its amounts takes that preset's,
// then the intensity, speed and roughness given. Anything missing stays as `base` has it.
Json shake_to_json(const Shake& shake);
Shake shake_from_json(const Json& json, const Shake& base);

// Text forms used by the commands, the menu and the log.
std::string describe(const Shake& shake);
std::string describe(const Mapping& mapping);
std::string seconds(double value);
std::string hex(std::uint64_t value);
std::optional<std::uint64_t> parse_hex(std::string_view text) noexcept;
// "0x1a2b3c4d/0x5e6f7081": field hashes separated by slashes; "." is an empty path.
std::optional<std::vector<std::uint32_t>> parse_path(std::string_view text);
std::string path_text(std::span<const std::uint32_t> path);
} // namespace dingosdk::replay_shake

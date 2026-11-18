#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Replay Shake's camera shake, with no game access: what a shake is, how one keyframe's shake turns
// into the next along the replay's timeline, and how a shake moves a camera.
//
// A shake is smooth noise on six motions of the camera (and an optional zoom). Each keyframe can
// have its own; between two keyframes its strength, roughness and speed turn into the next one's
// the way the keyframe's blend says, as the game's own keyframes blend their field of view and roll.
// Everything is a pure function of the replay's time, so a paused replay holds still, scrubbing
// back shows the same shake again, and an export matches what the editor showed.
namespace dingosdk::replay_shake {

// The motions a shake moves the camera by, in the camera's own frame: turns in degrees, moves in
// metres, zoom in percent of the field of view.
namespace channel {
enum Index : std::size_t { pitch, yaw, roll, sideways, up, forward, zoom, count };
}
inline constexpr std::size_t channel_count = channel::count;
inline constexpr std::array<std::string_view, channel_count> channel_names{
    "pitch", "yaw", "roll", "sideways", "up", "forward", "zoom"};
using Amounts = std::array<float, channel_count>;

enum class Preset : std::uint8_t { handheld, walking, running, vehicle, earthquake, drift, custom, count };
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Preset::count)> preset_names{
    "handheld", "walking", "running", "vehicle", "earthquake", "drift", "custom"};
std::optional<Preset> preset_named(std::string_view name) noexcept;
std::string_view name(Preset preset) noexcept;

// How a keyframe's shake turns into the next keyframe's: eased in and out, evenly, or held until
// the next keyframe and then cut.
enum class Blend : std::uint8_t { smooth, linear, hold, count };
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Blend::count)> blend_names{
    "smooth", "linear", "hold"};
std::optional<Blend> blend_named(std::string_view name) noexcept;
std::string_view name(Blend blend) noexcept;

inline constexpr float max_intensity = 3.0f;
inline constexpr float min_frequency = 0.05f, max_frequency = 30.0f;
// The most each channel may move at intensity 1: 15 degrees, 0.5 m, 20 percent.
inline constexpr Amounts max_amounts{15, 15, 15, 0.5f, 0.5f, 0.5f, 20};

struct Shake {
    Preset preset = Preset::handheld; // where the amounts came from (custom once one is changed)
    float intensity = 1.0f;           // scales every amount: 0 none, 1 the amounts as they are
    float frequency = 1.0f;           // hertz: how fast it moves, its slowest layer
    float roughness = 0.4f;           // 0 smooth to 1 jittery: how much its faster layers add
    Amounts amounts{};                // per channel, at intensity 1
    bool operator==(const Shake&) const = default;
};
// A preset's shake, at intensity 1.
Shake preset_shake(Preset preset) noexcept;
// Takes a preset's frequency, roughness and amounts, keeping the intensity.
void apply_preset(Shake& shake, Preset preset) noexcept;
// Within the limits above; anything not finite becomes the preset's own value.
Shake clamped(const Shake& shake) noexcept;
// Whether it moves nothing.
bool silent(const Shake& shake) noexcept;

// One point of a replay's timeline: a keyframe's time (seconds) and the shake from there on.
struct Point {
    double time{};
    Shake shake;
    Blend blend = Blend::smooth;
};

// The shake at one moment: how far each channel may move (intensity times amount, blended between
// the keyframes either side), its roughness and speed there, and how far its noise has run.
struct Sample {
    Amounts amplitude{};
    float roughness{};
    double frequency{}; // hertz
    double phase{};     // cycles: the integral of the frequency over time, so changing speed never jumps
};
// `points` sorted by time. With none, `fallback` holds for the whole replay. Before the first point
// its shake holds; after the last, the last's does.
Sample sample(std::span<const Point> points, const Shake& fallback, double time) noexcept;

// The camera's offsets at a sample: degrees, metres and percent, per channel.
using Offsets = Amounts;
Offsets offsets(const Sample& sample, std::uint32_t seed) noexcept;

// Replay Plus's shake keys: from `time` (the replay's clock) the shake is on, or off. Before the first
// the shake is the other way round from it. How much of the shake there is at a time, 0 to 1: each key
// fades it in or out over gate_fade seconds of the replay (smoothly). With none, all of it.
struct Gate {
    double time{};
    bool on{true};
    bool operator==(const Gate&) const = default;
};
inline constexpr double gate_fade = 0.2;
float gate_at(std::span<const Gate> gates, double time) noexcept;
// The offsets scaled by a gate's level.
Offsets gated(Offsets offsets, float level) noexcept;
bool still(const Offsets& offsets) noexcept;

// Smooth gradient noise, about -1 to 1, one cycle per unit of x.
float noise(std::uint32_t seed, double x) noexcept;
// Three layers of it, each faster and (with less roughness) weaker than the last.
float layered(std::uint32_t seed, double x, float roughness) noexcept;

// A camera's world matrix (rows right, up, back, position; their fourth lanes kept as they are)
// moved by offsets given in the camera's own frame: pitch turns about its right, yaw about its up,
// roll about its back; sideways, up and forward move along them.
using Matrix = std::array<float, 16>;
Matrix shaken(const Matrix& camera, const Offsets& offsets) noexcept;
// A vertical field of view in radians, zoomed by offsets[channel::zoom] percent.
float zoomed(float fov, const Offsets& offsets) noexcept;

// A keyframe's shake (or a free one, while the game's keyframes cannot be read): from `time` on.
struct Key {
    double time{};
    Shake shake;
    Blend blend = Blend::smooth;
    bool operator==(const Key&) const = default;
};
// Keys sit on the keyframe at their time when one is within this many seconds.
inline constexpr double key_tolerance = 1.0 / 1000.0;
inline constexpr std::size_t max_keys = 256;
// The index of the keyframe a time sits on, if any.
std::optional<std::size_t> keyframe_at(std::span<const double> keyframes, double time) noexcept;
// The timeline to sample. With the game's keyframes, one point per keyframe: the key on it, or the
// default shake. Without them, the keys themselves. Sorted by time either way.
std::vector<Point> timeline(std::span<const double> keyframes, std::span<const Key> keys, const Shake& fallback);
// Keeps keys on their keyframes as the editor moves them: `before` and `after` are the keyframe
// times of two readings. One keyframe dragged (the same number of keyframes, all but one where they
// were) takes its key along. A keyframe added or deleted moves no other, and several changing at
// once is another replay: no key moves. Keys on no keyframe are left alone (they come back when a
// keyframe is at their time again).
void follow(std::vector<Key>& keys, std::span<const double> before, std::span<const double> after);
} // namespace dingosdk::replay_shake

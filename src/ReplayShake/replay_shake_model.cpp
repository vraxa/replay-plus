#include "replay_shake_model.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace dingosdk::replay_shake {
namespace {
template <class Names> std::optional<std::size_t> index_named(const Names& names, std::string_view text) noexcept {
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto& candidate = names[i];
        if (candidate.size() != text.size()) continue;
        bool same = true;
        for (std::size_t c = 0; c < candidate.size() && same; ++c) {
            const char lower = text[c] >= 'A' && text[c] <= 'Z' ? static_cast<char>(text[c] - 'A' + 'a') : text[c];
            same = lower == candidate[c];
        }
        if (same) return i;
    }
    return std::nullopt;
}

// Each preset at intensity 1: frequency (Hz), roughness, then pitch, yaw, roll (degrees), sideways,
// up, forward (metres) and zoom (percent). Hand-tuned against a 60 degree field of view: handheld
// is a camera operator standing still, walking and running follow a skater on foot, vehicle is a
// camera mounted on a car, drift a slow floating drone.
struct PresetValues {
    float frequency, roughness;
    Amounts amounts;
};
constexpr std::array<PresetValues, static_cast<std::size_t>(Preset::custom)> presets{{
    {0.8f, 0.35f, {0.6f, 0.8f, 0.5f, 0.010f, 0.010f, 0.006f, 0}},  // handheld
    {1.6f, 0.45f, {1.0f, 0.9f, 1.0f, 0.020f, 0.035f, 0.012f, 0}},  // walking
    {2.6f, 0.55f, {1.8f, 1.5f, 1.6f, 0.035f, 0.060f, 0.020f, 0}},  // running
    {6.0f, 0.70f, {0.5f, 0.3f, 0.6f, 0.006f, 0.020f, 0.006f, 0}},  // vehicle
    {7.5f, 0.80f, {2.2f, 2.2f, 2.8f, 0.080f, 0.080f, 0.060f, 0}},  // earthquake
    {0.18f, 0.10f, {0.5f, 0.7f, 0.4f, 0.060f, 0.050f, 0.060f, 0}}, // drift
}};

double ease(Blend blend, double u) noexcept {
    switch (blend) {
    case Blend::linear: return u;
    case Blend::hold: return 0;
    default: return u * u * (3 - 2 * u);
    }
}
// The integral of ease from 0 to u: how much of the way to the next keyframe's speed the noise
// has run by u.
double eased_area(Blend blend, double u) noexcept {
    switch (blend) {
    case Blend::linear: return u * u / 2;
    case Blend::hold: return 0;
    default: return u * u * u - u * u * u * u / 2;
    }
}
Amounts amplitude(const Shake& shake) noexcept {
    Amounts result{};
    for (std::size_t c = 0; c < channel_count; ++c) result[c] = shake.intensity * shake.amounts[c];
    return result;
}
struct Motion {
    double frequency;
    float roughness;
};
// The speed and roughness a point moves with. A silent point (nothing to move) takes its
// neighbour's, so a shake fading out to nothing does not speed up or slow down on its way.
Motion motion(std::span<const Point> points, std::size_t index) noexcept {
    const auto own = [](const Shake& shake) { return Motion{shake.frequency, shake.roughness}; };
    if (!silent(points[index].shake)) return own(points[index].shake);
    for (std::size_t i = index; i-- > 0;)
        if (!silent(points[i].shake)) return own(points[i].shake);
    for (std::size_t i = index + 1; i < points.size(); ++i)
        if (!silent(points[i].shake)) return own(points[i].shake);
    return own(points[index].shake);
}

std::uint32_t mix(std::uint32_t x) noexcept {
    // lowbias32 (Chris Wellons): a cheap integer hash with good avalanche.
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
// A gradient (slope) between -1 and 1 at each whole x.
float gradient(std::uint32_t seed, std::int64_t lattice) noexcept {
    const auto low = static_cast<std::uint32_t>(static_cast<std::uint64_t>(lattice));
    const auto high = static_cast<std::uint32_t>(static_cast<std::uint64_t>(lattice) >> 32);
    const auto hash = mix(seed ^ mix(low ^ mix(high + 0x9e3779b9U)));
    return static_cast<float>(hash >> 8) * (2.0f / 16777215.0f) - 1.0f;
}
std::uint32_t channel_seed(std::uint32_t seed, std::size_t channel) noexcept {
    return mix(seed * 0x2545f491U + static_cast<std::uint32_t>(channel) * 0x9e3779b9U + 1U);
}

using Vec = std::array<float, 3>;
Vec row(const Matrix& m, std::size_t r) noexcept { return {m[r * 4], m[r * 4 + 1], m[r * 4 + 2]}; }
Vec add(const Vec& a, const Vec& b) noexcept { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec scale(const Vec& a, float s) noexcept { return {a[0] * s, a[1] * s, a[2] * s}; }
float dot(const Vec& a, const Vec& b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec cross(const Vec& a, const Vec& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec normal(const Vec& a) noexcept {
    const float length = std::sqrt(dot(a, a));
    return length > 1e-6f ? scale(a, 1.0f / length) : a;
}
// Turns `a` and `b` (two axes of an orthonormal frame) about the third by `degrees`, the way a
// right-handed turn about that axis moves them: a goes towards b.
void turn(Vec& a, Vec& b, float degrees) noexcept {
    if (degrees == 0) return;
    const float radians = degrees * std::numbers::pi_v<float> / 180.0f;
    const float c = std::cos(radians), s = std::sin(radians);
    const Vec turned_a = add(scale(a, c), scale(b, s)), turned_b = add(scale(b, c), scale(a, -s));
    a = turned_a;
    b = turned_b;
}
} // namespace

std::optional<Preset> preset_named(std::string_view text) noexcept {
    const auto index = index_named(preset_names, text);
    return index ? std::optional(static_cast<Preset>(*index)) : std::nullopt;
}
std::string_view name(Preset preset) noexcept {
    const auto index = static_cast<std::size_t>(preset);
    return index < preset_names.size() ? preset_names[index] : "custom";
}
std::optional<Blend> blend_named(std::string_view text) noexcept {
    const auto index = index_named(blend_names, text);
    return index ? std::optional(static_cast<Blend>(*index)) : std::nullopt;
}
std::string_view name(Blend blend) noexcept {
    const auto index = static_cast<std::size_t>(blend);
    return index < blend_names.size() ? blend_names[index] : "smooth";
}

Shake preset_shake(Preset preset) noexcept {
    Shake shake;
    apply_preset(shake, preset);
    shake.intensity = 1.0f;
    return shake;
}
void apply_preset(Shake& shake, Preset preset) noexcept {
    const auto index = static_cast<std::size_t>(preset);
    shake.preset = preset;
    if (index >= presets.size()) return; // custom keeps what it has
    shake.frequency = presets[index].frequency;
    shake.roughness = presets[index].roughness;
    shake.amounts = presets[index].amounts;
}
Shake clamped(const Shake& shake) noexcept {
    const auto preset = static_cast<std::size_t>(shake.preset) < presets.size() ? shake.preset : Preset::custom;
    const auto reference = preset_shake(preset == Preset::custom ? Preset::handheld : preset);
    const auto within = [](float value, float low, float high, float fallback) {
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    };
    Shake result;
    result.preset = preset;
    result.intensity = within(shake.intensity, 0, max_intensity, 0);
    result.frequency = within(shake.frequency, min_frequency, max_frequency, reference.frequency);
    result.roughness = within(shake.roughness, 0, 1, reference.roughness);
    for (std::size_t c = 0; c < channel_count; ++c)
        result.amounts[c] = within(shake.amounts[c], 0, max_amounts[c], reference.amounts[c]);
    return result;
}
bool silent(const Shake& shake) noexcept {
    return !(shake.intensity > 0) || std::all_of(shake.amounts.begin(), shake.amounts.end(), [](float a) { return !(a > 0); });
}

Sample sample(std::span<const Point> points, const Shake& fallback, double time) noexcept {
    if (!std::isfinite(time)) time = 0;
    if (points.empty()) return {amplitude(fallback), fallback.roughness, fallback.frequency, time * fallback.frequency};
    const auto& first = points.front();
    if (time < first.time) {
        const auto m = motion(points, 0);
        return {amplitude(first.shake), m.roughness, m.frequency, (time - first.time) * m.frequency};
    }
    double phase = 0;
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& from = points[i];
        const auto& to = points[i + 1];
        const double span = to.time - from.time;
        if (!(span > 0)) continue; // keyframes at the same time: the later one takes over there
        const auto a = motion(points, i), b = motion(points, i + 1);
        if (time < to.time) {
            const double u = (time - from.time) / span;
            const auto w = static_cast<float>(ease(from.blend, u));
            Sample result;
            const auto start = amplitude(from.shake), end = amplitude(to.shake);
            for (std::size_t c = 0; c < channel_count; ++c) result.amplitude[c] = start[c] + (end[c] - start[c]) * w;
            result.roughness = a.roughness + (b.roughness - a.roughness) * w;
            result.frequency = a.frequency + (b.frequency - a.frequency) * w;
            result.phase = phase + span * (a.frequency * u + (b.frequency - a.frequency) * eased_area(from.blend, u));
            return result;
        }
        phase += span * (a.frequency + (b.frequency - a.frequency) * eased_area(from.blend, 1.0));
    }
    const auto& last = points.back();
    const auto m = motion(points, points.size() - 1);
    return {amplitude(last.shake), m.roughness, m.frequency, phase + (time - last.time) * m.frequency};
}

float noise(std::uint32_t seed, double x) noexcept {
    if (!std::isfinite(x)) return 0;
    x = std::clamp(x, -1e15, 1e15);
    const double whole = std::floor(x);
    const auto lattice = static_cast<std::int64_t>(whole);
    const auto f = static_cast<float>(x - whole);
    const float from = gradient(seed, lattice) * f, to = gradient(seed, lattice + 1) * (f - 1.0f);
    const float s = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    // A one-dimensional gradient noise peaks at half a unit; doubled, it spans about -1 to 1.
    return std::clamp(2.0f * (from + (to - from) * s), -1.0f, 1.0f);
}
float layered(std::uint32_t seed, double x, float roughness) noexcept {
    // Each layer runs a little over twice as fast as the one before, offset so that no two pass
    // through zero together. Rougher shakes keep more of the faster layers.
    const float gain = 0.15f + 0.6f * std::clamp(std::isfinite(roughness) ? roughness : 0.0f, 0.0f, 1.0f);
    float total = 0, weight = 1, sum = 0;
    double rate = 1;
    for (std::uint32_t layer = 0; layer < 3; ++layer) {
        total += weight * noise(seed + layer * 0x68e31da4U, x * rate + 0.618 * layer);
        sum += weight;
        weight *= gain;
        rate *= 2.17;
    }
    return total / sum;
}

float gate_at(std::span<const Gate> gates, double time) noexcept {
    if (gates.empty()) return 1.0f;
    if (!std::isfinite(time)) return 1.0f;
    double level = gates.front().on ? 0.0 : 1.0;
    for (const auto& gate : gates) {
        if (time < gate.time) break;
        const double target = gate.on ? 1.0 : 0.0;
        const double u = std::clamp((time - gate.time) / gate_fade, 0.0, 1.0);
        level += (target - level) * (u * u * (3.0 - 2.0 * u));
    }
    return static_cast<float>(std::clamp(level, 0.0, 1.0));
}
Offsets gated(Offsets offsets, float level) noexcept {
    if (level >= 1.0f) return offsets;
    for (auto& value : offsets) value *= std::max(level, 0.0f);
    return offsets;
}

Offsets offsets(const Sample& sample, std::uint32_t seed) noexcept {
    Offsets result{};
    for (std::size_t c = 0; c < channel_count; ++c) {
        const float reach = sample.amplitude[c];
        if (!(reach > 0) || !std::isfinite(reach)) continue;
        // Channels start apart in the noise as well as being seeded apart.
        result[c] = reach * layered(channel_seed(seed, c), sample.phase + 31.7 * static_cast<double>(c), sample.roughness);
    }
    return result;
}
bool still(const Offsets& offsets) noexcept {
    return std::all_of(offsets.begin(), offsets.end(), [](float value) { return !(std::abs(value) > 1e-6f); });
}

Matrix shaken(const Matrix& camera, const Offsets& offsets) noexcept {
    for (const auto value : offsets)
        if (!std::isfinite(value)) return camera;
    Vec right = row(camera, 0), upward = row(camera, 1), back = row(camera, 2);
    const auto position = add(add(add(row(camera, 3), scale(right, offsets[channel::sideways])),
        scale(upward, offsets[channel::up])), scale(back, -offsets[channel::forward]));
    // Roll about the back axis, then pitch about the (rolled) right, then yaw about the (pitched)
    // up. Right-handed: right x up = back, so a pitch turns up towards back, which looks up.
    turn(right, upward, offsets[channel::roll]);
    turn(upward, back, offsets[channel::pitch]);
    turn(back, right, offsets[channel::yaw]);
    // Keep the frame exactly orthonormal: rounding must not build up over a long replay.
    back = normal(back);
    right = normal(add(right, scale(back, -dot(right, back))));
    upward = cross(back, right);
    Matrix result = camera;
    for (std::size_t i = 0; i < 3; ++i) {
        result[i] = right[i];
        result[4 + i] = upward[i];
        result[8 + i] = back[i];
        result[12 + i] = position[i];
    }
    return result;
}
float zoomed(float fov, const Offsets& offsets) noexcept {
    const float percent = offsets[channel::zoom];
    if (!std::isfinite(fov) || !std::isfinite(percent) || percent == 0) return fov;
    return std::clamp(fov * (1.0f + percent / 100.0f), 0.02f, 3.0f);
}

std::optional<std::size_t> keyframe_at(std::span<const double> keyframes, double time) noexcept {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < keyframes.size(); ++i) {
        const double distance = std::abs(keyframes[i] - time);
        if (distance <= key_tolerance && (!best || distance < std::abs(keyframes[*best] - time))) best = i;
    }
    return best;
}
std::vector<Point> timeline(std::span<const double> keyframes, std::span<const Key> keys, const Shake& fallback) {
    std::vector<Point> points;
    if (!keyframes.empty()) {
        points.reserve(keyframes.size());
        for (const double time : keyframes) {
            if (!std::isfinite(time)) continue;
            const Key* on = nullptr;
            for (const auto& key : keys)
                if (std::abs(key.time - time) <= key_tolerance && (!on || std::abs(key.time - time) < std::abs(on->time - time)))
                    on = &key;
            points.push_back(on ? Point{time, on->shake, on->blend} : Point{time, fallback, Blend::smooth});
        }
    } else {
        points.reserve(keys.size());
        for (const auto& key : keys)
            if (std::isfinite(key.time)) points.push_back({key.time, key.shake, key.blend});
    }
    std::stable_sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.time < b.time; });
    return points;
}
void follow(std::vector<Key>& keys, std::span<const double> before, std::span<const double> after) {
    if (before.empty() || before.size() != after.size()) return;
    // Pair up the keyframes still where they were; what is left over is what moved, wherever the
    // editor now lists it.
    std::vector<bool> stayed(after.size());
    std::vector<double> left;
    for (const double time : before) {
        bool found = false;
        for (std::size_t i = 0; i < after.size() && !found; ++i)
            if (!stayed[i] && std::abs(after[i] - time) <= key_tolerance) {
                stayed[i] = true;
                found = true;
            }
        if (!found) left.push_back(time);
    }
    if (left.size() != 1) return;
    std::optional<double> arrived;
    for (std::size_t i = 0; i < after.size(); ++i)
        if (!stayed[i]) arrived = after[i];
    if (!arrived || !std::isfinite(*arrived)) return;
    for (auto& key : keys)
        if (std::abs(key.time - left.front()) <= key_tolerance) key.time = *arrived;
}
} // namespace dingosdk::replay_shake

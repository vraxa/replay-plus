#include "replay_plus_lens.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>

namespace dingosdk::replay_plus::lens {
namespace {
float clamp01(float v) noexcept { return std::clamp(v, 0.0f, 1.0f); }
float smoothstep(float a, float b, float x) noexcept {
    const float t = clamp01((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
// The picture's visible half-height, in q: a 2.39:1 letterbox hides the top and bottom.
float visible_height(const Frame& f, Bars bars) noexcept {
    return bars == Bars::scope ? std::min(1.0f, f.view_aspect / scope_aspect) : 1.0f;
}
} // namespace

namespace {
// How far a picture's see-through middle reaches along a direction in the picture's own units (x, y 0 and up).
float picture_reach(const Hole& hole, float ox, float oy) noexcept {
    const float k = std::atan2(std::abs(oy), std::abs(ox)) / (pi * 0.5f) * static_cast<float>(hole_steps);
    const int i = std::clamp(static_cast<int>(k), 0, static_cast<int>(hole_steps) - 1);
    const float t = std::clamp(k - static_cast<float>(i), 0.0f, 1.0f);
    return hole.radius[static_cast<std::size_t>(i)] + (hole.radius[static_cast<std::size_t>(i) + 1] - hole.radius[static_cast<std::size_t>(i)]) * t;
}
} // namespace

Frame frame_for(const Settings& s, float screen_aspect, float fov, const Hole* hole) {
    Frame f;
    f.fisheye = lens_on(s);
    f.fov = std::clamp(fov, 0.05f, 3.0f);
    f.source_aspect = std::clamp(screen_aspect, 0.5f, 4.0f);
    f.view_aspect = s.bars == Bars::four_three ? 4.0f / 3.0f : f.source_aspect;
    f.zoom = std::max(s.zoom, 1.0f);
    f.circle = std::clamp(s.circle, 0.5f, 3.0f);
    f.squeeze = std::clamp(std::isfinite(s.squeeze) ? s.squeeze : 1.0f, 0.5f, 2.0f);
    f.fit_to_lens = s.fov_mode == FovMode::automatic;
    f.ease = std::clamp(std::isfinite(s.compress) ? s.compress : 0.0f, 0.0f, 0.5f) / (pi * 0.5f);
    // The overlay at its size: a plain scale about the middle, the picture keeping its shape (hood_extent), the
    // circle's edge where the lens sees 90 degrees, that much further out.
    const float size = std::clamp(std::isfinite(s.hood_size) ? s.hood_size : 1.0f, 0.25f, 4.0f);
    if (s.enabled && s.hood == Hood::picture && hole) {
        if (hole->known) f.hole = *hole;
        f.hood_scale = size;
        // A picture made for this window's shape (within 13%: a 3:2 hood in a 4:3 window) fills it, as it was made to.
        const float ia = hole->aspect, va = f.view_aspect;
        f.hood_aspect = ia > 0 && std::max(ia / va, va / ia) > 1.13f ? ia : 0.0f;
    } else if (s.enabled && s.hood == Hood::circle) {
        f.hood_circle = f.circle * size;
    }
    f.fit_to_hood = s.enabled && s.hood_fit && s.hood != Hood::none;
    // The projection, and the scale that puts 90 degrees at the image circle (equisolid: f = circle / sqrt 2,
    // K = sqrt 2 / circle).
    f.curve = std::clamp(std::isfinite(s.curve) ? s.curve : 1.0f, 0.0f, 2.0f);
    f.scale = (f.curve >= 1.0f ? curve_radius(f.curve, pi * 0.5f) : pi * 0.5f) / f.circle;
    if (f.curve > 1.001f) {
        // Bent past equidistant, a lens runs out of angle (asin's 1) a little beyond its 90 degrees: never
        // inside what shows (the window, or what the overlay leaves showing), where it would smear. Only a strongly
        // bent lens on a wide window meets this (the Mk1 past about 160% without 4:3 bars).
        // (Furthest as the lens sees it: across, times the squeeze.)
        float furthest = 0.0f;
        for (int i = 0; i <= 64; ++i) {
            const float phi = static_cast<float>(i) * (pi * 0.5f / 64.0f);
            const float dx = std::cos(phi), dy = std::sin(phi);
            furthest = std::max(furthest, visible_radius(f, dx, dy) * std::sqrt(dx * dx * f.squeeze * f.squeeze + dy * dy));
        }
        f.scale = std::min(f.scale, 0.98f / ((f.curve - 1.0f) * furthest));
    }
    return f;
}

std::array<float, 2> window(const Frame& f) {
    const float inner = f.view_aspect, outer = f.source_aspect;
    return outer > inner ? std::array<float, 2>{inner / outer, 1.0f} : std::array<float, 2>{1.0f, outer / inner};
}

std::array<float, 2> tangents(const Frame& f) {
    const float t = std::tan(f.fov * 0.5f);
    return {t * f.source_aspect, t};
}

float curve_angle(float curve, float x) noexcept {
    if (curve < 0.999f) {
        const float a = 1.0f - curve;
        return std::atan(a * x) / a;
    }
    if (curve > 1.001f) {
        const float b = curve - 1.0f;
        return std::asin(std::min(b * x, 1.0f)) / b;
    }
    return x;
}
float curve_radius(float curve, float theta) noexcept {
    if (curve < 0.999f) {
        const float a = 1.0f - curve;
        return std::tan(std::min(a * theta, 1.5607f)) / a;
    }
    if (curve > 1.001f) {
        const float b = curve - 1.0f;
        return std::sin(std::min(b * theta, lens::pi * 0.5f)) / b;
    }
    return theta;
}
float theta(const Frame& f, float r, float fit) {
    const float x = f.scale * r;
    return f.scales_angles() ? fit * curve_angle(f.curve, x) : curve_angle(f.curve, fit * x);
}

float visible_radius(const Frame& f, float ux, float uy) noexcept {
    float rb = std::min(f.view_aspect / std::max(ux, 1e-5f), 1.0f / std::max(uy, 1e-5f));
    if (f.fit_to_hood) {
        if (f.hood_circle > 0) {
            rb = std::min(rb, f.hood_circle);
        } else if (f.hole.known) {
            // A point (q) r out along (ux, uy) is r (ux / half-width, uy / half-height) of the picture.
            const auto e = hood_extent(f);
            const float ox = ux / e[0], oy = uy / e[1];
            rb = std::min(rb, picture_reach(f.hole, ox, oy) / std::max(std::sqrt(ox * ox + oy * oy), 1e-6f));
        }
    }
    return rb;
}

float fit(const Frame& f) {
    if (!f.fisheye) return 1.0f;
    const auto tg = tangents(f);
    // A true fisheye (its own curve or more) sees no wider than the real lens (fit 1: 90 degrees at its image
    // circle). A lens bent less than that may take in all the game drew: at no bend at all the picture is the
    // game's own. (Auto: no more than the whole lens zoomed, which its field of view was picked for.)
    float best = f.fit_to_lens ? 1.0f / std::max(f.zoom, 1.0f) : f.curve >= 1.0f ? 1.0f : 1.0f / std::max(f.curve * f.curve, 1e-4f);
    const float va = f.view_aspect;
    const auto consider = [&](float phi) {
        const float dx = std::cos(phi), dy = std::sin(phi);
        const float rb = visible_radius(f, dx, dy);
        // The point the lens sees (across, times the squeeze): how far out, and which way the game's
        // frustum is met.
        const float lx = dx * f.squeeze, ly = dy, n = std::sqrt(lx * lx + ly * ly);
        const float ux = lx / std::max(n, 1e-6f), uy = ly / std::max(n, 1e-6f);
        const float lim = std::atan(std::min(tg[0] / std::max(ux, 1e-5f), tg[1] / std::max(uy, 1e-5f)) * 0.998f);
        const float rl = rb * n;
        if (rl <= 1e-6f) return;
        // The angle the lens may see there before its edge compression: what the compression brings to lim
        // (th (1 - ease th) = lim, the nearer root; compressed this hard, nothing it sees leaves the picture).
        const float room = 1.0f - 4.0f * f.ease * lim;
        if (room <= 0.0f) return;
        const float allowed = 2.0f * lim / (1.0f + std::sqrt(room));
        if (f.scales_angles()) {
            // The angle the frustum allows over the angle the lens sees there.
            const float th = curve_angle(f.curve, f.scale * rl);
            if (th > 1e-6f) best = std::min(best, allowed / th);
        } else {
            // The focal length at which this edge point sees just the frustum's edge.
            best = std::min(best, curve_radius(f.curve, allowed) / (f.scale * rl));
        }
    };
    // Every way round the quarter, and exactly where what shows has its corners (the fit is tightest there): the
    // window's corner; where the overlay's edge meets the window's top and its side; and a picture's measured
    // directions, between which its edge is taken as straight.
    constexpr int steps = 256;
    for (int i = 0; i <= steps; ++i) consider(static_cast<float>(i) * (pi * 0.5f / steps));
    consider(std::atan2(1.0f, va));
    if (f.fit_to_hood && f.hood_circle > 0) {
        const float hc = f.hood_circle;
        if (hc > 1.0f && hc * hc - 1.0f < va * va) consider(std::atan2(1.0f, std::sqrt(hc * hc - 1.0f)));
        if (hc > va && hc * hc - va * va < 1.0f) consider(std::atan2(std::sqrt(hc * hc - va * va), va));
    } else if (f.fit_to_hood && f.hole.known) {
        // In the picture's own units (where hood_extent puts it) the window's side is at view aspect / half-width and
        // its top at 1 / half-height; a direction there (psi) is this one in the picture (q).
        const auto e = hood_extent(f);
        const float side_edge = va / std::max(e[0], 1e-3f), top_edge = 1.0f / std::max(e[1], 1e-3f);
        const auto in_q = [&](float psi) { return std::atan2(e[1] * std::sin(psi), e[0] * std::cos(psi)); };
        constexpr float segment = pi * 0.5f / static_cast<float>(hole_steps);
        for (std::size_t k = 0; k <= hole_steps; ++k) {
            const float psi = static_cast<float>(k) * segment;
            consider(in_q(psi)); // (each a corner, however slight, of its edge taken as straight between them)
            if (k == hole_steps) break;
            for (const bool top : {true, false}) {
                // Where its edge crosses the window's top (reach sin psi = edge) or side (reach cos psi = edge).
                const auto past = [&](float at) {
                    return picture_reach(f.hole, std::cos(at), std::sin(at)) * (top ? std::sin(at) : std::cos(at)) > (top ? top_edge : side_edge);
                };
                float a = psi, b = psi + segment;
                const bool from = past(a);
                if (past(b) == from) continue;
                for (int i = 0; i < 24; ++i) {
                    const float m = 0.5f * (a + b);
                    (past(m) == from ? a : b) = m;
                }
                consider(in_q(a));
                consider(in_q(b));
            }
        }
    }
    return f.fit_to_lens ? best : best / std::max(f.zoom, 1.0f);
}

// ---- Settings saved before 2026-11-17 ("true angles") --------------------------------------------------
// The lens as it was then (frame_for, fit and true_ease of that version, word for word): it picked its own fit,
// allowing true_allowance more angle at the edge of what shows than the game's picture reached, and an ease
// that took just that back; the hood was a fixed ellipse (the image circle, stretched with the overlay
// across the window). Only to read such settings as they drew (legacy_true_angles).
namespace {
constexpr float legacy_allowance = 1.12f;
constexpr float legacy_hood_radius = 1.54f, legacy_hood_span = 720.0f / 704.0f;
struct LegacyFrame {
    float fov{}, source_aspect{}, view_aspect{}, zoom{}, circle{}, curve{}, scale{}, squeeze{};
    bool fit_to_lens{};
    float limit_x = 0, limit_y = 0;
    bool scales_angles() const noexcept { return curve >= 1.0f; }
};
std::array<float, 2> legacy_tangents(const LegacyFrame& f) {
    const float t = std::tan(f.fov * 0.5f);
    return {t * f.source_aspect, t};
}
LegacyFrame legacy_frame(const Settings& s, bool mk1_hood, float screen_aspect, float fov) {
    LegacyFrame f;
    f.fov = std::clamp(fov, 0.05f, 3.0f);
    f.source_aspect = std::clamp(screen_aspect, 0.5f, 4.0f);
    f.view_aspect = s.bars == Bars::four_three ? 4.0f / 3.0f : f.source_aspect;
    f.zoom = std::max(s.zoom, 1.0f);
    f.circle = std::clamp(s.circle, 0.5f, 3.0f);
    f.squeeze = std::clamp(std::isfinite(s.squeeze) ? s.squeeze : 1.0f, 0.5f, 2.0f);
    f.fit_to_lens = s.fov_mode == FovMode::automatic;
    if (s.hood_fit) {
        if (mk1_hood) {
            f.limit_x = legacy_hood_radius * f.view_aspect / (4.0f / 3.0f) / legacy_hood_span;
            f.limit_y = legacy_hood_radius;
        } else if (s.hood == Hood::circle) {
            f.limit_x = f.limit_y = f.circle;
        }
    }
    f.curve = std::clamp(std::isfinite(s.curve) ? s.curve : 1.0f, 0.0f, 2.0f);
    f.scale = (f.curve >= 1.0f ? curve_radius(f.curve, pi * 0.5f) : pi * 0.5f) / f.circle;
    if (f.curve > 1.001f) {
        const float across = f.view_aspect * f.squeeze;
        float furthest = std::sqrt(across * across + 1.0f);
        if (f.limit_x > 0 && f.limit_y > 0) furthest = std::min(furthest, std::max(f.limit_x * f.squeeze, f.limit_y));
        f.scale = std::min(f.scale, 0.98f / ((f.curve - 1.0f) * furthest));
    }
    return f;
}
float legacy_theta(const LegacyFrame& f, float r, float fit) {
    const float x = f.scale * r;
    return f.scales_angles() ? fit * curve_angle(f.curve, x) : curve_angle(f.curve, fit * x);
}
float legacy_fit(const LegacyFrame& f) {
    const auto tg = legacy_tangents(f);
    const float va = f.view_aspect;
    float best = f.fit_to_lens ? 1.0f / std::max(f.zoom, 1.0f) : f.curve >= 1.0f ? 1.0f : 1.0f / std::max(f.curve * f.curve, 1e-4f);
    std::array<float, 69> directions{};
    for (int i = 0; i <= 64; ++i) directions[static_cast<std::size_t>(i)] = static_cast<float>(i) * (pi * 0.5f / 64.0f);
    directions[65] = std::atan2(1.0f, va);
    directions[66] = directions[67] = directions[68] = directions[65];
    if (f.limit_x > 0 && f.limit_y > 0) {
        if (f.limit_y > 1.0f) directions[66] = std::atan2(1.0f, f.limit_x * std::sqrt(1.0f - 1.0f / (f.limit_y * f.limit_y)));
        if (f.limit_x > va) directions[67] = std::atan2(f.limit_y * std::sqrt(1.0f - (va * va) / (f.limit_x * f.limit_x)), va);
    }
    for (const float phi : directions) {
        const float dx = std::cos(phi), dy = std::sin(phi);
        float rb = std::min(va / std::max(dx, 1e-5f), 1.0f / std::max(dy, 1e-5f));
        if (f.limit_x > 0 && f.limit_y > 0) {
            const float ex = dx / f.limit_x, ey = dy / f.limit_y;
            rb = std::min(rb, 1.0f / std::sqrt(ex * ex + ey * ey));
        }
        const float lx = dx * f.squeeze, ly = dy, n = std::sqrt(lx * lx + ly * ly);
        const float ux = lx / std::max(n, 1e-6f), uy = ly / std::max(n, 1e-6f);
        const float limit = std::min(tg[0] / std::max(ux, 1e-5f), tg[1] / std::max(uy, 1e-5f)) * 0.998f;
        const float rl = rb * n;
        if (rl <= 1e-6f) continue;
        if (f.scales_angles()) {
            const float th = curve_angle(f.curve, f.scale * rl);
            if (th > 1e-6f) best = std::min(best, legacy_allowance * std::atan(limit) / th);
        } else {
            best = std::min(best, curve_radius(f.curve, std::atan(limit)) / (f.scale * rl));
        }
    }
    return f.fit_to_lens ? best : best / std::max(f.zoom, 1.0f);
}
float legacy_ease(const LegacyFrame& f, float fit) noexcept {
    if (!f.scales_angles()) return 0.0f;
    const auto tg = legacy_tangents(f);
    const float va = f.view_aspect;
    float ease = 0.0f;
    constexpr int steps = 512;
    for (int i = 0; i <= steps + 2; ++i) {
        float phi = static_cast<float>(std::min(i, steps)) * (pi * 0.5f / steps);
        if (i == steps + 1) phi = std::atan2(1.0f, va);
        if (i == steps + 2 && f.limit_x > 0 && f.limit_y > 0 && f.limit_x > va)
            phi = std::atan2(f.limit_y * std::sqrt(1.0f - (va * va) / (f.limit_x * f.limit_x)), va);
        const float ux = std::cos(phi), uy = std::sin(phi);
        float rb = std::min(va / std::max(ux, 1e-5f), 1.0f / std::max(uy, 1e-5f));
        if (f.limit_x > 0 && f.limit_y > 0) {
            const float ex = ux / f.limit_x, ey = uy / f.limit_y;
            rb = std::min(rb, 1.0f / std::sqrt(ex * ex + ey * ey));
        }
        const float lx = ux * f.squeeze, ly = uy, n = std::sqrt(lx * lx + ly * ly);
        const float lim = std::atan(std::min(tg[0] / std::max(lx / n, 1e-5f), tg[1] / std::max(ly / n, 1e-5f)) * 0.998f);
        const float vis = legacy_theta(f, rb * n, fit);
        if (vis > lim && vis > 1e-4f) ease = std::max(ease, (1.0f - lim / vis) / vis);
    }
    return ease;
}
} // namespace

Legacy legacy_true_angles(const Settings& s, bool mk1_hood, float screen_aspect) noexcept {
    Legacy out;
    try {
        float fov = s.fov_mode == FovMode::fixed ? s.fov * pi / 180.0f : 65.0f * pi / 180.0f;
        if (s.fov_mode == FovMode::automatic) {
            // Its Auto then: the narrowest field of view at which the whole lens (zoomed) shows.
            const float most = std::clamp(s.fov_widest, 30.0f, 170.0f) * pi / 180.0f;
            const auto full = [&](float v) {
                const auto f = legacy_frame(s, mk1_hood, screen_aspect, v);
                return legacy_fit(f) * f.zoom >= 0.999f;
            };
            fov = most;
            if (full(most)) {
                float low = 20.0f * pi / 180.0f, high = most;
                if (full(low)) {
                    high = low;
                } else {
                    for (int i = 0; i < 24; ++i) {
                        const float mid = (low + high) * 0.5f;
                        (full(mid) ? high : low) = mid;
                    }
                }
                fov = high;
            }
        }
        const auto f = legacy_frame(s, mk1_hood, screen_aspect, fov);
        const float fit = legacy_fit(f);
        out.compress = std::clamp(legacy_ease(f, fit) * (pi * 0.5f), 0.0f, 0.5f);
        out.zoom = std::clamp(fit > 1e-3f ? 1.0f / fit : 1.0f, 1.0f, 2.0f);
    } catch (...) {
        out = {};
    }
    return out;
}

Hole measure_hole(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height) noexcept {
    Hole hole;
    if (width && height) hole.aspect = static_cast<float>(width) / static_cast<float>(height);
    if (!rgba || width < 8 || height < 8) return hole;
    // 1 opaque (alpha half or more), 0 see-through, -1 off the picture; (u, v) its own -1..1, v up.
    const auto at = [&](float u, float v) {
        const float x = (u + 1.0f) * 0.5f * static_cast<float>(width), y = (1.0f - v) * 0.5f * static_cast<float>(height);
        if (!(x >= 0.0f && y >= 0.0f && x < static_cast<float>(width) && y < static_cast<float>(height))) return -1;
        return rgba[(static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4 + 3] >= 128 ? 1 : 0;
    };
    if (at(0.0f, 0.0f) != 0) return hole; // nothing to see through in the middle
    // Out from the middle in each direction in each quarter, half a pixel at a time: the nearest it turns opaque.
    const float step = 0.5f / static_cast<float>(std::max(width, height)) * 2.0f;
    for (std::size_t k = 0; k <= hole_steps; ++k) {
        const float psi = static_cast<float>(k) * (pi * 0.5f / static_cast<float>(hole_steps)), cu = std::cos(psi), sv = std::sin(psi);
        float nearest = 4.0f;
        for (const float sx : {1.0f, -1.0f})
            for (const float sy : {1.0f, -1.0f})
                for (float t = step; t < 2.0f; t += step) {
                    const int o = at(sx * t * cu, sy * t * sv);
                    if (o < 0) {
                        // See-through to the picture's edge: past it is black (the window can be wider or taller than
                        // the picture, which keeps its shape), so what shows ends there.
                        nearest = std::min(nearest, t - step * 0.5f);
                        break;
                    }
                    if (o > 0) {
                        nearest = std::min(nearest, t);
                        break;
                    }
                }
        hole.radius[k] = nearest;
    }
    hole.known = true;
    return hole;
}

std::array<float, 2> to_game(const Frame& f, float scale, float qx, float qy) {
    const auto tg = tangents(f);
    if (!f.fisheye) return {0.5f + 0.5f * qx / f.source_aspect, 0.5f - 0.5f * qy};
    qx *= f.squeeze;
    const float r = std::max(std::sqrt(qx * qx + qy * qy), 1e-6f);
    float th0 = theta(f, r, scale);
    if (f.ease > 0) {
        // Edge compression: eased in, level past where it would turn back (far beyond anything that shows).
        const float top = 0.5f / f.ease;
        th0 = th0 < top ? th0 * (1.0f - f.ease * th0) : 0.25f / f.ease;
    }
    const float th = std::min(th0, most_theta);
    const float k = std::tan(th) / r;
    return {0.5f + 0.5f * qx * k / tg[0], 0.5f - 0.5f * qy * k / tg[1]};
}

std::array<float, 2> hood_extent(const Frame& f) {
    const float s = std::max(f.hood_scale, 1e-3f), va = f.view_aspect;
    const float ia = f.hood_aspect > 0 ? f.hood_aspect : va;
    // As large as fits whole: the window's height, or its width for a picture wider than it.
    const float k = std::min(1.0f, va / ia);
    return {ia * k * s, k * s};
}
std::array<float, 2> hood_uv(const Frame& f, float qx, float qy) {
    const auto e = hood_extent(f);
    return {0.5f + 0.5f * qx / e[0], 0.5f - 0.5f * qy / e[1]};
}
float scale_for_old_size(const Settings& s, float old_size) noexcept {
    if (!std::isfinite(old_size)) return 1.0f;
    const float size = std::clamp(old_size, 0.0f, 2.0f);
    const float va = s.bars == Bars::four_three || s.replay_aspect == Aspect::four_three ? 4.0f / 3.0f : 16.0f / 9.0f;
    const float corner = std::sqrt(va * va + 1.0f);
    // Where its dark edge was drawn along the window's diagonal: the Mk1 hood's corners (its see-through middle reaching
    // 1.279 of its own half-diagonal, measured 2026-11-19), or the circle.
    const float drawn = s.hood == Hood::circle ? std::clamp(s.circle, 0.5f, 3.0f) : 1.279f * corner / std::sqrt(2.0f);
    if (drawn >= corner) return 1.0f;
    const float sized = std::max(corner - size * (corner - drawn), 0.05f);
    return std::clamp(sized / drawn, 0.25f, 4.0f);
}

float centre_zoom(const Frame& f, float fit) {
    // Near the middle every projection sees theta = fit K r (the fit scales the angles or the focal
    // length, alike there): the picture's half-height spans fit K radians, the game's tan(fov / 2).
    if (!f.fisheye || fit <= 0 || f.scale <= 0) return 1.0f;
    return std::tan(f.fov * 0.5f) / (fit * f.scale);
}
float auto_fov(const Settings& s, float screen_aspect, float widest, const Hole* hole) {
    const float most = std::clamp(widest, 30.0f, 170.0f) * pi / 180.0f;
    if (!lens_on(s)) return most;
    Settings a = s;
    a.fov_mode = FovMode::automatic;
    const auto full = [&](float fov) {
        const auto f = frame_for(a, screen_aspect, fov, hole);
        return fit(f) * f.zoom >= 0.9999f;
    };
    if (!full(most)) return most;
    float low = 20.0f * pi / 180.0f, high = most;
    if (full(low)) return low;
    for (int i = 0; i < 24; ++i) {
        const float mid = (low + high) * 0.5f;
        (full(mid) ? high : low) = mid;
    }
    return high;
}

Glass glass_for(const Settings& s) {
    Glass g;
    if (!lens_on(s)) return g;
    g.fringe = std::clamp(s.fringe, 0.0f, 4.0f);
    g.shade = std::clamp(s.shade, 0.0f, 3.0f);
    g.glare = std::clamp(std::isfinite(s.glare) ? s.glare : 0.0f, 0.0f, 3.0f) * glare_light;
    g.soft = std::clamp(s.soft, 0.0f, 1.0f);
    return g;
}

Grade grade_for(const Settings& s) {
    Grade g;
    if (!s.enabled) return g;
    g.exposure = s.exposure;
    g.contrast = s.contrast;
    g.saturation = s.saturation;
    g.warmth = s.warmth;
    g.tint = s.tint;
    g.black = s.black;
    g.vignette = s.vignette;
    g.camera = s.camera;
    g.camera_gamma = std::clamp(s.camera_gamma, 0.1f, 1.0f);
    g.camera_knee = std::clamp(s.camera_knee, 0.0f, 2.0f);
    g.camera_knee_slope = std::clamp(s.camera_knee_slope, 0.0f, 1.0f);
    g.camera_black = std::clamp(s.camera_black, -0.5f, 0.5f);
    g.camera_clip = std::clamp(s.camera_clip, 0.5f, 2.0f);
    g.camera_colour = std::clamp(s.camera_colour, 0.0f, 3.0f);
    g.vignette_soft = s.vignette_soft;
    g.vignette_round = s.vignette_round;
    g.hue = s.hue;
    g.filter = s.filter;
    g.filter_hue = s.filter_hue;
    g.lift = s.lift;
    g.gamma = s.gamma;
    g.gain = s.gain;
    g.split_shadows = s.split_shadows;
    g.split_shadows_hue = s.split_shadows_hue;
    g.split_highlights = s.split_highlights;
    g.split_highlights_hue = s.split_highlights_hue;
    g.split_balance = s.split_balance;
    g.grain = s.grain;
    g.grain_size = s.grain_size;
    return g;
}

std::array<float, 3> hue_colour(float degrees) noexcept {
    float h = std::fmod(degrees, 360.0f);
    if (h < 0) h += 360.0f;
    h /= 60.0f;
    const auto channel = [&](float offset) { return clamp01(std::abs(std::fmod(h + offset, 6.0f) - 3.0f) - 1.0f); };
    return {channel(0.0f), channel(4.0f), channel(2.0f)};
}
std::array<float, 9> colour_matrix(const Grade& g) noexcept {
    // The hue turned about the grey axis, keeping Rec.709 brightness (SVG's hueRotate, with Rec.709's weights).
    const float a = g.hue * pi / 180.0f, c = std::cos(a), s = std::sin(a);
    const float lr = 0.2126f, lg = 0.7152f, lb = 0.0722f;
    const std::array<float, 9> h{lr + c * (1 - lr) - s * lr,        lg - c * lg - s * lg,         lb - c * lb + s * (1 - lb),
                                 lr - c * lr + s * 0.143f,          lg + c * (1 - lg) + s * 0.140f, lb - c * lb - s * 0.283f,
                                 lr - c * lr - s * (1 - lr),        lg - c * lg + s * lg,         lb + c * (1 - lb) + s * lb};
    // Then the saturation, about the same brightness.
    const float k = g.saturation;
    const std::array<float, 9> sat{lr + (1 - lr) * k, lg - lg * k,       lb - lb * k,
                                   lr - lr * k,       lg + (1 - lg) * k, lb - lb * k,
                                   lr - lr * k,       lg - lg * k,       lb + (1 - lb) * k};
    std::array<float, 9> m{};
    for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col) {
            float v = 0;
            for (int i = 0; i < 3; ++i) v += sat[static_cast<std::size_t>(r * 3 + i)] * h[static_cast<std::size_t>(i * 3 + col)];
            m[static_cast<std::size_t>(r * 3 + col)] = v;
        }
    return m;
}
std::array<float, 3> filter_gains(const Grade& g) noexcept {
    std::array<float, 3> f{1, 1, 1};
    if (g.filter > 0) {
        const auto colour = hue_colour(g.filter_hue);
        for (std::size_t i = 0; i < 3; ++i) f[i] = 1.0f + (colour[i] - 1.0f) * std::clamp(g.filter, 0.0f, 1.0f);
        const float y = std::max(0.2126f * f[0] + 0.7152f * f[1] + 0.0722f * f[2], 1e-3f);
        for (auto& v : f) v /= y;
    }
    const auto wb = balance(g.warmth, g.tint);
    return {f[0] * wb[0], f[1] * wb[1], f[2] * wb[2]};
}
std::array<float, 3> split_offset(float hue, float amount) noexcept {
    if (!(amount > 0)) return {0, 0, 0};
    const auto colour = hue_colour(hue);
    const float y = 0.2126f * colour[0] + 0.7152f * colour[1] + 0.0722f * colour[2];
    const float k = 0.22f * std::clamp(amount, 0.0f, 1.0f);
    return {(colour[0] - y) * k, (colour[1] - y) * k, (colour[2] - y) * k};
}
float vignette_edge(const Grade& g, float qx, float qy, float va) noexcept {
    const float round = std::sqrt(qx * qx + qy * qy) / std::sqrt(va * va + 1.0f);
    const float sx = qx / std::max(va, 1e-3f);
    const float own = std::sqrt(sx * sx + qy * qy) / std::sqrt(2.0f);
    const float k = clamp01(g.vignette_round);
    return own + (round - own) * k;
}
float vignette_power(float soft) noexcept { return 2.5f * std::exp2((0.5f - clamp01(soft)) * 2.0f); }
float grain_noise(float x, float y, float size, float seed) noexcept {
    // Dave Hoskins's hash12, on cells of `size` pixels, smoothly between them (as the shader).
    const auto hash = [](float px, float py) {
        float a = px * 0.1031f, b = py * 0.1031f, c = px * 0.1031f;
        a -= std::floor(a);
        b -= std::floor(b);
        c -= std::floor(c);
        const float d = a * (b + 33.33f) + b * (c + 33.33f) + c * (a + 33.33f);
        a += d;
        b += d;
        c += d;
        const float v = (a + b) * c;
        return v - std::floor(v);
    };
    const float cell = std::max(size, 0.5f);
    const float gx = x / cell + seed * 17.0f, gy = y / cell + seed * 59.0f;
    const float ix = std::floor(gx), iy = std::floor(gy);
    float fx = gx - ix, fy = gy - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const float a = hash(ix, iy), b = hash(ix + 1, iy), c = hash(ix, iy + 1), d = hash(ix + 1, iy + 1);
    const float v = (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
    // Smoothing between cells lowers the spread: brought back for coarse grain.
    return (v - 0.5f) * (1.0f + 0.7f * clamp01(cell - 1.0f));
}

std::array<float, 3> balance(float warmth, float tint) {
    float r = (1.0f + 0.18f * warmth) * (1.0f + 0.05f * tint);
    float g = 1.0f - 0.15f * tint;
    float b = (1.0f - 0.22f * warmth) * (1.0f + 0.05f * tint);
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return {r / y, g / y, b / y};
}

float srgb_to_linear(float v) noexcept {
    v = std::max(v, 0.0f);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
float linear_to_srgb(float v) noexcept {
    v = std::max(v, 0.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

float inverse_filmic(float y) noexcept {
    y = std::clamp(y, 0.0f, 0.995f);
    const float a = 2.43f * y - 2.51f, b = 0.59f * y - 0.03f, c = 0.14f * y;
    return (-b - std::sqrt(std::max(b * b - 4.0f * a * c, 0.0f))) / (2.0f * a);
}
float camera_toe_slope(float gamma) noexcept {
    if (gamma == 0.45f) return 4.5f;
    const float here = 1.099f * std::pow(0.018f, gamma) - 0.099f, rec709 = 1.099f * std::pow(0.018f, 0.45f) - 0.099f;
    return 4.5f * here / rec709;
}
std::array<float, 3> camera_curve(const Grade& g, std::array<float, 3> l) noexcept {
    const float toe = camera_toe_slope(g.camera_gamma);
    for (auto& v : l) {
        v = std::max(v, 0.0f);
        v = v < 0.018f ? toe * v : 1.099f * std::pow(v, g.camera_gamma) - 0.099f;
        if (v > g.camera_knee) v = g.camera_knee + (v - g.camera_knee) * g.camera_knee_slope;
        v = std::min((v + g.camera_black) / (1.0f + g.camera_black), g.camera_clip);
    }
    const float y = 0.2126f * l[0] + 0.7152f * l[1] + 0.0722f * l[2];
    for (auto& v : l) v = clamp01(y + (v - y) * g.camera_colour);
    return l;
}

std::array<float, 3> graded(const Grade& g, std::array<float, 3> c, float edge, float shade, float glare) {
    if (g.camera)
        for (auto& v : c) v = inverse_filmic(v) * shade + glare;
    else
        for (auto& v : c) v = v * shade + glare;
    const float gain = std::exp2(g.exposure);
    const auto wb = filter_gains(g); // white balance and the colour filter
    for (int i = 0; i < 3; ++i) c[i] = std::max(c[i] * gain * wb[i], 0.0f);
    if (g.contrast != 1.0f)
        for (auto& v : c) v = 0.18f * std::pow(std::max(v, 1e-6f) / 0.18f, g.contrast);
    // The hue and the saturation.
    const auto m = colour_matrix(g);
    const std::array<float, 3> in = c;
    for (int r = 0; r < 3; ++r) {
        const auto row = static_cast<std::size_t>(r * 3);
        c[static_cast<std::size_t>(r)] = std::max(m[row] * in[0] + m[row + 1] * in[1] + m[row + 2] * in[2], 0.0f);
    }
    if (g.vignette > 0) {
        const float darken = 1.0f - 0.9f * g.vignette * std::pow(clamp01(edge), vignette_power(g.vignette_soft));
        for (auto& v : c) v *= darken;
    }
    if (g.camera) {
        c = camera_curve(g, c);
        for (auto& v : c) v = srgb_to_linear(v);
    }
    // On the picture's values: lift, gamma and gain, split toning, faded blacks.
    if (g.tones() || g.black > 0) {
        std::array<float, 3> p{linear_to_srgb(c[0]), linear_to_srgb(c[1]), linear_to_srgb(c[2])};
        if (g.lift != 0 || g.gain != 1 || g.gamma != 1)
            for (auto& v : p) {
                v = std::max(v * g.gain + g.lift * (1.0f - v), 0.0f);
                if (g.gamma != 1) v = std::pow(v, 1.0f / std::max(g.gamma, 0.05f));
            }
        if (g.split_shadows > 0 || g.split_highlights > 0) {
            const auto low = split_offset(g.split_shadows_hue, g.split_shadows), high = split_offset(g.split_highlights_hue, g.split_highlights);
            const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            const float pivot = 0.5f - 0.35f * std::clamp(g.split_balance, -1.0f, 1.0f);
            const float w = smoothstep(pivot - 0.35f, pivot + 0.35f, lum);
            for (std::size_t i = 0; i < 3; ++i) p[i] = std::max(p[i] + low[i] * (1.0f - w) + high[i] * w, 0.0f);
        }
        if (g.black > 0)
            for (auto& v : p) v = g.black + v * (1.0f - g.black);
        for (std::size_t i = 0; i < 3; ++i) c[i] = srgb_to_linear(p[i]);
    }
    return c;
}

Finish finish_for(const Settings& s, float screen_aspect, float fov, const Hole* hole) {
    Finish f;
    f.frame = frame_for(s, screen_aspect, fov, hole);
    f.fit = fit(f.frame);
    f.glass = glass_for(s);
    f.grade = grade_for(s);
    f.bars = s.enabled ? s.bars : Bars::none;
    f.hood = s.enabled ? s.hood : Hood::none;
    f.hood_opacity = clamp01(s.hood_opacity);
    return f;
}

bool changes(const Finish& f) noexcept {
    return f.frame.fisheye || !f.grade.identity() || !f.glass.none() || f.bars != Bars::none ||
           (f.hood != Hood::none && f.hood_opacity > 0 && (f.hood == Hood::circle || f.texture.rgba));
}

namespace {
// The trace of every pixel of a picture back into the game's, kept while nothing about it changes
// (most exports film one lens at one field of view throughout).
struct Trace {
    std::uint32_t width{}, height{};
    Frame frame;
    float fit{};
    Bars bars{};
    std::vector<float> uv;   // two per pixel: where in the game's picture (in its pixels); x < -1e8: black
    std::vector<float> foot; // two per pixel: how many of the game's pixels one of ours spans, across and up
};
std::mutex trace_mutex;
Trace& trace() {
    static auto* value = new Trace;
    return *value;
}
bool same(const Frame& a, const Frame& b) noexcept {
    return a.fisheye == b.fisheye && a.fov == b.fov && a.source_aspect == b.source_aspect && a.view_aspect == b.view_aspect && a.zoom == b.zoom &&
           a.circle == b.circle && a.curve == b.curve && a.scale == b.scale && a.squeeze == b.squeeze && a.ease == b.ease &&
           a.fit_to_hood == b.fit_to_hood && a.hole.known == b.hole.known && a.hole.radius == b.hole.radius && a.hood_scale == b.hood_scale && a.hood_aspect == b.hood_aspect &&
           a.hood_circle == b.hood_circle;
}
void build_trace(Trace& t, const Finish& f, std::uint32_t w, std::uint32_t h, unsigned threads) {
    t.width = w;
    t.height = h;
    t.frame = f.frame;
    t.fit = f.fit;
    t.bars = f.bars;
    t.uv.assign(std::size_t{w} * h * 2, 0.0f);
    t.foot.assign(std::size_t{w} * h * 2, 1.0f);
    const auto win = window(f.frame);
    const float va = f.frame.view_aspect;
    const float half = visible_height(f.frame, f.bars);
    const bool lens = f.frame.fisheye;
    const auto rows = [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t y = first; y < last; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t at = (std::size_t{y} * w + x) * 2;
                const float wx = ((static_cast<float>(x) + 0.5f) / static_cast<float>(w) - 0.5f) / win[0] + 0.5f;
                const float wy = ((static_cast<float>(y) + 0.5f) / static_cast<float>(h) - 0.5f) / win[1] + 0.5f;
                const float qx = (wx - 0.5f) * 2.0f * va, qy = 1.0f - 2.0f * wy;
                if (wx < 0 || wx > 1 || wy < 0 || wy > 1 || std::abs(qy) > half) {
                    t.uv[at] = -1e9f;
                    continue;
                }
                if (!lens) {
                    t.uv[at] = static_cast<float>(x) + 0.5f;
                    t.uv[at + 1] = static_cast<float>(y) + 0.5f;
                    continue;
                }
                const float dqx = 2.0f * va / static_cast<float>(w) / win[0], dqy = -2.0f / static_cast<float>(h) / win[1];
                const auto p = to_game(f.frame, f.fit, qx, qy);
                const auto px = to_game(f.frame, f.fit, qx + dqx, qy);
                const auto py = to_game(f.frame, f.fit, qx, qy + dqy);
                t.uv[at] = p[0] * static_cast<float>(w);
                t.uv[at + 1] = p[1] * static_cast<float>(h);
                t.foot[at] = std::hypot((px[0] - p[0]) * static_cast<float>(w), (px[1] - p[1]) * static_cast<float>(h));
                t.foot[at + 1] = std::hypot((py[0] - p[0]) * static_cast<float>(w), (py[1] - p[1]) * static_cast<float>(h));
            }
    };
    if (threads <= 1 || h < 64) {
        rows(0, h);
        return;
    }
    // (Every thread made is joined, even when one cannot be made: a thread left running would end the game. What no
    // thread took is done here.)
    std::vector<std::thread> pool;
    const std::uint32_t each = (h + threads - 1) / threads;
    std::uint32_t handed = 0;
    try {
        pool.reserve(threads);
        for (unsigned i = 0; i < threads; ++i) {
            const std::uint32_t first = i * each, last = std::min(h, first + each);
            if (first >= last) break;
            pool.emplace_back(rows, first, last);
            handed = last;
        }
    } catch (...) {}
    if (handed < h) rows(handed, h);
    for (auto& thread : pool) thread.join();
}

struct Luts {
    std::array<float, 256> decode{};
    std::array<std::uint8_t, 4097> encode{};
    Luts() {
        for (int i = 0; i < 256; ++i) decode[static_cast<std::size_t>(i)] = srgb_to_linear(static_cast<float>(i) / 255.0f);
        for (int i = 0; i <= 4096; ++i)
            encode[static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>(std::clamp(std::lround(linear_to_srgb(static_cast<float>(i) / 4096.0f) * 255.0f), 0L, 255L));
    }
};
const Luts& luts() {
    static const Luts value;
    return value;
}
std::uint8_t encode8(float linear) noexcept {
    const float at = std::clamp(linear, 0.0f, 1.0f) * 4096.0f;
    return luts().encode[static_cast<std::size_t>(at + 0.5f)];
}

// Reading the game's picture (the scratch copy): linear light, clamped to its edge.
struct Source {
    const std::uint8_t* pixels;
    std::uint32_t width, height, pitch;
    std::array<float, 3> texel(int x, int y) const noexcept {
        x = std::clamp(x, 0, static_cast<int>(width) - 1);
        y = std::clamp(y, 0, static_cast<int>(height) - 1);
        const auto* p = pixels + static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * 4;
        const auto& d = luts().decode;
        return {d[p[0]], d[p[1]], d[p[2]]};
    }
    std::array<float, 3> bilinear(float x, float y) const noexcept {
        x -= 0.5f;
        y -= 0.5f;
        const float fx = std::floor(x), fy = std::floor(y);
        const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
        const float ax = x - fx, ay = y - fy;
        const auto a = texel(ix, iy), b = texel(ix + 1, iy), c = texel(ix, iy + 1), d = texel(ix + 1, iy + 1);
        std::array<float, 3> r{};
        for (int i = 0; i < 3; ++i) {
            const auto k = static_cast<std::size_t>(i);
            r[k] = (a[k] * (1 - ax) + b[k] * ax) * (1 - ay) + (c[k] * (1 - ax) + d[k] * ax) * ay;
        }
        return r;
    }
    // Catmull-Rom where the picture is enlarged: sharp, and exact at the game's pixels.
    std::array<float, 3> bicubic(float x, float y) const noexcept {
        x -= 0.5f;
        y -= 0.5f;
        const float fx = std::floor(x), fy = std::floor(y);
        const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
        const auto weights = [](float t) {
            const float t2 = t * t, t3 = t2 * t;
            return std::array<float, 4>{-0.5f * t3 + t2 - 0.5f * t, 1.5f * t3 - 2.5f * t2 + 1.0f, -1.5f * t3 + 2.0f * t2 + 0.5f * t,
                                        0.5f * t3 - 0.5f * t2};
        };
        const auto wx = weights(x - fx), wy = weights(y - fy);
        std::array<float, 3> r{};
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) {
                const auto t = texel(ix - 1 + i, iy - 1 + j);
                const float w = wx[static_cast<std::size_t>(i)] * wy[static_cast<std::size_t>(j)];
                for (std::size_t k = 0; k < 3; ++k) r[k] += t[k] * w;
            }
        for (auto& v : r) v = std::max(v, 0.0f);
        return r;
    }
};

std::array<float, 4> hood_texel(const Hood8& t, float u, float v) noexcept {
    const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(t.width) - 0.5f;
    const float y = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(t.height) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float ax = x - fx, ay = y - fy;
    const auto at = [&](int ix, int iy) {
        ix = std::clamp(ix, 0, static_cast<int>(t.width) - 1);
        iy = std::clamp(iy, 0, static_cast<int>(t.height) - 1);
        const auto* p = t.rgba + (static_cast<std::size_t>(iy) * t.width + static_cast<std::size_t>(ix)) * 4;
        return std::array<float, 4>{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    };
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    const auto a = at(ix, iy), b = at(ix + 1, iy), c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
    std::array<float, 4> r{};
    for (std::size_t k = 0; k < 4; ++k) r[k] = (a[k] * (1 - ax) + b[k] * ax) * (1 - ay) + (c[k] * (1 - ax) + d[k] * ax) * ay;
    return r;
}
} // namespace

void finish(const Finish& f, Picture picture, std::vector<std::uint8_t>& scratch, unsigned threads) noexcept {
    try {
        if (!picture.pixels || picture.width < 16 || picture.height < 16 || picture.pitch < picture.width * 4) return;
        const std::uint32_t w = picture.width, h = picture.height;
        threads = std::clamp(threads, 1u, 32u);
        std::lock_guard lock(trace_mutex);
        auto& t = trace();
        if (t.width != w || t.height != h || !same(t.frame, f.frame) || t.fit != f.fit || t.bars != f.bars) build_trace(t, f, w, h, threads);
        const std::size_t bytes = std::size_t{picture.pitch} * h;
        scratch.resize(bytes);
        std::memcpy(scratch.data(), picture.pixels, bytes);
        const Source source{scratch.data(), w, h, picture.pitch};
        const bool lens = f.frame.fisheye;
        const auto win = window(f.frame);
        const float va = f.frame.view_aspect;
        // The grain's seed: a new one each frame finished.
        static std::atomic<std::uint32_t> frames_finished{0};
        const float seed = static_cast<float>(frames_finished.fetch_add(1, std::memory_order_relaxed) % 1024u);
        // The grade, with the lens's corner shade and glare (which go with it: graded()).
        const bool grade = !f.grade.identity() || (lens && (f.glass.shade > 0 || f.glass.glare > 0));
        const bool circle = f.hood == Hood::circle && f.hood_opacity > 0;
        const bool textured = f.hood == Hood::picture && f.texture.rgba && f.hood_opacity > 0;
        // Channel order: the hood's texture and the grade's warmth are in red, green, blue.
        const std::size_t red = picture.bgra ? 2 : 0, blue = picture.bgra ? 0 : 2;
        // The lens's glass, on what the lens shows (replay_plus.hlsl does the same on the screen): red and
        // blue a little bigger and smaller about the middle (colour fringe), softer towards the edges
        // (a wider average blended in, as wide for the picture's height as on a 1080-line one), and the
        // corners darker.
        const bool glassy = lens && !f.glass.none();
        const float to_1080 = static_cast<float>(h) / 1080.0f;
        const auto glass = [&](std::array<float, 3>& c, float u, float v, float magnified, float qx, float qy) {
            const float rn = std::sqrt(qx * qx + qy * qy) / (5.0f / 3.0f);
            if (f.glass.fringe > 0) {
                // As what moving red out and blue in changes, on the sharp sample: none in the middle.
                const float d = 0.003f * f.glass.fringe;
                const auto out = to_game(f.frame, f.fit, qx * (1 + d), qy * (1 + d));
                const auto in = to_game(f.frame, f.fit, qx * (1 - d), qy * (1 - d));
                const auto here = source.bilinear(u, v);
                c[red] = std::max(0.0f, c[red] + source.bilinear(out[0] * static_cast<float>(w), out[1] * static_cast<float>(h))[red] - here[red]);
                c[blue] = std::max(0.0f, c[blue] + source.bilinear(in[0] * static_cast<float>(w), in[1] * static_cast<float>(h))[blue] - here[blue]);
            }
            if (f.glass.soft > 0) {
                const float spread = f.glass.soft * rn * rn;
                const float o = 0.5f * magnified * std::exp2(1.0f + 3.0f * spread) * to_1080;
                std::array<float, 3> blur{};
                for (const auto& [ox, oy] : {std::pair{-o, -o}, std::pair{o, -o}, std::pair{-o, o}, std::pair{o, o}}) {
                    const auto b = source.bilinear(u + ox, v + oy);
                    for (std::size_t k = 0; k < 3; ++k) blur[k] += b[k] * 0.25f;
                }
                const float mix = std::clamp(spread * 1.5f, 0.0f, 1.0f);
                for (std::size_t k = 0; k < 3; ++k) c[k] += (blur[k] - c[k]) * mix;
            }
            // (The corner shade and the glare go with the grade: lens::graded.)
        };
        const auto rows = [&](std::uint32_t first, std::uint32_t last) {
            for (std::uint32_t y = first; y < last; ++y) {
                auto* out = picture.pixels + static_cast<std::size_t>(y) * picture.pitch;
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::size_t at = (std::size_t{y} * w + x) * 2;
                    auto* px = out + static_cast<std::size_t>(x) * 4;
                    const float u = t.uv[at];
                    if (u < -1e8f) {
                        px[0] = px[1] = px[2] = 0;
                        continue;
                    }
                    const float wx = ((static_cast<float>(x) + 0.5f) / static_cast<float>(w) - 0.5f) / win[0] + 0.5f;
                    const float wy = ((static_cast<float>(y) + 0.5f) / static_cast<float>(h) - 0.5f) / win[1] + 0.5f;
                    const float qx = (wx - 0.5f) * 2.0f * va, qy = 1.0f - 2.0f * wy;
                    std::array<float, 3> c;
                    if (!lens) {
                        c = source.texel(static_cast<int>(x), static_cast<int>(y));
                    } else {
                        const float v = t.uv[at + 1], fx = t.foot[at], fy = t.foot[at + 1];
                        if (std::max(fx, fy) <= 1.25f) {
                            c = source.bicubic(u, v);
                        } else {
                            // The footprint of our pixel in the game's: averaged over a grid along it.
                            const int nx = std::clamp(static_cast<int>(std::ceil(fx)), 1, 6), ny = std::clamp(static_cast<int>(std::ceil(fy)), 1, 6);
                            const float ux = (t.uv[(std::size_t{y} * w + std::min(x + 1, w - 1)) * 2] - u) * (x + 1 < w ? 1.0f : -1.0f);
                            const float uxy = (t.uv[(std::size_t{y} * w + std::min(x + 1, w - 1)) * 2 + 1] - v) * (x + 1 < w ? 1.0f : -1.0f);
                            const std::size_t below = (std::size_t{std::min(y + 1, h - 1)} * w + x) * 2;
                            const float vx = (t.uv[below] - u) * (y + 1 < h ? 1.0f : -1.0f);
                            const float vy = (t.uv[below + 1] - v) * (y + 1 < h ? 1.0f : -1.0f);
                            c = {0, 0, 0};
                            for (int j = 0; j < ny; ++j)
                                for (int i = 0; i < nx; ++i) {
                                    const float oi = (static_cast<float>(i) + 0.5f) / static_cast<float>(nx) - 0.5f;
                                    const float oj = (static_cast<float>(j) + 0.5f) / static_cast<float>(ny) - 0.5f;
                                    const auto s = source.bilinear(u + ux * oi + vx * oj, v + uxy * oi + vy * oj);
                                    for (std::size_t k = 0; k < 3; ++k) c[k] += s[k];
                                }
                            const float n = 1.0f / static_cast<float>(nx * ny);
                            for (auto& value : c) value *= n;
                        }
                        if (glassy) glass(c, u, v, std::max({fx, fy, 1.0f}), qx, qy);
                    }
                    if (grade) {
                        // In red, green, blue order for the grade.
                        std::array<float, 3> rgb{c[red], c[1], c[blue]};
                        const float rq = std::sqrt(qx * qx + qy * qy);
                        const float shade = lens && f.glass.shade > 0
                                                ? std::exp2(-f.glass.shade * std::pow(std::min(rq / (5.0f / 3.0f), 1.5f), 2.5f))
                                                : 1.0f;
                        rgb = graded(f.grade, rgb, vignette_edge(f.grade, qx, qy, va), shade, lens ? f.glass.glare : 0.0f);
                        c[red] = rgb[0];
                        c[1] = rgb[1];
                        c[blue] = rgb[2];
                    }
                    if (f.grade.grain > 0) {
                        // Grain, as the shader has it (more in the shadows and midtones).
                        std::array<float, 3> p{linear_to_srgb(c[0]), linear_to_srgb(c[1]), linear_to_srgb(c[2])};
                        const float lum = 0.2126f * p[red] + 0.7152f * p[1] + 0.0722f * p[blue];
                        const float n = grain_noise(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                                                    std::max(f.grade.grain_size, 0.5f) * static_cast<float>(h) / 1080.0f, seed) *
                                        f.grade.grain * 0.2f * (1.0f - 0.6f * clamp01(lum));
                        for (std::size_t k = 0; k < 3; ++k) c[k] = srgb_to_linear(std::max(p[k] + n, 0.0f));
                    }
                    if (circle) {
                        const float r = std::sqrt(qx * qx + qy * qy);
                        const float dark = smoothstep(f.frame.hood_circle - 0.025f, f.frame.hood_circle + 0.004f, r) * f.hood_opacity;
                        for (auto& value : c) value *= 1.0f - dark;
                    }
                    std::array<std::uint8_t, 3> e{encode8(c[0]), encode8(c[1]), encode8(c[2])};
                    if (textured) {
                        const auto uv = hood_uv(f.frame, qx, qy);
                        // (Past its edges: black, as the shader has it.)
                        const bool past = uv[0] < 0.0f || uv[0] > 1.0f || uv[1] < 0.0f || uv[1] > 1.0f;
                        const auto o = past ? std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f} : hood_texel(f.texture, uv[0], uv[1]);
                        const float a = o[3] * f.hood_opacity;
                        if (a > 0) {
                            const std::array<float, 3> in_order{o[red == 0 ? 0 : 2], o[1], o[red == 0 ? 2 : 0]};
                            for (std::size_t k = 0; k < 3; ++k)
                                e[k] = static_cast<std::uint8_t>(std::clamp(
                                    std::lround(static_cast<float>(e[k]) * (1 - a) + in_order[k] * 255.0f * a), 0L, 255L));
                        }
                    }
                    px[0] = e[0];
                    px[1] = e[1];
                    px[2] = e[2];
                }
            }
        };
        if (threads <= 1 || h < 64) {
            rows(0, h);
            return;
        }
        // (Every thread made is joined, even when one cannot be made: a thread left running would end the game.)
        std::vector<std::thread> pool;
        const std::uint32_t each = (h + threads - 1) / threads;
        std::uint32_t handed = 0;
        try {
            pool.reserve(threads);
            for (unsigned i = 0; i < threads; ++i) {
                const std::uint32_t first = i * each, last = std::min(h, first + each);
                if (first >= last) break;
                pool.emplace_back(rows, first, last);
                handed = last;
            }
        } catch (...) {}
        if (handed < h) rows(handed, h);
        for (auto& thread : pool) thread.join();
    } catch (...) { /* the frame goes out as the game made it */ }
}

// ---- The white of the game's picture under its UI ---------------------------------------------
namespace {
// R11G11B10's 11- and 10-bit floats, and a half float's magnitude: five bits of exponent, no sign.
float small_float(std::uint32_t bits, unsigned mantissa_bits) noexcept {
    const std::uint32_t mantissa = bits & ((1u << mantissa_bits) - 1u), exponent = (bits >> mantissa_bits) & 31u;
    if (exponent == 0) return static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits) / 16384.0f;
    if (exponent == 31) return 65504.0f;
    return std::bit_cast<float>(((exponent + 112u) << 23) | (mantissa << (23u - mantissa_bits)));
}
float half_float(std::uint16_t half) noexcept {
    const float magnitude = small_float(half & 0x7fffu, 10);
    return (half & 0x8000u) ? -magnitude : magnitude;
}
float pq_nits(float encoded) noexcept {
    const float p = std::pow(std::clamp(encoded, 0.0f, 1.0f), 1.0f / 78.84375f);
    return 10000.0f * std::pow(std::max(p - 0.8359375f, 0.0f) / (18.8515625f - 18.6875f * p), 1.0f / 0.1593017578125f);
}
float luma(const std::array<float, 3>& rgb) noexcept { return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]; }
float median_of(std::vector<float>& values) {
    if (values.empty()) return 0.0f;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}
// A block of the screen (16 pixels across a strip's rows) and the picture's texels under it.
struct WhiteBlock {
    std::array<float, 3> shown{}, picture{}; // linear light, 1 the screen's white; the picture's values as they are
    float shown_gamma{};                     // its level through a plain 2.2 gamma (SDR), to compare with sRGB's
    float low{1e30f}, high{};                // its darkest and brightest pixels' levels
};
float level_of(const WhiteBlock& block) noexcept { return luma(block.shown); }
// Even: no edge or texture across it, so the game's scaling of the picture changes nothing there.
bool even_block(const WhiteBlock& block) noexcept {
    const float level = level_of(block);
    return level > 0.0f && block.high - block.low <= 0.25f * level;
}
} // namespace

bool white_format(std::uint32_t format) noexcept {
    return format == 10 || format == 24 || format == 26 || format == 28 || format == 29 || format == 87 || format == 91;
}
std::uint32_t white_texel_bytes(std::uint32_t format) noexcept { return format == 10 ? 8u : 4u; }
std::array<float, 3> white_texel(std::uint32_t format, const std::uint8_t* texel) noexcept {
    std::uint32_t packed{};
    switch (format) {
    case 10: {
        std::array<std::uint16_t, 3> halves{};
        std::memcpy(halves.data(), texel, 6);
        return {half_float(halves[0]), half_float(halves[1]), half_float(halves[2])};
    }
    case 24:
        std::memcpy(&packed, texel, 4);
        return {static_cast<float>(packed & 1023u) / 1023.0f, static_cast<float>((packed >> 10) & 1023u) / 1023.0f,
                static_cast<float>((packed >> 20) & 1023u) / 1023.0f};
    case 26:
        std::memcpy(&packed, texel, 4);
        return {small_float(packed & 0x7ffu, 6), small_float((packed >> 11) & 0x7ffu, 6), small_float(packed >> 22, 5)};
    case 87:
    case 91: return {static_cast<float>(texel[2]) / 255.0f, static_cast<float>(texel[1]) / 255.0f, static_cast<float>(texel[0]) / 255.0f};
    default: return {static_cast<float>(texel[0]) / 255.0f, static_cast<float>(texel[1]) / 255.0f, static_cast<float>(texel[2]) / 255.0f};
    }
}

namespace {
// The strips' blocks: 16 screen pixels across each strip's rows, and the picture's texels under them.
std::vector<WhiteBlock> white_blocks(std::span<const WhiteStrip> strips, int transfer, float white_nits) {
    constexpr std::uint32_t side = 16;
    const float nits = std::max(white_nits, 1.0f);
    // The screen's value in linear light, 1 its white (SDR: sRGB's curve, or a plain 2.2 gamma to compare).
    const auto shown_linear = [transfer, nits](float value, bool gamma) {
        if (transfer == 1) return value * 80.0f / nits;
        if (transfer == 2) return pq_nits(value) / nits;
        return gamma ? std::pow(std::max(value, 0.0f), 2.2f) : srgb_to_linear(value);
    };
    std::vector<WhiteBlock> blocks;
    for (const auto& strip : strips) {
        if (!strip.screen || !strip.picture || !white_format(strip.screen_format) || !white_format(strip.picture_format)) continue;
        if (strip.screen_width < side * 4 || !strip.screen_rows || !strip.picture_width || !strip.picture_rows) continue;
        const double across = static_cast<double>(strip.picture_width) / static_cast<double>(strip.screen_width);
        const auto first_row =
            static_cast<std::uint32_t>(std::clamp(std::floor(strip.picture_from), 0.0, static_cast<double>(strip.picture_rows - 1)));
        const auto end_row = static_cast<std::uint32_t>(
            std::clamp(std::ceil(strip.picture_to), static_cast<double>(first_row + 1), static_cast<double>(strip.picture_rows)));
        const std::uint32_t screen_bytes = white_texel_bytes(strip.screen_format), picture_bytes = white_texel_bytes(strip.picture_format);
        const std::uint32_t margin = strip.screen_width / 20;
        for (std::uint32_t left = margin; left + side + margin <= strip.screen_width; left += side) {
            WhiteBlock block;
            for (std::uint32_t row = 0; row < strip.screen_rows; ++row)
                for (std::uint32_t column = left; column < left + side; ++column) {
                    const auto stored =
                        white_texel(strip.screen_format, strip.screen + std::size_t{row} * strip.screen_pitch + std::size_t{column} * screen_bytes);
                    const std::array<float, 3> light{shown_linear(stored[0], false), shown_linear(stored[1], false), shown_linear(stored[2], false)};
                    for (std::size_t c = 0; c < 3; ++c) block.shown[c] += light[c];
                    const float level = luma(light);
                    block.low = std::min(block.low, level);
                    block.high = std::max(block.high, level);
                    block.shown_gamma += luma({shown_linear(stored[0], true), shown_linear(stored[1], true), shown_linear(stored[2], true)});
                }
            const float shown_count = static_cast<float>(side * strip.screen_rows);
            for (auto& value : block.shown) value /= shown_count;
            block.shown_gamma /= shown_count;
            const auto from_column = static_cast<std::uint32_t>(static_cast<double>(left) * across);
            const auto to_column = std::min(strip.picture_width, std::max(from_column + 1, static_cast<std::uint32_t>(static_cast<double>(left + side) * across)));
            std::size_t picture_count = 0;
            for (std::uint32_t row = first_row; row < end_row; ++row)
                for (std::uint32_t column = from_column; column < to_column; ++column) {
                    const auto stored =
                        white_texel(strip.picture_format, strip.picture + std::size_t{row} * strip.picture_pitch + std::size_t{column} * picture_bytes);
                    for (std::size_t c = 0; c < 3; ++c) block.picture[c] += stored[c];
                    ++picture_count;
                }
            for (auto& value : block.picture) value /= static_cast<float>(std::max<std::size_t>(picture_count, 1));
            blocks.push_back(block);
        }
    }
    return blocks;
}
} // namespace

WhiteMeasure measure_white(std::span<const WhiteStrip> strips, int transfer, float white_nits, std::vector<WhitePair>* pairs) {
    WhiteMeasure m;
    try {
        const auto blocks = white_blocks(strips, transfer, white_nits);
        m.blocks = blocks.size();
        if (pairs) {
            pairs->clear();
            for (const auto& block : blocks)
                pairs->push_back({block.picture[0], block.picture[1], block.picture[2], block.shown[0], block.shown[1], block.shown[2],
                                  even_block(block) ? 1.0f : 0.0f});
        }
        // The white: the picture's level over the screen's, on even blocks in the screen's middle levels (above its
        // darks, below where a display mapping may round off towards white).
        std::vector<float> ratios;
        for (const auto& block : blocks) {
            const float level = level_of(block), source = luma(block.picture);
            if (even_block(block) && level >= 0.04f && level <= 0.7f && source > 0.0f) ratios.push_back(source / level);
        }
        m.used = ratios.size();
        std::string detail = std::format("{} of {} blocks", m.used, m.blocks);
        if (ratios.size() >= 24) {
            std::sort(ratios.begin(), ratios.end());
            const float lower = ratios[ratios.size() / 4], middle = ratios[ratios.size() / 2], upper = ratios[ratios.size() * 3 / 4];
            m.white = middle;
            m.spread = middle > 0.0f ? (upper - lower) / middle : 1.0f;
            m.ok = m.spread <= 0.1f && middle > 0.05f && middle < 100.0f;
            detail += std::format(": white {:.3f} (middle half {:.3f} to {:.3f})", middle, lower, upper);
        }
        // By the screen's level: one ratio all the way up is a plain scale; a ratio rising near the top, a shoulder.
        constexpr std::array<float, 11> edges{0.005f, 0.02f, 0.05f, 0.1f, 0.2f, 0.35f, 0.5f, 0.7f, 0.85f, 0.95f, 1.01f};
        const auto profile = [&blocks, &edges](bool gamma) {
            std::string text;
            for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
                std::vector<float> in_bin;
                for (const auto& block : blocks) {
                    const float level = gamma ? block.shown_gamma : level_of(block), source = luma(block.picture);
                    if (even_block(block) && level >= edges[i] && level < edges[i + 1] && source > 0.0f) in_bin.push_back(source / level);
                }
                if (!in_bin.empty()) text += std::format(" {:.3f}-{:.2f} {:.3f} ({})", edges[i], edges[i + 1], median_of(in_bin), in_bin.size());
            }
            return text;
        };
        detail += "; by level:" + profile(false);
        if (transfer == 0) detail += "; with a 2.2 gamma:" + profile(true);
        // Each channel on its own (a colour matrix in the game's last pass would show here).
        std::array<float, 3> channel{};
        for (std::size_t c = 0; c < 3; ++c) {
            std::vector<float> in_range;
            for (const auto& block : blocks)
                if (even_block(block) && block.shown[c] >= 0.04f && block.shown[c] <= 0.7f && block.picture[c] > 0.0f)
                    in_range.push_back(block.picture[c] / block.shown[c]);
            channel[c] = median_of(in_range);
        }
        detail += std::format("; red {:.3f}, green {:.3f}, blue {:.3f}", channel[0], channel[1], channel[2]);
        // Where the screen reaches its white, and the picture's brightest block.
        float white_from = 0.0f, brightest = 0.0f, brightest_shown = 0.0f;
        for (const auto& block : blocks) {
            const float source = luma(block.picture), level = level_of(block);
            if (level >= 0.985f && (white_from == 0.0f || source < white_from)) white_from = source;
            if (source > brightest) {
                brightest = source;
                brightest_shown = level;
            }
        }
        detail += std::format("; the picture's brightest block {:.3f} shows at {:.3f}", brightest, brightest_shown);
        if (white_from > 0.0f) detail += std::format(", the screen is white from {:.3f}", white_from);
        m.detail = std::move(detail);
    } catch (...) {
        m = {};
    }
    return m;
}

bool WhiteTrack::add(float measured) noexcept {
    if (!(measured > 0.0f) || !std::isfinite(measured)) return false;
    last[count % last.size()] = measured;
    ++count;
    const std::size_t n = std::min(count, last.size());
    auto sorted = last;
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n));
    const float middle = sorted[(n - 1) / 2];
    if (white > 0.0f && std::abs(middle / white - 1.0f) <= 0.01f) return false;
    white = middle;
    return true;
}

// ---- The game's display mapping (SDR) ------------------------------------------------------------
namespace {
float mapping_t(const DisplayMapping& m, float x) noexcept {
    const float w = std::max(m.w, 0.05f);
    const float z = (x - m.x0) / w;
    const float soft = std::max(z, 0.0f) + std::log2(1.0f + std::exp2(-std::abs(z)));
    return m.t0 + m.s1 * x + (m.s2 - m.s1) * w * soft;
}
float mapping_slope(const DisplayMapping& m, float x) noexcept {
    const float w = std::max(m.w, 0.05f);
    return m.s1 + (m.s2 - m.s1) / (1.0f + std::exp2(-(x - m.x0) / w));
}
} // namespace

float display_mapped(const DisplayMapping& mapping, float picture) noexcept {
    if (!(picture > 0.0f)) return 0.0f;
    const float t = mapping_t(mapping, std::log2(picture * std::max(mapping.scale, 1e-6f)));
    return 1.0f / (1.0f + std::exp2(-t));
}
float display_unmapped(const DisplayMapping& mapping, float shown) noexcept {
    if (!(shown > 0.0f)) return 0.0f;
    const float d = std::min(shown, 0.99995f);
    const float t = std::log2(d / (1.0f - d));
    // The curve lies under both its lines (concave, as its slope falls from s1 to s2): Newton from the larger of
    // their inverses starts below the root and closes in from there.
    float x = std::max((t - mapping.t0) / mapping.s1, (t - mapping.t0 - (mapping.s1 - mapping.s2) * mapping.x0) / mapping.s2);
    for (int i = 0; i < 5; ++i) x -= (mapping_t(mapping, x) - t) / mapping_slope(mapping, x);
    return std::exp2(x) / std::max(mapping.scale, 1e-6f);
}

ScaleMeasure measure_scale(std::span<const WhiteStrip> strips, const DisplayMapping& mapping) {
    ScaleMeasure m;
    try {
        const auto blocks = white_blocks(strips, 0, 80.0f);
        m.blocks = blocks.size();
        DisplayMapping curve = mapping;
        curve.scale = 1.0f;
        // Near-grey even blocks (a saturated colour's weakest channel shows a little lighter than the curve says),
        // each channel the screen shows between its darks and its whites.
        const auto near_grey = [](const WhiteBlock& block) {
            const float least = std::min({block.picture[0], block.picture[1], block.picture[2]});
            const float most = std::max({block.picture[0], block.picture[1], block.picture[2]});
            return least > 0.0f && most <= 1.25f * least;
        };
        std::vector<float> scales;
        for (const auto& block : blocks) {
            if (!even_block(block) || !near_grey(block)) continue;
            for (std::size_t c = 0; c < 3; ++c)
                if (block.shown[c] >= 0.02f && block.shown[c] <= 0.95f) scales.push_back(display_unmapped(curve, block.shown[c]) / block.picture[c]);
        }
        m.used = scales.size();
        std::string detail = std::format("{} channels of {} blocks", m.used, m.blocks);
        if (scales.size() >= 20) {
            std::sort(scales.begin(), scales.end());
            const float lower = scales[scales.size() / 4], middle = scales[scales.size() / 2], upper = scales[scales.size() * 3 / 4];
            m.scale = middle;
            m.spread = middle > 0.0f ? (upper - lower) / middle : 1.0f;
            m.ok = m.spread <= 0.08f && middle > 0.25f && middle < 4.0f;
            detail += std::format(": the picture goes into the game's display mapping at {:.3f} (middle half {:.3f} to {:.3f})", middle, lower, upper);
            // How closely the curve, at that scale, follows the screen at each level (the screen over the curve).
            curve.scale = middle;
            constexpr std::array<float, 9> edges{0.005f, 0.02f, 0.05f, 0.1f, 0.2f, 0.4f, 0.7f, 0.9f, 1.01f};
            detail += "; the screen against it by level:";
            for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
                std::vector<float> in_bin;
                for (const auto& block : blocks) {
                    if (!even_block(block) || !near_grey(block)) continue;
                    for (std::size_t c = 0; c < 3; ++c)
                        if (block.shown[c] >= edges[i] && block.shown[c] < edges[i + 1])
                            in_bin.push_back(block.shown[c] / std::max(display_mapped(curve, block.picture[c]), 1e-6f));
                }
                if (!in_bin.empty())
                    detail += std::format(" {:.3f}-{:.2f} {:+.1f}% ({})", edges[i], edges[i + 1], (median_of(in_bin) - 1.0f) * 100.0f, in_bin.size());
            }
        }
        m.detail = std::move(detail);
    } catch (...) {
        m = {};
    }
    return m;
}
} // namespace dingosdk::replay_plus::lens

#pragma once
#include "replay_plus_settings.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// The lens and the picture's finishing, with no game and no GPU: the same maths the shader runs
// (replay_plus.hlsl), here for the fit (worked out once a frame on the render thread) and for
// exported frames (finished on the CPU before the game encodes them: the game films its own frame,
// before ReSkate's overlay pass is drawn, so the shader never reaches a video).
//
// Picture coordinates (q): from the middle of the picture's window, in half its height, y up. A fisheye
// adapter is equidistant, theta = K * r, K = (pi/2) / circle: 90 degrees off-axis at its image circle
// (5/3: the corner of the 4:3 picture). A modern fisheye is equisolid, r = 2 f sin(theta / 2) with
// f = circle / sqrt(2): 90 degrees at its image circle. The game renders a plain (rectilinear) picture of a vertical field of
// view; each point of the fisheye's picture is traced back into it. `fit` scales the lens down until
// every point that shows lands inside what the game drew.
//
// How strongly a lens bends (`curve`, from the Distortion setting) picks its projection from one
// family, x being the picture radius in the lens's units (K r): curve 0 is rectilinear (theta = atan x,
// the game's own picture: straight lines stay straight), 0.5 stereographic, 1 equidistant (an adapter),
// 1.5 equisolid (the modern fisheye), 2 orthographic. Between them, theta = atan(a x) / a with
// a = 1 - curve, or asin(b x) / b with b = curve - 1. Distortion is the curve itself (100% an adapter's 1,
// 150% the modern fisheye's 1.5: one lens, set by its projection and its image circle). Squeeze takes
// in more across: a picture point's across part is multiplied by it before the lens sees it.
//
// From equidistant up, K puts 90 degrees at the lens's image circle and the fit scales the angles: at
// Distortion 100% and 150% the lenses are the plain equidistant and equisolid ones. Below equidistant no radius sees 90 degrees; there the fit
// scales the focal length instead (so 0% stays rectilinear at any fit), with K as at equidistant: the
// two meet at curve 1. Past equidistant, K is held down where the lens would otherwise run out of
// angle inside what shows (frame_for).
namespace dingosdk::replay_plus::lens {
inline constexpr float pi = 3.14159265358979323846f;
inline constexpr float mk1_k = (pi * 0.5f) / (5.0f / 3.0f);
// Points traced back further off-axis than this (89 degrees) are outside what the game drew anyway (the
// fit keeps what shows inside it): held here so they cannot blow up (tan of 90 degrees).
inline constexpr float most_theta = 1.55f;
inline constexpr float scope_aspect = 2.39f;

// An overlay picture's see-through middle, measured from its alpha (where it turns half opaque), as how far out
// it reaches in 1025 directions from across (0) to up (90 degrees), in the picture's own half-widths and
// half-heights (where it is drawn: hood_extent): the nearest of the four quarters; where it is see-through to
// the picture's edge, the edge (black past it). (Finely: where its black meets the window's edge the lens is fitted tightest, and taken as
// straight between directions its edge there is a pixel out at most: at 65 it was ten, 2026-11-17.) Not
// `known`: the picture has no see-through middle (or was not measured).
inline constexpr std::size_t hole_steps = 1024;
struct Hole {
    std::array<float, hole_steps + 1> radius{};
    bool known{};
    float aspect{}; // the picture's width over its height (0: not measured)
};
Hole measure_hole(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height) noexcept;

struct Frame {
    bool fisheye{};           // Replay Plus's lens is on (settings.h lens_on); otherwise the game's own picture
    float fov = 1.0f;          // the game's vertical field of view, radians
    float source_aspect = 16.0f / 9.0f; // the game's picture (and the screen)
    float view_aspect = 16.0f / 9.0f;   // the picture's window: 4:3 with side bars, else the screen's
    float zoom = 1.0f;
    float circle = 5.0f / 3.0f;  // where the lens sees 90 degrees (its image circle), half-heights
    float curve = 1.0f;          // the projection (0 rectilinear .. 1 equidistant .. 2 orthographic)
    float scale = mk1_k;         // the lens's units per half-height (K)
    float squeeze = 1.0f;        // across, times this before the lens
    // Auto's field of view: the zoom is of the whole lens (fit at most 1 / zoom, the game's field of
    // view picked to feed just that: auto_fov), not of what the field of view feeds.
    bool fit_to_lens{};
    // How the fit acts: on the angles (curve 1 and up) or on the focal length (below).
    bool scales_angles() const noexcept { return curve >= 1.0f; }
    // Edge compression (Settings::compress), per radian: an angle th off-axis is seen at th (1 - ease th), the
    // same every way round (circles stay circles), exact at the middle.
    float ease{};
    // What shows: the window, and in it what the overlay leaves showing when the lens is fitted to it (the
    // fit keeps what shows inside the game's picture): a picture's see-through hole (where hood_extent puts it)
    // or the circle's.
    bool fit_to_hood{};
    Hole hole;               // Hood::picture
    float hood_scale = 1.0f; // the overlay this much larger about the middle (Settings::hood_size, a plain scale)
    float hood_aspect = 0;   // Hood::picture: its width over its height as drawn (0: the window's, which it fills)
    float hood_circle = 0;   // Hood::circle: where its black begins, half-heights (0: none)
};
// Where what shows ends along a direction (q, unit, x and y 0 and up), in half-heights: the window's edge, or
// the overlay's when the lens is fitted to it.
float visible_radius(const Frame& frame, float ux, float uy) noexcept;
// Replay Plus's lens before 2026-11-17 picked its own fit and ease with "true angles" on: what they came to,
// for settings saved then (the Mk1's hood as its fixed ellipse then), as the zoom and edge compression that
// draw the same now (settings.cpp migrate).
struct Legacy {
    float zoom = 1.0f, compress = 0.0f;
};
Legacy legacy_true_angles(const Settings& settings, bool mk1_hood, float screen_aspect) noexcept;
// The angle off-axis a projection of this curve sees at lens radius x, and back.
float curve_angle(float curve, float x) noexcept;
float curve_radius(float curve, float theta) noexcept;
// The frame for these settings on a screen of this aspect, at the game's field of view (radians); `hole`: the
// overlay picture's, measured (Hood::picture).
Frame frame_for(const Settings& settings, float screen_aspect, float fov, const Hole* hole = nullptr);
// The window the picture is shown in, as a fraction of the screen (1, 1: all of it): side bars at 4:3.
std::array<float, 2> window(const Frame& frame);
// How far off-axis the lens sees at picture radius r, at a fit.
float theta(const Frame& frame, float r, float fit = 1.0f);
// The widest fit (at most 1, the real lens) at which every point that shows is inside the game's picture
// (with the edge compression), divided by the zoom.
float fit(const Frame& frame);
// A picture position (q) to the game's picture, 0..1 (x right, y down).
std::array<float, 2> to_game(const Frame& frame, float fit, float qx, float qy);
// Where the overlay picture is in the window: its half-width and half-height, in half-heights (q). It keeps its own
// shape (never stretched), as large as fits whole in the window, then hood_scale times that about the middle; past
// its edges its edges carry on. (A picture within 13% of the window's shape was made for it: it fills the window.)
std::array<float, 2> hood_extent(const Frame& frame);
// Where the overlay picture is read for a picture position (0..1), as hood_extent puts it.
std::array<float, 2> hood_uv(const Frame& frame, float qx, float qy);
// Overlay size before 1.3 was how far the overlay's dark edge reached in from the window's corner (1 as drawn, 0
// none, 2 twice as far), not a scale: the scale that draws it so, for these settings (a picture taken as a camera
// hood whose dark corners are the Mk1's; the circle as its own). Settings saved before are read with it.
float scale_for_old_size(const Settings& settings, float old_size) noexcept;
// tan(half the field of view), across and up (skate. measures it vertically).
std::array<float, 2> tangents(const Frame& frame);
// How much the lens enlarges the middle of the game's picture (the picture's pixels per one of the
// game's there, up and down), at a fit: 1 without a fisheye. The game's own depth of field is drawn in
// its picture, so it is enlarged as much.
float centre_zoom(const Frame& frame, float fit);
// Auto field of view: the narrowest (vertical, radians) at which the lens shows all it sees (its fit reaches
// 1), but no wider than `widest`; for these settings on a screen of this aspect.
float auto_fov(const Settings& settings, float screen_aspect, float widest, const Hole* hole = nullptr);

// The colour grade (Replay Plus's own), on light in 0..1 (sRGB decoded): exposure, contrast about
// mid grey (in the picture's perceptual values), saturation, warmth and tint as white balance, faded
// blacks, and a soft vignette. Identity at the defaults.
// The lens's own character, on the picture it makes: colour fringe (red and blue scaled about the
// middle, 0.3% at 1 for each), corner shade (stops at the 4:3 corner, rising as r^2.5) and
// edge softness (a blur that grows towards the edges).
struct Glass {
    float fringe = 0, shade = 0, soft = 0;
    float glare = 0; // veiling glare: scene light (camcorder) or picture light added before the grade
    bool none() const noexcept { return fringe == 0 && shade == 0 && soft == 0 && glare == 0; }
};
// The camera curve (Settings::camera): the game's filmic tone curve undone (the inverse of the Narkowicz ACES
// fit it is), then the camera's own curve, giving display code values: a
// Rec.709-shaped power curve (its gamma, with a linear toe below 0.018), a knee (above it the values rise by
// the knee's slope only), the black level (pedestal), the white clip and the colour level about Y'. At its
// defaults it is a Rec.709 camcorder's. Veiling glare at 1 is
// 0.0018 of scene light (0.01 of a typical frame's mean).
float inverse_filmic(float display_linear) noexcept;
struct Grade;
std::array<float, 3> camera_curve(const Grade& grade, std::array<float, 3> scene) noexcept;
// The toe's slope for a gamma: Rec.709's 4.5 at 0.45, and in step with the power curve's value at 0.018 else.
float camera_toe_slope(float gamma) noexcept;
inline constexpr float glare_light = 0.0018f;
Glass glass_for(const Settings& settings);
struct Grade {
    float exposure = 0, contrast = 1, saturation = 1, warmth = 0, tint = 0, black = 0, vignette = 0;
    bool camera = false;
    float camera_gamma = 0.45f, camera_knee = 0.85f, camera_knee_slope = 0.30f, camera_black = -0.02f, camera_clip = 1.04f, camera_colour = 1.05f;
    float vignette_soft = 0.5f, vignette_round = 1.0f;
    float hue = 0, filter = 0, filter_hue = 30;
    float lift = 0, gamma = 1, gain = 1;
    float split_shadows = 0, split_shadows_hue = 220, split_highlights = 0, split_highlights_hue = 40, split_balance = 0;
    float grain = 0, grain_size = 1; // (not graded(): it goes on the finished pixels, apply_grain)
    bool tones() const noexcept { return lift != 0 || gamma != 1 || gain != 1 || split_shadows > 0 || split_highlights > 0; }
    bool identity() const noexcept {
        return !camera && exposure == 0 && contrast == 1 && saturation == 1 && warmth == 0 && tint == 0 && black == 0 && vignette == 0 &&
               hue == 0 && filter == 0 && !tones() && grain == 0;
    }
};
// The colour wheel's full colour at a hue (degrees), red, green, blue 0..1.
std::array<float, 3> hue_colour(float degrees) noexcept;
// The grade's colour matrix (rows; on linear light after the contrast): the hue turned (keeping Rec.709
// brightness) and the saturation.
std::array<float, 9> colour_matrix(const Grade& grade) noexcept;
// The colour filter's gains (brightness kept), times white balance's.
std::array<float, 3> filter_gains(const Grade& grade) noexcept;
// Split toning's offsets on the picture's values (sRGB-like 0..1), for the shadows and the highlights:
// the tint's colour less its brightness, so a tint changes hue and not brightness.
std::array<float, 3> split_offset(float hue, float amount) noexcept;
// The vignette's distance from the middle: 1 at the window's corner, its roundness between the window's
// own shape (0) and a circle (1).
float vignette_edge(const Grade& grade, float qx, float qy, float view_aspect) noexcept;
// The vignette's exponent for its softness (2.5 at the default 0.5, as before).
float vignette_power(float soft) noexcept;
// Film grain (the same in replay_plus.hlsl): a moving value noise, -0.5..0.5, at an output pixel, cells of
// `size` pixels, a seed for the frame.
float grain_noise(float x, float y, float size, float seed) noexcept;
Grade grade_for(const Settings& settings);
// White balance gains (red, green, blue) for warmth and tint, normalised to keep brightness.
std::array<float, 3> balance(float warmth, float tint);
// One pixel: linear light in, linear light out; `edge` 0 in the middle to 1 at the corner (vignette).
// `shade`: the lens's corner shade there (a factor), and `glare` the veiling glare's light: both on the
// scene's light with the camera curve, else on the picture's before it.
std::array<float, 3> graded(const Grade& grade, std::array<float, 3> rgb, float edge, float shade = 1.0f, float glare = 0.0f);
float srgb_to_linear(float v) noexcept;
float linear_to_srgb(float v) noexcept;

// An exported frame finished on the CPU: the lens, the grade, the bars and the hood, as the shader
// draws them on the screen. 8-bit pixels, four to a pixel (`bgra`: blue first), rows `pitch` bytes
// apart, in place. The hood is its texture (8-bit RGBA, straight alpha), or none.
struct Picture {
    std::uint8_t* pixels{};
    std::uint32_t width{}, height{}, pitch{};
    bool bgra{};
};
struct Hood8 {
    const std::uint8_t* rgba{};
    std::uint32_t width{}, height{};
};
struct Finish {
    Frame frame;
    float fit{1};
    Glass glass;
    Grade grade;
    Bars bars = Bars::none;
    Hood hood = Hood::none;
    float hood_opacity = 1;
    Hood8 texture; // Hood::picture
};
Finish finish_for(const Settings& settings, float screen_aspect, float fov, const Hole* hole = nullptr);
// Whether finishing changes anything.
bool changes(const Finish& finish) noexcept;
// Finishes `picture` in place, on up to `threads` threads (the scratch copy is `scratch`, kept by the
// caller between frames). Never throws.
void finish(const Finish& finish, Picture picture, std::vector<std::uint8_t>& scratch, unsigned threads) noexcept;

// ---- The white of the game's picture under its UI ---------------------------------------------
// In the replay editor the pass draws into the game's picture before its UI, and the game's last pass
// shows that picture through its own display mapping: its white is not 1 (in SDR it was about 2.16 on
// 2026-11-18, so the camera curve's white showed at 181 of 255 and the picture's bright half went flat
// and grey, while exports, finished from the game's 8-bit frame, were right). The renderer copies a few
// rows of the screen and of the picture it put in for that same frame; these read them: the picture's
// value the screen shows as its white (SDR's 1, HDR's paper white). Formats are DXGI_FORMAT's numbers:
// R16G16B16A16_FLOAT 10, R10G10B10A2_UNORM 24, R11G11B10_FLOAT 26, R8G8B8A8_UNORM(_SRGB) 28 and 29,
// B8G8R8A8_UNORM(_SRGB) 87 and 91.
bool white_format(std::uint32_t format) noexcept;
std::uint32_t white_texel_bytes(std::uint32_t format) noexcept;
// One texel's red, green and blue as stored (no transfer undone).
std::array<float, 3> white_texel(std::uint32_t format, const std::uint8_t* texel) noexcept;
// One strip: rows of the screen, and the rows of the picture the game's last pass made them from (the
// picture at its own size, the same shape). `picture_from` / `picture_to`: where the screen's rows fall
// among the picture's copied rows.
struct WhiteStrip {
    const std::uint8_t* screen{};
    std::uint32_t screen_format{}, screen_pitch{}, screen_width{}, screen_rows{};
    const std::uint8_t* picture{};
    std::uint32_t picture_format{}, picture_pitch{}, picture_width{}, picture_rows{};
    double picture_from{}, picture_to{};
};
struct WhiteMeasure {
    bool ok{};           // enough even blocks, and they agree
    float white{};       // the picture's value the screen shows as its white
    float spread{};      // the middle half of the blocks' ratios, as a share of `white`
    std::size_t blocks{}, used{};
    std::string detail;  // how the ratio goes with the level and by channel, and where the screen reaches white
};
// One block's means: the picture's red, green and blue, the screen's (linear, 1 its white), and whether it is even.
using WhitePair = std::array<float, 7>;
// The screen's transfer: 0 SDR (sRGB), 1 scRGB (80-nit units), 2 HDR10 (PQ); `white_nits` its paper white in HDR.
// `pairs`, when given, gets every block (for `replayplus white dump`).
WhiteMeasure measure_white(std::span<const WhiteStrip> strips, int transfer, float white_nits, std::vector<WhitePair>* pairs = nullptr);
// The game's display mapping in SDR: its last pass shows the picture before its UI through a filmic curve, on
// each channel on its own (a curve on brightness alone missed the screen by 2.5% on saturated colours, per
// channel by 0.4%). Measured 2026-11-18 with `replayplus white dump` (build 25414733 as it comes: display mapping
// version 5, SDR peak 200 nits, world brightness 0): in t = log2(d / (1 - d)) over x = log2(p), a line of slope
// s1 in the toe bending to s2 in the shoulder about x0, w wide,
//     t(x) = t0 + s1 x + (s2 - s1) w log2(1 + 2^((x - x0) / w)),
// half of 1722 measurements within 0.6% of it and nine in ten within 1.5%, from p 0.03 to 17 (d 0.005 to 0.94).
// `scale`: what the picture is multiplied by before the curve (1 as the game comes; measured while drawing, in
// case a setting of the game's changes it). replay_plus.hlsl's dm_decode / dm_encode are the same maths.
struct DisplayMapping {
    float t0{9.50878f}, s1{2.87576f}, s2{0.98509f}, x0{-5.10907f}, w{1.14271f};
    float scale{1.0f};
};
// The screen's linear light (1 its white) the game shows a picture value as, and the picture value it shows a
// screen's linear light from (d up to 0.99995, the picture's then about 26000).
float display_mapped(const DisplayMapping& mapping, float picture) noexcept;
float display_unmapped(const DisplayMapping& mapping, float shown) noexcept;
// The scale the picture goes into the curve at, from rows as measure_white reads them (an SDR screen): on even,
// near-grey blocks, each channel's picture value against the one the curve says the screen shows; ok when enough
// agree. `detail` says how closely the curve follows the screen, by level.
struct ScaleMeasure {
    bool ok{};
    float scale{};
    float spread{}; // the middle half of the pairs' scales, as a share of `scale`
    std::size_t blocks{}, used{};
    std::string detail;
};
ScaleMeasure measure_scale(std::span<const WhiteStrip> strips, const DisplayMapping& mapping);
// The white in use, from measurements in turn: the median of the last few, so one odd measurement (the
// game's UI over much of the picture for a moment) does not move it.
struct WhiteTrack {
    std::array<float, 5> last{};
    std::size_t count{};
    float white{}; // 0 until the first
    // Adds one; true when the white in use moved (by more than 1%).
    bool add(float measured) noexcept;
};
} // namespace dingosdk::replay_plus::lens

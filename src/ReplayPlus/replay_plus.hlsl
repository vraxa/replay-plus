// Replay Plus's picture pass (replay_plus_render.cpp): the replay editor's finished frame, re-projected
// through a fisheye lens, graded, barred and hooded, ahead of ReSkate's own drawing. Built into the DLL
// as text (ReplayPlus.cmake) and compiled when it is first drawn; the build checks it with fxc.
//
// The lens: picture
// coordinates q (from the middle of the picture's window, in half its height, y up) traced back into
// the game's plain picture. Where the fisheye enlarges the game's picture (the middle) the edge-directed
// interpolation after AMD FidelityFX FSR 1 EASU (MIT licence) keeps edges crisp; where it squeezes it
// (the edges) the footprint is averaged in linear light from a mip chain. replay_plus_lens.cpp has the
// same maths for exported frames (the game films its frame before this pass is drawn).
//
// Passes: ps_decode (the frame -> linear light, mip 0), ps_down (each further mip), ps_main (to the
// screen).

cbuffer ReplayPlusPass : register(b0)
{
    float4 P0; // lens (0 none, 2 the fisheye), fit, tan(half FOV) across, tan(half FOV) up
    float4 P1; // view aspect (the window, half-widths per half-height), window scale x, window scale y, visible half-height (q)
    float4 P2; // screen width, height, transfer (0 SDR, 1 scRGB or linear, 2 HDR10, 3 the game's SDR display mapping), white (paper white, in the transfer's units)
    float4 P3; // the circle overlay's edge (half-heights), its darkness (0 none), the picture overlay's strength (0 none), mips in the linear chain
    float4 P4; // exposure gain, contrast, saturation, faded blacks
    float4 P5; // white balance gains r, g, b, vignette
    float4 P6; // pass the game's pixels through as they are (no lens, no grade), the lens's curve, its scale (K), colour fringe
    float4 P7; // (none: the camera light drawn over the picture, gone in 1.3.2; kept so the rest keep their places)
    float4 P8; // (none, likewise)
    float4 P9; // (none, likewise)
    float4 P10; // the lens's corner shade (stops), edge softness, squeeze (across, before the lens), its edge compression's ease (per radian; 0 none)
    float4 P11; // the camera curve (1 on), the lens's veiling glare (light added before the grade), the picture overlay's half-height (q), the camera's toe slope
    float4 P12; // the colour matrix's first row (hue and saturation), lift
    float4 P13; // its second row, gamma
    float4 P14; // its third row, gain
    float4 P15; // split toning: the shadows' offset r, g, b, balance
    float4 P16; // split toning: the highlights' offset r, g, b; film grain
    float4 P17; // grain size (pixels), the frame's seed, vignette power, vignette roundness
    float4 P18; // the camera curve: gamma, knee, knee slope, black level
    float4 P19; // the camera curve: white clip, colour level; the enlarged middle's sharpening (0 none, 1 the most); the picture overlay's half-width (q)
    float4 P20; // the game's display mapping (transfer 3): t0, its toe's slope s1, its shoulder's slope s2, the bend x0
    float4 P21; // the bend's width w, log2 of the scale the picture is taken at; (research, test builds: the display probe's set, the screen's width)
};
Texture2D source : register(t0); // ps_decode: the frame's copy; ps_down: the mip above
Texture2D linear_chain : register(t1); // ps_main: the frame in linear light, with its mips
Texture2D hood : register(t3);  // the hood's picture (sRGB colour, straight alpha)
// The game's display mapping as measured, and its inverse (replay_plus_display_table.h; dm_shown, dm_encode).
Texture3D display_forward : register(t4);
Texture3D display_inverse : register(t5);
SamplerState linear_clamp : register(s0);
SamplerState point_clamp : register(s1);

static const float PI = 3.14159265359;
static const float MK1_K = (PI * 0.5) / (5.0 / 3.0);

struct Interpolated { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };

Interpolated vs_main(uint id : SV_VertexID)
{
    Interpolated o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = p;
    return o;
}

// ---- transfer: the swap chain's values <-> linear light, 1 its paper white (P2.w) --------------
// The grade (the camcorder curve's undoing of the game's tone curve above all) takes 1 as white. On an
// HDR display PQ's nits and scRGB's 80-nit units went in as they were, and drawn over the screen (when
// the game's picture before its UI is not to be had, as while a video is filmed) the picture came out
// one flat grey: its white written back as 1 nit (2026-11-17). In exports P2.w is 1.
// Under the game's UI the picture is the game's own before its last pass, which shows it through its display
// mapping. Taken as linear light with 1 as white, the camera curve's white showed at 181 of 255 and the picture's
// bright half went flat and grey (2026-11-18). In SDR (transfer 3) that mapping is applied to read the picture and
// undone to write it back, so the grade sees what an exported frame shows; in HDR (transfer 1) P2.w is the
// picture's white, measured. Undone as a filmic curve on each channel (replay_plus_lens.h DisplayMapping), bright
// saturated colours came out pale (sky 137,166,239 for the video's 69,140,242, 2026-11-20): the game's mapping mixes
// the channels. It is measured now, as a table (dm_shown).
float3 srgb_to_linear(float3 c)
{
    c = max(c, 0.0);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float3 linear_to_srgb(float3 c)
{
    c = max(c, 0.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
float3 pq_to_linear(float3 e)
{
    e = pow(saturate(e), 1.0 / 78.84375);
    return 10000.0 * pow(max(e - 0.8359375, 0.0) / (18.8515625 - 18.6875 * e), 1.0 / 0.1593017578125);
}
float3 linear_to_pq(float3 nits)
{
    float3 y = pow(saturate(nits / 10000.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
// The game's display mapping (transfer 3): in t = log2(d / (1 - d)) over x = log2(p), a line of slope s1 in the
// toe bending to s2 in the shoulder about x0, w wide; replay_plus_lens.cpp has the same (display_mapped).
float3 dm_t(float3 x)
{
    float w = max(P21.x, 0.05);
    float3 z = (x - P20.w) / w;
    float3 soft = max(z, 0.0) + log2(1.0 + exp2(-abs(z)));
    return P20.x + P20.y * x + (P20.z - P20.y) * w * soft;
}
float3 dm_slope(float3 x)
{
    float w = max(P21.x, 0.05);
    return P20.y + (P20.z - P20.y) / (1.0 + exp2(-(x - P20.w) / w));
}
float dm_curve(float x)
{
    return 1.0 / (1.0 + exp2(-dm_t(float3(x, x, x)).x));
}
// The curve is per channel; the game's mapping is not (it lifts the other channels of a bright, saturated colour
// toward white, and pushes them down in the mids). So the screen's colour for a picture value is read from what it
// was measured to be (replay_plus_display_table.h): x = log2 of the value (at the scale it goes in at) -> the
// screen's linear light (1 its white). Below the table (1/64) a channel's own part goes on down as the curve's
// does, at the slope of the table's first half stop; what the others lift it by stays.
float3 dm_shown(float3 x)
{
    float3 f = clamp((x + 6.0) * 2.0, 0.0, 26.0);
    float3 shown = srgb_to_linear(display_forward.SampleLevel(linear_clamp, (f + 0.5) / 27.0, 0).rgb);
    [branch] if (any(x < -6.0))
    {
        float at = dm_curve(-6.0);
        float step = dm_curve(-5.5) - at;
        [unroll] for (int c = 0; c < 3; ++c)
        {
            if (x[c] < -6.0)
            {
                float3 g = f;
                g[c] = 1.0;
                float up = srgb_to_linear(display_forward.SampleLevel(linear_clamp, (g + 0.5) / 27.0, 0).rgb)[c];
                shown[c] = max(shown[c] - (up - shown[c]) / step * (at - dm_curve(x[c])), 0.0);
            }
        }
    }
    return shown;
}
// The picture's value -> the screen's linear light.
float3 dm_decode(float3 p)
{
    return dm_shown(log2(max(p, 1e-12)) + P21.y);
}
// The screen's linear light -> the picture's value that shows it, or the nearest colour the mapping can show (in
// sRGB). The per-channel curve's answer first (it lies under both its lines: Newton from the larger of their
// inverses starts below the root and, the curve being concave, closes in from there); that picks the place in the
// inverse table, and two Gauss-Newton steps on the measured mapping finish it (a step kept only when it comes closer).
float3 dm_encode(float3 d)
{
    float3 shown = clamp(d, 1e-7, 0.99995);
    float3 t = log2(shown / (1.0 - shown));
    float3 x = max((t - P20.x) / P20.y, (t - P20.x - (P20.y - P20.z) * P20.w) / P20.z);
    [unroll] for (int i = 0; i < 5; ++i)
        x -= (dm_t(x) - t) / dm_slope(x);
    float3 f = (clamp(x, -9.0, 7.5) + 9.0) / 16.5 * 32.0;
    x = display_inverse.SampleLevel(linear_clamp, (f + 0.5) / 33.0, 0).rgb * 32.0 - 16.0;
    float3 want = linear_to_srgb(max(d, 0.0));
    float3 now = linear_to_srgb(dm_shown(x));
    [unroll] for (int k = 0; k < 2; ++k)
    {
        float3 e = now - want;
        float3x3 J; // J[i][j]: what sRGB channel i does as x's channel j moves
        [unroll] for (int j = 0; j < 3; ++j)
        {
            float3 moved = x;
            moved[j] += 0.05;
            float3 column = (linear_to_srgb(dm_shown(moved)) - now) / 0.05;
            J[0][j] = column.x;
            J[1][j] = column.y;
            J[2][j] = column.z;
        }
        float3x3 A = mul(transpose(J), J) + float3x3(1e-4, 0, 0, 0, 1e-4, 0, 0, 0, 1e-4);
        float3 b = -mul(transpose(J), e);
        float det = determinant(A);
        if (abs(det) > 1e-20)
        {
            float3 c0 = A[0], c1 = A[1], c2 = A[2]; // A is symmetric: its rows are its columns
            float3 delta = float3(dot(b, cross(c1, c2)), dot(c0, cross(b, c2)), dot(c0, cross(c1, b))) / det;
            float3 next = clamp(x + clamp(delta, -2.0, 2.0), -14.0, 7.0);
            float3 then = linear_to_srgb(dm_shown(next));
            if (dot(then - want, then - want) < dot(e, e))
            {
                x = next;
                now = then;
            }
        }
    }
    return d > 0.0 ? exp2(x - P21.y) : 0.0;
}
float3 decode(float3 c)
{
    float transfer = P2.z;
    float white = max(P2.w, 1e-3);
    float3 result = srgb_to_linear(c);
    if (transfer > 2.5) result = dm_decode(c);
    else if (transfer > 1.5) result = pq_to_linear(c) / white;
    else if (transfer > 0.5) result = c / white;
    return result;
}
float3 encode(float3 x)
{
    float transfer = P2.z;
    float3 result = linear_to_srgb(x);
    if (transfer > 2.5) result = dm_encode(max(x, 0.0));
    else if (transfer > 1.5) result = linear_to_pq(max(x, 0.0) * P2.w);
    else if (transfer > 0.5) result = x * P2.w;
    return result;
}
// Perceptual 0..1, for the edge-directed interpolation: sRGB and PQ already are; scRGB goes to PQ. Under the
// display mapping the linear light is the screen's, as in SDR.
float3 to_perceptual(float3 linear_value)
{
    float transfer = P2.z;
    float3 result = linear_to_srgb(linear_value);
    if (transfer > 2.5) result = linear_to_srgb(linear_value);
    else if (transfer > 1.5) result = linear_to_pq(max(linear_value, 0.0) * P2.w);
    else if (transfer > 0.5) result = linear_to_pq(max(linear_value, 0.0) * 80.0 * P2.w);
    return result;
}
float3 from_perceptual(float3 p)
{
    float transfer = P2.z;
    float3 result = srgb_to_linear(p);
    float white = max(P2.w, 1e-3);
    if (transfer > 2.5) result = srgb_to_linear(p);
    else if (transfer > 1.5) result = pq_to_linear(p) / white;
    else if (transfer > 0.5) result = pq_to_linear(p) / (80.0 * white);
    return result;
}
// The hood's sRGB colour as the swap chain shows it (at paper white in HDR; through the game's display mapping
// under its UI).
float3 overlay_colour(float3 v)
{
    float transfer = P2.z;
    float3 result = v;
    if (transfer > 2.5) result = dm_encode(srgb_to_linear(v));
    else if (transfer > 1.5) result = linear_to_pq(srgb_to_linear(v) * P2.w);
    else if (transfer > 0.5) result = srgb_to_linear(v) * P2.w;
    return result;
}

// ---- pass 1: the frame in linear light ------------------------------------------------------
float4 ps_decode(Interpolated i) : SV_Target
{
    float3 c = decode(source.Load(int3(int2(i.position.xy), 0)).rgb);
    return float4(c, 1.0);
}

// ---- pass 2: the mips (a 2x2 box of the one above) --------------------------------------------
float4 ps_down(Interpolated i) : SV_Target
{
    uint sw, sh;
    source.GetDimensions(sw, sh);
    int2 last = int2(sw, sh) - 1;
    int2 p = int2(i.position.xy) * 2;
    float3 a = source.Load(int3(min(p, last), 0)).rgb;
    float3 b = source.Load(int3(min(p + int2(1, 0), last), 0)).rgb;
    float3 c = source.Load(int3(min(p + int2(0, 1), last), 0)).rgb;
    float3 d = source.Load(int3(min(p + int2(1, 1), last), 0)).rgb;
    return float4((a + b + c + d) * 0.25, 1.0);
}

// ---- pass 3: lens, grade, bars and hood -----------------------------------------------------
// The lens's projection, from one family (replay_plus_lens.h): its curve 0 is rectilinear, 1
// equidistant (the Mk1), 1.5 equisolid (a modern fisheye), 2 orthographic.
float curve_angle(float x)
{
    float curve = P6.y;
    float result = x;
    if (curve < 0.999)
    {
        float a = 1.0 - curve;
        result = atan(a * x) / a;
    }
    else if (curve > 1.001)
    {
        float b = curve - 1.0;
        result = asin(min(b * x, 1.0)) / b;
    }
    return result;
}
float lens_theta(float r)
{
    // From equidistant up the fit scales the angles; below, the
    // focal length (replay_plus_lens.h).
    float x = P6.z * r;
    return P6.y >= 1.0 ? P0.y * curve_angle(x) : curve_angle(P0.y * x);
}
// Edge compression (Settings::compress): the lens's angles eased in, th (1 - ease th), the same every way round,
// the middle as it is (replay_plus_lens.cpp to_game).
float true_angle(float th)
{
    float ease = max(P10.w, 1e-6);
    float top = 0.5 / ease;
    return th < top ? th * (1.0 - ease * th) : 0.25 / ease;
}
float2 to_game(float2 q)
{
    float2 tg = P0.zw;
    q.x *= P10.z;
    float r = max(length(q), 1e-6);
    float th = lens_theta(r);
    if (P10.w > 0.0)
        th = true_angle(th);
    th = min(th, 1.55);
    float2 t = q * (tan(th) / r);
    return float2(0.5 + 0.5 * t.x / tg.x, 0.5 - 0.5 * t.y / tg.y);
}

float3 easu_fetch(int2 p)
{
    int2 size = int2(P2.xy);
    return to_perceptual(linear_chain.Load(int3(clamp(p, int2(0, 0), size - 1), 0)).rgb);
}
float easu_luma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }
void easu_set(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = 1.0 / max(max(abs(dc), abs(cb)), 1e-5);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    len += lenX * lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = 1.0 / max(max(abs(ec), abs(ca)), 1e-5);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    len += lenY * lenY * w;
}
void easu_tap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len2, float lob, float clp, float3 c)
{
    float2 v = float2(off.x * dir.x + off.y * dir.y, off.y * dir.x - off.x * dir.y) * len2;
    float d2 = min(dot(v, v), clp);
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    float w = (1.5625 * wB - 0.5625) * wA;
    aC += c * w;
    aW += w;
}
// The enlarged middle's sharpening: where the fisheye enlarges the game's picture, each of its pixels is spread
// over several of the screen's and the game's own softness (its temporal antialiasing) is enlarged with them:
// skater textures looked like mush. Each pixel the interpolation reads is sharpened first, after AMD FidelityFX
// FSR 1 RCAS (MIT licence): a negative lobe on its four neighbours, as strong as it can be before any channel
// would pass black or white (no clipping, so no blocks), half as strong where the pixel stands out from all four
// alike (noise, a sparkle), times `amount`. On perceptual values, as the interpolation works.
float3 rcas(float3 up, float3 left, float3 e, float3 right, float3 down, float amount)
{
    float3 mn4 = min(min(up, left), min(right, down));
    float3 mx4 = max(max(up, left), max(right, down));
    float3 hit_min = min(mn4, e) / max(4.0 * mx4, 1e-5);
    float3 hit_max = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobes = max(-hit_min, hit_max);
    float lobe = max(-0.1875, min(max(lobes.r, max(lobes.g, lobes.b)), 0.0)) * amount;
    float uL = easu_luma(up), lL = easu_luma(left), eL = easu_luma(e), rL = easu_luma(right), dL = easu_luma(down);
    float range = max(max(max(uL, lL), max(eL, rL)), dL) - min(min(min(uL, lL), min(eL, rL)), dL);
    float noise = saturate(abs(0.25 * (uL + lL + rL + dL) - eL) / max(range, 1e-5));
    lobe *= 1.0 - 0.5 * noise;
    return (lobe * (up + left + right + down) + e) / (4.0 * lobe + 1.0);
}
float3 sample_easu(float2 uv, float sharpen)
{
    float2 pp = uv * P2.xy - 0.5;
    float2 fp = floor(pp);
    pp -= fp;
    int2 q = int2(fp);
    float3 b = easu_fetch(q + int2(0, -1)), c = easu_fetch(q + int2(1, -1));
    float3 e = easu_fetch(q + int2(-1, 0)), f = easu_fetch(q), g = easu_fetch(q + int2(1, 0)), h = easu_fetch(q + int2(2, 0));
    float3 i = easu_fetch(q + int2(-1, 1)), j = easu_fetch(q + int2(0, 1)), k = easu_fetch(q + int2(1, 1)), l = easu_fetch(q + int2(2, 1));
    float3 n = easu_fetch(q + int2(0, 2)), o = easu_fetch(q + int2(1, 2));
    // The edges' direction and length from the pixels as the game drew them; what is filtered, sharpened.
    float bL = easu_luma(b), cL = easu_luma(c), eL = easu_luma(e), fL = easu_luma(f), gL = easu_luma(g), hL = easu_luma(h);
    float iL = easu_luma(i), jL = easu_luma(j), kL = easu_luma(k), lL = easu_luma(l), nL = easu_luma(n), oL = easu_luma(o);
    [branch] if (sharpen > 0.0)
    {
        // The ring of pixels around the twelve, which their outer ones are sharpened against.
        float3 b0 = easu_fetch(q + int2(0, -2)), c0 = easu_fetch(q + int2(1, -2));
        float3 a1 = easu_fetch(q + int2(-1, -1)), d1 = easu_fetch(q + int2(2, -1));
        float3 e0 = easu_fetch(q + int2(-2, 0)), h0 = easu_fetch(q + int2(3, 0));
        float3 i0 = easu_fetch(q + int2(-2, 1)), l0 = easu_fetch(q + int2(3, 1));
        float3 m2 = easu_fetch(q + int2(-1, 2)), p2 = easu_fetch(q + int2(2, 2));
        float3 n3 = easu_fetch(q + int2(0, 3)), o3 = easu_fetch(q + int2(1, 3));
        float3 sb = rcas(b0, a1, b, c, f, sharpen), sc = rcas(c0, b, c, d1, g, sharpen);
        float3 se = rcas(a1, e0, e, f, i, sharpen), sf = rcas(b, e, f, g, j, sharpen);
        float3 sg = rcas(c, f, g, h, k, sharpen), sh = rcas(d1, g, h, h0, l, sharpen);
        float3 si = rcas(e, i0, i, j, m2, sharpen), sj = rcas(f, i, j, k, n, sharpen);
        float3 sk = rcas(g, j, k, l, o, sharpen), sl = rcas(h, k, l, l0, p2, sharpen);
        float3 sn = rcas(j, m2, n, o, n3, sharpen), so = rcas(k, n, o, p2, o3, sharpen);
        b = sb; c = sc; e = se; f = sf; g = sg; h = sh;
        i = si; j = sj; k = sk; l = sl; n = sn; o = so;
    }
    float2 dir = 0.0;
    float len = 0.0;
    easu_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    easu_set(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    easu_set(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    easu_set(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    float dirR = dot(dir, dir);
    bool zro = dirR < (1.0 / 32768.0);
    dirR = zro ? 1.0 : rsqrt(dirR);
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    len *= 0.5;
    len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;
    float3 aC = 0.0;
    float aW = 0.0;
    easu_tap(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, b);
    easu_tap(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, c);
    easu_tap(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, i);
    easu_tap(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, j);
    easu_tap(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, f);
    easu_tap(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, e);
    easu_tap(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, k);
    easu_tap(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, l);
    easu_tap(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, h);
    easu_tap(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, g);
    easu_tap(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, o);
    easu_tap(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, n);
    float3 mn = min(min(f, g), min(j, k));
    float3 mx = max(max(f, g), max(j, k));
    return min(mx, max(mn, aC / max(aW, 1e-5)));
}

// White balance, exposure, contrast about mid grey, saturation, vignette, faded blacks: as
// replay_plus_lens.cpp's graded(), on linear light.
// The camera curve (replay_plus_lens.cpp has the same): the game's filmic tone curve undone (the inverse of the
// Narkowicz ACES fit), and the camera's own curve, giving display code values: a Rec.709-shaped power curve
// (gamma P18.x, its toe's slope P11.w below 0.018), a knee (P18.y, slope P18.z), the black level (P18.w), the
// white clip (P19.x) and the colour level about Y' (P19.y). At its defaults a Rec.709 camcorder's.
float3 inverse_filmic(float3 y)
{
    y = clamp(y, 0.0, 0.995);
    float3 a = 2.43 * y - 2.51, b = 0.59 * y - 0.03, k = 0.14 * y;
    return (-b - sqrt(max(b * b - 4.0 * a * k, 0.0))) / (2.0 * a);
}
float3 camera_curve(float3 l)
{
    l = max(l, 0.0);
    float3 v = l < 0.018 ? P11.w * l : 1.099 * pow(l, P18.x) - 0.099;
    v = v > P18.y ? P18.y + (v - P18.y) * P18.z : v;
    v = min((v + P18.w) / (1.0 + P18.w), P19.x);
    float y = dot(v, float3(0.2126, 0.7152, 0.0722));
    return saturate(y + (v - y) * P19.y);
}
// The grade. The lens's corner shade (a factor) and veiling glare (light) go first: on the scene's light
// with the camcorder curve, else on the picture's. White balance and the colour
// filter (P5.rgb), exposure, contrast, the hue and saturation (the colour matrix), the vignette, the
// camcorder curve; then on the picture's values lift, gamma and gain, split toning and faded blacks.
float3 grade(float3 c, float edge, float shade, float glare)
{
    if (P11.x > 0.5)
        c = inverse_filmic(c) * shade + glare;
    else
        c = c * shade + glare;
    c = max(c * P4.x * P5.rgb, 0.0);
    if (abs(P4.y - 1.0) > 1e-4)
        c = 0.18 * pow(max(c, 1e-6) / 0.18, P4.y);
    c = max(float3(dot(P12.xyz, c), dot(P13.xyz, c), dot(P14.xyz, c)), 0.0);
    if (P5.w > 0.0)
        c *= 1.0 - 0.9 * P5.w * pow(saturate(edge), P17.z);
    if (P11.x > 0.5)
        c = srgb_to_linear(camera_curve(c));
    bool lgg = P12.w != 0.0 || P13.w != 1.0 || P14.w != 1.0;
    bool split = any(P15.xyz != 0.0) || any(P16.xyz != 0.0);
    if (lgg || split || P4.w > 0.0)
    {
        float3 p = linear_to_srgb(c);
        if (lgg)
        {
            p = max(p * P14.w + P12.w * (1.0 - p), 0.0);
            if (P13.w != 1.0)
                p = pow(p, 1.0 / max(P13.w, 0.05));
        }
        if (split)
        {
            float lum = dot(p, float3(0.2126, 0.7152, 0.0722));
            float pivot = 0.5 - 0.35 * P15.w;
            float w = smoothstep(pivot - 0.35, pivot + 0.35, lum);
            p = max(p + P15.xyz * (1.0 - w) + P16.xyz * w, 0.0);
        }
        if (P4.w > 0.0)
            p = P4.w + p * (1.0 - P4.w);
        c = srgb_to_linear(p);
    }
    return c;
}
// Film grain (replay_plus_lens.cpp grain_noise): Dave Hoskins's hash12, on cells of `size` pixels,
// smoothly between them; -0.5..0.5.
float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}
float grain_noise(float2 pixel, float size, float seed)
{
    float cell = max(size, 0.5);
    float2 g = pixel / cell + seed * float2(17.0, 59.0);
    float2 i = floor(g), f = frac(g);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash12(i), b = hash12(i + float2(1.0, 0.0)), c = hash12(i + float2(0.0, 1.0)), d = hash12(i + float2(1.0, 1.0));
    float v = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
    return (v - 0.5) * (1.0 + 0.7 * saturate(cell - 1.0));
}

// (Research, test builds: `replayplus dmprobe <set>`, under the game's UI. Known picture values written as they are
// into the white measurement's blocks (16 of the screen's pixels across, from a twentieth of its width in, along the
// strips at 32%, 47% and 62% of its height: render.cpp white_strips), so `replayplus white dump` gives the game's
// display mapping, picture value against screen. Sets 1 to 61: every red, green and blue of 27 levels, 1/64 to 128
// in half stops (324 a set); 900 to 999: one grey everywhere, 2^((set - 950) / 4).)
float3 probe_value(float2 pixel)
{
    float set = P21.z;
    float3 result = exp2((set - 950.0) / 4.0);
    if (set < 899.5)
    {
        float screen_w = max(P21.w, 64.0);
        float u = pixel.x / P2.x * screen_w, v = pixel.y / P2.y;
        float margin = floor(screen_w / 20.0);
        float blocks = floor((screen_w - 2.0 * margin) / 16.0);
        float bx = floor((u - margin) / 16.0);
        float band = v < 0.395 ? 0.0 : (v < 0.545 ? 1.0 : 2.0);
        float g = (set - 1.0) * 3.0 * blocks + band * blocks + bx;
        float3 k = float3(fmod(g, 27.0), fmod(floor(g / 27.0), 27.0), floor(g / 729.0));
        result = bx >= 0.0 && bx < blocks && g < 19683.0 ? exp2((k - 12.0) * 0.5) : 0.18;
    }
    return result;
}

float4 ps_main(Interpolated i) : SV_Target
{
    [branch] if (P21.z > 0.5)
        return float4(probe_value(i.position.xy), 1.0);
    float va = P1.x;
    float2 win = P1.yz;
    float2 w = (i.uv - 0.5) / win + 0.5;
    float2 q = float2((w.x - 0.5) * 2.0 * va, 1.0 - 2.0 * w.y);
    bool outside = any(w < 0.0) || any(w > 1.0) || abs(q.y) > P1.w;
    float3 c = 0.0;
    float shade = 1.0, glare = 0.0;
    if (!outside)
    {
        if (P0.x < 1.5)
        {
            // No lens: the game's own picture, pixel for pixel.
            c = linear_chain.Load(int3(int2(i.position.xy), 0)).rgb;
        }
        else
        {
            float2 dq = float2(2.0 * va / P2.x / win.x, -2.0 / P2.y / win.y); // one screen pixel
            float2 uv0 = to_game(q);
            float2 ux = to_game(q + float2(dq.x, 0.0)) - uv0;
            float2 uy = to_game(q + float2(0.0, dq.y)) - uv0;
            float fx = length(ux * P2.xy);
            float fy = length(uy * P2.xy);
            float mag = max(fx, fy);
            float3 box = 0.0;
            float3 p = 0.0;
            [branch] if (mag > 1.0)
            {
                float lod = min(log2(max(max(fx / 3.0, fy / 4.0), 1.0)), P3.w - 1.0);
                [unroll] for (int jj = 0; jj < 4; jj++)
                {
                    [unroll] for (int ii = 0; ii < 3; ii++)
                    {
                        float2 o = float2((ii - 1) / 3.0, (jj - 1.5) / 4.0);
                        box += linear_chain.SampleLevel(linear_clamp, uv0 + ux * o.x + uy * o.y, lod).rgb;
                    }
                }
                p = to_perceptual(box * (1.0 / 12.0));
            }
            // Sharpened as much as the lens enlarges the game's picture there: from none where it is drawn
            // as large as it is, to all of P19.z where it is half as large again or more.
            float sharpen = P19.z * saturate((1.0 / max(mag, 1e-3) - 1.0) * 2.0);
            [branch] if (mag < 1.5)
                p = lerp(sample_easu(uv0, sharpen), p, saturate((mag - 1.0) * 2.0));
            c = from_perceptual(p);
            // The glass (replay_plus_lens.cpp does the same on exported frames): red and blue a little
            // bigger and smaller about the middle (lateral chromatic aberration), softer towards the edges
            // (a coarser mip blended in, as wide for the picture's height as on a 1080-line one), and the
            // corners darker (fall-off, as r^2.5).
            float rn = length(q) / (5.0 / 3.0);
            float lod0 = log2(max(mag, 1.0));
            if (P6.w > 0.0)
            {
                // As what moving red out and blue in changes, on the sharp sample: none in the middle.
                float d = 0.003 * P6.w;
                float3 here = linear_chain.SampleLevel(linear_clamp, uv0, lod0).rgb;
                c.r = max(c.r + linear_chain.SampleLevel(linear_clamp, to_game(q * (1.0 + d)), lod0).r - here.r, 0.0);
                c.b = max(c.b + linear_chain.SampleLevel(linear_clamp, to_game(q * (1.0 - d)), lod0).b - here.b, 0.0);
            }
            if (P10.y > 0.0)
            {
                float spread = P10.y * rn * rn;
                float lod = max(lod0 + 1.0 + 3.0 * spread + log2(max(P2.y, 1.0) / 1080.0), 0.0);
                c = lerp(c, linear_chain.SampleLevel(linear_clamp, uv0, min(lod, P3.w - 1.0)).rgb, saturate(spread * 1.5));
            }
            if (P10.x > 0.0)
                shade = exp2(-P10.x * pow(min(rn, 1.5), 2.5));
            glare = P11.y;
        }
        // The vignette's distance from the middle: its roundness between the window's shape and a circle.
        float edge_round = length(q) / sqrt(va * va + 1.0);
        float edge_own = length(float2(q.x / max(va, 1e-3), q.y)) / sqrt(2.0);
        c = grade(c, lerp(edge_own, edge_round, P17.w), shade, glare);
        if (P16.w > 0.0)
        {
            // Grain, more in the shadows and midtones than in the highlights, on the picture's values.
            float3 p = linear_to_srgb(c);
            float lum = dot(p, float3(0.2126, 0.7152, 0.0722));
            p += grain_noise(i.position.xy, P17.x, P17.y) * P16.w * 0.2 * (1.0 - 0.6 * saturate(lum));
            c = srgb_to_linear(max(p, 0.0));
        }
        if (P3.y > 0.0)
        {
            // The modern fisheye's round picture: black past its image circle.
            float r = length(q);
            c *= 1.0 - smoothstep(P3.x - 0.025, P3.x + 0.004, r) * P3.y;
        }
    }
    float3 shown = encode(c);
    // Nothing changes the picture itself (bars or a hood only): the game's values exactly, untouched.
    if (!outside && P6.x > 0.5)
        shown = source.Load(int3(int2(i.position.xy), 0)).rgb;
    if (!outside && P3.z > 0.0)
    {
        // Where lens::hood_extent puts it: its own shape, about the middle. Past its edges (a window wider or taller
        // than it), black, as a frame's bars.
        float2 hood_at = float2(0.5 + 0.5 * q.x / max(P19.w, 1e-3), 0.5 - 0.5 * q.y / max(P11.z, 1e-3));
        float4 o = hood.SampleLevel(linear_clamp, hood_at, 0.0);
        if (any(hood_at < 0.0) || any(hood_at > 1.0)) o = float4(0.0, 0.0, 0.0, 1.0);
        shown = lerp(shown, overlay_colour(o.rgb), o.a * P3.z);
    }
    return float4(shown, 1.0);
}

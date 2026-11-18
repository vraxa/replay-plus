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
    float4 P7; // camera light: power (0 none), reach (m), cos of half its beam, cos of half its bright core
    float4 P8; // camera light: colour r, g, b; the game's near plane (m)
    float4 P9; // the game's far plane (m), the depth is there (1), one depth texel across, up (uv)
    float4 P10; // the lens's corner shade (stops), edge softness, squeeze (across, before the lens), its edge compression's ease (per radian; 0 none)
    float4 P11; // the camera curve (1 on), the lens's veiling glare (light added before the grade), the picture overlay's scale, the camera's toe slope
    float4 P12; // the colour matrix's first row (hue and saturation), lift
    float4 P13; // its second row, gamma
    float4 P14; // its third row, gain
    float4 P15; // split toning: the shadows' offset r, g, b, balance
    float4 P16; // split toning: the highlights' offset r, g, b; film grain
    float4 P17; // grain size (pixels), the frame's seed, vignette power, vignette roundness
    float4 P18; // the camera curve: gamma, knee, knee slope, black level
    float4 P19; // the camera curve: white clip, colour level; unused, unused
    float4 P20; // the game's display mapping (transfer 3): t0, its toe's slope s1, its shoulder's slope s2, the bend x0
    float4 P21; // the bend's width w, log2 of the scale the picture is taken at; the camera light's facing smoothed over (m, 0 none), how much it counts
};
Texture2D source : register(t0); // ps_decode: the frame's copy; ps_down: the mip above
Texture2D linear_chain : register(t1); // ps_main: the frame in linear light, with its mips
Texture2D depth : register(t2); // the game's depth, for the camera light (R32, reversed)
Texture2D hood : register(t3);  // the hood's picture (sRGB colour, straight alpha)
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
// mapping, a filmic curve on each channel (replay_plus_lens.h DisplayMapping). Taken as linear light with 1 as
// white, the camera curve's white showed at 181 of 255 and the picture's bright half went flat and grey
// (2026-11-18). In SDR (transfer 3) that curve is applied to read the picture and undone to write it back, so
// the grade sees what an exported frame shows; in HDR (transfer 1) P2.w is the picture's white, measured.
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
// The picture's value -> the screen's linear light (1 its white).
float3 dm_decode(float3 p)
{
    float3 t = dm_t(log2(max(p, 1e-12)) + P21.y);
    return p > 0.0 ? 1.0 / (1.0 + exp2(-t)) : 0.0;
}
// The screen's linear light -> the picture's value that shows it. The curve lies under both its lines: Newton
// from the larger of their inverses starts below the root and, the curve being concave, closes in from there.
float3 dm_encode(float3 d)
{
    float3 shown = clamp(d, 1e-7, 0.99995);
    float3 t = log2(shown / (1.0 - shown));
    float3 x = max((t - P20.x) / P20.y, (t - P20.x - (P20.y - P20.z) * P20.w) / P20.z);
    [unroll] for (int i = 0; i < 5; ++i)
        x -= (dm_t(x) - t) / dm_slope(x);
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

// ---- the camera light ----------------------------------------------------------------------
// A light at the camera, pointing where it looks, worked out from the game's depth (reversed: 1 at
// the near plane, 0 at the far): each pixel's place in front of the camera, a beam with a soft edge, and the
// light falling off with the square of the distance, to nothing at its reach. The game's picture is already
// lit (its own shading, smooth and with its materials' detail): the light brightens what it shows by its own
// colour, and lifts the darkest a little with their own hue, as a lamp does to a scene at night.
//
// Which way each surface faces is not taken into it (P21.w 0, as it comes): in the game's depth its meshes are
// flat triangles, and lit by their facing every triangle showed on its own ("at night it really likes to
// highlight all the triangles on the meshes with the light on", a player, 2026-11-18); smoothed over a patch of
// the surface (smooth_facing) a thin thing, a leg, still showed them, and the picture went flat. A light on the
// camera lights mostly what faces it anyway. `replayplus lightfacing` brings the facing back, to compare.
float3 view_at(float2 uv)
{
    float d = depth.SampleLevel(point_clamp, uv, 0.0).r;
    float n = P8.w, f = P9.x;
    float z = n * f / max(d * (f - n) + n, 1e-6);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(ndc * P0.zw * z, z);
}
// Two rings of eight points about the pixel, P21.z metres out on the surface (and half that): the facing of the
// patch they span (Newell's method: each fan triangle's normal by its area). Each point counts as fully as it
// lies near the plane of the pixel's own triangle (fully within about 25 degrees, nothing past 55: not round a
// corner) and near the pixel (fully within twice the patch's reach, nothing past four times: not on something
// nearer or further away); and the patch's facing takes over from the triangle's as fully as the points count.
// All of it eases, nothing switches: a point that came or went at once made the facing jump from one pixel to
// the next (patches and steps on a shirt, 2026-11-18).
float3 smooth_facing(float2 uv, float3 p, float3 n0, float3 to_light)
{
    // The patch's reach on the screen at the pixel's distance (at most a quarter of the picture's height).
    float2 reach_uv = P21.z / (2.0 * max(p.z, 0.05) * max(P0.zw, 1e-3));
    reach_uv *= min(1.0, 0.25 / max(reach_uv.y, 1e-6));
    float3 sum = 0.0;
    float weight = 0.0;
    [unroll] for (int ring = 0; ring < 2; ++ring)
    {
        float scale = ring == 0 ? 1.0 : 0.5;
        float turn = ring == 0 ? 0.0 : 0.39269908;
        float metres = P21.z * scale;
        float3 first = 0.0, previous = 0.0;
        float first_w = 0.0, previous_w = 0.0;
        [unroll] for (int k = 0; k < 8; ++k)
        {
            float a = turn + 0.78539816 * k;
            float3 v = view_at(uv + float2(cos(a), sin(a)) * reach_uv * scale) - p;
            float len = max(length(v), 1e-6);
            float w = saturate((0.82 - abs(dot(v, n0)) / len) / 0.4) * saturate((4.0 * metres - len) / (2.0 * metres));
            if (k == 0)
            {
                first = v;
                first_w = w;
            }
            else
            {
                sum += cross(previous, v) * (w * previous_w);
                weight += w * previous_w;
            }
            previous = v;
            previous_w = w;
        }
        sum += cross(previous, first) * (previous_w * first_w);
        weight += previous_w * first_w;
    }
    float3 n = n0;
    if (dot(sum, sum) > 1e-24)
    {
        float3 patch = normalize(sum);
        if (dot(patch, to_light) < 0.0) patch = -patch;
        n = normalize(lerp(n0, patch, saturate(weight / 8.0)));
    }
    return n;
}
// The light's strength at a place in front of the camera: its beam, its reach, the square of the distance.
float light_at(float3 q)
{
    float d = max(length(q), 1e-4);
    float beam = smoothstep(P7.z, P7.w, q.z / d);
    float window = saturate(1.0 - pow(d / P7.y, 4.0));
    return q.z > 0.0 ? P7.x * beam * window * window / max(d * d, 0.25) : 0.0;
}
float3 camera_light(float2 uv, float3 c)
{
    float3 p = view_at(uv);
    float2 texel = P9.zw;
    float3 l = view_at(uv - float2(texel.x, 0.0)), r = view_at(uv + float2(texel.x, 0.0));
    float3 u = view_at(uv - float2(0.0, texel.y)), b = view_at(uv + float2(0.0, texel.y));
    // Its strength here as a tent over the pixel and its eight neighbours: the game's depth is not antialiased as its
    // picture is, and taken per pixel the light's edge round anything nearer than what is behind it came out jagged.
    float3 lu = view_at(uv + float2(-texel.x, -texel.y)), ru = view_at(uv + float2(texel.x, -texel.y));
    float3 lb = view_at(uv + float2(-texel.x, texel.y)), rb = view_at(uv + float2(texel.x, texel.y));
    float e = (4.0 * light_at(p) + 2.0 * (light_at(l) + light_at(r) + light_at(u) + light_at(b)) + light_at(lu) + light_at(ru) + light_at(lb) +
               light_at(rb)) / 16.0;
    if (e <= 0.0)
        return c;
    if (P21.w > 0.0)
    {
        // The surface's facing, when asked for (replayplus lightfacing): smoothed over P21.z metres of it, counting P21.w.
        float dist = max(length(p), 1e-4);
        float3 dx = abs(r.z - p.z) < abs(p.z - l.z) ? r - p : p - l;
        float3 dy = abs(b.z - p.z) < abs(p.z - u.z) ? b - p : p - u;
        float3 n = normalize(cross(dy, dx));
        float3 to_light = -p / dist;
        if (dot(n, to_light) < 0.0) n = -n;
        if (P21.z > 0.0)
            n = smooth_facing(uv, p, n, to_light);
        e *= lerp(1.0, saturate(dot(n, to_light)), saturate(P21.w));
    }
    float y = dot(c, float3(0.2126, 0.7152, 0.0722));
    float3 hue = min(c / max(y, 1e-3), 2.0);
    // More light on the same surfaces: the picture brightened by it, so its textures keep their contrast. Only what is
    // very bright already (above 0.5: a lamp, a light dropped in with the object dropper, its glow, a lit sign) takes
    // less of it, or the light flared other lights ("the camera light also affects lights placed with the object
    // dropper", 2026-11-18; a cap at 0.25 for everything washed the textures out). And the lift for what is black in
    // the picture, its own hue at most 0.05, so close up it does not grey the darks.
    float keep = 1.0 / (1.0 + 4.0 * max(y - 0.5, 0.0));
    float3 added = c * 4.0 * e * P8.rgb * keep + min(0.02 * e, 0.05) * P8.rgb * hue;
    // And it rolls off into white as film does instead of clipping: what it adds fills at most the room left below
    // white, nearing it smoothly. Close up, a calf lit past white clipped in blocks wherever its skin's texture
    // crossed the clip (2026-11-18).
    float3 room = max(1.0 - c, 0.0);
    return c + room * (1.0 - exp(-added / max(room, 1e-4)));
}

// ---- pass 1: the frame in linear light ------------------------------------------------------
float4 ps_decode(Interpolated i) : SV_Target
{
    float3 c = decode(source.Load(int3(int2(i.position.xy), 0)).rgb);
    if (P7.x > 0.0 && P9.y > 0.5)
        c = camera_light(i.uv, c);
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
float3 sample_easu(float2 uv)
{
    float2 pp = uv * P2.xy - 0.5;
    float2 fp = floor(pp);
    pp -= fp;
    int2 q = int2(fp);
    float3 b = easu_fetch(q + int2(0, -1)), c = easu_fetch(q + int2(1, -1));
    float3 e = easu_fetch(q + int2(-1, 0)), f = easu_fetch(q), g = easu_fetch(q + int2(1, 0)), h = easu_fetch(q + int2(2, 0));
    float3 i = easu_fetch(q + int2(-1, 1)), j = easu_fetch(q + int2(0, 1)), k = easu_fetch(q + int2(1, 1)), l = easu_fetch(q + int2(2, 1));
    float3 n = easu_fetch(q + int2(0, 2)), o = easu_fetch(q + int2(1, 2));
    float bL = easu_luma(b), cL = easu_luma(c), eL = easu_luma(e), fL = easu_luma(f), gL = easu_luma(g), hL = easu_luma(h);
    float iL = easu_luma(i), jL = easu_luma(j), kL = easu_luma(k), lL = easu_luma(l), nL = easu_luma(n), oL = easu_luma(o);
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

float4 ps_main(Interpolated i) : SV_Target
{
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
            [branch] if (mag < 1.5)
                p = lerp(sample_easu(uv0), p, saturate((mag - 1.0) * 2.0));
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
        float4 o = hood.SampleLevel(linear_clamp, float2(0.5 + 0.5 * q.x / (va * P11.z), 0.5 - 0.5 * q.y / P11.z), 0.0);
        shown = lerp(shown, overlay_colour(o.rgb), o.a * P3.z);
    }
    return float4(shown, 1.0);
}

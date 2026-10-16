#include "ui/crt/mac/CrtShaders.h"
#include "ui/crt/CrtLens.h"
#include "ui/crt/CrtPalette.h"
#include <cmath>

namespace
{
const char* const shaderBody = R"MSL(
// ---- shared with MetalCrtView.mm ------------------------------------------------------------
struct Beam             { float2 a; float2 b; float density; float sigma; };
struct BeamUniforms     { float2 viewSize; float2 scale; float energy; float unused; };
struct PictureUniforms
{
    float4 galleryRect;               // device px: x, y, w, h
    float2 drift; float2 viewSize;
    float keep; float pictureMix; float overlayLevel; float galleryGain;
    float cellSize; float tear; float tearY; float tearHeight;
    float tearSeed; float time; float hasGallery; float hasOverlay;
    float saturation; float pad0; float pad1; float pad2;
};
struct CompositeUniforms
{
    float2 viewSize; float2 collapse;
    float cornerRadius; float brightness; float bloom; float afterglow;
    float degauss; float time; float illumination; float faceGlow;
    float pxScale; float scanPeriod; float lineWidth; float hintLevel;
    float4 grid;                      // graticule, device px: x, y, w, h
    float hasHint; float pad0; float pad1; float pad2;
};
// ----------------------------------------------------------------------------------------------

struct FullscreenOut { float4 position [[position]]; float2 uv; };

vertex FullscreenOut fullscreenVertex (uint vid [[vertex_id]])
{
    const float2 p = float2 ((vid << 1) & 2, vid & 2);
    FullscreenOut o;
    o.position = float4 (p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2 (p.x, 1.0 - p.y);
    return o;
}

float hash12 (float2 p)
{
    float3 p3 = fract (float3 (p.xyx) * 0.1031);
    p3 += dot (p3, p3.yzx + 33.33);
    return fract ((p3.x + p3.y) * p3.z);
}

// ---- beam: gaussian spot swept along a segment (integrated, so polylines join seamlessly) -----
struct BeamOut
{
    float4 position [[position]];
    float2 local;   // x along the segment from a, y across, device px
    float len; float density; float sigma;
};

vertex BeamOut beamVertex (uint vid [[vertex_id]], uint iid [[instance_id]],
                           const device Beam* beams [[buffer(0)]], constant BeamUniforms& u [[buffer(1)]])
{
    const Beam bm = beams[iid];
    const float2 a = bm.a * u.scale, b = bm.b * u.scale;
    const float sigma = max (bm.sigma * u.scale.x, 0.7);
    const float len = length (b - a);
    const float2 dir = len > 1.0e-3 ? (b - a) / len : float2 (1.0, 0.0);
    const float2 nrm = float2 (-dir.y, dir.x);
    const float r = sigma * 3.0;

    const float along = (vid & 1) ? len + r : -r;
    const float across = (vid & 2) ? r : -r;
    const float2 p = a + dir * along + nrm * across;

    BeamOut o;
    o.position = float4 (p.x / u.viewSize.x * 2.0 - 1.0, 1.0 - p.y / u.viewSize.y * 2.0, 0.0, 1.0);
    o.local = float2 (along, across);
    o.len = len;
    o.density = bm.density * u.energy;
    o.sigma = sigma;
    return o;
}

float erfApprox (float x)
{
    const float x2 = x * x, a = 0.147;
    return sign (x) * sqrt (max (0.0, 1.0 - exp (-x2 * (1.2732395 + a * x2) / (1.0 + a * x2))));
}

fragment float beamFragment (BeamOut in [[stage_in]])
{
    const float s = in.sigma, x = in.local.x, y = in.local.y;
    const float across = exp (-y * y / (2.0 * s * s));
    const float along = in.len < 0.5 ? exp (-x * x / (2.0 * s * s))
                                     : 0.5 * (erfApprox (x / (1.4142136 * s)) - erfApprox ((x - in.len) / (1.4142136 * s)));
    return in.density * along * across;
}

// ---- picture: persistence decay + text overlay + gallery image refresh -----------------------
float bayer4 (uint2 p)
{
    const float m[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    return (m[(p.y & 3u) * 4u + (p.x & 3u)] + 0.5) / 16.0;
}

float galleryTarget (float2 px, constant PictureUniforms& u, texture2d<float> gallery)
{
    constexpr sampler lin (filter::linear, address::clamp_to_zero);
    const float cell = u.cellSize;
    const float2 index = floor (px / cell);
    const float2 centre = (index + 0.5) * cell;

    // tracking tear: rows inside the band slide sideways by a per-row random amount
    const float rel = centre.y / u.viewSize.y;
    const float band = smoothstep (u.tearHeight, 0.0, abs (rel - u.tearY)) * u.tear;
    const float rowNoise = hash12 (float2 (index.y, u.tearSeed));
    const float shift = band * (0.03 + 0.09 * rowNoise) * u.galleryRect.z * (rowNoise > 0.5 ? 1.0 : -0.6);

    const float2 q = (centre - u.galleryRect.xy - u.drift - float2 (shift, 0.0)) / u.galleryRect.zw;
    float l = gallery.sample (lin, q).r;
    l = max (l, band * step (0.82, hash12 (float2 (index.y * 0.37, floor (index.x / 7.0) + u.tearSeed))) * 0.7);

    l = pow (smoothstep (0.06, 1.0, l), 1.5);               // keep the background truly dark
    l = floor (l * 6.0 + bayer4 (uint2 (index))) / 6.0;   // ordered dither, 6 phosphor levels
    const float2 f = fract (px / cell) - 0.5;
    const float dotShape = exp (-f.x * f.x / (2.0 * 0.24 * 0.24)) * smoothstep (0.5, 0.3, abs (f.y));
    return l * dotShape * u.galleryGain;
}

fragment float pictureFragment (FullscreenOut in [[stage_in]], constant PictureUniforms& u [[buffer(0)]],
                                texture2d<float> previous [[texture(0)]], texture2d<float> overlay [[texture(1)]],
                                texture2d<float> gallery [[texture(2)]])
{
    const uint2 pixel = uint2 (in.position.xy);
    const float before = previous.read (pixel).r;
    float target = 0.0;
    if (u.hasOverlay > 0.5)
        target = overlay.read (pixel).r * u.overlayLevel;
    if (u.hasGallery > 0.5)
        target = max (target, galleryTarget (in.position.xy, u, gallery));

    float e = before * u.keep;
    if (target > 0.0)
        e = max (e, mix (before, target, u.pictureMix));
    e = min (e, u.saturation);   // phosphor saturation: overlapping sweeps cannot pile up into a white blob
    return e < 1.0e-3 ? 0.0 : e;
}

// ---- bloom: dual-filter (Kawase) pyramid ----------------------------------------------------
// First level straight to quarter resolution: four bilinear taps average the 4x4 source block.
fragment float bloomQuarterFragment (FullscreenOut in [[stage_in]], texture2d<float> src [[texture(0)]])
{
    constexpr sampler lin (filter::linear, address::clamp_to_zero);
    const float2 o = 1.0 / float2 (src.get_width(), src.get_height());
    return 0.25 * (src.sample (lin, in.uv + float2 (-o.x, -o.y)).r + src.sample (lin, in.uv + float2 (o.x, -o.y)).r
                 + src.sample (lin, in.uv + float2 (-o.x,  o.y)).r + src.sample (lin, in.uv + float2 (o.x,  o.y)).r);
}

fragment float bloomDownFragment (FullscreenOut in [[stage_in]], texture2d<float> src [[texture(0)]])
{
    constexpr sampler lin (filter::linear, address::clamp_to_zero);
    const float2 o = 1.0 / float2 (src.get_width(), src.get_height());
    float sum = src.sample (lin, in.uv).r * 4.0;
    sum += src.sample (lin, in.uv + float2 (-o.x, -o.y)).r;
    sum += src.sample (lin, in.uv + float2 ( o.x, -o.y)).r;
    sum += src.sample (lin, in.uv + float2 (-o.x,  o.y)).r;
    sum += src.sample (lin, in.uv + float2 ( o.x,  o.y)).r;
    return sum / 8.0;
}

fragment float bloomUpFragment (FullscreenOut in [[stage_in]], texture2d<float> smaller [[texture(0)]],
                                texture2d<float> same [[texture(1)]])
{
    constexpr sampler lin (filter::linear, address::clamp_to_zero);
    const float2 o = 1.0 / float2 (smaller.get_width(), smaller.get_height());
    float sum = 0.0;
    sum += smaller.sample (lin, in.uv + float2 (-2.0 * o.x, 0.0)).r;
    sum += smaller.sample (lin, in.uv + float2 ( 2.0 * o.x, 0.0)).r;
    sum += smaller.sample (lin, in.uv + float2 (0.0, -2.0 * o.y)).r;
    sum += smaller.sample (lin, in.uv + float2 (0.0,  2.0 * o.y)).r;
    sum += smaller.sample (lin, in.uv + float2 (-o.x, -o.y)).r * 2.0;
    sum += smaller.sample (lin, in.uv + float2 ( o.x, -o.y)).r * 2.0;
    sum += smaller.sample (lin, in.uv + float2 (-o.x,  o.y)).r * 2.0;
    sum += smaller.sample (lin, in.uv + float2 ( o.x,  o.y)).r * 2.0;
    return sum / 12.0 + same.sample (lin, in.uv).r;
}

// ---- composite: tube face -----------------------------------------------------------------
float roundedRectDistance (float2 p, float2 halfSize, float r)
{
    const float2 q = abs (p) - halfSize + r;
    return length (max (q, 0.0)) + min (max (q.x, q.y), 0.0) - r;
}

// Energy -> colour. The skirt of a trace is teal; as the beam dwells the centre burns through to
// near-white cyan. A separate, deeper blue-green halo carries the soft glow.
float3 phosphorColour (float core, float halo)
{
    const float body = 1.0 - exp (-core * toneBodyGain);
    const float heat = smoothstep (toneWhiteStart, toneWhiteEnd, core);
    const float3 hot = mix (palBody, palCore, heat) * body;
    return hot + palHalo * (1.0 - exp (-halo * toneHaloGain));
}

// One axis of the graticule: coverage of the nearest line (snapped to whole device pixels so lines
// stay sharp), and which line it is.
float lineCoverage (float p, float origin, float cell, float count, float width, thread float& index)
{
    index = clamp (round ((p - origin) / cell), 0.0, count);
    const float centre = origin + index * cell;
    const float snapped = fmod (width, 2.0) > 0.5 ? floor (centre) + 0.5 : floor (centre + 0.5);
    return saturate ((width + 1.0) * 0.5 - abs (p - snapped));
}

// 10 x 8 divisions, brighter frame and centre cross, minor ticks on the centre axes. Evaluated
// analytically on the (slightly curved) face plane, so it is always exactly one crisp line wide.
float graticuleLevel (float2 p, float4 g, float width)
{
    const float2 cell = g.zw / float2 (10.0, 8.0);
    const float2 end = g.xy + g.zw;
    const float inX = step (g.x - width, p.x) * step (p.x, end.x + width);
    const float inY = step (g.y - width, p.y) * step (p.y, end.y + width);

    float ix, iy;
    const float v = lineCoverage (p.x, g.x, cell.x, 10.0, width, ix) * inY;
    const float h = lineCoverage (p.y, g.y, cell.y, 8.0, width, iy) * inX;
    const float vWeight = (ix < 0.5 || ix > 9.5) ? 1.0 : (abs (ix - 5.0) < 0.5 ? 1.0 : 0.62);
    const float hWeight = (iy < 0.5 || iy > 7.5) ? 1.0 : (abs (iy - 4.0) < 0.5 ? 1.0 : 0.62);
    float level = max (v * vWeight, h * hWeight);

    const float2 centre = g.xy + g.zw * 0.5;
    const float tick = cell.x * 0.075;
    float ti;
    const float alongX = lineCoverage (p.x, g.x, cell.x * 0.2, 50.0, width, ti) * inX * step (abs (p.y - centre.y), tick);
    const float alongY = lineCoverage (p.y, g.y, cell.y * 0.2, 40.0, width, ti) * inY * step (abs (p.x - centre.x), tick);
    return max (level, max (alongX, alongY) * 0.9);
}

fragment float4 compositeFragment (FullscreenOut in [[stage_in]], constant CompositeUniforms& u [[buffer(0)]],
                                   texture2d<float> phosphor [[texture(0)]], texture2d<float> bloom [[texture(1)]],
                                   texture2d<float> hint [[texture(2)]])
{
    constexpr sampler lin (filter::linear, address::clamp_to_zero);
    const float2 px = in.position.xy;
    const float2 uv0 = px / u.viewSize;

    // degauss wobble at power-on moves the picture only, never the glass
    float2 uv = uv0;
    uv.x += u.degauss * 0.014 * sin (uv.y * 21.0 + u.time * 47.0);
    uv.y += u.degauss * 0.006 * sin (uv.x * 13.0 - u.time * 31.0);

    float2 c = uv * 2.0 - 1.0;
    c *= 1.0 + barrel * dot (c, c);
    const float2 faceUv = c * 0.5 + 0.5;

    float2 c0 = uv0 * 2.0 - 1.0;
    c0 *= 1.0 + barrel * dot (c0, c0);
    const float2 glassUv = c0 * 0.5 + 0.5;
    const float rr = dot (c0, c0);

    // power-off: picture squashes to a line, then to a dot (light concentrates as it shrinks)
    const float2 cuv = 0.5 + (faceUv - 0.5) / u.collapse;
    const float squeeze = min (1.0 / sqrt (u.collapse.x * u.collapse.y), 6.0);
    const float lit = u.brightness * squeeze;

    float core = phosphor.sample (lin, cuv).r * lit;
    float halo = bloom.sample (lin, cuv).r * lit * u.bloom;

    const float dotDistance = length ((faceUv - 0.5) * u.viewSize) / u.pxScale;
    core += u.afterglow * 3.0 * exp (-dotDistance * dotDistance / 6.0);
    halo += u.afterglow * 1.2 * exp (-dotDistance * dotDistance / 220.0);

    float3 col = phosphorColour (core, halo);

    // unlit glass: dark teal-grey with a faint, uniform glow that is a touch lighter in the middle
    col += mix (palFace, palFaceCentre, 1.0 - smoothstep (0.0, 1.8, rr)) * (0.55 + 0.45 * u.faceGlow);

    // very faint scanlines: one device-pixel row in every `scanPeriod`, dimmer where the beam is hot
    const float scanRow = fmod (floor (px.y), u.scanPeriod) < 0.5 ? 1.0 : 0.0;
    col *= 1.0 - scanDepth * scanRow * (1.0 - smoothstep (0.55, 0.95, max (col.r, max (col.g, col.b))));

    // graticule: its own crisp layer in front of the glow, lit by the scale lamp, brighter towards
    // the edges where the lamps sit
    const float edgeLit = 0.82 + 0.18 * smoothstep (0.0, 1.0, rr);
    const float level = graticuleLevel (glassUv * u.viewSize, u.grid, u.lineWidth);
    const float3 lines = mix (palGrid, palGridAxis, smoothstep (0.7, 1.0, level)) * (level * u.illumination * edgeLit * gridLevel);
    col = 1.0 - (1.0 - col) * (1.0 - lines);

    col *= 1.0 - vignetteAmount * smoothstep (0.35, 2.0, rr);

    // glass: a broad faint sheen up-left and a thin catch light along the top edge
    const float2 sheen = (uv0 - float2 (0.28, 0.2)) * float2 (1.5, 2.4);
    col += float3 (0.85, 0.95, 0.95) * sheenAmount * exp (-dot (sheen, sheen) * 2.5);
    const float arc = uv0.y - (0.045 + 0.06 * (uv0.x - 0.5) * (uv0.x - 0.5));
    col += float3 (0.9, 1.0, 1.0) * catchAmount * exp (-arc * arc * 9000.0) * smoothstep (0.1, 0.35, uv0.x) * smoothstep (0.9, 0.6, uv0.x);

    // help text: a crisp mask read one-to-one, over everything on the glass but under the tube edge
    if (u.hasHint > 0.5)
    {
        const float m = hint.read (uint2 (px)).r;
        col = 1.0 - (1.0 - col) * (1.0 - m * u.hintLevel * palText);
    }

    const float edge = roundedRectDistance (px - u.viewSize * 0.5, u.viewSize * 0.5, u.cornerRadius);
    col *= mix (0.3, 1.0, smoothstep (0.0, 22.0 * u.pxScale, -edge));
    col *= clamp (0.5 - edge, 0.0, 1.0);
    return float4 (col, 1.0);
}
)MSL";
}

namespace Crt
{
namespace
{
    // integer formatting: std::to_string (float) follows the C locale's decimal separator
    std::string literal (float v)
    {
        return std::to_string (std::lround (v * 1.0e6f)) + ".0e-6";
    }

    std::string constant (const char* name, float v)
    {
        return std::string ("constant float ") + name + " = " + literal (v) + ";\n";
    }

    std::string constant (const char* name, Rgb c)
    {
        return std::string ("constant float3 ") + name + " = float3 (" + literal (c.r) + ", " + literal (c.g) + ", " + literal (c.b) + ");\n";
    }
}

std::string metalShaderSource()
{
    const auto& p = palette;
    return "#include <metal_stdlib>\nusing namespace metal;\n"
           + constant ("barrel", Lens::barrel)
           + constant ("palFace", p.face) + constant ("palFaceCentre", p.faceCentre)
           + constant ("palBody", p.body) + constant ("palCore", p.core) + constant ("palHalo", p.halo)
           + constant ("palGrid", p.grid) + constant ("palGridAxis", p.gridAxis) + constant ("palText", p.text)
           + constant ("toneBodyGain", p.bodyGain) + constant ("toneWhiteStart", p.whiteStart)
           + constant ("toneWhiteEnd", p.whiteEnd) + constant ("toneHaloGain", p.haloGain)
           + constant ("scanDepth", p.scanlineDepth) + constant ("vignetteAmount", p.vignette)
           + constant ("sheenAmount", p.sheen) + constant ("catchAmount", p.catchLight) + constant ("gridLevel", p.gridLevel)
           + shaderBody;
}
}

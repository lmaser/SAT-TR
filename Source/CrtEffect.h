#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <cstdint>
#include <cstring>

//======================================================================
//  CrtEffect — full-screen CRT post-process via juce::ImageEffectFilter
//
//  Implements the core effects from the Retro-Windows-Terminal CRT shader
//  (pincushion distortion, chromatic aberration, scanlines, noise,
//  vignette, flicker) entirely on the CPU, per-pixel.
//
//  Optimised path: processes 2×2 blocks (4× fewer heavy computations)
//  with direct pointer arithmetic — avoids per-sample function calls
//  and redundant getLinePointer() lookups.
//
//  Usage:
//    1. Declare as member:  CrtEffect crtEffect;
//    2. Attach in ctor:     setComponentEffect (&crtEffect);
//    3. Animate in timer:   crtEffect.setTime (t); repaint();
//    4. Detach in dtor:     setComponentEffect (nullptr);
//======================================================================
class CrtEffect final : public juce::ImageEffectFilter
{
public:
    //-- setters (safe to call from any thread) -------------------------
    void setTime       (float t)          noexcept { crtTime = t; }
    void setEnabled    (bool  e)          noexcept { effectEnabled = e; }
    void setResolution (float w, float h) noexcept { resW = w; resH = h; }

    //-- ImageEffectFilter override ------------------------------------
    void applyEffect (juce::Image& src, juce::Graphics& destCtx,
                      float /*scaleFactor*/, float alpha) override
    {
        const int w = src.getWidth();
        const int h = src.getHeight();

        if (! effectEnabled || w <= 0 || h <= 0)
        {
            destCtx.setOpacity (alpha);
            destCtx.drawImageAt (src, 0, 0);
            return;
        }

        // Reuse output buffer to avoid per-frame allocation.
        const bool sizeChanged = (outputBuf.getWidth() != w || outputBuf.getHeight() != h);
        if (sizeChanged)
        {
            outputBuf = juce::Image (juce::Image::ARGB, w, h, false);
            prevSrc   = juce::Image (juce::Image::ARGB, w, h, true);   // zero-filled
            lastProcessedTime = -1.0f;
        }

        // When crtTime hasn't advanced (i.e. between timer ticks), only
        // block-rows whose source pixels actually changed need the heavy
        // CRT math — the rest can reuse the cached outputBuf from the
        // previous frame.  This dramatically reduces CPU during slider
        // drag (typically < 5 % of rows change per frame).
        const bool timeChanged = (crtTime != lastProcessedTime);
        const bool canSkipUnchanged = ! timeChanged
                                    && prevSrc.isValid()
                                    && prevSrc.getWidth() == w
                                    && prevSrc.getHeight() == h;

        {
            juce::Image::BitmapData srcData  (src,       juce::Image::BitmapData::readOnly);
            juce::Image::BitmapData dstData  (outputBuf, juce::Image::BitmapData::readWrite);
            juce::Image::BitmapData prevData (prevSrc,   juce::Image::BitmapData::readWrite);

            const float invW = 1.0f / static_cast<float> (w);
            const float invH = 1.0f / static_cast<float> (h);
            const float fW   = static_cast<float> (w);
            const float fH   = static_cast<float> (h);
            const int   wMax = w - 1;
            const int   hMax = h - 1;

            const float timeFracA = std::fmod (crtTime, 1.0f);
            const float timeFracB = std::fmod (crtTime * 0.7f, 1.0f);
            const float flickMult = 1.0f - kFlicker * hash01 (crtTime * 3.7f, 0.0f);

            // Cache raw pointer bases + strides for direct access.
            const int srcLineStride  = srcData.lineStride;
            const int srcPixStride   = srcData.pixelStride;
            const int dstLineStride  = dstData.lineStride;
            const int dstPixStride   = dstData.pixelStride;
            const int prevLineStride = prevData.lineStride;
            const juce::uint8* srcBase  = srcData.getLinePointer (0);
                  juce::uint8* dstBase  = dstData.getLinePointer (0);
            const juce::uint8* prevBase = prevData.getLinePointer (0);
                  juce::uint8* prevWBase = prevData.getLinePointer (0);

            const int rowBytes = w * srcPixStride;

            // ── Process in 2×2 blocks ────────────────────────────────
            // This quarters the heavy per-pixel math (distortion,
            // 3× chromatic sample, noise hash, vignette) while giving
            // an authentic CRT "chunky pixel" look.  Scanline dimming
            // is applied at block-row granularity (~every 2 px), which
            // matches real shadow-mask CRT scan gap spacing.

            for (int by = 0; by < h; by += 2)
            {
                // ── Row-skip optimisation ──
                // If CRT time unchanged and these source rows match the
                // cached previous source, outputBuf already holds the
                // correct CRT output → skip the heavy per-pixel math.
                if (canSkipUnchanged)
                {
                    const juce::uint8* srcRow0  = srcBase  + by * srcLineStride;
                    const juce::uint8* prevRow0 = prevBase + by * prevLineStride;
                    bool match = (std::memcmp (srcRow0, prevRow0, static_cast<size_t> (rowBytes)) == 0);
                    if (match && by + 1 < h)
                    {
                        const juce::uint8* srcRow1  = srcBase  + (by + 1) * srcLineStride;
                        const juce::uint8* prevRow1 = prevBase + (by + 1) * prevLineStride;
                        match = (std::memcmp (srcRow1, prevRow1, static_cast<size_t> (rowBytes)) == 0);
                    }
                    if (match)
                        continue;   // cached CRT output is still valid
                }

                const float v = (static_cast<float> (by) + 1.0f) * invH;

                // Precompute scanline multiplier for this block-row.
                // Use a fast saw/square approximation instead of sin():
                //   odd block-rows are dimmed, even are bright.
                const float scanMul = ((by >> 1) & 1)
                    ? (1.0f - kScanline)
                    : 1.0f;

                juce::uint8* dstRow0 = dstBase + by * dstLineStride;
                juce::uint8* dstRow1 = (by + 1 < h)
                    ? dstRow0 + dstLineStride
                    : nullptr;

                for (int bx = 0; bx < w; bx += 2)
                {
                    const float u = (static_cast<float> (bx) + 1.0f) * invW;

                    // ── Pincushion (barrel) distortion ──
                    float du = u, dv = v;
                    {
                        const float cx = du - 0.5f;
                        const float cy = dv - 0.5f;
                        const float r2 = cx * cx + cy * cy;
                        du += cx * r2 * kDistortion;
                        dv += cy * r2 * kDistortion;
                    }

                    if (du < 0.0f || du >= 1.0f || dv < 0.0f || dv >= 1.0f)
                    {
                        // Write black 2×2 block.
                        writeBlackRaw (dstRow0 + bx * dstPixStride);
                        if (bx + 1 < w) writeBlackRaw (dstRow0 + (bx + 1) * dstPixStride);
                        if (dstRow1)
                        {
                            writeBlackRaw (dstRow1 + bx * dstPixStride);
                            if (bx + 1 < w) writeBlackRaw (dstRow1 + (bx + 1) * dstPixStride);
                        }
                        continue;
                    }

                    // ── Radial chromatic aberration (3 channels) ──
                    const float qx = du - 0.5f;
                    const float qy = dv - 0.5f;

                    // Red channel (shifted outward)
                    const int rxR = juce::jlimit (0, wMax, static_cast<int> ((du + qx * kChromaticSpread) * fW));
                    const int ryR = juce::jlimit (0, hMax, static_cast<int> ((dv + qy * kChromaticSpread) * fH));
                    const auto* pxR = reinterpret_cast<const juce::PixelARGB*> (
                        srcBase + ryR * srcLineStride + rxR * srcPixStride);

                    // Green channel (centre)
                    const int gx = juce::jlimit (0, wMax, static_cast<int> (du * fW));
                    const int gy = juce::jlimit (0, hMax, static_cast<int> (dv * fH));
                    const auto* pxG = reinterpret_cast<const juce::PixelARGB*> (
                        srcBase + gy * srcLineStride + gx * srcPixStride);

                    // Blue channel (shifted inward)
                    const int bxB = juce::jlimit (0, wMax, static_cast<int> ((du - qx * kChromaticSpread) * fW));
                    const int byB = juce::jlimit (0, hMax, static_cast<int> ((dv - qy * kChromaticSpread) * fH));
                    const auto* pxB = reinterpret_cast<const juce::PixelARGB*> (
                        srcBase + byB * srcLineStride + bxB * srcPixStride);

                    // Read channels directly (avoid per-sample function call).
                    const float rr = pxR->getRed()   * (1.0f / 255.0f);
                    const float gg = pxG->getGreen() * (1.0f / 255.0f);
                    const float bb = pxB->getBlue()  * (1.0f / 255.0f);

                    // ── Noise ──
                    const float n = hash01 (u + timeFracA, v + timeFracB) * kNoise;

                    // ── Vignette (reuse qx/qy already computed) ──
                    const float vig = juce::jmax (0.0f,
                        1.0f - (qx * qx + qy * qy) * kVigStr4);

                    // ── Final combine ──
                    const float mult = scanMul * flickMult * vig;
                    const auto outR = toByte ((rr + n) * mult);
                    const auto outG = toByte ((gg + n) * mult);
                    const auto outB = toByte ((bb + n) * mult);

                    // ── Write 2×2 block (direct pointer, no getLinePointer) ──
                    writePxRaw (dstRow0 + bx * dstPixStride, outR, outG, outB);
                    if (bx + 1 < w)
                        writePxRaw (dstRow0 + (bx + 1) * dstPixStride, outR, outG, outB);
                    if (dstRow1)
                    {
                        writePxRaw (dstRow1 + bx * dstPixStride, outR, outG, outB);
                        if (bx + 1 < w)
                            writePxRaw (dstRow1 + (bx + 1) * dstPixStride, outR, outG, outB);
                    }
                }

                // ── Update source cache for processed rows ──
                std::memcpy (prevWBase + by * prevLineStride,
                             srcBase  + by * srcLineStride,
                             static_cast<size_t> (rowBytes));
                if (by + 1 < h)
                    std::memcpy (prevWBase + (by + 1) * prevLineStride,
                                 srcBase  + (by + 1) * srcLineStride,
                                 static_cast<size_t> (rowBytes));
            }

            // When time changed (full reprocess), every row was processed
            // above, so prevSrc is already fully updated.  Nothing extra
            // to do here.
        }

        lastProcessedTime = crtTime;

        destCtx.setOpacity (alpha);
        destCtx.drawImageAt (outputBuf, 0, 0);
    }

private:
    //-- tunable constants ---------------------------------------------
    static constexpr float kDistortion      = 0.02f;   // barrel curvature
    static constexpr float kChromaticSpread = 0.0045f; // radial R/B spread (UV units)
    static constexpr float kNoise        = 0.10f;      // noise intensity
    static constexpr float kScanline     = 0.18f;      // scanline dim %
    static constexpr float kVignetteStr  = 0.30f;      // edge darkening
    static constexpr float kVigStr4      = kVignetteStr * 4.0f; // pre-multiplied
    static constexpr float kFlicker      = 0.05f;     // brightness jitter

    //-- state ---------------------------------------------------------
    float crtTime       = 0.0f;
    bool  effectEnabled = true;
    float resW          = 800.0f;
    float resH          = 600.0f;
    juce::Image outputBuf;
    juce::Image prevSrc;              // cached source for row-dirty detection
    float lastProcessedTime = -1.0f;  // last crtTime that triggered full processing

    //-- fast integer hash → [0, 1) ------------------------------------
    static float hash01 (float x, float y) noexcept
    {
        unsigned n = static_cast<unsigned> (static_cast<int> (x * 12345.6789f))
                   * 1619u
                   + static_cast<unsigned> (static_cast<int> (y * 67890.1234f))
                   * 31337u
                   + 1013u;
        n = (n << 13) ^ n;
        n = n * (n * n * 15731u + 789221u) + 1376312589u;
        return static_cast<float> (n & 0x7fffffffu) * (1.0f / 2147483647.0f);
    }

    //-- byte helpers (raw-pointer, no getLinePointer) -----------------
    static juce::uint8 toByte (float f) noexcept
    {
        return static_cast<juce::uint8> (
            juce::jlimit (0, 255, static_cast<int> (f * 255.0f)));
    }

    static void writePxRaw (juce::uint8* dest,
                             juce::uint8 r, juce::uint8 g, juce::uint8 b) noexcept
    {
        auto* px = reinterpret_cast<juce::PixelARGB*> (dest);
        px->setARGB (255, r, g, b);
    }

    static void writeBlackRaw (juce::uint8* dest) noexcept
    {
        auto* px = reinterpret_cast<juce::PixelARGB*> (dest);
        px->setARGB (255, 0, 0, 0);
    }
};

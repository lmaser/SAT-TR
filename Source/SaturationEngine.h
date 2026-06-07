#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>
#include <atomic>
#include "SatDspDiag.h"

// ----------------------------------------------------------------
//  SaturationEngine -- header-only DSP for SAT-TR
//  6 physically-modeled saturation algorithms with:
//    ADAA (1st-order antiderivative anti-aliasing)
//    REACT (Airwindows-inspired energy tracking -> parameter modulation)
//    CHAR (post-waveshaper wavefolding + sharpen)
//    Instability (component tolerance + slow continuous thermal drift)
//    MOD (input-domain power warp + model-specific secondary)
//    Internal emphasis / de-emphasis EQ per model
//    Safety LPF for x1 (no oversampling) mode
// ----------------------------------------------------------------

namespace SatEngine
{

// ----------------------------------------------------------------
//  Model enum
// ----------------------------------------------------------------
enum class Model : int
{
    Clean       = 0,   // Bypass - 1:1 pass-through (no saturation)
    Tape        = 1,   // Tape stage - soft magnetic compression + losses
    Tube        = 2,   // 12AX7 -> EL34/6L6-inspired morph via MOD
    Transistor  = 3,   // BJT <-> FET preamp stages
    Diode       = 4,   // feedback/hard/open diode clipper topologies
    Clipper     = 5,   // broadband/pedal clipper - classic -> TS -> Klon voice
    NumModels   = 6
};

// ----------------------------------------------------------------
//  Constants
// ----------------------------------------------------------------
static constexpr float kPi         = 3.14159265358979323846f;
static constexpr float kHalfPi     = 1.57079632679489661923f;
static constexpr float kTwoPi      = 6.28318530717958647692f;
static constexpr float kInvPi      = 0.31830988618379067154f;
static constexpr float kLn2        = 0.69314718055994530942f;
static constexpr float kSmoothCoeff = 0.995f;   // ~10ms @ 48kHz
static constexpr int   kReactBufSize = 8192;
static constexpr int   kTriodeSagBufSize = 512;
static constexpr int   kTriodeBloomSlotCount = 1024;
static constexpr int   kMaxSeries    = 4;

// ----------------------------------------------------------------
//  ADAA1 helpers  (1st-order antiderivative anti-aliasing)
// ----------------------------------------------------------------
namespace adaa
{
    // -- Fast math helpers (avoid expensive std::exp/log1p in hot path) --
    inline float fastExp (float x) noexcept
    {
        x = std::max (x, -87.0f);
        x = std::min (x,  88.0f);
        if (x > 5.0f)  return std::exp (x);
        if (x < -5.0f) return std::exp (x);
        // Pade 3/3 approximation of exp(x) for |x| < ~5
        const float num = 1680.0f + x * (840.0f + x * (180.0f + x * 20.0f));
        const float den = 1680.0f + x * (-840.0f + x * (180.0f + x * (-20.0f)));
        return num / den;
    }

    inline float fastLog1p (float x) noexcept
    {
        // log1p(x) for x >= 0: Pade approximation
        // Accurate to ~1e-4 for x in [0, 2]
        if (x > 2.0f || x < -0.5f) return std::log1p (x);
        // Pade [2/2]: log(1+x) ~= x(6+x) / (6+4x)
        return x * (6.0f + x) / (6.0f + 4.0f * x);
    }

    // Antiderivative of tanh(k*x):  F1(x) = (1/k)*ln(cosh(k*x))
    // Numerically stable form: (|kx| + log1p(exp(-2|kx|)) - ln2) / k
    inline float tanhAD1 (float x, float k) noexcept
    {
        const float kx = k * x;
        const float akx = std::abs (kx);
        return (akx + fastLog1p (fastExp (-2.0f * akx)) - kLn2) / k;
    }

    // Antiderivative of sin(pi*x) wavefolder:  F1(x) = -(1/pi)*cos(pi*x)
    inline float sinFoldAD1 (float x) noexcept
    {
        return -kInvPi * std::cos (kPi * x);
    }

    // Generic ADAA1 processor for tanh(k*x)
    struct TanhADAA
    {
        float prev    = 0.0f;
        float ad1Prev = 0.0f;

        inline float process (float x, float k) noexcept
        {
            constexpr float kTol = 1.0e-5f;
            const float ad1 = tanhAD1 (x, k);
            const float dx  = x - prev;
            const float y   = (std::abs (dx) < kTol)
                            ? std::tanh (k * 0.5f * (x + prev))
                            : (ad1 - ad1Prev) / dx;
            prev    = x;
            ad1Prev = ad1;
            return y;
        }

        void reset() noexcept { prev = 0.0f; ad1Prev = 0.0f; }
    };

    // Dedicated ADAA for tape-style tanh stages.
    // Uses exact std::exp/log1p evaluation plus a wide fallback/blend zone to
    // avoid the pathological spikes seen with the generic fast-math variant.
    struct TapeTanhADAA
    {
        float prev = 0.0f;
        float ad1Prev = 0.0f;
        bool initialised = false;

        static inline float tanhAD1Exact (float x, float k) noexcept
        {
            const double kd = (double) k * (double) x;
            const double a = std::abs (kd);
            const double ad = (a + std::log1p (std::exp (-2.0 * a)) - std::log (2.0)) / (double) k;
            return (float) ad;
        }

        inline float process (float x, float k) noexcept
        {
            constexpr float smallDx = 1.0e-4f;
            constexpr float blendDx = 2.0e-3f;
            constexpr float jumpReset = 8.0f;

            const float direct = std::tanh (k * x);

            if (! initialised || ! std::isfinite (prev) || ! std::isfinite (ad1Prev)
                || std::abs (x - prev) > jumpReset)
            {
                prev = x;
                ad1Prev = tanhAD1Exact (x, k);
                initialised = true;
                return direct;
            }

            const float ad1 = tanhAD1Exact (x, k);
            const float dx = x - prev;
            const float adx = std::abs (dx);
            const float mid = std::tanh (k * 0.5f * (x + prev));

            float y = mid;
            if (adx > smallDx)
            {
                const float adaaY = (ad1 - ad1Prev) / dx;
                if (adx >= blendDx)
                {
                    y = adaaY;
                }
                else
                {
                    const float t = (adx - smallDx) / (blendDx - smallDx);
                    const float s = t * t * (3.0f - 2.0f * t);
                    y = mid + (adaaY - mid) * s;
                }
            }

            if (! std::isfinite (y) || std::abs (y) > 1.25f)
                y = mid;

            prev = x;
            ad1Prev = ad1;
            return std::max (-1.1f, std::min (1.1f, y));
        }

        void reset() noexcept
        {
            prev = 0.0f;
            ad1Prev = 0.0f;
            initialised = false;
        }
    };

    using StableTanhADAA = TapeTanhADAA;

    // Generic ADAA1 processor for sin(pi*x) wavefolding
    struct SinFoldADAA
    {
        float prev    = 0.0f;
        float ad1Prev = 0.0f;

        inline float process (float x) noexcept
        {
            constexpr float kTol = 1.0e-5f;
            const float ad1 = sinFoldAD1 (x);
            const float dx  = x - prev;
            const float y   = (std::abs (dx) < kTol)
                            ? std::sin (kPi * 0.5f * (x + prev))
                            : (ad1 - ad1Prev) / dx;
            prev    = x;
            ad1Prev = ad1;
            return y;
        }

        void reset() noexcept { prev = 0.0f; ad1Prev = 0.0f; }
    };

    struct ClipperADAA
    {
        float prev = 0.0f;
        float ad1Prev = 0.0f;
        bool initialised = false;

        static inline float clipPositive (float x, float threshold, float knee) noexcept
        {
            const float T = std::max (threshold, 1.0e-4f);
            const float W = std::max (knee, 1.0e-5f);
            const float L = std::max (0.0f, T - W);
            const float H = T + W;

            if (x <= L)
                return x;
            if (x >= H)
                return T;

            const float z = x - L;
            return x - (z * z) / (4.0f * W);
        }

        static inline float clipPositiveAD1 (float x, float threshold, float knee) noexcept
        {
            const float T = std::max (threshold, 1.0e-4f);
            const float W = std::max (knee, 1.0e-5f);
            const float L = std::max (0.0f, T - W);
            const float H = T + W;

            if (x <= L)
                return 0.5f * x * x;

            if (x >= H)
            {
                const float zH = H - L;
                const float FH = 0.5f * H * H - (zH * zH * zH) / (12.0f * W);
                return FH + T * (x - H);
            }

            const float z = x - L;
            return 0.5f * x * x - (z * z * z) / (12.0f * W);
        }

        static inline float clip (float x,
                                  float thresholdPos, float thresholdNeg,
                                  float kneePos, float kneeNeg) noexcept
        {
            if (x >= 0.0f)
                return clipPositive (x, thresholdPos, kneePos);

            return -clipPositive (-x, thresholdNeg, kneeNeg);
        }

        static inline float clipAD1 (float x,
                                     float thresholdPos, float thresholdNeg,
                                     float kneePos, float kneeNeg) noexcept
        {
            if (x >= 0.0f)
                return clipPositiveAD1 (x, thresholdPos, kneePos);

            return clipPositiveAD1 (-x, thresholdNeg, kneeNeg);
        }

        inline float process (float x, float k) noexcept
        {
            const float threshold = std::max (1.0e-4f, 1.0f / std::max (k, 1.0e-4f));
            const float knee = threshold * 0.18f;
            return process (x, threshold, threshold, knee, knee);
        }

        inline float process (float x,
                              float thresholdPos, float thresholdNeg,
                              float kneePos, float kneeNeg) noexcept
        {
            constexpr float smallDx = 1.0e-4f;
            constexpr float blendDx = 2.0e-3f;
            constexpr float jumpReset = 8.0f;

            const float direct = clip (x, thresholdPos, thresholdNeg, kneePos, kneeNeg);
            const float posCeil = std::max (thresholdPos + kneePos + 0.25f, 0.5f);
            const float negCeil = std::max (thresholdNeg + kneeNeg + 0.25f, 0.5f);

            if (! initialised || ! std::isfinite (prev) || ! std::isfinite (ad1Prev))
            {
                prev = x;
                ad1Prev = clipAD1 (x, thresholdPos, thresholdNeg, kneePos, kneeNeg);
                initialised = true;
                return direct;
            }

            if (std::abs (x - prev) > jumpReset)
            {
                prev = x;
                ad1Prev = clipAD1 (x, thresholdPos, thresholdNeg, kneePos, kneeNeg);
                return direct;
            }

            const float ad1 = clipAD1 (x, thresholdPos, thresholdNeg, kneePos, kneeNeg);
            const float dx = x - prev;
            const float mid = clip (0.5f * (x + prev), thresholdPos, thresholdNeg, kneePos, kneeNeg);
            const float adx = std::abs (dx);
            float y = mid;

            if (adx > smallDx)
            {
                const float adaaY = (ad1 - ad1Prev) / dx;
                if (adx >= blendDx)
                {
                    y = adaaY;
                }
                else
                {
                    const float t = (adx - smallDx) / (blendDx - smallDx);
                    const float s = t * t * (3.0f - 2.0f * t);
                    y = mid + (adaaY - mid) * s;
                }
            }

            if (! std::isfinite (y) || y > posCeil || y < -negCeil)
                y = direct;

            prev = x;
            ad1Prev = ad1;
            return juce::jlimit (-negCeil, posCeil, y);
        }

        void reset() noexcept
        {
            prev = 0.0f;
            ad1Prev = 0.0f;
            initialised = false;
        }
    };

    // ----------------------------------------------------------------
    //  ADAA-2 (2nd-order antiderivative anti-aliasing) for steep hard clippers
    //  F2(x) = second antiderivative of tanh(k*x)
    // ----------------------------------------------------------------

    // Second antiderivative of tanh(k*x):
    //   F2(x) = (1/k^2) * [ Li2(-e^{-2kx}) + kx*ln(1+e^{-2kx}) + (kx)^2/2 ]
    //   Stable approx: use direct integration of F1
    //   F2(x) ~= x*F1(x) - (1/(2k^2))*ln^2(cosh(k*x)) ... complex.
    //   Practical: numerical F2 via Simpson integration of F1.
    //   Better: closed-form  F2(x) = x*F1(x) - (1/k^2)*( Li2(-e^{2kx}) + kx*ln(1+e^{-2kx}) )
    //   Simplest accurate approach: store two previous samples + AD1 values.
    inline float tanhAD2 (float x, float k) noexcept
    {
        const float kx = k * x;
        const float akx = std::abs (kx);
        const float lnCosh = akx + fastLog1p (fastExp (-2.0f * akx)) - kLn2;
        const float f1 = lnCosh / k;
        return 0.5f * x * f1 + lnCosh * (1.0f / (2.0f * k * k + 1.0e-10f));
    }

    struct TanhADAA2
    {
        float x1 = 0.0f, x2 = 0.0f;
        float ad1_1 = 0.0f;
        float ad2_1 = 0.0f, ad2_2 = 0.0f;

        inline float process (float x0, float k) noexcept
        {
            constexpr float kTol = 1.0e-5f;
            const float ad1_0 = tanhAD1 (x0, k);
            const float ad2_0 = tanhAD2 (x0, k);

            const float dx = x0 - x2;
            const float adx = std::abs (dx);

            // Smooth blend between ADAA-2 and fallback to avoid clicks
            // at low signal levels when dx oscillates around kTol
            float y;
            if (adx < kTol)
            {
                // Pure fallback: midpoint tanh
                y = std::tanh (k * 0.5f * (x0 + x1));
            }
            else
            {
                // ADAA-2: second divided difference of F2
                const float dx01 = x0 - x1;
                const float dx12 = x1 - x2;
                const float dd01 = (std::abs (dx01) < kTol)
                    ? ad1_0
                    : (ad2_0 - ad2_1) / dx01;
                const float dd12 = (std::abs (dx12) < kTol)
                    ? ad1_1
                    : (ad2_1 - ad2_2) / dx12;
                const float adaa2 = 2.0f * (dd01 - dd12) / dx;

                // Crossfade zone: blend ADAA-2 with fallback when near tolerance
                // This prevents clicks from hard switching between paths
                const float blendZone = 10.0f * kTol;  // 1e-4
                if (adx < blendZone)
                {
                    const float fb = std::tanh (k * 0.5f * (x0 + x1));
                    const float t = (adx - kTol) / (blendZone - kTol);
                    y = fb + (adaa2 - fb) * t;
                }
                else
                {
                    y = adaa2;
                }
            }

            x2 = x1;      x1 = x0;
            ad1_1 = ad1_0;
            ad2_2 = ad2_1; ad2_1 = ad2_0;
            return y;
        }

        void reset() noexcept
        {
            x1 = x2 = 0.0f;
            ad1_1 = 0.0f;
            ad2_1 = ad2_2 = 0.0f;
        }
    };
} // namespace adaa

// ----------------------------------------------------------------
//  REACT  (Airwindows-inspired circular buffer energy tracker)
// ----------------------------------------------------------------
struct ReactState
{
    float buf[kReactBufSize] = {};
    float control = 0.0f;
    int   gcount  = 0;

    void reset() noexcept
    {
        std::memset (buf, 0, sizeof (buf));
        control = 0.0f;
        gcount  = 0;
    }
};

inline void reactTrackEnergy (ReactState& s, float input, int windowSize) noexcept
{
    if (s.gcount < 0 || s.gcount >= kReactBufSize)
        s.gcount = kReactBufSize - 1;

    const float absIn = std::abs (input);
    s.buf[s.gcount] = absIn;
    s.control += absIn;

    int oldIdx = s.gcount + windowSize;
    if (oldIdx >= kReactBufSize) oldIdx -= kReactBufSize;
    s.control -= s.buf[oldIdx];

    s.gcount--;

    if (s.control > (float) windowSize) s.control = (float) windowSize;
    if (s.control < 0.0f) s.control = 0.0f;
}

inline float reactGetDepletion (const ReactState& s, int windowSize) noexcept
{
    return s.control / ((float) windowSize + 0.001f);
}

// ----------------------------------------------------------------
//  Multiband REACT  (3-band energy tracker: sub / mid / air)
//  Crossover at ~200Hz (sub/mid) and ~4kHz (mid/air).
//  Each band has its own energy window -> frequency-dependent sag.
// ----------------------------------------------------------------
struct MultibandReactState
{
    // Per-band 1st-order crossover filter states (per-channel)
    float lpSub  = 0.0f;   // LP for sub band (~200Hz)
    float lpAir  = 0.0f;   // LP for mid/air split (~4kHz)

    // Per-band energy trackers
    ReactState sub;
    ReactState mid;
    ReactState air;

    // Per-band sag envelopes
    float sagSub = 0.0f;
    float sagMid = 0.0f;
    float sagAir = 0.0f;

    void reset() noexcept
    {
        lpSub = lpAir = 0.0f;
        sub.reset(); mid.reset(); air.reset();
        sagSub = sagMid = sagAir = 0.0f;
    }
};

struct MultibandSagResult
{
    float sagPreSub  = 1.0f;
    float sagPreMid  = 1.0f;
    float sagPreAir  = 1.0f;
    float sagPostSub = 1.0f;
    float sagPostMid = 1.0f;
    float sagPostAir = 1.0f;
};

struct DynamicsCompState
{
    float scLP    = 0.0f;
    float env     = 0.0f;
    float hfEnv   = 0.0f;
    float bodyEnv = 0.0f;
    float gain    = 1.0f;

    void reset() noexcept
    {
        scLP = env = hfEnv = bodyEnv = 0.0f;
        gain = 1.0f;
    }
};

struct ClipperPeakState
{
    float peakEnv = 0.0f;
    float bodyEnv = 0.0f;
    float gain    = 1.0f;

    void reset() noexcept
    {
        peakEnv = 0.0f;
        bodyEnv = 0.0f;
        gain = 1.0f;
    }
};

struct ClipperKlonState
{
    float cleanLP = 0.0f;
    float dirtyLowLP = 0.0f;
    float dirtyLP = 0.0f;
    adaa::StableTanhADAA softAdaa;

    void reset() noexcept
    {
        cleanLP = 0.0f;
        dirtyLowLP = 0.0f;
        dirtyLP = 0.0f;
        softAdaa.reset();
    }
};

struct TransistorPeakCatchState
{
    float peakEnv = 0.0f;
    float bodyEnv = 0.0f;
    float gain    = 1.0f;

    void reset() noexcept
    {
        peakEnv = 0.0f;
        bodyEnv = 0.0f;
        gain = 1.0f;
    }
};

struct TriodeReactState
{
    float sagBuf[kTriodeSagBufSize] = {};
    float bloomBuf[kTriodeBloomSlotCount] = {};
    float control = 0.0f;
    float bloomSum = 0.0f;
    float bloomSlotSum = 0.0f;
    int   gcount = 0;
    int   bloomSlot = 0;
    int   bloomSlotSamples = 0;
    int   bloomWindowSlots = 0;
    bool  bloomActive = false;
    float prevIn = 0.0f;
    float prevOut = 0.0f;
    float prevHyst = 0.0f;
    float lastSag = 0.0f;
    float lastSupply = 1.0f;
    float supplyEnv = 0.0f;
    float supplyDrop = 0.0f;
    float strikeEnv = 0.0f;
    float bloomEnv = 0.0f;
    float bloomFastDemandEnv = 0.0f;
    float bloomDemandEnv = 0.0f;
    float burnFast = 0.0f;
    float burnSlow = 0.0f;
    float burnEnv = 0.0f;
    float atrophyEnv = 0.0f;
    float reservoirDrainEnv = 0.0f;

    void reset() noexcept
    {
        std::memset (sagBuf, 0, sizeof (sagBuf));
        std::memset (bloomBuf, 0, sizeof (bloomBuf));
        control = 0.0f;
        bloomSum = 0.0f;
        bloomSlotSum = 0.0f;
        gcount = 0;
        bloomSlot = 0;
        bloomSlotSamples = 0;
        bloomWindowSlots = 0;
        bloomActive = false;
        prevIn = prevOut = prevHyst = 0.0f;
        lastSag = 0.0f;
        lastSupply = 1.0f;
        supplyEnv = 0.0f;
        supplyDrop = 0.0f;
        strikeEnv = 0.0f;
        bloomEnv = 0.0f;
        bloomFastDemandEnv = 0.0f;
        bloomDemandEnv = 0.0f;
        burnFast = 0.0f;
        burnSlow = 0.0f;
        burnEnv = 0.0f;
        atrophyEnv = 0.0f;
        reservoirDrainEnv = 0.0f;
    }
};

inline void multibandReactSplit (MultibandReactState& mb, float x,
                                 float coeffSub, float coeffAir,
                                 float& outSub, float& outMid, float& outAir) noexcept
{
    // 1st-order LP at ~200Hz -> sub
    mb.lpSub += (x - mb.lpSub) * coeffSub;
    const float subBand = mb.lpSub;
    const float hiPass  = x - mb.lpSub;

    // 1st-order LP at ~4kHz -> mid (of hi-passed signal)
    mb.lpAir += (hiPass - mb.lpAir) * coeffAir;
    const float midBand = mb.lpAir;
    const float airBand = hiPass - mb.lpAir;

    outSub = subBand;
    outMid = midBand;
    outAir = airBand;
}

inline MultibandSagResult multibandReactProcess (
    MultibandReactState& mb, float x,
    float coeffSub, float coeffAir,
    int window, float react,
    float attCoeff, float relCoeff) noexcept
{
    float subSig, midSig, airSig;
    multibandReactSplit (mb, x, coeffSub, coeffAir, subSig, midSig, airSig);

    // Track per-band energy
    reactTrackEnergy (mb.sub, subSig, window);
    reactTrackEnergy (mb.mid, midSig, window);
    reactTrackEnergy (mb.air, airSig, window);

    float depSub = reactGetDepletion (mb.sub, window);
    float depMid = reactGetDepletion (mb.mid, window);
    float depAir = reactGetDepletion (mb.air, window);

    // Per-band sag envelope (asymmetric attack/release)
    auto envFollow = [&] (float& env, float dep) {
        if (dep > env)
            env += (dep - env) * attCoeff;
        else
            env += (dep - env) * relCoeff;
    };
    envFollow (mb.sagSub, depSub);
    envFollow (mb.sagMid, depMid);
    envFollow (mb.sagAir, depAir);

    // Basses sag more than highs (realistic transformer/tube behavior)
    MultibandSagResult r;
    const float sSub = mb.sagSub * mb.sagSub;
    const float sMid = mb.sagMid * mb.sagMid;
    const float sAir = mb.sagAir * mb.sagAir;

    r.sagPreSub  = 1.0f + sSub * react * 14.0f;  // bass sags most
    r.sagPreMid  = 1.0f + sMid * react * 10.0f;
    r.sagPreAir  = 1.0f + sAir * react * 6.0f;   // treble sags least
    r.sagPostSub = 1.0f / (1.0f + sSub * react * 10.0f);
    r.sagPostMid = 1.0f / (1.0f + sMid * react * 7.0f);
    r.sagPostAir = 1.0f / (1.0f + sAir * react * 4.0f);

    return r;
}

// ----------------------------------------------------------------
//  Instability -- analog component tolerance + slow thermal drift
//  Static tolerance (dominant): per-instance hash -> fixed offset
//  Thermal drift: 3 incommensurate sub-Hz sines
// ----------------------------------------------------------------
struct DriftOsc
{
    float phase1 = 0.0f;
    float phase2 = 0.0f;
    float phase3 = 0.0f;
    float staticTol = 0.0f;     // per-component tolerance (fixed per instance)
    float dynamic = 0.0f;
    float output = 0.0f;

    // Deterministic per-component tolerance from hash seed
    void initTolerance (uint32_t seed, int paramIdx) noexcept
    {
        uint32_t h = seed ^ (uint32_t (paramIdx) * 2654435761u);
        h = ((h >> 16) ^ h) * 0x45d9f3bu;
        h = ((h >> 16) ^ h) * 0x45d9f3bu;
        h = (h >> 16) ^ h;
        staticTol = float (h & 0xFFFFu) / 32768.0f - 1.0f;  // [-1, 1]
    }

    void advance (float rate, float depth, float sampleRate) noexcept
    {
        // Very slow non-periodic thermal drift (~0.03x0.15 Hz)
        phase1 += rate / sampleRate;
        phase2 += rate * 0.7937f / sampleRate;   // cube-root of 2
        phase3 += rate * 0.6180f / sampleRate;   // 1/phi (golden ratio inverse)

        if (phase1 >= 1.0f) phase1 -= 1.0f;
        if (phase2 >= 1.0f) phase2 -= 1.0f;
        if (phase3 >= 1.0f) phase3 -= 1.0f;

        const float drift = std::sin (phase1 * kTwoPi) * 0.5f
                          + std::sin (phase2 * kTwoPi) * 0.3f
                          + std::sin (phase3 * kTwoPi) * 0.2f;
        dynamic = drift;

        // Static tolerance (70%) + slow thermal drift (30%)
        output = (staticTol * 0.7f + drift * 0.3f) * depth;
    }

    void reset() noexcept { phase1 = phase2 = phase3 = 0.0f; dynamic = 0.0f; output = 0.0f; }
};

struct InstabilityState
{
    DriftOsc gainDrift;
    DriftOsc shapeDrift;
    bool     tolerancesReady = false;

    void initTolerances (uint32_t seed) noexcept
    {
        gainDrift.initTolerance  (seed, 0);
        shapeDrift.initTolerance (seed, 2);
        tolerancesReady = true;
    }

    void reset() noexcept
    {
        gainDrift.reset();
        shapeDrift.reset();
    }
};

inline uint32_t nextInstabilitySeed() noexcept
{
    static std::atomic<uint32_t> counter { 0x5A54'5231u };
    return counter.fetch_add (0x9E37'79B9u, std::memory_order_relaxed);
}

// ----------------------------------------------------------------
//  Internal emphasis/de-emphasis EQ state (1st-order filters)
// ----------------------------------------------------------------
struct EmphasisState
{
    float preHP  = 0.0f;
    float preSh  = 0.0f;
    float postLP = 0.0f;
    float postHP = 0.0f;

    void reset() noexcept { preHP = 0.0f; preSh = 0.0f; postLP = 0.0f; postHP = 0.0f; }
};

// ----------------------------------------------------------------
//  Safety LPF for x1 mode (2nd-order Butterworth at 0.4xfs)
// ----------------------------------------------------------------
struct SafetyLPF
{
    float x1 = 0.0f, x2 = 0.0f;
    float y1 = 0.0f, y2 = 0.0f;

    void reset() noexcept { x1 = x2 = y1 = y2 = 0.0f; }
};

struct DetailState
{
    float hpZ1 = 0.0f;
    float hpZ2 = 0.0f;
    float shelfZ1 = 0.0f;
    float shelfZ2 = 0.0f;
    float env = 0.0f;
    float lastReduction = 0.0f;

    void reset() noexcept
    {
        hpZ1 = hpZ2 = shelfZ1 = shelfZ2 = env = lastReduction = 0.0f;
    }
};

// ----------------------------------------------------------------
//  Full per-channel / per-loader state
// ----------------------------------------------------------------
struct State
{
    // Per-stage dynamic state. Series should replicate the internal stage
    // black box, so these states must not be shared across passes.
    ReactState react[kMaxSeries][2];
    float sagEnvelope[kMaxSeries][2] = {};
    DynamicsCompState dynamicsComp[kMaxSeries][2];
    ClipperPeakState clipperPeak[kMaxSeries][2];
    ClipperKlonState clipperKlon[kMaxSeries][2];
    TransistorPeakCatchState transistorPeakCatch[kMaxSeries][2];
    TriodeReactState triodeReact[kMaxSeries][2];

    // Per-stage DC blocker (1st-order HPF, post-saturation)
    float dcX[kMaxSeries][2] = {};
    float dcY[kMaxSeries][2] = {};

    // TAPE flutter LFO phase
    float flutterPhase = 0.0f;

    // TAPE head bump resonance (2-pole band-pass state per-channel)
    // [series pass][channel]
    float bumpZ1[kMaxSeries][2] = {};
    float bumpZ2[kMaxSeries][2] = {};

    // Model-specific dynamic states [series pass][channel]
    float triodeBlock[kMaxSeries][2] = {};   // grid conduction / blocking memory
    float powerSag[kMaxSeries][2]    = {};   // supply compression memory
    float tapeFlux[kMaxSeries][2]    = {};   // magnetic remanence proxy
    float tapeStressEnv[kMaxSeries][2] = {}; // high-level magnetic stress / drag
    float triodeBodyPreLP[kMaxSeries][2] = {};
    float triodeBodyPostLP[kMaxSeries][2] = {};
    float triodeCouplingDc[kMaxSeries][2] = {};

    // Internal emphasis/de-emphasis (per-stage / per-channel)
    EmphasisState emphasis[kMaxSeries][2];

    // DETAIL preservation path (post-series / per-channel).
    // The clipped residual is filtered and used as an RMSC-like sidechain
    // reducer, never injected directly as audio.
    DetailState detailState[2];

    // ADAA states -- main waveshaper [series pass][channel]
    adaa::StableTanhADAA triodeAdaa[kMaxSeries][2];
    adaa::StableTanhADAA transistorCoreAdaa[kMaxSeries][2];
    adaa::TapeTanhADAA tapeAdaa[kMaxSeries][2];
    adaa::ClipperADAA clipperAdaa[kMaxSeries][2];
    // CHAR wavefolder ADAA [series pass][channel]
    adaa::SinFoldADAA girthAdaa[kMaxSeries][2];

    // Instability drift
    InstabilityState instability;
    uint32_t instabilitySeed = 0;

    // Multiband REACT (per-stage / per-channel)
    MultibandReactState mbReact[kMaxSeries][2];

    // Safety LPF (per-channel, for x1 mode)
    SafetyLPF safetyLpf[2];

    // TRANSISTOR black-box filters [series pass][channel]
    float transistorPreHP[kMaxSeries][2] = {};
    float transistorPreEdge[kMaxSeries][2] = {};
    float transistorPostLP[kMaxSeries][2] = {};
    float transistorDbgPre[kMaxSeries][2] = {};
    float transistorDbgCoreIn[kMaxSeries][2] = {};
    float transistorDbgCoreOut[kMaxSeries][2] = {};
    float transistorDbgRailIn[kMaxSeries][2] = {};
    float transistorDbgRailOut[kMaxSeries][2] = {};
    float transistorDbgPost[kMaxSeries][2] = {};
    float transistorDbgInputPad[kMaxSeries][2] = {};
    float transistorDbgSatK[kMaxSeries][2] = {};
    float transistorDbgRailThresh[kMaxSeries][2] = {};

    // Inter-stage LPF for series chaining (one-pole per pass per channel)
    float interStageLPF[kMaxSeries][2] = {};

    // Inter-stage DC blocker (coupling cap between series passes)
    float interStageDCx[kMaxSeries][2] = {};
    float interStageDCy[kMaxSeries][2] = {};

    // Current series pass / total series count (set by processBlock before waveshapers)
    int currentSeriesPass = 0;
    int currentSeriesCount = 1;

    // Model-switch detection (reset filters/feedback on change to prevent transients)
    Model lastModel = Model::Clean;

    // Parameter smoothing (one-pole IIR)
    float sDrive = 0.0f;
    float sDetailDrive = 0.0f;
    float sGirth = 0.0f;
    float sBias  = 0.0f;
    float sReact = 0.0f;
    float sMod   = 0.0f;
    float sDetail = 0.0f;
    float sInstability   = 0.0f;
    float lastTapeDrive = 0.0f;
    bool tapeWasActive = false;
    float lastTransistorDrive = 0.0f;
    float lastTransistorGirth = 0.0f;
    float lastTransistorMod = 0.0f;
    float lastTransistorBias = 0.0f;
    float lastTransistorReact = 0.0f;
    float lastTransistorShape = 0.0f;
    bool transistorWasActive = false;

    void reset()
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            safetyLpf[ch].reset();
            detailState[ch].reset();
            for (int sp = 0; sp < kMaxSeries; ++sp)
            {
                react[sp][ch].reset();
                mbReact[sp][ch].reset();
                sagEnvelope[sp][ch] = 0.0f;
                dynamicsComp[sp][ch].reset();
                clipperPeak[sp][ch].reset();
                clipperKlon[sp][ch].reset();
                transistorPeakCatch[sp][ch].reset();
                triodeReact[sp][ch].reset();
                dcX[sp][ch] = dcY[sp][ch] = 0.0f;
                emphasis[sp][ch].reset();
                bumpZ1[sp][ch] = bumpZ2[sp][ch] = 0.0f;
                triodeBlock[sp][ch] = 0.0f;
                powerSag[sp][ch] = 0.0f;
                tapeFlux[sp][ch] = 0.0f;
                tapeStressEnv[sp][ch] = 0.0f;
                triodeBodyPreLP[sp][ch] = 0.0f;
                triodeBodyPostLP[sp][ch] = 0.0f;
                triodeCouplingDc[sp][ch] = 0.0f;
                triodeAdaa[sp][ch].reset();
                transistorCoreAdaa[sp][ch].reset();
                tapeAdaa[sp][ch].reset();
                clipperAdaa[sp][ch].reset();
                girthAdaa[sp][ch].reset();
                interStageLPF[sp][ch] = 0.0f;
                interStageDCx[sp][ch] = 0.0f;
                interStageDCy[sp][ch] = 0.0f;
                transistorPreHP[sp][ch] = 0.0f;
                transistorPreEdge[sp][ch] = 0.0f;
                transistorPostLP[sp][ch] = 0.0f;
                transistorDbgPre[sp][ch] = 0.0f;
                transistorDbgCoreIn[sp][ch] = 0.0f;
                transistorDbgCoreOut[sp][ch] = 0.0f;
                transistorDbgRailIn[sp][ch] = 0.0f;
                transistorDbgRailOut[sp][ch] = 0.0f;
                transistorDbgPost[sp][ch] = 0.0f;
                transistorDbgInputPad[sp][ch] = 0.0f;
                transistorDbgSatK[sp][ch] = 0.0f;
                transistorDbgRailThresh[sp][ch] = 0.0f;
            }
        }
        flutterPhase = 0.0f;
        currentSeriesPass = 0;
        currentSeriesCount = 1;
        lastModel = Model::Clean;
        instability.reset();
        sDrive = sDetailDrive = sGirth = sBias = sReact = sMod = sDetail = sInstability = 0.0f;
        lastTapeDrive = 0.0f;
        tapeWasActive = false;
        lastTransistorDrive = 0.0f;
        lastTransistorGirth = 0.0f;
        lastTransistorMod = 0.0f;
        lastTransistorBias = 0.0f;
        lastTransistorReact = 0.0f;
        lastTransistorShape = 0.0f;
        transistorWasActive = false;
    }

    // Flush denormal-prone filter state to zero (call once per block)
    void flushDenormals() noexcept
    {
        auto fl = [] (float& v) { if (std::abs (v) < 1e-20f) v = 0.0f; };
        for (int ch = 0; ch < 2; ++ch)
        {
            fl (detailState[ch].hpZ1);
            fl (detailState[ch].hpZ2);
            fl (detailState[ch].shelfZ1);
            fl (detailState[ch].shelfZ2);
            fl (detailState[ch].env);
            fl (detailState[ch].lastReduction);
            for (int sp = 0; sp < kMaxSeries; ++sp)
            {
                fl (sagEnvelope[sp][ch]);
                fl (dcX[sp][ch]);
                fl (dcY[sp][ch]);
                fl (emphasis[sp][ch].preHP);
                fl (emphasis[sp][ch].preSh);
                fl (emphasis[sp][ch].postHP);
                fl (emphasis[sp][ch].postLP);
                fl (dynamicsComp[sp][ch].scLP);
                fl (dynamicsComp[sp][ch].env);
                fl (dynamicsComp[sp][ch].hfEnv);
                fl (dynamicsComp[sp][ch].gain);
                fl (clipperPeak[sp][ch].peakEnv);
                fl (clipperPeak[sp][ch].bodyEnv);
                fl (clipperKlon[sp][ch].cleanLP);
                fl (clipperKlon[sp][ch].dirtyLowLP);
                fl (clipperKlon[sp][ch].dirtyLP);
                fl (transistorPeakCatch[sp][ch].peakEnv);
                fl (transistorPeakCatch[sp][ch].bodyEnv);
                fl (transistorPeakCatch[sp][ch].gain);
                fl (triodeReact[sp][ch].control);
                fl (triodeReact[sp][ch].prevIn);
                fl (triodeReact[sp][ch].prevOut);
                fl (triodeReact[sp][ch].prevHyst);
                fl (triodeReact[sp][ch].lastSag);
                fl (triodeReact[sp][ch].lastSupply);
                fl (triodeReact[sp][ch].bloomSum);
                fl (triodeReact[sp][ch].bloomSlotSum);
                fl (triodeReact[sp][ch].supplyEnv);
                fl (triodeReact[sp][ch].supplyDrop);
                fl (triodeReact[sp][ch].strikeEnv);
                fl (triodeReact[sp][ch].bloomEnv);
                fl (triodeReact[sp][ch].bloomFastDemandEnv);
                fl (triodeReact[sp][ch].bloomDemandEnv);
                fl (triodeReact[sp][ch].burnFast);
                fl (triodeReact[sp][ch].burnSlow);
                fl (triodeReact[sp][ch].burnEnv);
                fl (triodeReact[sp][ch].atrophyEnv);
                fl (triodeReact[sp][ch].reservoirDrainEnv);
                fl (transistorPreHP[sp][ch]);
                fl (transistorPreEdge[sp][ch]);
                fl (transistorPostLP[sp][ch]);
                fl (transistorDbgPre[sp][ch]);
                fl (transistorDbgCoreIn[sp][ch]);
                fl (transistorDbgCoreOut[sp][ch]);
                fl (transistorDbgRailIn[sp][ch]);
                fl (transistorDbgRailOut[sp][ch]);
                fl (transistorDbgPost[sp][ch]);
                fl (transistorDbgInputPad[sp][ch]);
                fl (transistorDbgSatK[sp][ch]);
                fl (transistorDbgRailThresh[sp][ch]);
                fl (interStageLPF[sp][ch]);
                fl (interStageDCx[sp][ch]);
                fl (interStageDCy[sp][ch]);
                fl (bumpZ1[sp][ch]);
                fl (bumpZ2[sp][ch]);
                fl (triodeBlock[sp][ch]);
                fl (powerSag[sp][ch]);
                fl (tapeFlux[sp][ch]);
                fl (tapeStressEnv[sp][ch]);
                fl (triodeBodyPreLP[sp][ch]);
                fl (triodeBodyPostLP[sp][ch]);
                fl (triodeCouplingDc[sp][ch]);
            }
        }
    }
};

// ----------------------------------------------------------------
//  Internal helpers
// ----------------------------------------------------------------
namespace detail
{
    inline float fastTanh (float x) noexcept
    {
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    inline float onePoleCoeff (float freqHz, float sr) noexcept
    {
        return 1.0f - adaa::fastExp (-kTwoPi * freqHz / sr);
    }

    inline float clampF (float x, float lo, float hi) noexcept
    {
        return x < lo ? lo : (x > hi ? hi : x);
    }

    inline float sech2FromTanh (float t) noexcept
    {
        return 1.0f - t * t;
    }

    inline float smoothRect (float x, float softness) noexcept
    {
        const float s = std::max (softness, 1.0e-6f);
        return 0.5f * (std::sqrt (x * x + s * s) + x) - 0.5f * s;
    }

    inline float smoothRectDeriv (float x, float softness) noexcept
    {
        const float s = std::max (softness, 1.0e-6f);
        return 0.5f * (x / std::sqrt (x * x + s * s) + 1.0f);
    }

    inline float tube2AsymSection (float x, float asymPad, float amount) noexcept
    {
        const float pad = std::max (asymPad, 1.0f);
        float s = clampF (x / pad, -1.25f, 1.25f);
        float sharpen = -s;
        if (sharpen > 0.0f)
            sharpen = 1.0f + std::sqrt (sharpen);
        else
            sharpen = 1.0f - std::sqrt (-sharpen);

        s -= s * std::abs (s) * sharpen * amount;
        return s * pad;
    }

    inline float airwindowsTubeCurve (float x, int powerFactor) noexcept
    {
        const int pf = juce::jlimit (1, 12, powerFactor);
        x = clampF (x, -1.0f, 1.0f);
        float factor = x;
        for (int i = 0; i < pf; ++i)
            factor *= x;

        if ((pf & 1) == 1 && std::abs (x) > 1.0e-8f)
            factor = (factor / x) * std::abs (x);

        const float gainScaling = 1.0f / (float) (pf + 1);
        const float outputScaling = 1.0f + 1.0f / (float) pf;
        return (x - factor * gainScaling) * outputScaling;
    }

    inline float normalizeSmallSignal (float raw, float raw0, float slope0) noexcept
    {
        const float denom = std::max (std::abs (slope0), 1.0e-4f);
        return (raw - raw0) / denom;
    }

    inline float smoothStep01 (float t) noexcept
    {
        t = clampF (t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    inline float hotDetectorMagnitude (float x) noexcept
    {
        const float mag = std::abs (x);
        if (mag <= 1.0f)
            return mag;

        constexpr float kHotCurve = 1.5f;
        return 1.0f + adaa::fastLog1p ((mag - 1.0f) * kHotCurve) / kHotCurve;
    }

    inline float compressionBlendMix (float react) noexcept
    {
        const float t = clampF (react / 0.58f, 0.0f, 1.0f);
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    inline float morphThreeWay (float t, float a, float b, float c) noexcept
    {
        t = clampF (t, 0.0f, 1.0f);
        if (t <= 0.5f)
            return juce::jmap (smoothStep01 (t * 2.0f), a, b);

        return juce::jmap (smoothStep01 ((t - 0.5f) * 2.0f), b, c);
    }

    inline float interpDrive5 (float d, float p0, float p25, float p50,
                               float p75, float p100) noexcept
    {
        d = clampF (d, 0.0f, 1.0f);
        if (d <= 0.25f)
        {
            const float t = smoothStep01 (d / 0.25f);
            return juce::jmap (t, p0, p25);
        }
        if (d <= 0.50f)
        {
            const float t = smoothStep01 ((d - 0.25f) / 0.25f);
            return juce::jmap (t, p25, p50);
        }
        if (d <= 0.75f)
        {
            const float t = smoothStep01 ((d - 0.50f) / 0.25f);
            return juce::jmap (t, p50, p75);
        }

        const float t = smoothStep01 ((d - 0.75f) / 0.25f);
        return juce::jmap (t, p75, p100);
    }
} // namespace detail

struct DetailCoeffs
{
    float hpB0 = 0.0f;
    float hpB1 = 0.0f;
    float hpB2 = 0.0f;
    float hpA1 = 0.0f;
    float hpA2 = 0.0f;
    float shelfB0 = 0.0f;
    float shelfB1 = 0.0f;
    float shelfB2 = 0.0f;
    float shelfA1 = 0.0f;
    float shelfA2 = 0.0f;
    float envAttack = 0.0f;
    float envRelease = 0.0f;
    float correction = 0.0f;
};

inline DetailCoeffs makeDetailCoeffs (float sampleRate) noexcept
{
    DetailCoeffs coeffs;

    const float cutoff = detail::clampF (2500.0f, 20.0f, sampleRate * 0.45f);
    const float w0 = kTwoPi * cutoff / sampleRate;
    const float cosW = std::cos (w0);
    const float sinW = std::sin (w0);
    const float q = 0.70710678f; // 2nd-order Butterworth, equivalent to a clean 12 dB/oct HP.
    const float alpha = sinW / (2.0f * q);
    const float a0 = 1.0f + alpha;

    coeffs.hpB0 = ((1.0f + cosW) * 0.5f) / a0;
    coeffs.hpB1 = (-(1.0f + cosW)) / a0;
    coeffs.hpB2 = coeffs.hpB0;
    coeffs.hpA1 = (-2.0f * cosW) / a0;
    coeffs.hpA2 = (1.0f - alpha) / a0;

    constexpr float kAirShelfHz = 2000.0f;
    constexpr float kAirShelfMaxDb = 18.0f;
    constexpr float kAirShelfSlope = 0.45f;
    const float shelfHz = detail::clampF (kAirShelfHz, 20.0f, sampleRate * 0.45f);
    const float shelfW0 = kTwoPi * shelfHz / sampleRate;
    const float shelfCosW = std::cos (shelfW0);
    const float shelfSinW = std::sin (shelfW0);
    const float shelfA = std::pow (10.0f, kAirShelfMaxDb / 40.0f);
    const float shelfSqrtA = std::sqrt (shelfA);
    const float shelfAlpha = (shelfSinW * 0.5f)
                           * std::sqrt ((shelfA + 1.0f / shelfA) * (1.0f / kAirShelfSlope - 1.0f) + 2.0f);
    const float shelfA0 = (shelfA + 1.0f) - (shelfA - 1.0f) * shelfCosW
                        + 2.0f * shelfSqrtA * shelfAlpha;

    coeffs.shelfB0 = shelfA * ((shelfA + 1.0f) + (shelfA - 1.0f) * shelfCosW
                            + 2.0f * shelfSqrtA * shelfAlpha) / shelfA0;
    coeffs.shelfB1 = -2.0f * shelfA * ((shelfA - 1.0f) + (shelfA + 1.0f) * shelfCosW) / shelfA0;
    coeffs.shelfB2 = shelfA * ((shelfA + 1.0f) + (shelfA - 1.0f) * shelfCosW
                            - 2.0f * shelfSqrtA * shelfAlpha) / shelfA0;
    coeffs.shelfA1 = 2.0f * ((shelfA - 1.0f) - (shelfA + 1.0f) * shelfCosW) / shelfA0;
    coeffs.shelfA2 = ((shelfA + 1.0f) - (shelfA - 1.0f) * shelfCosW
                   - 2.0f * shelfSqrtA * shelfAlpha) / shelfA0;

    coeffs.envAttack = detail::onePoleCoeff (3183.0f, sampleRate);
    coeffs.envRelease = 1.0f;
    coeffs.correction = 1.0f;

    return coeffs;
}

inline float makeDetailHardClipDelta (float stageInput, float drive) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float threshold = detail::interpDrive5 (d,
                                                  1.08f, 0.92f, 0.72f, 0.48f, 0.24f);
    const float clipped = detail::clampF (stageInput, -threshold, threshold);
    return stageInput - clipped;
}

inline float applyDetailPreservation (float core,
                                      float clipDelta,
                                      float detailAmount,
                                      DetailState& state,
                                      const DetailCoeffs& coeffs) noexcept
{
    const float d = detail::clampF (detailAmount, 0.0f, 1.0f);
    if (d <= 1.0e-5f)
    {
        state.reset();
        return core;
    }

    // 12 dB/oct Butterworth high-pass, matching the EQ-style sidechain path
    // used by the reference rack more closely than cascaded one-poles.
    const float hpDelta = coeffs.hpB0 * clipDelta + state.hpZ1;
    state.hpZ1 = coeffs.hpB1 * clipDelta - coeffs.hpA1 * hpDelta + state.hpZ2;
    state.hpZ2 = coeffs.hpB2 * clipDelta - coeffs.hpA2 * hpDelta;

    const float preserveAmount = std::min (d * 2.0f, 1.0f);
    const float airAmount = detail::smoothStep01 ((d - 0.5f) * 2.0f);

    // Above 50%, DETAIL brightens the sidechain residual, not the audio core.
    // This makes the reducer react more to clipped HF texture without adding
    // post-core gain or acting as a static treble boost.
    const float shelfedDelta = coeffs.shelfB0 * hpDelta + state.shelfZ1;
    state.shelfZ1 = coeffs.shelfB1 * hpDelta - coeffs.shelfA1 * shelfedDelta + state.shelfZ2;
    state.shelfZ2 = coeffs.shelfB2 * hpDelta - coeffs.shelfA2 * shelfedDelta;

    constexpr float kAirShelfMaxDb = 18.0f;
    constexpr float kAirShelfMaxGain = 7.94328235f; // 10^(18/20)
    constexpr float kDbToNaturalGain = 0.11512925465f; // ln(10)/20
    const float targetAirGain = adaa::fastExp (airAmount * kAirShelfMaxDb * kDbToNaturalGain);
    const float airMix = detail::clampF ((targetAirGain - 1.0f) / (kAirShelfMaxGain - 1.0f),
                                         0.0f, 1.0f);
    const float sideSource = hpDelta + (shelfedDelta - hpDelta) * airMix;

    const float ceiling = 0.57f;
    const float limited = detail::clampF (sideSource, -ceiling, ceiling);

    const float sideTarget = std::abs (limited) * preserveAmount;
    const float envCoeff = sideTarget > state.env ? coeffs.envAttack : coeffs.envRelease;
    state.env += (sideTarget - state.env) * envCoeff;

    // RMSC/Compactor-style sidechain reduction: the HP clipped residual
    // controls how much magnitude is subtracted from the already-saturated
    // signal. It never injects the delta as audio, so no input/core means no
    // generated output.
    const float coreAbs = std::abs (core);
    const float targetReduction = std::min (state.env, coreAbs);
    state.lastReduction += (targetReduction - state.lastReduction) * coeffs.correction;
    state.lastReduction = std::min (state.lastReduction, coreAbs);

    const float preserved = core >= 0.0f ? core - state.lastReduction
                                         : core + state.lastReduction;

    return preserved;
}

struct DynamicsCompResult
{
    float sample    = 0.0f;
    float driveLift = 1.0f;
    float amount    = 0.0f;
};

struct ClipperPeakResult
{
    float sample = 0.0f;
    float amount = 0.0f;
};

struct TriodeReactResult
{
    float sample    = 0.0f;
    float supply    = 1.0f;
    float biasShift = 0.0f;
    float amount    = 0.0f;
};

inline DynamicsCompResult processTapeComp (float x, DynamicsCompState& st,
                                           float react, float drive, float program,
                                           float sr) noexcept
{
    DynamicsCompResult r;
    r.sample = x;

    if (react <= 0.0001f)
        return r;

    const float splitHz = juce::jmap (react, 1100.0f, 2600.0f);
    const float splitC  = detail::onePoleCoeff (splitHz, sr);
    st.scLP += (x - st.scLP) * splitC;

    const float low  = st.scLP;
    const float high = x - low;
    const float highWeight = 1.15f + react * 1.85f;
    const bool hotInput = std::abs (x) > 1.0f;
    const float lowDet  = hotInput ? detail::hotDetectorMagnitude (low)  : std::abs (low);
    const float highDet = hotInput ? detail::hotDetectorMagnitude (high) : std::abs (high);
    const float detSq = lowDet * lowDet * (0.42f + react * 0.10f)
                      + highDet * highDet * highWeight;
    const float detector = std::sqrt (std::max (detSq, 0.0f) + 1.0e-12f);

    const float optoAmount = detail::smoothStep01 (react);
    const float attackHz  = 18.0f + optoAmount * 38.0f + drive * 14.0f;
    const float fastReleaseHz = 2.1f + program * (1.3f + react * 1.6f);
    const float slowReleaseHz = 0.16f + react * 0.16f + program * 0.26f;
    const float atk = detail::onePoleCoeff (attackHz, sr);
    const float rel = detail::onePoleCoeff (fastReleaseHz, sr);

    if (detector > st.env)
        st.env += (detector - st.env) * atk;
    else
        st.env += (detector - st.env) * rel;

    const float bodyAtk = detail::onePoleCoeff (6.0f + react * 10.0f, sr);
    const float bodyRel = detail::onePoleCoeff (slowReleaseHz, sr);
    if (detector > st.bodyEnv)
        st.bodyEnv += (detector - st.bodyEnv) * bodyAtk;
    else
        st.bodyEnv += (detector - st.bodyEnv) * bodyRel;

    const float hfDet = highDet * (1.0f + react * 1.2f);
    const float hfAtk = detail::onePoleCoeff (80.0f + react * 190.0f + drive * 45.0f, sr);
    const float hfRel = detail::onePoleCoeff (2.4f + react * 4.8f + program * 3.2f, sr);
    if (hfDet > st.hfEnv)
        st.hfEnv += (hfDet - st.hfEnv) * hfAtk;
    else
        st.hfEnv += (hfDet - st.hfEnv) * hfRel;

    const float threshold = juce::jmap (react, 0.34f, 0.14f)
                          * juce::jmap (drive, 1.04f, 0.90f);
    const float ratio = juce::jmap (optoAmount, 1.18f, 3.2f);
    const float bodyRef = st.bodyEnv * (0.92f + program * 0.06f);
    const float transient = std::max (0.0f, st.env - bodyRef);
    const float transientNorm = detail::smoothStep01 (transient / std::max (st.env + 1.0e-4f, 1.0e-4f));
    const float programBlend = juce::jlimit (0.0f, 0.86f, 0.35f + transientNorm * 0.40f + optoAmount * 0.10f);
    const float programEnv = st.bodyEnv + (st.env - st.bodyEnv) * programBlend;
    const float over = programEnv / std::max (threshold, 1.0e-4f);
    const float knee = detail::smoothStep01 ((over - 0.78f) / 0.92f);

    float compGain = 1.0f;
    if (over > 1.0f)
        compGain = std::pow (over, -(ratio - 1.0f) / ratio);
    compGain = juce::jlimit (0.35f, 1.0f, juce::jmap (knee, 1.0f, compGain));

    const float hfOver = st.hfEnv / std::max (threshold * (0.74f - react * 0.08f), 1.0e-4f);
    const float hfKnee = detail::smoothStep01 ((hfOver - 0.78f) / 0.65f);
    float hfGain = 1.0f;
    if (hfOver > 1.0f)
        hfGain = 1.0f / (1.0f + (hfOver - 1.0f) * (0.28f + react * 1.05f));
    hfGain = juce::jlimit (0.46f, 1.0f, juce::jmap (hfKnee, 1.0f, hfGain));

    const float makeup = 1.0f + (1.0f - compGain)
                                   * (0.05f + 0.06f * program + 0.04f * react);
    const float targetGain = juce::jlimit (0.35f, 1.0f, compGain * makeup);
    const float gainAtk = detail::onePoleCoeff (22.0f + react * 58.0f, sr);
    const float gainReleaseHz = slowReleaseHz + (fastReleaseHz - slowReleaseHz)
                              * (0.30f + transientNorm * 0.42f + program * 0.12f);
    const float gainRel = detail::onePoleCoeff (gainReleaseHz, sr);

    if (targetGain < st.gain)
        st.gain += (targetGain - st.gain) * gainAtk;
    else
        st.gain += (targetGain - st.gain) * gainRel;

    r.sample = (low + high * hfGain) * st.gain;
    r.driveLift = 1.0f + (1.0f - compGain) * react * (0.04f + 0.04f * program);
    r.amount = 1.0f - compGain;
    return r;
}

inline DynamicsCompResult processDiodeComp (float x, DynamicsCompState& st,
                                            float react, float drive, float program,
                                            float sr) noexcept
{
    DynamicsCompResult r;
    r.sample = x;

    if (react <= 0.0001f)
        return r;

    const float reactDepth = detail::clampF (react, 0.0f, 1.0f);
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float p = detail::clampF (program, 0.0f, 1.0f);
    const float detector = detail::hotDetectorMagnitude (x) * (1.0f + d * 0.08f);

    // Diode-bridge style compression is full-wave and feedback-like: a fast
    // control path catches level changes while a slower body reference keeps
    // the recovery musical instead of tape/HF-split based.
    const float bodyAtk = detail::onePoleCoeff (34.0f + reactDepth * 76.0f, sr);
    const float bodyRel = detail::onePoleCoeff (0.70f + reactDepth * 1.90f + p * 1.25f, sr);
    if (detector > st.scLP)
        st.scLP += (detector - st.scLP) * bodyAtk;
    else
        st.scLP += (detector - st.scLP) * bodyRel;

    const float attackHz = 62.0f + reactDepth * 185.0f + d * 44.0f;
    const float releaseHz = 0.95f + reactDepth * 2.55f + p * (1.55f + reactDepth * 3.35f);
    const float atk = detail::onePoleCoeff (attackHz, sr);
    const float rel = detail::onePoleCoeff (releaseHz, sr);
    if (detector > st.env)
        st.env += (detector - st.env) * atk;
    else
        st.env += (detector - st.env) * rel;

    const float bridgeDetector = st.env * (0.76f + reactDepth * 0.08f)
                               + st.scLP * (0.24f - reactDepth * 0.08f);
    const float threshold = juce::jmap (reactDepth, 0.34f, 0.115f)
                          * juce::jmap (d, 1.06f, 0.82f);
    const float ratio = juce::jmap (reactDepth, 1.5f, 6.0f);
    const float over = bridgeDetector / std::max (threshold, 1.0e-4f);
    const float knee = detail::smoothStep01 ((over - 0.84f) / 0.72f);

    float compGain = 1.0f;
    if (over > 1.0f)
        compGain = std::pow (over, -(ratio - 1.0f) / ratio);

    const float bridgeClamp = detail::smoothStep01 ((reactDepth - 0.68f) / 0.32f)
                            * detail::smoothStep01 ((over - 1.25f) / 2.25f);
    compGain *= 1.0f - bridgeClamp * 0.065f;
    compGain = juce::jlimit (0.30f, 1.0f, juce::jmap (knee, 1.0f, compGain));

    const float makeup = 1.0f + (1.0f - compGain) * (0.025f + p * 0.045f);
    const float targetGain = juce::jlimit (0.30f, 1.0f, compGain * makeup);
    const float gainAtk = detail::onePoleCoeff (attackHz * 1.22f, sr);
    const float gainRel = detail::onePoleCoeff (releaseHz, sr);
    if (targetGain < st.gain)
        st.gain += (targetGain - st.gain) * gainAtk;
    else
        st.gain += (targetGain - st.gain) * gainRel;

    st.hfEnv += (detector - st.hfEnv) * detail::onePoleCoeff (releaseHz * 0.80f + 0.45f, sr);

    r.sample = x * st.gain;
    r.amount = 1.0f - st.gain;
    return r;
}

inline DynamicsCompResult processTransistorComp (float x, DynamicsCompState& st,
                                                 float react, float drive, float type,
                                                 float sr) noexcept
{
    DynamicsCompResult r;
    r.sample = x;

    if (react <= 0.0001f)
        return r;

    const float det = detail::hotDetectorMagnitude (x);
    const float detectorTilt = juce::jmap (type, 1.05f, 1.10f);
    const float detector = det * detectorTilt;

    const float allButtons = detail::smoothStep01 ((react - 0.82f) / 0.18f) * type;
    const float attackHz = juce::jmap (type,
                                       1150.0f + react * 2500.0f,
                                       1350.0f + react * 3000.0f)
                         * (1.0f + allButtons * 0.12f);
    const float releaseFastHz = juce::jmap (type,
                                            1.45f + react * 5.2f,
                                            1.65f + react * 6.2f)
                              + detector * juce::jmap (type, 1.8f, 2.3f);
    const float releaseSlowHz = juce::jmap (type,
                                            0.58f + react * 1.35f,
                                            0.72f + react * 1.65f);
    const float atk = detail::onePoleCoeff (attackHz, sr);
    const float rel = detail::onePoleCoeff (releaseFastHz, sr);

    if (detector > st.env)
        st.env += (detector - st.env) * atk;
    else
        st.env += (detector - st.env) * rel;

    const float bodyAtk = detail::onePoleCoeff (28.0f + react * 72.0f, sr);
    const float bodyRel = detail::onePoleCoeff (releaseSlowHz, sr);
    if (detector > st.hfEnv)
        st.hfEnv += (detector - st.hfEnv) * bodyAtk;
    else
        st.hfEnv += (detector - st.hfEnv) * bodyRel;

    const float threshold = juce::jmap (type,
                                        juce::jmap (react, 0.36f, 0.095f),
                                        juce::jmap (react, 0.35f, 0.090f))
                          * juce::jmap (drive, 1.02f, 0.78f);
    const float ratioNorm = detail::smoothStep01 (react);
    const float ratioBlackface = 4.0f + ratioNorm * ratioNorm * 8.0f;
    const float ratioBlueStripe = 4.5f + ratioNorm * ratioNorm * 9.5f;
    const float baseRatio = juce::jmap (type, ratioBlackface, ratioBlueStripe);
    const float ratio = juce::jlimit (4.0f, 20.0f, baseRatio + allButtons * 6.0f);
    const float bodyRef = st.hfEnv * (0.80f + react * 0.16f);
    const float transient = std::max (0.0f, st.env - bodyRef);
    const float transientNorm = detail::smoothStep01 (transient / std::max (st.env + 1.0e-4f, 1.0e-4f));
    const float programBlend = juce::jlimit (0.0f, 1.0f, 0.54f + transientNorm * 0.34f + allButtons * 0.10f);
    const float programEnv = st.hfEnv + (st.env - st.hfEnv) * programBlend;
    const float over = programEnv / std::max (threshold, 1.0e-4f);

    float compGain = 1.0f;
    if (over > 1.0f)
    {
        const float knee = 0.22f - allButtons * 0.06f;
        const float kneeBlend = detail::smoothStep01 ((over - 1.0f) / std::max (knee, 1.0e-4f));
        const float hardGain = std::pow (over, -(ratio - 1.0f) / ratio);
        compGain = juce::jmap (kneeBlend, 1.0f, hardGain);
    }

    compGain *= 1.0f - allButtons * detail::smoothStep01 ((over - 1.0f) / 1.6f) * 0.14f;
    compGain = juce::jlimit (0.18f, 1.0f, compGain);

    const float makeup = 1.0f + (1.0f - compGain) * juce::jmap (type, 0.00f, 0.02f);
    const float targetGain = juce::jlimit (0.18f, 1.0f, compGain * makeup);
    const float gainAtk = detail::onePoleCoeff (attackHz * juce::jmap (type, 1.35f, 1.55f), sr);
    const float gainReleaseHz = releaseSlowHz + (releaseFastHz - releaseSlowHz)
                              * (0.28f + transientNorm * 0.60f + allButtons * 0.12f);
    const float gainRel = detail::onePoleCoeff (gainReleaseHz, sr);

    if (targetGain < st.gain)
        st.gain += (targetGain - st.gain) * gainAtk;
    else
        st.gain += (targetGain - st.gain) * gainRel;

    st.scLP = detector;

    r.sample = x * st.gain;
    r.amount = 1.0f - st.gain;
    return r;
}

inline DynamicsCompResult processTransistorPeakCatch (float x, float detectorInput,
                                                      TransistorPeakCatchState& st,
                                                      float react, float drive, float type,
                                                      float sr) noexcept
{
    DynamicsCompResult r;
    r.sample = x;

    if (react <= 0.0001f)
        return r;

    const float reactDepth = detail::clampF (react, 0.0f, 1.0f);
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float absX = (std::abs (x) > 1.0f)
        ? detail::hotDetectorMagnitude (detectorInput)
        : std::abs (detectorInput);

    // FET-style catch: nearly immediate peak detector over nominal 0 dB,
    // with a slower body reference so sustained material releases naturally.
    const float peakAtk = detail::onePoleCoeff (5400.0f + reactDepth * 5200.0f, sr);
    const float peakRel = detail::onePoleCoeff (7.0f + reactDepth * 18.0f + d * 8.0f, sr);
    if (absX > st.peakEnv)
        st.peakEnv += (absX - st.peakEnv) * peakAtk;
    else
        st.peakEnv += (absX - st.peakEnv) * peakRel;

    const float bodyAtk = detail::onePoleCoeff (42.0f + reactDepth * 72.0f, sr);
    const float bodyRel = detail::onePoleCoeff (1.25f + reactDepth * 2.25f, sr);
    if (absX > st.bodyEnv)
        st.bodyEnv += (absX - st.bodyEnv) * bodyAtk;
    else
        st.bodyEnv += (absX - st.bodyEnv) * bodyRel;

    const float peakCatch = detail::smoothStep01 ((st.peakEnv - 1.0f) / 1.0f);
    const float transient = std::max (0.0f, st.peakEnv - st.bodyEnv * 0.92f);
    const float transientCatch = detail::smoothStep01 ((transient - 0.06f) / 0.42f);
    const float driveSusceptibility = 0.55f + detail::smoothStep01 ((d - 0.42f) / 0.58f) * 0.45f;
    const float catchDepth = reactDepth * driveSusceptibility;
    const float catchTarget = juce::jlimit (
        0.0f, 1.0f,
        peakCatch * (0.78f + transientCatch * 0.22f) * catchDepth);
    const float maxCatchDb = juce::jmap (type, 3.7f, 3.2f) + reactDepth * 0.45f;
    const float targetGain = adaa::fastExp (-(catchTarget * maxCatchDb) * 0.11512925465f);

    const float gainAtk = detail::onePoleCoeff (7200.0f + reactDepth * 5200.0f, sr);
    const float gainRel = detail::onePoleCoeff (1.8f + reactDepth * 3.2f + d * 1.2f, sr);
    if (targetGain < st.gain)
        st.gain += (targetGain - st.gain) * gainAtk;
    else
        st.gain += (targetGain - st.gain) * gainRel;
    st.gain = juce::jlimit (0.55f, 1.0f, st.gain);

    float out = x * st.gain;
    const float stress = (1.0f - st.gain) * (0.006f + d * 0.014f)
                       * juce::jmap (type, 1.15f, 0.85f);
    out += out * std::abs (out) * stress;

    r.sample = out;
    r.amount = 1.0f - st.gain;
    return r;
}

inline ClipperPeakResult processClipperPeak (float x, ClipperPeakState& st,
                                             float react, float drive, float program,
                                             float sr) noexcept
{
    ClipperPeakResult r;
    r.sample = x;

    if (react <= 0.0001f)
        return r;

    const float absX = detail::hotDetectorMagnitude (x);

    const float peakAtk = detail::onePoleCoeff (1800.0f + react * 5200.0f, sr);
    const float peakRel = detail::onePoleCoeff (48.0f + react * 120.0f + drive * 60.0f, sr);
    if (absX > st.peakEnv)
        st.peakEnv += (absX - st.peakEnv) * peakAtk;
    else
        st.peakEnv += (absX - st.peakEnv) * peakRel;

    const float bodyAtk = detail::onePoleCoeff (22.0f + program * 60.0f + react * 18.0f, sr);
    const float bodyRel = detail::onePoleCoeff (3.0f + react * 5.0f + program * 4.0f, sr);
    if (absX > st.bodyEnv)
        st.bodyEnv += (absX - st.bodyEnv) * bodyAtk;
    else
        st.bodyEnv += (absX - st.bodyEnv) * bodyRel;

    const float bodyRef = st.bodyEnv * (0.96f + program * 0.08f);
    const float transient = std::max (0.0f, st.peakEnv - bodyRef);

    const float threshold = juce::jmap (react, 0.22f, 0.04f)
                          * juce::jmap (drive, 1.05f, 0.82f);
    const float window = juce::jmap (react, 0.09f, 0.025f);
    const float shave = detail::smoothStep01 ((transient - threshold)
                                            / std::max (window, 1.0e-4f));

    const float strength = (0.12f + react * 0.78f) * (0.85f + program * 0.45f);
    const float targetGain = juce::jlimit (
        0.55f, 1.0f,
        1.0f / (1.0f + shave * strength * (1.0f + transient * 4.5f)));

    const float gainAtk = detail::onePoleCoeff (1400.0f + react * 3600.0f, sr);
    const float gainRel = detail::onePoleCoeff (14.0f + program * 18.0f + react * 26.0f, sr);
    if (targetGain < st.gain)
        st.gain += (targetGain - st.gain) * gainAtk;
    else
        st.gain += (targetGain - st.gain) * gainRel;

    r.sample = x * st.gain;
    r.amount = 1.0f - st.gain;
    return r;
}

inline float getTriodeSagSenseInput (float x) noexcept
{
    // Sense the actual signal arriving at the stage. Keep headroom for
    // already-hot material so sag follows real input energy.
    return juce::jlimit (-12.0f, 12.0f, x);
}

inline TriodeReactResult processTriodeReact (float sample, float sense,
                                             TriodeReactState& st,
                                             float react,
                                             float sr,
                                             int samplesPerBloomSlot) noexcept
{
    TriodeReactResult r;
    r.sample = sample;

    if (react <= 0.0001f)
        return r;

    // Fast shape-sag branch: Airwindows-style short memory that directly
    // deforms the sample before it hits the Tube2ustyle stage. The slower
    // supply-sag branch is intentionally kept separate from this character
    // layer so it can modulate stage conditions instead of just shape.
    const float reactDepth = detail::clampF (react, 0.0f, 1.0f);
    const float shapeDepth = reactDepth;
    const float shapeDepth2 = shapeDepth * shapeDepth;
    // Pro-style sag controls are usually subtle in the lower half and get
    // much steeper near the top. Keep 0x30% gentle, but make 100% clearly
    // more extreme than the old near-linear mapping.
    const float shapeDepthCurve = juce::jlimit (0.0f, 1.5f,
                                                shapeDepth * (0.18f + shapeDepth * 1.18f));
    const float overallscale = sr / 44100.0f;
    const float fastSagWindowSamples = 2.42f + shapeDepth2 * 30.0f;
    const int offset = juce::jlimit (1, kTriodeSagBufSize - 2,
                                     (int) std::round (fastSagWindowSamples * overallscale));

    if (st.gcount < 0 || st.gcount >= kTriodeSagBufSize)
        st.gcount = kTriodeSagBufSize - 1;

    const int idx = st.gcount;
    int oldIdx = idx + offset;
    if (oldIdx >= kTriodeSagBufSize) oldIdx -= kTriodeSagBufSize;

    // Current draw feeds the short Airwindows-style shape sag. The slower
    // supply detector below uses dB so already-hot host gain remains distinct.
    const float senseMag = std::abs (sense);
    const float senseNorm = juce::jlimit (0.0f, 4.0f, senseMag);
    const float currentDraw = senseNorm * (0.55f + 0.30f * std::min (1.0f, senseNorm))
                            + (senseNorm * senseNorm) * (0.18f + 0.22f * shapeDepth);

    // Slow supply-sag detector: model SAG as supply stiffness. Low SAG needs
    // very hot input to move; high SAG starts reacting around nominal 0 dBFS.
    const float senseDb = juce::jlimit (
        -80.0f, 24.0f,
        20.0f * std::log10 (std::max (senseMag, 1.0e-6f)));
    const float sagDepth = std::pow (reactDepth, 1.55f);
    const float supplyThresholdDb = juce::jmap (reactDepth, 18.0f, -3.0f);
    const float supplyScaleDb = juce::jmap (reactDepth, 24.0f, 10.0f);
    const float supplyOverDb = std::max (0.0f, senseDb - supplyThresholdDb);
    const float supplyTarget = juce::jlimit (
        0.0f, 1.0f,
        sagDepth * (1.0f - adaa::fastExp (-supplyOverDb / std::max (1.0f, supplyScaleDb))));
    const float supplyAttackHz = 5.0f + reactDepth * 12.0f;       // ~32 ms -> ~9 ms
    const float supplyReleaseHz = 1.35f - reactDepth * 0.95f;     // ~118 ms -> ~398 ms
    const float supplyCoeff = detail::onePoleCoeff (
        supplyTarget > st.supplyEnv ? supplyAttackHz : supplyReleaseHz, sr);
    st.supplyEnv += (supplyTarget - st.supplyEnv) * supplyCoeff;
    st.supplyDrop = juce::jlimit (0.0f, 1.0f, st.supplyEnv);

    // Bloom remembers hot input, sustained supply collapse and current demand.
    // Demand starts around -6 dBFS and curves toward +24 dBFS, so hot playing
    // gets a supply-recovery bloom without changing the Tube waveshaper core.
    const float hotOverDb = std::max (0.0f, senseDb);
    const float hotTarget = (1.0f - adaa::fastExp (-hotOverDb / (10.0f - reactDepth * 3.0f)))
                          * reactDepth;
    const float demandOverDb = std::max (0.0f, senseDb + 6.0f);
    const float demandCurve = detail::smoothStep01 (juce::jlimit (0.0f, 1.0f,
                                                                  demandOverDb / 30.0f));
    const float demandTarget = demandCurve * sagDepth;
    const float fastDemandAttackHz = 24.0f + reactDepth * 48.0f;  // ~7 ms -> ~2 ms
    const float fastDemandReleaseHz = 2.6f + reactDepth * 1.4f;   // ~61 ms -> ~40 ms
    const float fastDemandCoeff = detail::onePoleCoeff (
        demandTarget > st.bloomFastDemandEnv ? fastDemandAttackHz : fastDemandReleaseHz, sr);
    st.bloomFastDemandEnv += (demandTarget - st.bloomFastDemandEnv) * fastDemandCoeff;
    st.bloomFastDemandEnv = juce::jlimit (0.0f, 1.0f, st.bloomFastDemandEnv);

    const float demandAttackHz = juce::jmap (reactDepth, 5.3f, 2.9f); // ~30 ms -> ~55 ms
    const float demandReleaseHz = juce::jmap (reactDepth, 0.45f, 0.099f); // ~350 ms -> ~1.6 s
    const float demandCoeff = detail::onePoleCoeff (
        demandTarget > st.bloomDemandEnv ? demandAttackHz : demandReleaseHz, sr);
    st.bloomDemandEnv += (demandTarget - st.bloomDemandEnv) * demandCoeff;
    st.bloomDemandEnv = juce::jlimit (0.0f, 1.0f, st.bloomDemandEnv);
    const float demandRecovery = juce::jlimit (
        0.0f, 1.0f,
        std::max (0.0f, st.bloomDemandEnv - st.bloomFastDemandEnv * 0.35f)
            + st.bloomDemandEnv * 0.22f);
    const float demandBloomTarget = juce::jlimit (0.0f, 1.0f,
                                                  demandRecovery * (0.58f + reactDepth * 0.92f));
    const float bloomInputTarget = juce::jlimit (
        0.0f, 1.0f,
        std::max (std::max (hotTarget, supplyTarget * (0.25f + reactDepth * 0.50f)),
                  demandBloomTarget));
    const float strikeAttackHz = 40.0f + reactDepth * 45.0f;     // ~4 ms -> ~2 ms
    const float strikeReleaseHz = 2.3f + reactDepth * 1.8f;      // ~70 ms -> ~39 ms
    const float strikeCoeff = detail::onePoleCoeff (
        hotTarget > st.strikeEnv ? strikeAttackHz : strikeReleaseHz, sr);
    st.strikeEnv += (hotTarget - st.strikeEnv) * strikeCoeff;
    st.strikeEnv = juce::jlimit (0.0f, 1.0f, st.strikeEnv);

    const float bloomWindowMs = 45.0f + (reactDepth * reactDepth) * 905.0f;
    const int targetBloomWindowSlots = juce::jlimit (1, kTriodeBloomSlotCount - 2,
                                                     (int) std::round (bloomWindowMs));

    // Airwindows-style bloom memory: a real time window, stored in 1 ms slots
    // so the musical recovery length stays consistent at any host SR or
    // oversampling factor. React 100% now reaches a long ~950 ms bloom tail.
    if (! st.bloomActive)
    {
        std::memset (st.bloomBuf, 0, sizeof (st.bloomBuf));
        st.bloomSum = 0.0f;
        st.bloomSlotSum = 0.0f;
        st.bloomSlot = 0;
        st.bloomSlotSamples = 0;
        st.bloomWindowSlots = targetBloomWindowSlots;
        st.bloomEnv = 0.0f;
        st.bloomActive = true;
    }

    if (st.bloomSlot < 0 || st.bloomSlot >= kTriodeBloomSlotCount)
        st.bloomSlot = 0;
    if (st.bloomSlotSamples < 0 || st.bloomSlotSamples > samplesPerBloomSlot)
        st.bloomSlotSamples = 0;
    if (st.bloomWindowSlots < 1 || st.bloomWindowSlots > kTriodeBloomSlotCount - 2)
        st.bloomWindowSlots = targetBloomWindowSlots;

    st.bloomSlotSum += bloomInputTarget;
    ++st.bloomSlotSamples;

    if (st.bloomSlotSamples >= samplesPerBloomSlot)
    {
        const int writeSlot = st.bloomSlot;
        const float slotValue = st.bloomSlotSum / (float) st.bloomSlotSamples;
        const int oldBloomWindowSlots = st.bloomWindowSlots;

        int dropSlot = writeSlot - oldBloomWindowSlots;
        if (dropSlot < 0)
            dropSlot += kTriodeBloomSlotCount;

        st.bloomBuf[writeSlot] = slotValue;
        ++st.bloomSlot;
        if (st.bloomSlot >= kTriodeBloomSlotCount)
            st.bloomSlot = 0;

        st.bloomSlotSum = 0.0f;
        st.bloomSlotSamples = 0;

        st.bloomSum += slotValue - st.bloomBuf[dropSlot];

        if (targetBloomWindowSlots > oldBloomWindowSlots)
        {
            for (int distance = oldBloomWindowSlots; distance < targetBloomWindowSlots; ++distance)
            {
                int addSlot = writeSlot - distance;
                if (addSlot < 0)
                    addSlot += kTriodeBloomSlotCount;
                st.bloomSum += st.bloomBuf[addSlot];
            }
        }
        else if (targetBloomWindowSlots < oldBloomWindowSlots)
        {
            for (int distance = targetBloomWindowSlots; distance < oldBloomWindowSlots; ++distance)
            {
                int removeSlot = writeSlot - distance;
                if (removeSlot < 0)
                    removeSlot += kTriodeBloomSlotCount;
                st.bloomSum -= st.bloomBuf[removeSlot];
            }
        }

        st.bloomWindowSlots = targetBloomWindowSlots;
    }

    const int activeBloomWindowSlots = juce::jlimit (1, kTriodeBloomSlotCount - 2,
                                                     st.bloomWindowSlots);
    const float bloomTarget = juce::jlimit (0.0f, 1.0f,
                                            st.bloomSum / (float) activeBloomWindowSlots);
    const float bloomRiseHz = 4.8f + reactDepth * 3.6f;
    const float bloomFallHz = juce::jmap (reactDepth, 0.55f, 0.227f); // ~290 ms -> ~700 ms
    const float bloomCoeff = detail::onePoleCoeff (
        bloomTarget > st.bloomEnv ? bloomRiseHz : bloomFallHz, sr);
    st.bloomEnv += (bloomTarget - st.bloomEnv) * bloomCoeff;
    st.bloomEnv = juce::jlimit (0.0f, 1.0f, st.bloomEnv);

    // Subsonic burn is not a free LFO: it is the small mechanical wobble left
    // by supply movement. A fast/slow pair gives motion only while sag changes.
    const float burnDrive = st.supplyDrop * (0.35f + reactDepth * 0.65f);
    const float burnFastCoeff = detail::onePoleCoeff (2.6f + reactDepth * 2.4f, sr);
    const float burnSlowCoeff = detail::onePoleCoeff (0.24f + reactDepth * 0.36f, sr);
    st.burnFast += (burnDrive - st.burnFast) * burnFastCoeff;
    st.burnSlow += (burnDrive - st.burnSlow) * burnSlowCoeff;
    st.burnEnv = juce::jlimit (-1.0f, 1.0f,
                               (st.burnFast - st.burnSlow) * (0.65f + reactDepth * 0.55f));

    // Sustained hot input should also make the virtual supply feel atrophied:
    // an exponential level-dependent compressor with amp-like long recovery.
    const float atrophyDrive = (1.0f - adaa::fastExp (-hotOverDb / 7.5f)) * sagDepth;
    const float atrophyTarget = juce::jlimit (0.0f, 1.0f, atrophyDrive);
    const float atrophyAttackHz = 7.5f + reactDepth * 8.5f;       // ~21 ms -> ~10 ms
    const float atrophyReleaseHz = juce::jmap (reactDepth, 1.10f, 0.318f); // ~145 ms -> ~500 ms
    const float atrophyCoeff = detail::onePoleCoeff (
        atrophyTarget > st.atrophyEnv ? atrophyAttackHz : atrophyReleaseHz, sr);
    st.atrophyEnv += (atrophyTarget - st.atrophyEnv) * atrophyCoeff;
    st.atrophyEnv = juce::jlimit (0.0f, 1.0f, st.atrophyEnv);

    // The reservoir drain is slower and only appears on very hot material:
    // it simulates the supply reserve taking seconds, not milliseconds, to recover.
    const float drainOverDb = std::max (0.0f, senseDb - 6.0f);
    const float drainHot = std::pow (1.0f - adaa::fastExp (-drainOverDb / 9.0f), 1.35f);
    const float drainSusceptibility = 0.15f + sagDepth * 0.85f;
    const float drainTarget = juce::jlimit (0.0f, 1.0f, drainHot * drainSusceptibility);
    const float drainAttackHz = 1.15f + reactDepth * 0.85f;       // ~138 ms -> ~80 ms
    const float drainReleaseHz = juce::jmap (reactDepth, 0.90f, 0.106f); // ~177 ms -> ~1.5 s
    const float drainCoeff = detail::onePoleCoeff (
        drainTarget > st.reservoirDrainEnv ? drainAttackHz : drainReleaseHz, sr);
    st.reservoirDrainEnv += (drainTarget - st.reservoirDrainEnv) * drainCoeff;
    st.reservoirDrainEnv = juce::jlimit (0.0f, 1.0f, st.reservoirDrainEnv);

    const float intensity = 0.0445556f * (0.12f + shapeDepthCurve * 1.65f);
    const float powerSag = 0.0033002237f;
    const float rawContrib = currentDraw * (intensity - (st.control * powerSag));
    const float contrib = std::max (0.0f, rawContrib);
    const float old = st.sagBuf[oldIdx];
    st.sagBuf[idx] = contrib;
    st.control += (contrib / (float) offset);
    st.control -= (old / (float) offset);
    st.gcount--;

    // Tiny leakage keeps the stage from sticking forever.
    st.control -= 1.0e-6f * overallscale;
    if (st.control < 0.0f)
        st.control = 0.0f;

    // The raw Airwindows control is subtle in its original desk context.
    // Here we remap it with depth so SAG=100% is genuinely extreme.
    float control = st.control * (0.45f + shapeDepthCurve * 7.50f + shapeDepth2 * 4.00f);
    float clamp = 1.0f;
    if (control > 1.0f)
    {
        clamp -= (control - 1.0f);
        control = 1.0f;
    }
    if (clamp < 0.35f)
        clamp = 0.35f;

    const float effectiveControl = detail::clampF (control, 0.0f, 1.0f);
    const float thickness = ((1.0f - effectiveControl) * 2.0f) - 1.0f;
    const float blend = std::abs (thickness);
    float bridgerectifier = std::abs (sample);
    if (bridgerectifier > 1.57079633f)
        bridgerectifier = 1.57079633f;
    if (thickness > 0.0f)
        bridgerectifier = std::sin (bridgerectifier);
    else
        bridgerectifier = 1.0f - std::cos (bridgerectifier);

    float sagged = sample >= 0.0f
                 ? (sample * (1.0f - blend)) + (bridgerectifier * blend)
                 : (sample * (1.0f - blend)) - (bridgerectifier * blend);
    if (clamp != 1.0f)
        sagged *= clamp;

    const float shapeWet = juce::jlimit (0.0f, 1.0f,
                                         shapeDepth * (0.35f + 0.85f * shapeDepth));
    const float supply = juce::jlimit (0.5f, 1.0f, 1.0f - st.supplyDrop * 0.5f);
    r.sample = sample + (sagged - sample) * shapeWet;
    r.amount = effectiveControl * shapeWet;
    r.supply = supply;
    r.biasShift = 0.0f;
    st.lastSag = r.amount;
    st.lastSupply = supply;
    return r;
}

inline float getTapeLevelTrim (float drive, float mod, float girth, float react) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float m = detail::clampF (mod,   0.0f, 1.0f);
    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float r = detail::clampF (react, 0.0f, 1.0f);
    const float rabbit = 1.0f - m;
    const float rabbitSq = rabbit * rabbit;

    const float driveTrimA = detail::interpDrive5 (d,
                                                   1.00f, 1.00f, 0.99f, 0.98f, 0.96f);
    const float driveTrimB = detail::interpDrive5 (d,
                                                   0.45f, 0.58f, 0.70f, 0.78f, 0.85f);
    const float driveTrim = juce::jmap (rabbitSq, driveTrimA, driveTrimB);

    const float girthAmt = g * g;
    const float girthTrim = 1.0f / (1.0f + girthAmt
                                           * (0.08f + 0.10f * rabbitSq + 0.03f * m)
                                           * (0.82f + 0.18f * d));

    const float reactTrim = 1.0f / (1.0f + r
                                           * (0.05f + 0.12f * rabbitSq + 0.015f * m)
                                           * (0.60f + 0.40f * d));

    return driveTrim * girthTrim * reactTrim;
}

inline float getTapeLevelCorrection (float drive, float girth, float mod, int seriesCount) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float m = detail::clampF (mod, 0.0f, 1.0f);
    const int seriesIndex = juce::jlimit (1, kMaxSeries, seriesCount) - 1;

    static constexpr float trims[4][3][3][5] =
    {
        {
            {
                { 0.6752f, 0.5677f, 0.6460f, 0.7959f, 0.9966f },
                { 0.6694f, 0.7421f, 0.9009f, 1.5175f, 2.0841f },
                { 0.9308f, 1.1343f, 1.4494f, 2.2313f, 2.7029f },
            },
            {
                { 0.7398f, 0.6148f, 0.6673f, 0.7911f, 0.9667f },
                { 0.6638f, 0.7238f, 0.8608f, 1.4083f, 1.9225f },
                { 0.8806f, 1.0602f, 1.3422f, 2.0475f, 2.4784f },
            },
            {
                { 0.9441f, 0.7490f, 0.6737f, 0.6802f, 0.7464f },
                { 0.5781f, 0.5891f, 0.6422f, 0.9211f, 1.2193f },
                { 0.6336f, 0.7221f, 0.8747f, 1.2759f, 1.5356f },
            },
        },
        {
            {
                { 0.3458f, 0.3426f, 0.4693f, 0.6384f, 0.9068f },
                { 0.4279f, 0.5703f, 0.7997f, 1.4781f, 1.9930f },
                { 0.8724f, 1.2160f, 1.6867f, 2.1980f, 2.5942f },
            },
            {
                { 0.4078f, 0.3943f, 0.4991f, 0.6335f, 0.8540f },
                { 0.4380f, 0.5547f, 0.7441f, 1.3515f, 1.8342f },
                { 0.7774f, 1.0652f, 1.4649f, 1.9968f, 2.3868f },
            },
            {
                { 0.7188f, 0.6181f, 0.5809f, 0.5748f, 0.6214f },
                { 0.4559f, 0.4763f, 0.5309f, 0.8647f, 1.1735f },
                { 0.4849f, 0.5970f, 0.7813f, 1.2312f, 1.5029f },
            },
        },
        {
            {
                { 0.2958f, 0.2921f, 0.4185f, 0.5766f, 0.8507f },
                { 0.3181f, 0.4924f, 0.7431f, 1.4531f, 1.9789f },
                { 0.8234f, 1.2700f, 1.8509f, 2.1835f, 2.5934f },
            },
            {
                { 0.3516f, 0.3416f, 0.4524f, 0.5786f, 0.7931f },
                { 0.3423f, 0.4857f, 0.6883f, 1.3279f, 1.8239f },
                { 0.7027f, 1.0578f, 1.5254f, 1.9835f, 2.3867f },
            },
            {
                { 0.6771f, 0.5972f, 0.5689f, 0.5601f, 0.5997f },
                { 0.4273f, 0.4530f, 0.5070f, 0.8583f, 1.1565f },
                { 0.4364f, 0.5527f, 0.7431f, 1.2287f, 1.4856f },
            },
        },
        {
            {
                { 0.2831f, 0.2786f, 0.4027f, 0.5547f, 0.8159f },
                { 0.2714f, 0.4557f, 0.7117f, 1.4446f, 1.9765f },
                { 0.7822f, 1.3070f, 1.9736f, 2.1797f, 2.5930f },
            },
            {
                { 0.3383f, 0.3280f, 0.4400f, 0.5630f, 0.7706f },
                { 0.3017f, 0.4561f, 0.6619f, 1.3219f, 1.8225f },
                { 0.6485f, 1.0460f, 1.5564f, 1.9809f, 2.3864f },
            },
            {
                { 0.6719f, 0.5972f, 0.5696f, 0.5595f, 0.5969f },
                { 0.4218f, 0.4495f, 0.5033f, 0.8578f, 1.1535f },
                { 0.4194f, 0.5366f, 0.7287f, 1.2280f, 1.4799f },
            },
        },
    };

    auto interpDrive = [d] (const float (&v)[5]) noexcept
    {
        return detail::interpDrive5 (d, v[0], v[1], v[2], v[3], v[4]);
    };

    float charTrim[3];
    for (int charIndex = 0; charIndex < 3; ++charIndex)
    {
        const float trimType0 = interpDrive (trims[seriesIndex][charIndex][0]);
        const float trimType1 = interpDrive (trims[seriesIndex][charIndex][1]);
        const float trimType2 = interpDrive (trims[seriesIndex][charIndex][2]);
        charTrim[charIndex] = detail::morphThreeWay (m, trimType0, trimType1, trimType2);
    }

    return detail::morphThreeWay (g, charTrim[0], charTrim[1], charTrim[2]);
}

inline float getTriodeLevelTrim (float drive, float mod, int seriesCount) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float tubeMorph = detail::smoothStep01 (detail::clampF (mod, 0.0f, 1.0f));
    juce::ignoreUnused (seriesCount);

    // Final post-chain trim only. Do not compensate per-series here:
    // if the stage itself is calibrated correctly, repeating it N times
    // should not need a special series loudness hack.
    const float trim12AX7 = detail::interpDrive5 (d,
                                                  1.16f, 1.13f, 1.09f, 1.04f, 0.99f);
    const float trimPower = detail::interpDrive5 (d,
                                                  1.08f, 1.06f, 1.03f, 1.00f, 0.97f);
    return juce::jmap (tubeMorph, trim12AX7, trimPower);
}

inline float getTriodeLevelCorrection (float drive, float girth, float mod, int seriesCount) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float m = detail::clampF (mod, 0.0f, 1.0f);
    const int seriesIndex = juce::jlimit (1, kMaxSeries, seriesCount) - 1;

    static constexpr float trims[4][3][3][5] =
    {
        {
            {
                { 0.9932f, 0.9417f, 0.7991f, 0.6025f, 0.5112f },
                { 1.0132f, 0.9504f, 0.7829f, 0.5830f, 0.4950f },
                { 1.1283f, 1.0403f, 0.8224f, 0.6070f, 0.5625f },
            },
            {
                { 0.9997f, 0.9489f, 0.8086f, 0.6165f, 0.5320f },
                { 1.0231f, 0.9611f, 0.7959f, 0.5998f, 0.5168f },
                { 1.1414f, 1.0542f, 0.8384f, 0.6264f, 0.5851f },
            },
            {
                { 0.9349f, 0.8912f, 0.7717f, 0.6125f, 0.5465f },
                { 0.9588f, 0.9052f, 0.7641f, 0.6011f, 0.5334f },
                { 1.0660f, 0.9900f, 0.8041f, 0.6252f, 0.5906f },
            },
        },
        {
            {
                { 1.1408f, 1.0205f, 0.7238f, 0.4225f, 0.4135f },
                { 1.1444f, 1.0039f, 0.6778f, 0.4127f, 0.3974f },
                { 1.3613f, 1.1559f, 0.7271f, 0.4479f, 0.4605f },
            },
            {
                { 1.1533f, 1.0345f, 0.7421f, 0.4434f, 0.4362f },
                { 1.1642f, 1.0248f, 0.7016f, 0.4345f, 0.4212f },
                { 1.3890f, 1.1838f, 0.7553f, 0.4745f, 0.4862f },
            },
            {
                { 1.0050f, 0.9119f, 0.6858f, 0.4604f, 0.4594f },
                { 1.0200f, 0.9103f, 0.6597f, 0.4552f, 0.4490f },
                { 1.2065f, 1.0432f, 0.7070f, 0.4907f, 0.5037f },
            },
        },
        {
            {
                { 1.3071f, 1.1038f, 0.6584f, 0.3737f, 0.3956f },
                { 1.2869f, 1.0578f, 0.5936f, 0.3713f, 0.3811f },
                { 1.6265f, 1.2761f, 0.6523f, 0.3851f, 0.4383f },
            },
            {
                { 1.3252f, 1.1242f, 0.6848f, 0.3927f, 0.4180f },
                { 1.3168f, 1.0886f, 0.6263f, 0.3912f, 0.4049f },
                { 1.6707f, 1.3184f, 0.6897f, 0.4090f, 0.4639f },
            },
            {
                { 1.0729f, 0.9296f, 0.6213f, 0.4133f, 0.4431f },
                { 1.0771f, 0.9132f, 0.5868f, 0.4149f, 0.4354f },
                { 1.3478f, 1.0903f, 0.6394f, 0.4366f, 0.4868f },
            },
        },
        {
            {
                { 1.4943f, 1.1918f, 0.6017f, 0.3533f, 0.3912f },
                { 1.4411f, 1.1121f, 0.5261f, 0.3551f, 0.3772f },
                { 1.9244f, 1.4006f, 0.5937f, 0.3660f, 0.4341f },
            },
            {
                { 1.5176f, 1.2182f, 0.6354f, 0.3713f, 0.4132f },
                { 1.4813f, 1.1524f, 0.5661f, 0.3739f, 0.4010f },
                { 1.9869f, 1.4574f, 0.6380f, 0.3872f, 0.4596f },
            },
            {
                { 1.1391f, 0.9450f, 0.5725f, 0.3938f, 0.4392f },
                { 1.1306f, 0.9144f, 0.5355f, 0.3999f, 0.4321f },
                { 1.4897f, 1.1323f, 0.5918f, 0.4159f, 0.4842f },
            },
        },
    };

    auto interpDrive = [d] (const float (&v)[5]) noexcept
    {
        return detail::interpDrive5 (d, v[0], v[1], v[2], v[3], v[4]);
    };

    float charTrim[3];
    for (int charIndex = 0; charIndex < 3; ++charIndex)
    {
        const float trimType0 = interpDrive (trims[seriesIndex][charIndex][0]);
        const float trimType1 = interpDrive (trims[seriesIndex][charIndex][1]);
        const float trimType2 = interpDrive (trims[seriesIndex][charIndex][2]);
        charTrim[charIndex] = detail::morphThreeWay (m, trimType0, trimType1, trimType2);
    }

    return detail::morphThreeWay (g, charTrim[0], charTrim[1], charTrim[2]);
}

inline float getTransistorLevelTrim (float drive, float mod, float girth, float react) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float type = detail::smoothStep01 (detail::clampF (mod, 0.0f, 1.0f));
    const float body = detail::clampF (girth, 0.0f, 1.0f);
    const float r = detail::clampF (react, 0.0f, 1.0f);
    const float bodyCurve = 1.0f - std::pow (1.0f - body, 1.35f);

    const float trimBjt = detail::interpDrive5 (d,
                                                6.988f, 2.598f, 0.946f, 0.885f, 1.148f);
    const float trimFet = detail::interpDrive5 (d,
                                                5.791f, 2.459f, 1.040f, 0.738f, 0.837f);
    const float driveTrim = juce::jmap (type, trimBjt, trimFet);

    const float bodyTrim = 1.0f / (1.0f + bodyCurve
                                          * (0.06f + 0.10f * d)
                                          * juce::jmap (type, 1.0f, 0.75f));
    const float reactTrim = 1.0f / (1.0f + r
                                           * (0.04f + 0.08f * d)
                                           * juce::jmap (type, 1.0f, 0.85f));

    return driveTrim * bodyTrim * reactTrim;
}

inline float getTransistorLevelCorrection (float drive, float girth, float mod, int seriesCount) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float t = detail::clampF (mod, 0.0f, 1.0f);
    const int seriesIndex = juce::jlimit (1, kMaxSeries, seriesCount) - 1;

    static constexpr float trims[4][3][3][5] =
    {
        {
            {
                { 1.0051f, 0.9688f, 1.0042f, 1.0315f, 1.0226f },
                { 0.9920f, 0.9634f, 1.0109f, 1.0327f, 1.0065f },
                { 1.0052f, 0.9685f, 1.0491f, 1.0766f, 1.0450f },
            },
            {
                { 0.8875f, 0.9162f, 1.0785f, 1.1807f, 1.1921f },
                { 0.8976f, 0.9162f, 1.0534f, 1.1474f, 1.1445f },
                { 0.9267f, 0.9281f, 1.0676f, 1.1603f, 1.1590f },
            },
            {
                { 0.8524f, 0.9183f, 1.1596f, 1.3103f, 1.2852f },
                { 0.8680f, 0.9142f, 1.1080f, 1.2462f, 1.2384f },
                { 0.9008f, 0.9241f, 1.1015f, 1.2339f, 1.2516f },
            },
        },
        {
            {
                { 1.0102f, 0.9408f, 0.9638f, 0.9864f, 1.0076f },
                { 0.9841f, 0.9312f, 0.9852f, 0.9813f, 0.9793f },
                { 1.0104f, 0.9403f, 1.0586f, 1.0483f, 1.0026f },
            },
            {
                { 0.7877f, 0.8534f, 1.0432f, 1.1429f, 1.1811f },
                { 0.8059f, 0.8516f, 1.0312f, 1.1004f, 1.1219f },
                { 0.8590f, 0.8699f, 1.0763f, 1.1353f, 1.1210f },
            },
            {
                { 0.7267f, 0.8579f, 1.1318f, 1.2758f, 1.2752f },
                { 0.7538f, 0.8495f, 1.0957f, 1.2032f, 1.2173f },
                { 0.8119f, 0.8639f, 1.1206f, 1.2154f, 1.2156f },
            },
        },
        {
            {
                { 1.0154f, 0.9158f, 0.9334f, 0.9785f, 1.0079f },
                { 0.9763f, 0.9029f, 0.9586f, 0.9634f, 0.9781f },
                { 1.0156f, 0.9151f, 1.0541f, 1.0246f, 0.9946f },
            },
            {
                { 0.6993f, 0.8061f, 1.0175f, 1.1378f, 1.1815f },
                { 0.7238f, 0.8018f, 1.0064f, 1.0865f, 1.1213f },
                { 0.7965f, 0.8229f, 1.0685f, 1.1145f, 1.1147f },
            },
            {
                { 0.6197f, 0.8130f, 1.1092f, 1.2714f, 1.2756f },
                { 0.6549f, 0.8007f, 1.0745f, 1.1908f, 1.2168f },
                { 0.7321f, 0.8162f, 1.1174f, 1.1969f, 1.2097f },
            },
        },
        {
            {
                { 1.0206f, 0.8936f, 0.9162f, 0.9776f, 1.0081f },
                { 0.9686f, 0.8782f, 0.9386f, 0.9588f, 0.9781f },
                { 1.0208f, 0.8927f, 1.0451f, 1.0119f, 0.9936f },
            },
            {
                { 0.6210f, 0.7704f, 1.0053f, 1.1373f, 1.1818f },
                { 0.6504f, 0.7633f, 0.9894f, 1.0836f, 1.1213f },
                { 0.7388f, 0.7849f, 1.0573f, 1.1042f, 1.1140f },
            },
            {
                { 0.5288f, 0.7797f, 1.0988f, 1.2709f, 1.2760f },
                { 0.5694f, 0.7638f, 1.0596f, 1.1882f, 1.2168f },
                { 0.6606f, 0.7784f, 1.1087f, 1.1877f, 1.2091f },
            },
        },
    };

    auto interpDrive = [d] (const float (&v)[5]) noexcept
    {
        return detail::interpDrive5 (d, v[0], v[1], v[2], v[3], v[4]);
    };

    float charTrim[3];
    for (int charIndex = 0; charIndex < 3; ++charIndex)
    {
        const float trimType0 = interpDrive (trims[seriesIndex][charIndex][0]);
        const float trimType1 = interpDrive (trims[seriesIndex][charIndex][1]);
        const float trimType2 = interpDrive (trims[seriesIndex][charIndex][2]);
        charTrim[charIndex] = detail::morphThreeWay (t, trimType0, trimType1, trimType2);
    }

    return detail::morphThreeWay (g, charTrim[0], charTrim[1], charTrim[2]);
}

inline float getDiodeLevelTrim (float drive, float girth, float mod, int seriesCount) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float t = detail::clampF (mod, 0.0f, 1.0f);
    const int seriesIndex = juce::jlimit (1, kMaxSeries, seriesCount) - 1;

    static constexpr float trims[4][3][3][5] =
    {
        {
            {
                { 0.7965f, 0.6278f, 0.4468f, 0.3956f, 0.3874f },
                { 0.7014f, 0.4599f, 0.4073f, 0.3931f, 0.3912f },
                { 0.6589f, 0.4874f, 0.3937f, 0.3683f, 0.3601f },
            },
            {
                { 1.0822f, 0.8208f, 0.4876f, 0.4030f, 0.3903f },
                { 0.9560f, 0.5219f, 0.4185f, 0.3965f, 0.3926f },
                { 0.8834f, 0.6168f, 0.4133f, 0.3749f, 0.3633f },
            },
            {
                { 1.1852f, 0.8987f, 0.5129f, 0.4064f, 0.3917f },
                { 1.0438f, 0.5636f, 0.4228f, 0.3978f, 0.3930f },
                { 0.9578f, 0.6706f, 0.4217f, 0.3772f, 0.3642f },
            },
        },
        {
            {
                { 0.6485f, 0.4622f, 0.3910f, 0.3780f, 0.3801f },
                { 0.5160f, 0.3979f, 0.3825f, 0.3833f, 0.3811f },
                { 0.4654f, 0.3757f, 0.3440f, 0.3377f, 0.3331f },
            },
            {
                { 1.1737f, 0.6705f, 0.4025f, 0.3789f, 0.3804f },
                { 0.9135f, 0.4229f, 0.3847f, 0.3830f, 0.3809f },
                { 0.7781f, 0.4244f, 0.3512f, 0.3380f, 0.3363f },
            },
            {
                { 1.4110f, 0.8057f, 0.4108f, 0.3795f, 0.3805f },
                { 1.0898f, 0.4357f, 0.3857f, 0.3828f, 0.3807f },
                { 0.9165f, 0.4564f, 0.3539f, 0.3376f, 0.3353f },
            },
        },
        {
            {
                { 0.5511f, 0.4258f, 0.3854f, 0.3765f, 0.3785f },
                { 0.4460f, 0.3832f, 0.3815f, 0.3791f, 0.3807f },
                { 0.3941f, 0.3483f, 0.3359f, 0.3318f, 0.3305f },
            },
            {
                { 1.2760f, 0.5480f, 0.3906f, 0.3770f, 0.3786f },
                { 0.8725f, 0.3949f, 0.3813f, 0.3786f, 0.3804f },
                { 0.6831f, 0.3753f, 0.3368f, 0.3304f, 0.3301f },
            },
            {
                { 1.6865f, 0.7204f, 0.3939f, 0.3773f, 0.3786f },
                { 1.1384f, 0.4020f, 0.3811f, 0.3782f, 0.3801f },
                { 0.8761f, 0.3917f, 0.3370f, 0.3291f, 0.3289f },
            },
        },
        {
            {
                { 0.4973f, 0.4241f, 0.3887f, 0.3761f, 0.3774f },
                { 0.4177f, 0.3804f, 0.3822f, 0.3784f, 0.3807f },
                { 0.3669f, 0.3387f, 0.3355f, 0.3301f, 0.3303f },
            },
            {
                { 1.3899f, 0.4729f, 0.3983f, 0.3765f, 0.3775f },
                { 0.8330f, 0.3845f, 0.3817f, 0.3780f, 0.3804f },
                { 0.5974f, 0.3547f, 0.3345f, 0.3287f, 0.3292f },
            },
            {
                { 2.0224f, 0.6420f, 0.4022f, 0.3767f, 0.3776f },
                { 1.1895f, 0.3880f, 0.3813f, 0.3777f, 0.3801f },
                { 0.8366f, 0.3652f, 0.3334f, 0.3274f, 0.3279f },
            },
        },
    };

    auto interpDrive = [d] (const float (&v)[5]) noexcept
    {
        return detail::interpDrive5 (d, v[0], v[1], v[2], v[3], v[4]);
    };

    float charTrim[3];
    for (int charIndex = 0; charIndex < 3; ++charIndex)
    {
        const float trimType0 = interpDrive (trims[seriesIndex][charIndex][0]);
        const float trimType1 = interpDrive (trims[seriesIndex][charIndex][1]);
        const float trimType2 = interpDrive (trims[seriesIndex][charIndex][2]);
        charTrim[charIndex] = detail::morphThreeWay (t, trimType0, trimType1, trimType2);
    }

    return detail::morphThreeWay (g, charTrim[0], charTrim[1], charTrim[2]);
}

inline float getClipperLevelCorrection (float drive, float girth, float mod, int seriesCount) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float m = detail::clampF (mod, 0.0f, 1.0f);
    const int seriesIndex = juce::jlimit (1, kMaxSeries, seriesCount) - 1;

    static constexpr float trims[4][3][3][5] =
    {
        {
            {
                { 1.1947f, 1.0844f, 0.9598f, 0.8698f, 0.8194f },
                { 1.4078f, 1.2698f, 1.1015f, 0.9591f, 0.9067f },
                { 1.1973f, 1.1136f, 0.9961f, 0.8773f, 0.8273f },
            },
            {
                { 1.1257f, 1.0160f, 0.9191f, 0.8560f, 0.8145f },
                { 1.3509f, 1.1967f, 1.0350f, 0.9401f, 0.9018f },
                { 1.1605f, 1.0632f, 0.9437f, 0.8600f, 0.8224f },
            },
            {
                { 1.1040f, 0.9803f, 0.9068f, 0.8511f, 0.8127f },
                { 1.3488f, 1.1747f, 1.0087f, 0.9337f, 0.9001f },
                { 1.1595f, 1.0510f, 0.9192f, 0.8541f, 0.8206f },
            },
        },
        {
            {
                { 1.3685f, 1.1336f, 0.9154f, 0.8092f, 0.7848f },
                { 1.9093f, 1.5390f, 1.1710f, 0.9515f, 0.8880f },
                { 1.4052f, 1.2139f, 0.9844f, 0.8123f, 0.7752f },
            },
            {
                { 1.2469f, 1.0163f, 0.8622f, 0.8028f, 0.7837f },
                { 1.8220f, 1.4076f, 1.0570f, 0.9240f, 0.8822f },
                { 1.3456f, 1.1227f, 0.9007f, 0.7970f, 0.7729f },
            },
            {
                { 1.2189f, 0.9581f, 0.8544f, 0.8009f, 0.7834f },
                { 1.8192f, 1.3800f, 1.0187f, 0.9135f, 0.8804f },
                { 1.3445f, 1.1046f, 0.8708f, 0.7931f, 0.7722f },
            },
        },
        {
            {
                { 1.5370f, 1.1649f, 0.8800f, 0.7844f, 0.7817f },
                { 2.5751f, 1.8311f, 1.2350f, 0.9764f, 0.8893f },
                { 1.6344f, 1.3069f, 0.9715f, 0.7797f, 0.7618f },
            },
            {
                { 1.3767f, 1.0108f, 0.8277f, 0.7810f, 0.7816f },
                { 2.4575f, 1.6535f, 1.0845f, 0.9439f, 0.8806f },
                { 1.5603f, 1.1819f, 0.8695f, 0.7665f, 0.7610f },
            },
            {
                { 1.3457f, 0.9394f, 0.8226f, 0.7800f, 0.7815f },
                { 2.4537f, 1.6212f, 1.0381f, 0.9297f, 0.8777f },
                { 1.5590f, 1.1610f, 0.8376f, 0.7639f, 0.7608f },
            },
        },
        {
            {
                { 1.7088f, 1.1851f, 0.8540f, 0.7750f, 0.7811f },
                { 3.4730f, 2.1560f, 1.2804f, 0.9844f, 0.8924f },
                { 1.8955f, 1.3964f, 0.9611f, 0.7633f, 0.7596f },
            },
            {
                { 1.5199f, 1.0029f, 0.8053f, 0.7728f, 0.7808f },
                { 3.3146f, 1.9424f, 1.1022f, 0.9508f, 0.8813f },
                { 1.8092f, 1.2427f, 0.8479f, 0.7513f, 0.7593f },
            },
            {
                { 1.4857f, 0.9233f, 0.8015f, 0.7722f, 0.7807f },
                { 3.3095f, 1.9045f, 1.0515f, 0.9344f, 0.8771f },
                { 1.8077f, 1.2202f, 0.8132f, 0.7493f, 0.7592f },
            },
        },
    };

    auto interpDrive = [d] (const float (&v)[5]) noexcept
    {
        return detail::interpDrive5 (d, v[0], v[1], v[2], v[3], v[4]);
    };

    float charTrim[3];
    for (int charIndex = 0; charIndex < 3; ++charIndex)
    {
        const float trimType0 = interpDrive (trims[seriesIndex][charIndex][0]);
        const float trimType1 = interpDrive (trims[seriesIndex][charIndex][1]);
        const float trimType2 = interpDrive (trims[seriesIndex][charIndex][2]);
        charTrim[charIndex] = detail::morphThreeWay (m, trimType0, trimType1, trimType2);
    }

    return detail::morphThreeWay (g, charTrim[0], charTrim[1], charTrim[2]);
}

inline float getHotInputReferenceCorrection (Model model, float drive, float girth,
                                             float mod, int seriesCount) noexcept
{
    int modelIndex = -1;
    switch (model)
    {
        case Model::Tape:       modelIndex = 0; break;
        case Model::Tube:       modelIndex = 1; break;
        case Model::Transistor: modelIndex = 2; break;
        case Model::Diode:      modelIndex = 3; break;
        default: return 1.0f;
    }

    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float m = detail::clampF (mod, 0.0f, 1.0f);
    const int seriesIndex = juce::jlimit (1, kMaxSeries, seriesCount) - 1;

    static constexpr float trims[4][4][3][3][5] =
    {
        {
            {
                {
                    { 1.2508f, 1.4269f, 1.5606f, 1.5683f, 1.6320f },
                    { 1.1587f, 1.3656f, 1.5456f, 1.9202f, 1.9689f },
                    { 1.1576f, 1.4698f, 1.6821f, 1.9676f, 1.9884f },
                },
                {
                    { 1.3231f, 1.4891f, 1.6125f, 1.6124f, 1.6543f },
                    { 1.2444f, 1.4228f, 1.5745f, 1.9209f, 1.9689f },
                    { 1.2170f, 1.4923f, 1.6890f, 1.9675f, 1.9884f },
                },
                {
                    { 1.6996f, 1.7920f, 1.8411f, 1.8075f, 1.7585f },
                    { 1.6597f, 1.6884f, 1.7068f, 1.9244f, 1.9688f },
                    { 1.4988f, 1.5973f, 1.7218f, 1.9677f, 1.9882f },
                },
            },
            {
                {
                    { 1.7904f, 1.7931f, 1.8526f, 1.8321f, 1.7834f },
                    { 1.4133f, 1.6482f, 1.7614f, 1.9611f, 1.9869f },
                    { 1.2845f, 1.6166f, 1.7751f, 1.9854f, 1.9977f },
                },
                {
                    { 1.8100f, 1.8226f, 1.8800f, 1.8697f, 1.8224f },
                    { 1.5283f, 1.7152f, 1.8003f, 1.9666f, 1.9891f },
                    { 1.3928f, 1.6619f, 1.7962f, 1.9875f, 1.9984f },
                },
                {
                    { 1.9350f, 1.9592f, 1.9730f, 1.9699f, 1.9585f },
                    { 1.9067f, 1.9269f, 1.9305f, 1.9878f, 1.9970f },
                    { 1.8023f, 1.8465f, 1.8937f, 1.9961f, 1.9785f },
                },
            },
            {
                {
                    { 1.9400f, 1.9361f, 1.9495f, 1.9403f, 1.8795f },
                    { 1.6683f, 1.8165f, 1.8733f, 1.9871f, 1.9977f },
                    { 1.3906f, 1.7005f, 1.8241f, 1.9961f, 1.9994f },
                },
                {
                    { 1.9501f, 1.9476f, 1.9637f, 1.9620f, 1.9198f },
                    { 1.7514f, 1.8681f, 1.9074f, 1.9913f, 1.9987f },
                    { 1.5347f, 1.7597f, 1.8540f, 1.9974f, 1.9993f },
                },
                {
                    { 1.9908f, 1.9982f, 2.0005f, 1.9975f, 1.9940f },
                    { 1.9793f, 1.9868f, 1.9879f, 1.9994f, 2.0000f },
                    { 1.9259f, 1.9429f, 1.9605f, 1.9993f, 1.9923f },
                },
            },
            {
                {
                    { 1.9832f, 1.9835f, 1.9861f, 1.9821f, 1.9513f },
                    { 1.8307f, 1.9072f, 1.9354f, 1.9963f, 1.9996f },
                    { 1.4811f, 1.7576f, 1.8569f, 1.9991f, 2.0004f },
                },
                {
                    { 1.9886f, 1.9887f, 1.9919f, 1.9903f, 1.9660f },
                    { 1.8804f, 1.9426f, 1.9605f, 1.9981f, 1.9998f },
                    { 1.6473f, 1.8249f, 1.8923f, 1.9997f, 2.0005f },
                },
                {
                    { 1.9984f, 2.0008f, 2.0013f, 2.0012f, 2.0006f },
                    { 1.9981f, 1.9990f, 1.9986f, 1.9972f, 2.0001f },
                    { 1.9751f, 1.9817f, 1.9876f, 2.0006f, 2.0001f },
                },
            },
        },
        {
            {
                {
                    { 1.0227f, 1.0268f, 1.0544f, 1.3006f, 1.6764f },
                    { 1.0313f, 1.0386f, 1.0903f, 1.3636f, 1.6817f },
                    { 1.0402f, 1.0533f, 1.1207f, 1.3770f, 1.6781f },
                },
                {
                    { 1.0406f, 1.0461f, 1.0804f, 1.3284f, 1.6972f },
                    { 1.0492f, 1.0580f, 1.1159f, 1.3888f, 1.6993f },
                    { 1.0561f, 1.0708f, 1.1429f, 1.3993f, 1.6906f },
                },
                {
                    { 1.1229f, 1.1347f, 1.1871f, 1.4246f, 1.7447f },
                    { 1.1316f, 1.1475f, 1.2222f, 1.4782f, 1.7501f },
                    { 1.1288f, 1.1512f, 1.2417f, 1.4878f, 1.7378f },
                },
            },
            {
                {
                    { 1.0418f, 1.0504f, 1.1196f, 1.7198f, 1.9389f },
                    { 1.0593f, 1.0737f, 1.1976f, 1.7826f, 1.9438f },
                    { 1.0756f, 1.0992f, 1.2367f, 1.7096f, 1.9225f },
                },
                {
                    { 1.0726f, 1.0848f, 1.1492f, 1.7204f, 1.9414f },
                    { 1.0901f, 1.1083f, 1.2206f, 1.7819f, 1.9461f },
                    { 1.1013f, 1.1285f, 1.2724f, 1.7051f, 1.9241f },
                },
                {
                    { 1.2115f, 1.2377f, 1.3368f, 1.7703f, 1.9511f },
                    { 1.2286f, 1.2621f, 1.3944f, 1.8210f, 1.9594f },
                    { 1.2184f, 1.2615f, 1.4343f, 1.7655f, 1.9455f },
                },
            },
            {
                {
                    { 1.0579f, 1.0714f, 1.1915f, 1.8654f, 1.9853f },
                    { 1.0850f, 1.1062f, 1.3142f, 1.9069f, 1.9879f },
                    { 1.1092f, 1.1404f, 1.3460f, 1.9022f, 1.9856f },
                },
                {
                    { 1.0979f, 1.1175f, 1.2176f, 1.8653f, 1.9827f },
                    { 1.1251f, 1.1525f, 1.3240f, 1.9059f, 1.9886f },
                    { 1.1411f, 1.1779f, 1.3885f, 1.8933f, 1.9855f },
                },
                {
                    { 1.2786f, 1.3197f, 1.4651f, 1.8967f, 1.9864f },
                    { 1.3040f, 1.3545f, 1.5380f, 1.9298f, 1.9913f },
                    { 1.2864f, 1.3476f, 1.5840f, 1.9086f, 1.9911f },
                },
            },
            {
                {
                    { 1.0716f, 1.0903f, 1.2683f, 1.9268f, 1.9940f },
                    { 1.1091f, 1.1364f, 1.4361f, 1.9569f, 1.9966f },
                    { 1.1428f, 1.1785f, 1.4463f, 1.9628f, 1.9974f },
                },
                {
                    { 1.1184f, 1.1456f, 1.2849f, 1.9266f, 1.9888f },
                    { 1.1560f, 1.1920f, 1.4241f, 1.9574f, 1.9959f },
                    { 1.1789f, 1.2218f, 1.4905f, 1.9602f, 1.9975f },
                },
                {
                    { 1.3315f, 1.3866f, 1.5725f, 1.9488f, 1.9938f },
                    { 1.3651f, 1.4310f, 1.6532f, 1.9718f, 1.9968f },
                    { 1.3417f, 1.4175f, 1.6984f, 1.9702f, 1.9986f },
                },
            },
        },
        {
            {
                {
                    { 1.0019f, 1.2557f, 1.7586f, 1.9250f, 1.9761f },
                    { 1.0028f, 1.2155f, 1.6722f, 1.8891f, 1.9574f },
                    { 1.0037f, 1.1768f, 1.5639f, 1.8320f, 1.9278f },
                },
                {
                    { 1.0023f, 1.3468f, 1.8323f, 1.9456f, 1.9837f },
                    { 1.0036f, 1.2812f, 1.7555f, 1.9149f, 1.9681f },
                    { 1.0049f, 1.2210f, 1.6456f, 1.8661f, 1.9417f },
                },
                {
                    { 1.0023f, 1.3987f, 1.8604f, 1.9545f, 1.9859f },
                    { 1.0039f, 1.3193f, 1.7913f, 1.9260f, 1.9722f },
                    { 1.0057f, 1.2467f, 1.6841f, 1.8803f, 1.9481f },
                },
            },
            {
                {
                    { 1.0038f, 1.4115f, 1.8799f, 1.9860f, 2.0003f },
                    { 1.0056f, 1.3604f, 1.8137f, 1.9630f, 1.9978f },
                    { 1.0074f, 1.3051f, 1.7151f, 1.9164f, 1.9857f },
                },
                {
                    { 1.0055f, 1.5373f, 1.9227f, 1.9921f, 2.0001f },
                    { 1.0080f, 1.4601f, 1.8690f, 1.9753f, 1.9990f },
                    { 1.0104f, 1.3784f, 1.7794f, 1.9365f, 1.9898f },
                },
                {
                    { 1.0060f, 1.5933f, 1.9368f, 1.9938f, 2.0001f },
                    { 1.0091f, 1.5073f, 1.8889f, 1.9795f, 1.9993f },
                    { 1.0123f, 1.4146f, 1.8056f, 1.9440f, 1.9911f },
                },
            },
            {
                {
                    { 1.0055f, 1.5235f, 1.9414f, 1.9983f, 2.0002f },
                    { 1.0084f, 1.4700f, 1.8906f, 1.9906f, 2.0001f },
                    { 1.0110f, 1.4066f, 1.8036f, 1.9615f, 1.9982f },
                },
                {
                    { 1.0097f, 1.6653f, 1.9676f, 1.9992f, 2.0001f },
                    { 1.0136f, 1.5897f, 1.9296f, 1.9948f, 2.0000f },
                    { 1.0168f, 1.5005f, 1.8561f, 1.9730f, 1.9988f },
                },
                {
                    { 1.0115f, 1.7172f, 1.9747f, 1.9994f, 2.0001f },
                    { 1.0162f, 1.6377f, 1.9418f, 1.9958f, 2.0000f },
                    { 1.0204f, 1.5412f, 1.8748f, 1.9768f, 1.9990f },
                },
            },
            {
                {
                    { 1.0073f, 1.6095f, 1.9733f, 1.9997f, 2.0003f },
                    { 1.0113f, 1.5570f, 1.9368f, 1.9980f, 2.0001f },
                    { 1.0145f, 1.4899f, 1.8624f, 1.9836f, 1.9998f },
                },
                {
                    { 1.0154f, 1.7566f, 1.9878f, 1.9999f, 2.0001f },
                    { 1.0204f, 1.6875f, 1.9638f, 1.9990f, 2.0001f },
                    { 1.0240f, 1.5982f, 1.9056f, 1.9895f, 1.9998f },
                },
                {
                    { 1.0196f, 1.8021f, 1.9909f, 2.0000f, 2.0000f },
                    { 1.0256f, 1.7326f, 1.9711f, 1.9993f, 2.0001f },
                    { 1.0299f, 1.6399f, 1.9192f, 1.9911f, 1.9998f },
                },
            },
        },
        {
            {
                {
                    { 1.1765f, 1.3659f, 1.7763f, 1.9366f, 1.9735f },
                    { 1.2750f, 1.7732f, 1.9194f, 1.9665f, 1.9801f },
                    { 1.2390f, 1.5609f, 1.8241f, 1.8996f, 1.9266f },
                },
                {
                    { 0.9838f, 1.1088f, 1.6644f, 1.9143f, 1.9642f },
                    { 1.0314f, 1.6128f, 1.8877f, 1.9553f, 1.9744f },
                    { 1.0133f, 1.2855f, 1.7649f, 1.8763f, 1.9126f },
                },
                {
                    { 0.9721f, 1.0438f, 1.5983f, 1.9040f, 1.9597f },
                    { 0.9957f, 1.5115f, 1.8753f, 1.9504f, 1.9723f },
                    { 0.9835f, 1.2012f, 1.7376f, 1.8660f, 1.9064f },
                },
            },
            {
                {
                    { 1.4101f, 1.8028f, 1.9649f, 1.9951f, 1.9978f },
                    { 1.6232f, 1.9384f, 1.9949f, 1.9788f, 1.9992f },
                    { 1.6037f, 1.8631f, 1.9650f, 1.9913f, 1.9935f },
                },
                {
                    { 0.9611f, 1.3213f, 1.9389f, 1.9935f, 1.9985f },
                    { 1.0639f, 1.8644f, 1.9853f, 1.9790f, 1.9992f },
                    { 1.0765f, 1.7156f, 1.9362f, 1.9841f, 1.9697f },
                },
                {
                    { 0.9477f, 1.1405f, 1.9181f, 1.9919f, 1.9985f },
                    { 0.9877f, 1.8280f, 1.9806f, 1.9782f, 1.9989f },
                    { 0.9804f, 1.6215f, 1.9232f, 1.9806f, 1.9694f },
                },
            },
            {
                {
                    { 1.6943f, 2.0156f, 2.0132f, 1.9991f, 1.9978f },
                    { 1.8160f, 1.9874f, 2.0007f, 1.9963f, 1.9999f },
                    { 1.8071f, 1.9483f, 1.9987f, 1.9993f, 1.9999f },
                },
                {
                    { 0.9355f, 1.6466f, 2.0354f, 1.9987f, 1.9976f },
                    { 1.1067f, 1.9461f, 1.9998f, 1.9970f, 1.9999f },
                    { 1.1738f, 1.8595f, 1.9874f, 1.9975f, 1.9949f },
                },
                {
                    { 0.9269f, 1.2871f, 2.0432f, 1.9982f, 1.9976f },
                    { 0.9791f, 1.9228f, 1.9994f, 1.9973f, 2.0000f },
                    { 0.9922f, 1.8058f, 1.9806f, 1.9967f, 1.9946f },
                },
            },
            {
                {
                    { 1.8874f, 2.0302f, 2.0131f, 1.9992f, 1.9991f },
                    { 1.9155f, 1.9997f, 1.9999f, 1.9991f, 1.9998f },
                    { 1.8888f, 1.9855f, 1.9989f, 1.9997f, 2.0003f },
                },
                {
                    { 0.9112f, 1.9290f, 2.0047f, 1.9988f, 1.9989f },
                    { 1.1598f, 1.9838f, 2.0009f, 1.9996f, 1.9998f },
                    { 1.2994f, 1.9224f, 1.9992f, 2.0000f, 1.9991f },
                },
                {
                    { 0.9089f, 1.4648f, 2.0032f, 1.9987f, 1.9985f },
                    { 0.9709f, 1.9710f, 2.0014f, 1.9996f, 1.9999f },
                    { 1.0126f, 1.8852f, 1.9986f, 1.9999f, 1.9991f },
                },
            },
        },
    };

    auto interpDrive = [d] (const float (&v)[5]) noexcept
    {
        return detail::interpDrive5 (d, v[0], v[1], v[2], v[3], v[4]);
    };

    float charTrim[3];
    for (int charIndex = 0; charIndex < 3; ++charIndex)
    {
        const float trimType0 = interpDrive (trims[modelIndex][seriesIndex][charIndex][0]);
        const float trimType1 = interpDrive (trims[modelIndex][seriesIndex][charIndex][1]);
        const float trimType2 = interpDrive (trims[modelIndex][seriesIndex][charIndex][2]);
        charTrim[charIndex] = detail::morphThreeWay (m, trimType0, trimType1, trimType2);
    }

    return detail::morphThreeWay (g, charTrim[0], charTrim[1], charTrim[2]);
}

// ----------------------------------------------------------------
//  CHAR -- post-waveshaper fold + sharpen
// ----------------------------------------------------------------
inline float applyGirth (float shaped, float girth,
                         adaa::SinFoldADAA& foldAdaa) noexcept
{
    if (girth < 0.01f) return shaped;

    // Stage 1: Sine wavefolding (with ADAA)
    const float foldK = 1.0f + girth * girth * 3.0f;
    const float foldInput = shaped * foldK;
    const float folded = foldAdaa.process (foldInput);

    // Stage 2: Transfer function sharpening
    const float sharpK = 1.0f + girth * 1.5f;
    const float sgn = shaped >= 0.0f ? 1.0f : -1.0f;
    const float tanhVal = std::abs (detail::fastTanh (shaped * (1.0f + girth * 2.0f)));
    const float sharpened = sgn * std::pow (tanhVal + 1.0e-12f, 1.0f / sharpK);

    // Stage 3: Blend fold + sharpen
    const float aggressive = folded * 0.6f + sharpened * 0.4f;

    // Stage 4: Wet/dry crossfade (quadratic ease-in)
    const float girthCurve = girth * girth;
    return shaped * (1.0f - girthCurve) + aggressive * girthCurve;
}

inline float applyTapeGirth (float shaped, float girth) noexcept
{
    if (girth < 0.01f) return shaped;

    const float girthCurve = girth * girth;
    const float density = detail::fastTanh (shaped * (1.0f + girth * 1.2f));
    const float body = shaped + shaped * (1.0f - std::min (1.0f, std::abs (shaped)))
                                 * girth * 0.12f;
    const float tapeLike = density * 0.82f + body * 0.18f;
    return juce::jmap (girthCurve, shaped, tapeLike);
}

inline float applyTriodeGirth (float shaped, float girth) noexcept
{
    if (girth < 0.01f) return shaped;

    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float g2 = g * g;
    const float density = detail::fastTanh (shaped * (1.0f + g * 0.50f));
    const float body = shaped + shaped * (1.0f - std::min (1.0f, std::abs (shaped)))
                                 * (0.035f + g * 0.045f);
    const float oddDensity = shaped + shaped * std::abs (shaped) * (0.010f + g * 0.028f);
    const float thick = density * 0.70f + body * 0.18f + oddDensity * 0.12f;
    const float out = juce::jmap (g2 * 0.60f, shaped, thick);
    return out * (1.0f - g2 * 0.08f);
}

inline float applyTriodePreGirth (float x, float girth, float drive) noexcept
{
    if (girth < 0.01f) return x;

    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float g2 = g * g;
    const float shaped = x + x * std::abs (x) * (0.045f + g * 0.100f + drive * 0.060f);
    const float pushed = detail::fastTanh (shaped * (1.0f + g * (0.22f + 0.28f * drive)));
    return juce::jmap (g2 * 0.78f, x, pushed);
}

// ----------------------------------------------------------------
//  Internal Emphasis / De-emphasis
// ----------------------------------------------------------------
struct EmphCoeffs {
    float preHP = 0, preSh = 0, postLP = 0, postHP = 0;
    float preHPAlt = 0, preShAlt = 0, postLPAlt = 0;
};

inline float preEmphasize (float x, EmphasisState& st, Model model,
                           float drive, float mod, const EmphCoeffs& ec) noexcept
{
    switch (model)
    {
        case Model::Tube:
        {
            st.preHP += (x - st.preHP) * ec.preHP;
            float hp = x - st.preHP;
            st.preSh += (hp - st.preSh) * ec.preSh;
            const float edge = hp - st.preSh;
            const float tubeMorph = detail::smoothStep01 (mod);
            const float edgeMix12AX7 = 0.005f + drive * 0.015f;
            const float edgeMixPower = 0.0015f + drive * 0.0045f;
            const float edgeMix = juce::jmap (tubeMorph, edgeMix12AX7, edgeMixPower);
            const float body12AX7 = 1.0f + drive * 0.006f;
            const float bodyPower = 1.004f + drive * 0.020f;
            const float body = juce::jmap (tubeMorph, body12AX7, bodyPower);
            const float bloom = st.preSh * tubeMorph * (0.004f + drive * 0.010f);
            return hp * body + edge * edgeMix + bloom;
        }
        case Model::Diode:
        {
            const float topo = detail::clampF (mod, 0.0f, 1.0f);
            const float openTopo = topo <= 0.5f ? 0.0f
                                                : detail::smoothStep01 ((topo - 0.5f) * 2.0f);
            const float hpCoeff = juce::jmap (openTopo, ec.preHP, ec.preHPAlt);
            const float shCoeff = juce::jmap (openTopo, ec.preSh, ec.preShAlt);

            st.preHP += (x - st.preHP) * hpCoeff;
            const float hp = x - st.preHP;
            st.preSh += (hp - st.preSh) * shCoeff;
            const float edge = hp - st.preSh;
            const float feedback = hp + edge * (0.032f + drive * 0.060f);
            const float hard = juce::jmap (0.56f, x, hp) + edge * (0.010f + drive * 0.028f);
            if (topo <= 0.5f)
            {
                const float t = detail::smoothStep01 (topo * 2.0f);
                return juce::jmap (t, feedback, hard);
            }
            const float u = openTopo;
            const float open = juce::jmap (0.08f + drive * 0.04f, x, hp)
                             + edge * (0.004f + drive * 0.012f);
            return juce::jmap (u, hard, open);
        }
        case Model::Clipper:
        {
            st.preHP += (x - st.preHP) * ec.preHP;
            const float hp = x - st.preHP;
            st.preSh += (hp - st.preSh) * ec.preSh;
            const float edge = hp - st.preSh;
            const float voice = detail::clampF (mod, 0.0f, 1.0f);
            const float ts = hp + edge * (0.050f + drive * 0.080f);
            if (voice <= 0.5f)
            {
                const float t = detail::smoothStep01 (voice * 2.0f);
                return juce::jmap (t, x, ts);
            }
            const float u = detail::smoothStep01 ((voice - 0.5f) * 2.0f);
            const float lowRetain = 0.74f + drive * 0.20f;
            const float klon = juce::jmap (lowRetain, x, hp)
                             + edge * (0.0008f + drive * 0.0014f);
            return juce::jmap (u, ts, klon);
        }
        case Model::Tape:
        {
            st.preHP += (x - st.preHP) * ec.preHP;
            const float hp = x - st.preHP;
            st.preSh += (hp - st.preSh) * ec.preSh;
            const float edge = hp - st.preSh;
            const float rabbitMod = 1.0f - mod;
            const float rabbit = rabbitMod * rabbitMod;
            const float gritMix = rabbitMod * (0.048f + drive * 0.145f + rabbit * (0.010f + drive * 0.040f));
            const float bodyLift = rabbit * (0.022f + drive * 0.055f);
            return hp * (1.0f + bodyLift) + edge * gritMix;
        }
        default:
            return x;
    }
}

inline float deEmphasize (float y, EmphasisState& st, Model model,
                          float drive, float mod, const EmphCoeffs& ec) noexcept
{
    switch (model)
    {
        case Model::Tube:
        {
            st.postLP += (y - st.postLP) * ec.postLP;
            const float tubeMorph = detail::smoothStep01 (mod);
            const float lpMix12AX7 = 0.025f + drive * 0.085f;
            const float lpMixPower = 0.065f + drive * 0.140f;
            const float lpMix = juce::jmap (tubeMorph, lpMix12AX7, lpMixPower);
            const float softened = y + (st.postLP - y) * lpMix;
            const float bright = y - st.postLP;
            const float brightKeep = (1.0f - tubeMorph) * (0.015f + drive * 0.025f);
            const float powerDamp = tubeMorph * (0.010f + drive * 0.025f);
            return softened + bright * (brightKeep - powerDamp);
        }
        case Model::Diode:
        {
            const float topo = detail::clampF (mod, 0.0f, 1.0f);
            const float openTopo = topo <= 0.5f ? 0.0f
                                                : detail::smoothStep01 ((topo - 0.5f) * 2.0f);
            const float lpCoeff = juce::jmap (openTopo, ec.postLP, ec.postLPAlt);
            st.postLP += (y - st.postLP) * lpCoeff;
            const float feedback = y + (st.postLP - y) * (0.18f + drive * 0.28f);
            const float hard = y + (st.postLP - y) * (0.045f + drive * 0.14f);
            if (topo <= 0.5f)
            {
                const float t = detail::smoothStep01 (topo * 2.0f);
                return juce::jmap (t, feedback, hard);
            }
            const float u = openTopo;
            const float bright = y - st.postLP;
            const float open = y + (st.postLP - y) * (0.006f + drive * 0.030f)
                             + bright * (0.026f + (1.0f - drive) * 0.018f);
            return juce::jmap (u, hard, open);
        }
        case Model::Clipper:
        {
            st.postLP += (y - st.postLP) * ec.postLP;
            const float voice = detail::clampF (mod, 0.0f, 1.0f);
            const float ts = y + (st.postLP - y) * (0.22f + drive * 0.36f);
            if (voice <= 0.5f)
            {
                const float t = detail::smoothStep01 (voice * 2.0f);
                return juce::jmap (t, y, ts);
            }
            const float u = detail::smoothStep01 ((voice - 0.5f) * 2.0f);
            const float klonBase = y + (st.postLP - y) * (0.42f + drive * 0.45f);
            const float bright = y - st.postLP;
            const float klon = klonBase + bright * ((1.0f - drive) * 0.002f);
            return juce::jmap (u, ts, klon);
        }
        case Model::Tape:
        {
            st.postLP += (y - st.postLP) * ec.postLP;
            const float lpMixA = 0.025f + drive * 0.095f;
            const float lpMixB = 0.002f + drive * 0.008f;
            const float rabbitMod = 1.0f - mod;
            const float lpMix = juce::jmap (rabbitMod, lpMixA, lpMixB);
            const float rabbit = rabbitMod * rabbitMod;
            const float softened = y + (st.postLP - y) * lpMix;
            const float bright = y - st.postLP;
            return softened + bright * rabbit * (0.050f + drive * 0.120f);
        }
        default:
            return y;
    }
}

inline float applyDriveCurve (float driveParam, Model model) noexcept
{
    float exp;
    switch (model)
    {
        case Model::Tube:        exp = 1.85f; break;
        case Model::Transistor:  exp = 0.62f; break;
        case Model::Diode:       exp = 1.3f; break;
        case Model::Clipper:     exp = 1.0f; break;
        case Model::Tape:        exp = 1.0f; break;
        default:                 exp = 1.5f; break;
    }
    return std::pow (driveParam, exp);
}

struct SafetyLPFCoeffs { float b0=0, b1=0, b2=0, a1=0, a2=0; };

inline float processSafetyLPF (SafetyLPF& st, float x, const SafetyLPFCoeffs& c) noexcept
{
    const float y = c.b0 * x + c.b1 * st.x1 + c.b2 * st.x2
                  - c.a1 * st.y1 - c.a2 * st.y2;

    st.x2 = st.x1; st.x1 = x;
    st.y2 = st.y1; st.y1 = y;
    return y;
}

//  Per-sample waveshaper functions (new models)
// ----------------------------------------------------------------

// TUBE: 12AX7 -> EL34/6L6-inspired stage morph with Tube2ustyle core
inline float processTriode (float x, float drive, float girth, float bias, float mod,
                            float react, bool rawMode,
                            State& state, int ch, float sr,
                            int triodeBloomSamplesPerSlot,
                            adaa::StableTanhADAA& adaaState) noexcept
{
    const int sp = state.currentSeriesPass;
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float m = detail::clampF (mod,   0.0f, 1.0f);
    const float b = detail::clampF (bias, -1.0f, 1.0f);
    const float tubeMorph = detail::smoothStep01 (m);
    const float tubeMorph2 = tubeMorph * tubeMorph;
    auto& triodeSag = state.triodeReact[sp][ch];
    auto& bodyPreLp = state.triodeBodyPreLP[sp][ch];
    auto& bodyPostLp = state.triodeBodyPostLP[sp][ch];
    const float sagInput = x;
    float xStage = x;
    float bEff = b;
    float bodyControl = g;
    constexpr float bodyUpperPivot = 2.0f / 3.0f;
    if (bodyControl > bodyUpperPivot)
    {
        const float t = (bodyControl - bodyUpperPivot) / (1.0f - bodyUpperPivot);
        bodyControl = bodyUpperPivot + (1.0f - bodyUpperPivot) * std::pow (t, 2.0f);
    }
    const float bodyCurve = 1.0f - std::pow (1.0f - bodyControl, 1.55f);

    {
        const float bodyPreHz12AX7 = 210.0f - d * 45.0f;
        const float bodyPreHzPower = 170.0f - d * 35.0f;
        const float bodyPreHz = juce::jmap (tubeMorph, bodyPreHz12AX7, bodyPreHzPower);
        const float bodyPreCoeff = detail::onePoleCoeff (bodyPreHz, sr);
        bodyPreLp += (xStage - bodyPreLp) * bodyPreCoeff;

        const float lfFeedAmt12AX7 = 0.08f + d * 0.10f;
        const float lfFeedAmtPower = 0.06f + d * 0.08f;
        const float lfFeedAmt = juce::jmap (tubeMorph, lfFeedAmt12AX7, lfFeedAmtPower);
        xStage += bodyPreLp * bodyCurve * lfFeedAmt;
    }

    if (react > 0.0001f)
    {
        const float sagSense = getTriodeSagSenseInput (sagInput);
        const TriodeReactResult triodeComp = processTriodeReact (
            xStage, sagSense, triodeSag, react, sr, triodeBloomSamplesPerSlot);
        xStage = triodeComp.sample;
        state.sagEnvelope[sp][ch] = triodeComp.amount;
    }
    else
    {
        triodeSag.control *= 0.5f;
        triodeSag.lastSag *= 0.5f;
        triodeSag.lastSupply += (1.0f - triodeSag.lastSupply) * 0.25f;
        triodeSag.supplyEnv *= 0.5f;
        triodeSag.supplyDrop *= 0.5f;
        triodeSag.strikeEnv *= 0.5f;
        triodeSag.bloomEnv *= 0.5f;
        triodeSag.bloomFastDemandEnv *= 0.5f;
        triodeSag.bloomDemandEnv *= 0.5f;
        triodeSag.burnFast *= 0.5f;
        triodeSag.burnSlow *= 0.5f;
        triodeSag.burnEnv *= 0.5f;
        triodeSag.atrophyEnv *= 0.5f;
        triodeSag.reservoirDrainEnv *= 0.5f;
        triodeSag.bloomSum *= 0.5f;
        triodeSag.bloomSlotSum = 0.0f;
        triodeSag.bloomSlotSamples = 0;
        triodeSag.bloomWindowSlots = 0;
        triodeSag.bloomActive = false;
        state.sagEnvelope[sp][ch] = 0.0f;
    }

    // Tube2 semantics are much closer to an input pad plus a shape control
    // than to an internal preamp boost. Using drive as a straight gain boost
    // overheats the repeated series stages and buries sag, because later
    // passes hit the same hard ceiling even at drive=0.
    const float inputPad12AX7 = detail::interpDrive5 (d,
                                                      0.42f, 0.52f, 0.66f, 0.82f, 1.00f);
    const float inputPadPower = detail::interpDrive5 (d,
                                                      0.48f, 0.58f, 0.72f, 0.90f, 1.08f);
    const float inputPad = juce::jmap (tubeMorph, inputPad12AX7, inputPadPower);
    xStage *= inputPad;
    const float sagAmt = triodeSag.lastSag;
    const float sagCore = detail::smoothStep01 (juce::jlimit (0.0f, 1.0f, sagAmt));
    const float supplyCore = detail::smoothStep01 (
        juce::jlimit (0.0f, 1.0f, triodeSag.supplyDrop));
    const float strikeCore = detail::smoothStep01 (
        juce::jlimit (0.0f, 1.0f, triodeSag.strikeEnv));
    const float bloomCore = detail::smoothStep01 (
        juce::jlimit (0.0f, 1.0f, triodeSag.bloomEnv)) * (1.0f - strikeCore * 0.18f);
    const float bloomRecoveryCore = bloomCore * (supplyCore + (1.0f - supplyCore) * 0.55f);
    const float burnCore = juce::jlimit (-1.0f, 1.0f, triodeSag.burnEnv);
    const float burnPress = std::max (0.0f, burnCore);
    const float atrophyCore = detail::smoothStep01 (
        juce::jlimit (0.0f, 1.0f, triodeSag.atrophyEnv)) * (0.45f + supplyCore * 0.55f);
    const float bloomAtrophyRelief = 1.0f - bloomRecoveryCore * (0.16f + tubeMorph * 0.10f);
    const float atrophyDb = atrophyCore * juce::jmap (tubeMorph, 4.5f, 7.5f)
                          * juce::jlimit (0.70f, 1.0f, bloomAtrophyRelief);
    const float reservoirCore = detail::smoothStep01 (
        juce::jlimit (0.0f, 1.0f, triodeSag.reservoirDrainEnv))
        * (0.35f + supplyCore * 0.65f);
    const float bloomReservoirRelief = 1.0f - bloomRecoveryCore * (0.10f + tubeMorph * 0.08f);
    const float reservoirDb = reservoirCore * juce::jmap (tubeMorph, 1.6f, 2.5f)
                            * juce::jlimit (0.78f, 1.0f, bloomReservoirRelief);
    const float atrophyGain = adaa::fastExp (-(atrophyDb + reservoirDb) * 0.11512925465f);

    // Tube2ustyle stage inside the black box. MOD/CHAR stay mostly outside
    // for now so we can match the core behavior first.
    const float overallscale = sr / 44100.0f;
    float s = xStage;

    if (!rawMode && overallscale > 1.9f)
    {
        const float stored = s;
        s = 0.5f * (s + triodeSag.prevIn);
        triodeSag.prevIn = stored;
    }
    else
    {
        triodeSag.prevIn = s;
    }

    // Supply starvation should alter the actual stage conditions, not only a
    // pre-distortion input waveform. If we only deform the input and then hit
    // the same hard ceiling, the effect becomes inaudible. Sag therefore also
    // tightens headroom and shifts the operating point of the Tube2ustyle core.
    const float biasPos = std::max (0.0f, bEff);
    const float biasNeg = std::max (0.0f, -bEff);
    const float burnBiasShift = burnCore * (0.004f + tubeMorph * 0.006f);
    const float stageBias12AX7 = bEff * 0.050f - sagCore * 0.095f
                               - supplyCore * 0.040f - burnBiasShift;
    const float stageBiasPower = bEff * 0.028f - sagCore * 0.072f
                               - supplyCore * 0.055f - burnBiasShift * 1.25f;
    const float stageBias = juce::jmap (tubeMorph, stageBias12AX7, stageBiasPower);
    const float cathodeDepth12AX7 = bodyCurve * (0.040f + d * 0.050f);
    const float cathodeDepthPower = bodyCurve * (0.026f + d * 0.032f);
    const float cathodeDepth = juce::jmap (tubeMorph, cathodeDepth12AX7, cathodeDepthPower);
    const float bloomHeadroomRecovery = bloomRecoveryCore * (0.066f + tubeMorph * 0.154f);

    const float headroom12AX7 = juce::jlimit (0.54f, 1.0f,
                                              1.0f - sagCore * 0.40f
                                                    - supplyCore * 0.16f
                                                    - biasPos * 0.05f + biasNeg * 0.02f);
    const float headroomPower = juce::jlimit (0.58f, 1.08f,
                                              1.04f - sagCore * 0.28f
                                                     - supplyCore * 0.18f
                                                     - biasPos * 0.08f + biasNeg * 0.05f);
    const float stageHeadroomBase = juce::jlimit (
        0.52f, 1.08f,
        juce::jmap (tubeMorph, headroom12AX7, headroomPower) + bloomHeadroomRecovery);
    const float burnHeadroomLoss = burnPress * (0.010f + tubeMorph * 0.018f);
    const float stageHeadroom = juce::jlimit (0.52f, 1.08f,
                                              stageHeadroomBase * (1.0f - cathodeDepth * 0.12f
                                                                        - burnHeadroomLoss));
    s += stageBias;
    s = detail::clampF (s, -stageHeadroom, stageHeadroom);
    s /= stageHeadroom;

    const float iterations12AX7 = 1.0f - d;
    const float iterationsPower = juce::jlimit (0.0f, 1.0f, 1.0f - d * 0.82f);
    const float iterations = juce::jmap (tubeMorph, iterations12AX7, iterationsPower);
    const int powerFactorBase = juce::jlimit (1, 10, 1 + (int) std::floor (9.0f * iterations));
    const int sagPowerDrop = juce::jlimit (0, 4, (int) std::floor (sagCore * juce::jmap (tubeMorph, 4.0f, 2.5f) + 0.35f));
    const int powerFactor = juce::jlimit (1, 10, powerFactorBase - sagPowerDrop);
    const float asymPad = (float) powerFactor;
    const float gainScaling = 1.0f / (float) (powerFactor + 1);

    // First Tube2 asymmetry section.
    const float asymAmt12AX7 = 0.25f + sagCore * 0.36f + biasPos * 0.08f;
    const float asymAmtPower = 0.14f + sagCore * 0.20f + biasNeg * 0.06f;
    const float asymAmt = juce::jmap (tubeMorph, asymAmt12AX7, asymAmtPower)
                        + juce::jmap (tubeMorph,
                                      bodyCurve * (0.018f + d * 0.012f),
                                      bodyCurve * (0.010f + d * 0.008f));
    s = detail::tube2AsymSection (s, asymPad, asymAmt);
    // Original Tube curve.
    s = detail::airwindowsTubeCurve (s, powerFactor);

    if (tubeMorph > 0.001f)
    {
        const float hotness = detail::clampF (0.55f + bEff * 0.35f, 0.0f, 1.0f);
        const float idleBias = 0.010f + hotness * (0.012f + d * 0.010f);
        const float crossover = 0.010f + (1.0f - hotness) * (0.012f + d * 0.008f);
        const float bloomPower = juce::jlimit (0.0f, 1.0f,
                                               bloomRecoveryCore * (0.65f + tubeMorph * 1.05f));
        const float supplyPowerLoss = juce::jlimit (
            0.0f, 1.0f,
            1.0f - supplyCore * (0.08f + tubeMorph * 0.10f)
                 + bloomPower * (0.078f + tubeMorph * 0.168f));
        const float bloomLiftDamp = 1.0f - atrophyCore * (0.18f + tubeMorph * 0.12f);
        const float powerBloomLift = 1.0f + bloomPower * (0.069f + tubeMorph * 0.184f)
                                           * bloomLiftDamp;
        const float burnPowerMod = juce::jlimit (
            0.955f, 1.035f,
            1.0f - burnCore * (0.012f + tubeMorph * 0.020f));
        const float powerGain = (1.0f + d * (0.70f + tubeMorph * 0.55f))
                              * supplyPowerLoss * powerBloomLift * burnPowerMod;

        const float posV = s * powerGain + idleBias;
        const float negV = -s * powerGain + idleBias;
        const float posC = detail::smoothRect (posV, crossover);
        const float negC = detail::smoothRect (negV, crossover);

        float powerShape = posC - negC;
        powerShape += s * std::abs (s) * (0.010f + tubeMorph * 0.035f);
        powerShape = detail::clampF (powerShape * (0.92f + hotness * 0.08f), -1.20f, 1.20f);

        const float satDrive = 1.0f + tubeMorph * (0.10f + 0.18f * d);
        const float satK = 0.85f + d * (0.55f + 0.25f * tubeMorph);
        const float satRaw = adaaState.process (powerShape * satDrive, satK);
        const float satNorm = detail::normalizeSmallSignal (satRaw, 0.0f, satK * satDrive);
        const float powerMix = tubeMorph * (0.35f + 0.35f * d);
        s = juce::jmap (powerMix, s, satNorm);
    }

    {
        float& couplingDc = state.triodeCouplingDc[sp][ch];
        couplingDc += (s - couplingDc) * detail::onePoleCoeff (2.0f, sr);
        s -= couplingDc;
    }

    if (!rawMode && overallscale > 1.9f)
    {
        const float stored = s;
        s = 0.5f * (s + triodeSag.prevOut);
        triodeSag.prevOut = stored;
    }
    else
    {
        triodeSag.prevOut = s;
    }

    // Tube2 hysteresis / spiky fuzz layer.
    float slew = 1.0f;
    if (!rawMode)
    {
        slew = triodeSag.prevHyst - s;
        if (overallscale > 1.9f)
        {
            const float stored = s;
            s = 0.5f * (s + triodeSag.prevHyst);
            triodeSag.prevHyst = stored;
        }
        else
        {
            triodeSag.prevHyst = s;
        }

        if (slew > 0.0f)
            slew = 1.0f + (std::sqrt (slew) * 0.5f);
        else
            slew = 1.0f - (std::sqrt (-slew) * 0.5f);

        const float hystAmt = juce::jmap (tubeMorph, 1.0f + sagCore * 1.80f,
                                                     0.65f + sagCore * 0.95f);
        s -= s * std::abs (s) * slew * gainScaling * hystAmt;
    }

    const float ceiling = juce::jmap (tubeMorph2, 0.52f, 0.62f);
    const float ceilingClamped = detail::clampF (s, -ceiling, ceiling);
    s = ceilingClamped * (1.0f / ceiling);

    {
        const float bodyPostHz12AX7 = 145.0f - d * 18.0f;
        const float bodyPostHzPower = 120.0f - d * 15.0f;
        const float bodyPostHz = juce::jmap (tubeMorph, bodyPostHz12AX7, bodyPostHzPower);
        const float bodyPostCoeff = detail::onePoleCoeff (bodyPostHz, sr);
        bodyPostLp += (s - bodyPostLp) * bodyPostCoeff;

        const float depthAmt12AX7 = 0.055f + d * 0.040f;
        const float depthAmtPower = 0.070f + d * 0.045f;
        const float depthAmt = juce::jmap (tubeMorph, depthAmt12AX7, depthAmtPower);
        const float depth = bodyPostLp * bodyCurve * depthAmt;
        s = (s + depth) / (1.0f + bodyCurve * depthAmt * 0.35f);
    }

    s *= atrophyGain;

    state.triodeBlock[sp][ch] = 0.0f;
    return s;
}

// TRANSISTOR: common-emitter/common-source inspired black box.
// MOD morphs BJT punch into softer FET behaviour while CHAR/BODY relaxes
// degeneration and lets more low-mid energy hit the nonlinear stage.
inline float processTransistorStage (float x, float drive, float girth, float bias, float mod,
                                     float react, bool rawMode,
                                     State& state, int ch, float sr,
                                     adaa::ClipperADAA& clipAdaa) noexcept
{
    const int sp = state.currentSeriesPass;
    auto& preHP = state.transistorPreHP[sp][ch];
    auto& preEdge = state.transistorPreEdge[sp][ch];
    auto& postLP = state.transistorPostLP[sp][ch];
    auto& compState = state.dynamicsComp[sp][ch];
    auto& peakCatchState = state.transistorPeakCatch[sp][ch];
    auto& coreAdaa = state.transistorCoreAdaa[sp][ch];

    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float body = detail::clampF (girth, 0.0f, 1.0f);
    const float b = detail::clampF (bias, -1.0f, 1.0f);
    const float type = detail::smoothStep01 (detail::clampF (mod, 0.0f, 1.0f));
    const float bodyToneCurve = 1.0f - std::pow (1.0f - body, 1.85f);
    const float bodyClipCurve = 1.0f - std::pow (1.0f - body, 1.10f);
    auto trackDbg = [] (float& dst, float v) noexcept
    {
        dst = std::max (dst, std::abs (v));
    };

    if (!rawMode)
    {
        const float hpHz = juce::jmap (type,
                                       34.0f + bodyToneCurve * 18.0f,
                                       22.0f + bodyToneCurve * 13.0f);
        const float hpC = detail::onePoleCoeff (hpHz, sr);
        preHP += (x - preHP) * hpC;
        const float hp = x - preHP;

        const float edgeHz = juce::jmap (type,
                                         2100.0f + d * 1200.0f,
                                         1200.0f + d * 650.0f);
        const float edgeC = detail::onePoleCoeff (edgeHz, sr);
        preEdge += (hp - preEdge) * edgeC;
        const float edge = hp - preEdge;

        const float lowRetain = juce::jmap (type,
                                            0.12f + bodyToneCurve * 0.16f,
                                            0.28f + bodyToneCurve * 0.20f);
        const float edgeAmt = juce::jmap (type,
                                          0.010f + d * 0.050f,
                                          0.004f + d * 0.025f);
        x = juce::jmap (lowRetain, x, hp) + edge * edgeAmt;
    }
    else
    {
        preHP = preEdge = postLP = 0.0f;
    }

    const float inputPadBjt = detail::interpDrive5 (d,
                                                    0.14f, 0.30f, 0.68f, 1.32f, 2.75f)
                            * juce::jmap (bodyToneCurve, 1.00f, 1.22f);
    const float inputPadFet = detail::interpDrive5 (d,
                                                    0.18f, 0.36f, 0.74f, 1.26f, 2.20f)
                            * juce::jmap (bodyToneCurve, 1.00f, 1.14f);
    const float inputPad = juce::jmap (type, inputPadBjt, inputPadFet);

    if (react > 0.001f)
    {
        const float compAmt = detail::clampF (react, 0.0f, 1.0f);
        const DynamicsCompResult peak = processTransistorPeakCatch (
            x, x * inputPad, peakCatchState, compAmt, d, type, sr);
        x = peak.sample;
        const DynamicsCompResult comp = processTransistorComp (x, compState, compAmt, d, type, sr);
        x = comp.sample;
    }
    else
    {
        peakCatchState.peakEnv *= 0.5f;
        peakCatchState.bodyEnv *= 0.5f;
        peakCatchState.gain += (1.0f - peakCatchState.gain) * 0.25f;
        compState.gain = 1.0f;
        compState.env *= 0.5f;
        compState.hfEnv *= 0.5f;
    }

    x *= inputPad;
    state.transistorDbgInputPad[sp][ch] = inputPad;
    trackDbg (state.transistorDbgPre[sp][ch], x);

    const float headroomBjt = juce::jlimit (0.48f, 1.14f,
                                            1.10f - d * 0.60f
                                                  - bodyClipCurve * 0.05f
                                                  - juce::jmax (0.0f, b) * 0.11f
                                                  + juce::jmax (0.0f, -b) * 0.05f);
    const float headroomFet = juce::jlimit (0.50f, 1.16f,
                                            1.10f - d * 0.44f
                                                  - bodyClipCurve * 0.035f
                                                  - juce::jmax (0.0f, b) * 0.08f
                                                  + juce::jmax (0.0f, -b) * 0.03f);
    const float headroom = juce::jmap (type, headroomBjt, headroomFet);

    const float opBias = juce::jmap (type, b * 0.16f, b * 0.10f);
    const float oddAmt = juce::jmap (type,
                                     0.018f + bodyClipCurve * 0.016f + d * 0.040f,
                                    -0.006f - bodyClipCurve * 0.007f - d * 0.012f);
    const float cubicAmt = juce::jmap (type,
                                       0.008f + d * 0.038f,
                                       0.014f + d * 0.020f);

    auto applyCoreShape = [oddAmt, cubicAmt] (float v) noexcept
    {
        return v
             + v * std::abs (v) * oddAmt
             + v * v * v * cubicAmt;
    };

    const float biasNorm = detail::clampF (opBias, -headroom, headroom) / headroom;
    float z = x + opBias;
    z = detail::clampF (z, -headroom, headroom);
    z /= headroom;
    trackDbg (state.transistorDbgCoreIn[sp][ch], z);

    const float shifted = applyCoreShape (z);
    const float z0 = applyCoreShape (biasNorm);
    const float preDeriv0 = 1.0f
                          + 2.0f * std::abs (biasNorm) * oddAmt
                          + 3.0f * biasNorm * biasNorm * cubicAmt;

    const float satK = juce::jmap (type,
                                   0.98f + d * (2.24f + bodyClipCurve * 0.24f),
                                   0.84f + d * (1.55f + bodyClipCurve * 0.14f));
    state.transistorDbgSatK[sp][ch] = satK;
    const float raw = coreAdaa.process (satK * shifted, 1.0f);
    const float raw0 = std::tanh (satK * z0);
    const float slopeRef = satK * (1.0f - raw0 * raw0);
    const float slope0 = std::max (1.0e-4f, slopeRef * preDeriv0 * (1.0f / headroom));
    float core = detail::normalizeSmallSignal (raw, raw0, slope0);
    trackDbg (state.transistorDbgCoreOut[sp][ch], core);

    const float railDrive = juce::jmap (type,
                                        1.02f + d * 1.02f,
                                        0.96f + d * 0.68f)
                          * juce::jmap (bodyClipCurve, 1.00f, 1.025f);
    const float posThresh = juce::jmap (type, 1.24f - d * 0.24f,
                                              1.34f - d * 0.14f)
                          * (1.0f - juce::jmax (0.0f, b) * 0.18f
                                   + juce::jmax (0.0f, -b) * 0.04f);
    const float negThresh = juce::jmap (type, 1.16f - d * 0.22f,
                                              1.28f - d * 0.12f)
                          * (1.0f - juce::jmax (0.0f, -b) * 0.18f
                                   + juce::jmax (0.0f,  b) * 0.04f);
    const float kneeBase = juce::jmap (type,
                                       juce::jmap (bodyClipCurve, 0.24f, 0.17f),
                                       juce::jmap (bodyClipCurve, 0.30f, 0.22f));
    const float kneePos = std::max (1.0e-4f, kneeBase * (1.0f - d * 0.10f));
    const float kneeNeg = std::max (1.0e-4f, kneeBase * juce::jmap (type, 0.95f, 1.08f) * (1.0f - d * 0.08f));
    state.transistorDbgRailThresh[sp][ch] = 0.5f * (posThresh + negThresh);

    const float railIn = core * railDrive;
    trackDbg (state.transistorDbgRailIn[sp][ch], railIn);
    float out = clipAdaa.process (railIn, posThresh, negThresh, kneePos, kneeNeg);
    trackDbg (state.transistorDbgRailOut[sp][ch], out);

    if (!rawMode)
    {
        const float lpHz = juce::jmap (type,
                                       9800.0f - d * 2600.0f,
                                       6200.0f - d * 1700.0f);
        const float lpC = detail::onePoleCoeff (detail::clampF (lpHz, 1800.0f, 12000.0f), sr);
        postLP += (out - postLP) * lpC;
        const float lpMix = juce::jmap (type,
                                        0.03f + d * 0.08f,
                                        0.08f + d * 0.12f);
        out = out + (postLP - out) * lpMix;
    }

    trackDbg (state.transistorDbgPost[sp][ch], out);
    return out;
}

inline float processDiodeStage (float x, float drive, float girth, float bias, float mod,
                                float reactColor,
                                adaa::ClipperADAA& adaaState) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float c = detail::clampF (girth, 0.0f, 1.0f);
    const float s = detail::clampF (bias, -1.0f, 1.0f);
    const float diodeSym = s * 2.0f;
    const float t = detail::clampF (mod, 0.0f, 1.0f);
    const float bridgeWork = detail::smoothStep01 (detail::clampF (reactColor * 1.75f, 0.0f, 1.0f));

    const float condCurve = 1.0f - std::pow (1.0f - c, 1.7f);
    const float condDriveCurve = 1.0f - std::pow (1.0f - c, 1.35f);
    const float condThreshold = juce::jmap (condCurve, 0.68f, 1.14f);
    const float condKnee = juce::jmap (condCurve, 0.24f, 0.075f);
    const float condDrive = juce::jmap (condDriveCurve, 1.00f, 1.12f);

    const float driveFb = detail::interpDrive5 (d, 1.00f, 1.45f, 2.80f, 5.80f, 9.50f);
    const float driveHard = detail::interpDrive5 (d, 1.00f, 2.20f, 5.20f, 10.80f, 18.00f);
    const float driveOpen = detail::interpDrive5 (d, 1.00f, 1.65f, 3.40f, 6.10f, 9.60f);

    float driveGain = driveHard;
    float thresholdMul = 1.0f;
    float kneeMul = 1.0f;
    float cleanBlend = 0.0f;
    float voiceTrim = 1.0f;
    float symRange = 0.34f;
    float edgeShape = 0.0f;

    if (t <= 0.5f)
    {
        const float u = detail::smoothStep01 (t * 2.0f);
        driveGain = juce::jmap (u, driveFb, driveHard);
        thresholdMul = juce::jmap (u, 0.92f, 0.82f);
        kneeMul = juce::jmap (u, 1.35f, 0.58f);
        cleanBlend = juce::jmap (u, 0.0f, 0.02f);
        voiceTrim = juce::jmap (u, 1.00f, 0.96f);
        symRange = juce::jmap (u, 0.28f, 0.38f);
        edgeShape = juce::jmap (u, 0.10f, 0.03f);
    }
    else
    {
        const float u = detail::smoothStep01 ((t - 0.5f) * 2.0f);
        driveGain = juce::jmap (u, driveHard, driveOpen);
        thresholdMul = juce::jmap (u, 0.82f, 1.02f);
        kneeMul = juce::jmap (u, 0.58f, 1.00f);
        cleanBlend = juce::jmap (u, 0.02f, 0.07f);
        voiceTrim = juce::jmap (u, 0.96f, 1.07f);
        symRange = juce::jmap (u, 0.36f, 0.32f);
        edgeShape = juce::jmap (u, 0.03f, 0.06f);
    }

    thresholdMul *= 1.0f - bridgeWork * 0.065f;
    kneeMul      *= 1.0f - bridgeWork * 0.045f;

    const float threshold = condThreshold * thresholdMul;
    const float knee = std::max (1.0e-4f, condKnee * kneeMul);

    const float thresholdPos = detail::clampF (threshold * (1.0f + diodeSym * symRange), 0.22f, 1.65f);
    const float thresholdNeg = detail::clampF (threshold * (1.0f - diodeSym * symRange), 0.22f, 1.65f);
    const float kneePos = std::max (1.0e-4f, knee * (1.0f - diodeSym * 0.18f));
    const float kneeNeg = std::max (1.0e-4f, knee * (1.0f + diodeSym * 0.18f));

    float clipIn = x * driveGain * condDrive;
    clipIn += x * std::abs (x) * edgeShape;

    float clipped = adaaState.process (clipIn, thresholdPos, thresholdNeg,
                                       kneePos, kneeNeg);

    const float outputScale = 2.0f / (thresholdPos + thresholdNeg);
    clipped *= outputScale;

    if (cleanBlend > 0.0001f)
    {
        const float clean = detail::clampF (x * juce::jmap (c, 0.96f, 1.06f), -1.30f, 1.30f);
        clipped = juce::jmap (cleanBlend, clipped, clean);
    }

    return clipped * voiceTrim;
}

// CLIPPER: threshold-driven clipper with continuous soft->hard knee control
// and a voice morph from broadband classic -> TS-style -> Klon-style.
inline float processClipper (float x, float drive, float girth, float bias, float mod,
                             bool rawMode, State& state, int ch, float sr,
                             adaa::ClipperADAA& adaaState) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float k = detail::clampF (girth, 0.0f, 1.0f);
    const float b = detail::clampF (bias, -1.0f, 1.0f);
    const float m = detail::clampF (mod, 0.0f, 1.0f);

    // DRIVE sets the clipping threshold, but we keep a fixed clip ceiling.
    // This makes the control behave like a real threshold while preserving a
    // practical output range similar to pro clippers and pedal stages.
    const float threshold = detail::interpDrive5 (d,
                                                  1.08f, 0.92f, 0.72f, 0.48f, 0.24f);

    float voiceScale = 1.0f;
    float legacyCleanBlend = 0.0f;
    float voiceLift = 1.0f;
    float klonVoice = 0.0f;
    if (m <= 0.5f)
    {
        const float t = detail::smoothStep01 (m * 2.0f);
        voiceScale = juce::jmap (t, 1.00f, 1.08f); // TS gets slightly tighter
    }
    else
    {
        klonVoice = detail::smoothStep01 ((m - 0.5f) * 2.0f);
        voiceScale = juce::jmap (klonVoice, 1.08f, 0.92f); // Klon opens back up
        legacyCleanBlend = 0.18f * klonVoice * (1.0f - klonVoice);
        voiceLift = juce::jmap (klonVoice, 1.0f, 1.12f);
    }

    const float legacyClipIn = x * (voiceScale / std::max (threshold, 0.05f));
    const float klonDriveGain = detail::interpDrive5 (d,
                                                      0.90f, 2.15f, 7.20f, 32.00f, 160.0f);
    const float clipIn = juce::jmap (klonVoice, legacyClipIn, x * klonDriveGain);

    // BIAS becomes symmetry / mismatch: shifts positive and negative clip
    // thresholds independently, but keep their mean around unity.
    const float klonDiodeHeadroom = juce::jmap (k, 0.74f, 0.60f);
    const float klonBiasRange = 0.26f + (1.0f - k) * 0.06f;
    const float legacyThresholdPos = detail::clampF (1.0f + b * 0.45f, 0.45f, 1.55f);
    const float legacyThresholdNeg = detail::clampF (1.0f - b * 0.45f, 0.45f, 1.55f);
    const float klonThresholdPos = detail::clampF (klonDiodeHeadroom * (1.0f + b * klonBiasRange), 0.30f, 1.35f);
    const float klonThresholdNeg = detail::clampF (klonDiodeHeadroom * (1.0f - b * klonBiasRange), 0.30f, 1.35f);
    const float thresholdPos = juce::jmap (klonVoice, legacyThresholdPos, klonThresholdPos);
    const float thresholdNeg = juce::jmap (klonVoice, legacyThresholdNeg, klonThresholdNeg);
    const float legacyKneeSoft = 0.01f + (1.0f - k) * 0.56f;
    const float klonKneeSoft = 0.045f + (1.0f - k) * 0.34f;
    const float kneeSoft = juce::jmap (klonVoice, legacyKneeSoft, klonKneeSoft);
    const float kneePos = std::max (1.0e-4f, thresholdPos * kneeSoft);
    const float kneeNeg = std::max (1.0e-4f, thresholdNeg * kneeSoft);

    float clipped = adaaState.process (clipIn, thresholdPos, thresholdNeg,
                                       kneePos, kneeNeg);

    // Preserve average ceiling when asymmetry moves thresholds apart.
    const float outputScale = 2.0f / (thresholdPos + thresholdNeg);
    clipped *= outputScale;

    if (klonVoice > 0.0001f)
    {
        auto& klonState = state.clipperKlon[state.currentSeriesPass][ch];
        const float klonSoftIn = x * klonDriveGain * juce::jmap (k, 0.82f, 1.00f);
        const float klonSoftK = 0.48f + k * 0.38f;
        const float klonSoft = klonState.softAdaa.process (klonSoftIn, klonSoftK)
                             * juce::jmap (k, 0.88f, 1.04f);
        const float klonSoftBlend = klonVoice * 0.97f;
        clipped = juce::jmap (klonSoftBlend, clipped, klonSoft);
    }

    if (legacyCleanBlend > 0.0001f)
    {
        const float clean = detail::clampF (x * (0.90f + 0.10f * voiceScale), -1.25f, 1.25f);
        clipped = juce::jmap (legacyCleanBlend, clipped, clean);
    }

    if (klonVoice > 0.0001f && ! rawMode)
    {
        auto& klonState = state.clipperKlon[state.currentSeriesPass][ch];

        const float cleanHz = 452.0f + d * 78.0f;
        const float cleanCoeff = detail::onePoleCoeff (cleanHz, sr);
        klonState.cleanLP += (x - klonState.cleanLP) * cleanCoeff;

        const float dirtyLowHz = 205.0f + d * 105.0f;
        const float dirtyLowCoeff = detail::onePoleCoeff (dirtyLowHz, sr);
        klonState.dirtyLowLP += (clipped - klonState.dirtyLowLP) * dirtyLowCoeff;

        const float dirtyHz = 760.0f + d * 220.0f;
        const float dirtyCoeff = detail::onePoleCoeff (dirtyHz, sr);
        klonState.dirtyLP += (clipped - klonState.dirtyLP) * dirtyCoeff;

        const float cleanPath = klonState.cleanLP * juce::jmap (d, 1.10f, 0.86f);
        const float dirtyToneMix = 0.95f + (1.0f - d) * 0.025f;
        const float dirtyFiltered = clipped + (klonState.dirtyLP - clipped) * dirtyToneMix;
        const float dirtyPath = dirtyFiltered - klonState.dirtyLowLP * (0.30f + d * 0.20f);

        const float cleanAmount = 0.46f * std::pow (1.0f - d, 2.65f);
        const float dirtyAmount = 1.0f - cleanAmount * 0.42f;
        const float klonSum = dirtyPath * dirtyAmount + cleanPath * cleanAmount;
        clipped = juce::jmap (klonVoice, clipped, klonSum);
    }

    const float klonMakeup = juce::jmap (klonVoice, 1.0f, 1.0f + d * 0.44f);
    const float finalTrim = voiceLift * juce::jmap (d, 0.98f, 0.92f) * klonMakeup;

    return clipped * finalTrim;
}

// TAPE: ADAA tape stage with two fitted families:
//   MOD=0   -> Rabbit-style grittier tape reference
//   MOD=1   -> current smoother tape reference
// Interpolate parameters, not outputs, so the mode remains a single cohesive
// nonlinear path instead of behaving like a parallel blend.
inline float processTape (float x, float drive, float bias, float mod,
                          bool rawMode, State& state, int ch, float sr,
                          adaa::TapeTanhADAA& adaaState,
                          bool advanceOsc = true) noexcept
{
    const int sp = state.currentSeriesPass;
    (void) sr;
    (void) advanceOsc;
    juce::ignoreUnused (rawMode);

    // Flutter/head-bump remain out for now, but keep tapeFlux alive so Rabbit
    // can use a mild memory compression stage instead of sounding like a plain
    // static level boost.
    state.bumpZ1[sp][ch] = 0.0f;
    state.bumpZ2[sp][ch] = 0.0f;
    if (ch == 0)
        state.flutterPhase = 0.0f;

    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float m = detail::clampF (mod,   0.0f, 1.0f);
    const float rabbitMod = 1.0f - m;

    // Tape A: current smoother family (MOD=100%)
    float pregainA;
    float totalGainA;
    {
        constexpr float baseGainA = 1.1438f;
        constexpr float pre0A     = 1.0f;
        constexpr float pre50A    = 3.30f;
        constexpr float pre100A   = 22.50f;
        constexpr float mu50A     = 0.95f;
        constexpr float mu100A    = 3.00f;

        float makeupA = 1.0f;
        if (d <= 0.5f)
        {
            const float t = detail::smoothStep01 (d * 2.0f);
            pregainA = juce::jmap (t, pre0A, pre50A);
            makeupA = juce::jmap (t, 1.0f, mu50A);
        }
        else
        {
            const float u = (d - 0.5f) * 2.0f;
            const float t = detail::smoothStep01 (u);
            pregainA = juce::jmap (t, pre50A, pre100A);
            makeupA = juce::jmap (t, mu50A, mu100A);
        }
        totalGainA = baseGainA * makeupA;
    }

    // Tape B: Rabbit-style family measured from the second reference (MOD=0%).
    const float pregainB = detail::interpDrive5 (d,
                                                 0.78f, 1.02f, 1.55f, 2.00f, 2.55f);
    const float totalGainB = detail::interpDrive5 (d,
                                                   2.35f, 2.28f, 2.20f, 2.14f, 2.10f);

    const float pregain = juce::jmap (rabbitMod, pregainA, pregainB);
    const float totalGain = juce::jmap (rabbitMod, totalGainA, totalGainB);

    // Tape bias is better treated as under/over-bias behaviour than as a plain
    // DC offset. Negative values under-bias the record stage (brighter, grittier);
    // positive values over-bias it (smoother, slightly softer, less HF aggression).
    const float biasClamped = detail::clampF (bias, -1.0f, 1.0f);
    const float underBias = std::max (-biasClamped, 0.0f);
    const float overBias  = std::max ( biasClamped, 0.0f);
    const float biasShift = biasClamped * (0.0005f + d * 0.0010f);
    float satIn = (x + biasShift) * pregain;
    satIn *= 1.0f + underBias * (0.04f + d * 0.08f)
                  - overBias  * (0.03f + d * 0.05f);
    if (underBias > 0.0001f)
    {
        const float oddBias = satIn * std::abs (satIn);
        satIn += oddBias * underBias * (0.004f + d * 0.012f);
    }

    // Magnetic stress: only hot material or high DRIVE should load the tape.
    // This stays out of the static curve so normal levels keep the existing feel.
    float& tapeStress = state.tapeStressEnv[sp][ch];
    const float stressDb = juce::jlimit (
        -80.0f, 30.0f,
        20.0f * std::log10 (std::max (std::abs (satIn), 1.0e-6f)));
    const float stressOverDb = std::max (0.0f, stressDb - 3.0f);
    const float driveStress = detail::smoothStep01 (detail::clampF ((d - 0.55f) / 0.45f, 0.0f, 1.0f));
    const float stressTarget = juce::jlimit (
        0.0f, 1.0f,
        (1.0f - adaa::fastExp (-stressOverDb / 10.0f))
            * (0.25f + driveStress * 0.75f));
    const float stressAttackHz = 5.5f + driveStress * 7.5f;      // ~29 ms -> ~12 ms
    const float stressReleaseHz = 0.80f + driveStress * 0.55f;   // ~199 ms -> ~118 ms
    const float stressCoeff = detail::onePoleCoeff (
        stressTarget > tapeStress ? stressAttackHz : stressReleaseHz, sr);
    tapeStress += (stressTarget - tapeStress) * stressCoeff;
    tapeStress = juce::jlimit (0.0f, 1.0f, tapeStress);
    const float stressCore = detail::smoothStep01 (tapeStress);

    // Tape B needs a different transfer feel, not only different gain. Morph
    // the drive shape itself so MOD becomes audible even at identical drive.
    if (rabbitMod > 0.0001f)
    {
        const float rabbit = rabbitMod * rabbitMod;
        const float hiTaper = 1.0f - 0.06f * detail::smoothStep01 ((d - 0.58f) * 1.55f);
        const float odd2 = satIn * std::abs (satIn);
        const float cubic = satIn * satIn * satIn;
        satIn += odd2 * rabbit * (0.042f + d * 0.110f) * hiTaper;
        satIn += cubic * rabbit * (0.0060f + d * 0.032f) * hiTaper;
    }

    // Use a fixed tanh ADAA kernel. Varying k inside the ADAA state was a
    // likely source of the pathological spikes seen in the diagnostics.
    float raw = adaaState.process (satIn, 1.0f);

    if (underBias > 0.0001f)
    {
        const float oddBias = raw * std::abs (raw);
        raw += oddBias * underBias * (0.006f + d * 0.015f);
    }
    if (overBias > 0.0001f)
    {
        const float oddBias = raw * std::abs (raw);
        raw -= oddBias * overBias * (0.004f + d * 0.010f);
        raw *= 1.0f - overBias * (0.02f + d * 0.03f);
    }

    if (stressCore > 0.0001f)
    {
        const float stressDensity = stressCore * (0.006f + d * 0.018f)
                                  * (0.65f + rabbitMod * 0.35f);
        raw += raw * std::abs (raw) * stressDensity;
    }

    if (rabbitMod > 0.0001f)
    {
        // Rabbit's drive curve is better described by a fitted odd-polynomial
        // family than by the manual post-core boosts we were using before.
        // These coefficients were extracted from comparison2 relative to
        // Rabbit's own 0% drive path.
        const float rabbit = rabbitMod * rabbitMod;
        const float a1 = detail::interpDrive5 (d,
                                               1.00000f, 1.21111f, 1.33567f, 1.35707f, 1.31288f);
        const float a3 = detail::interpDrive5 (d,
                                               0.00000f, -0.65204f, -1.19988f, -1.61318f, -1.83975f);
        const float a5 = detail::interpDrive5 (d,
                                               0.00000f, 0.50839f, 0.96282f, 1.34735f, 1.56277f);
        const float raw2 = raw * raw;
        const float rabbitRaw = a1 * raw + a3 * raw * raw2 + a5 * raw * raw2 * raw2;
        raw = juce::jmap (rabbit, raw, rabbitRaw);

        // Rabbit's perceived drive comes more from density/compression than
        // from simply adding output. A light stateful squeeze keeps peaks under
        // control while lifting sustain and small-signal material.
        const float envIn = std::abs (raw);
        const float atk = detail::onePoleCoeff (900.0f + d * 2200.0f, sr);
        const float rel = detail::onePoleCoeff (3.0f + d * 5.0f, sr);
        float& flux = state.tapeFlux[sp][ch];
        const float envCoeff = envIn > flux ? atk : rel;
        flux += (envIn - flux) * envCoeff;

        const float squeeze = 1.0f / (1.0f + flux * (0.55f + d * 0.40f));
        const float sustainLift = 1.0f + rabbit * (0.36f - d * 0.10f);
        raw *= juce::jmap (rabbit, 1.0f, squeeze * sustainLift);

        const float oddHard = raw * std::abs (raw);
        raw += oddHard * rabbit * (0.010f + d * 0.022f);
    }

    float outScale = totalGain / pregain;
    float out = raw * outScale;
    const float stressDrag = 1.0f / (1.0f + stressCore * (0.040f + d * 0.070f));
    out *= stressDrag;
    outScale *= stressDrag;

    if (rabbitMod > 0.0001f)
    {
        const float rabbit = rabbitMod * rabbitMod;
        const float rabbitMakeup = detail::interpDrive5 (d,
                                                         1.24f, 1.20f, 1.16f, 1.13f, 1.10f);
        out *= juce::jmap (rabbit, 1.0f, rabbitMakeup);
        outScale *= juce::jmap (rabbit, 1.0f, rabbitMakeup);
    }

    return out;
}


// ----------------------------------------------------------------
//  Main block processor
// ----------------------------------------------------------------
inline void processBlock (State& state,
                          float* left, float* right,
                          int numSamples,
                          Model model,
                          float driveParam,     // 0..1
                          float girthParam,     // 0..1
                          float modParam,       // 0..1
                          float biasParam,      // -1..1
                          float reactParam,     // 0..1
                          float detailParam,    // 0..1
                          float instabilityParam,       // 0..1
                          float sampleRate,
                          int   seriesCount = 1,
                          bool  isSafetyLpfOn = false,
                          bool  rawMode = false,
                          SatDiag::Collector* diagCollector = nullptr) noexcept
{

    // CLEAN model: 1:1 pass-through. Flush model state once on entry so
    // returning to a saturator never reuses stale envelopes/residuals.
    if (model == Model::Clean)
    {
        if (state.lastModel != Model::Clean)
            state.reset();
        return;
    }

    // -- Model-switch detection: reset filters & feedback to prevent transient explosions --
    if (model != state.lastModel)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            state.detailState[ch].reset();
            for (int sp = 0; sp < kMaxSeries; ++sp)
            {
                state.react[sp][ch].reset();
                state.mbReact[sp][ch].reset();
                state.sagEnvelope[sp][ch] = 0.0f;
                state.dynamicsComp[sp][ch].reset();
                state.clipperPeak[sp][ch].reset();
                state.transistorPeakCatch[sp][ch].reset();
                state.triodeReact[sp][ch].reset();
                state.emphasis[sp][ch].reset();
                state.dcX[sp][ch] = state.dcY[sp][ch] = 0.0f;
                state.triodeAdaa[sp][ch].reset();
                state.transistorCoreAdaa[sp][ch].reset();
                state.tapeAdaa[sp][ch].reset();
                state.clipperAdaa[sp][ch].reset();
                state.girthAdaa[sp][ch].reset();
                state.triodeBlock[sp][ch] = 0.0f;
                state.powerSag[sp][ch] = 0.0f;
                state.tapeFlux[sp][ch] = 0.0f;
                state.tapeStressEnv[sp][ch] = 0.0f;
                state.triodeBodyPreLP[sp][ch] = 0.0f;
                state.triodeBodyPostLP[sp][ch] = 0.0f;
                state.triodeCouplingDc[sp][ch] = 0.0f;
                state.interStageLPF[sp][ch] = 0.0f;
                state.interStageDCx[sp][ch] = 0.0f;
                state.interStageDCy[sp][ch] = 0.0f;
                state.transistorPreHP[sp][ch] = 0.0f;
                state.transistorPreEdge[sp][ch] = 0.0f;
                state.transistorPostLP[sp][ch] = 0.0f;
                state.transistorDbgPre[sp][ch] = 0.0f;
                state.transistorDbgCoreIn[sp][ch] = 0.0f;
                state.transistorDbgCoreOut[sp][ch] = 0.0f;
                state.transistorDbgRailIn[sp][ch] = 0.0f;
                state.transistorDbgRailOut[sp][ch] = 0.0f;
                state.transistorDbgPost[sp][ch] = 0.0f;
                state.transistorDbgInputPad[sp][ch] = 0.0f;
                state.transistorDbgSatK[sp][ch] = 0.0f;
                state.transistorDbgRailThresh[sp][ch] = 0.0f;
                state.bumpZ1[sp][ch] = state.bumpZ2[sp][ch] = 0.0f;
            }
        }
        state.lastModel = model;
    }

    // Sample-rate-aware parameter smoothing (~15ms time constant at any SR).
    // Using onePoleCoeff(11Hz) -> 63% in ~15ms, 95% in ~43ms. Consistent
    // whether running at 44.1kHz native or 176.4kHz (4x oversampled).
    const float oneMinusSmooth = detail::onePoleCoeff (11.0f, sampleRate);

    // DC blocker coefficient
    const float dcR = 1.0f - (kTwoPi * 5.0f / sampleRate);
    // REACT window size (model-dependent base, scaled by react amount)
    int reactBaseWindow = 1024;
    switch (model)
    {
        case Model::Tube:        reactBaseWindow = 1024; break;
        case Model::Diode:       reactBaseWindow = 2048; break;
        case Model::Clipper:     reactBaseWindow = 2048; break;
        case Model::Tape:        reactBaseWindow = 2048; break;
        default:                 break;
    }

    // Precomputed emphasis/de-emphasis coefficients (hoisted from per-sample)
    EmphCoeffs emphCoeffs;
    switch (model)
    {
        case Model::Tube:
            emphCoeffs.preHP  = detail::onePoleCoeff (20.0f,   sampleRate);
            emphCoeffs.preSh  = detail::onePoleCoeff (3800.0f, sampleRate);
            emphCoeffs.postLP = detail::onePoleCoeff (9500.0f, sampleRate);
            emphCoeffs.postHP = detail::onePoleCoeff (30.0f,   sampleRate);
            break;
        case Model::Diode:
            emphCoeffs.preHP  = detail::onePoleCoeff (720.0f,  sampleRate);
            emphCoeffs.preSh  = detail::onePoleCoeff (1800.0f, sampleRate);
            emphCoeffs.postLP = detail::onePoleCoeff (3200.0f, sampleRate);
            emphCoeffs.preHPAlt  = detail::onePoleCoeff (420.0f,  sampleRate);
            emphCoeffs.preShAlt  = detail::onePoleCoeff (2600.0f, sampleRate);
            emphCoeffs.postLPAlt = detail::onePoleCoeff (5600.0f, sampleRate);
            break;
        case Model::Clipper:
            emphCoeffs.preHP  = detail::onePoleCoeff (720.0f,  sampleRate);
            emphCoeffs.preSh  = detail::onePoleCoeff (1800.0f, sampleRate);
            emphCoeffs.postLP = detail::onePoleCoeff (2200.0f, sampleRate);
            break;
        case Model::Tape:
            emphCoeffs.preHP  = detail::onePoleCoeff (24.0f,   sampleRate);
            emphCoeffs.preSh  = detail::onePoleCoeff (2400.0f, sampleRate);
            emphCoeffs.postLP = detail::onePoleCoeff (14500.0f, sampleRate);
            break;
        default:
            break;
    }

    // Precomputed REACT envelope coefficients (hoisted from per-sample)
    const float reactAttCoeff  = 1.0f - std::exp (-kTwoPi * 1000.0f / sampleRate);
    const float reactRelCoeff  = 1.0f - std::exp (-kTwoPi * 2.0f / sampleRate);
    const int triodeBloomSamplesPerSlot = juce::jmax (1, (int) std::round (sampleRate * 0.001f));

    // Multiband REACT crossover coefficients (~200Hz sub/mid, ~4kHz mid/air)
    const float mbSubCoeff = detail::onePoleCoeff (200.0f, sampleRate);
    const float mbAirCoeff = detail::onePoleCoeff (4000.0f, sampleRate);
    const DetailCoeffs detailCoeffs = makeDetailCoeffs (sampleRate);

    // TRANSISTOR owns its colour inside the black box instead of relying on
    // generic interstage coupling.

    // Precomputed safety LPF coefficients (constant since fc = 0.4xsr)
    SafetyLPFCoeffs safetyCoeffs;
    if (isSafetyLpfOn)
    {
        const float fc   = sampleRate * 0.4f;
        const float w0   = kTwoPi * fc / sampleRate;
        const float cosW = std::cos (w0);
        const float sinW = std::sin (w0);
        const float alpha = sinW / (2.0f * 0.7071f);
        const float a0 = 1.0f + alpha;
        safetyCoeffs.b0 = ((1.0f - cosW) * 0.5f) / a0;
        safetyCoeffs.b1 = (1.0f - cosW) / a0;
        safetyCoeffs.b2 = safetyCoeffs.b0;
        safetyCoeffs.a1 = (-2.0f * cosW) / a0;
        safetyCoeffs.a2 = (1.0f - alpha) / a0;
    }

    // -- Per-block hoisted computations (avoid per-sample transcendentals) --
    // Drive curve: std::pow only once per block (driveParam is constant within a block)
    const float driveCurved = applyDriveCurve (driveParam, model);
    const float detailDriveTarget = detail::clampF (driveParam, 0.0f, 1.0f);
    const float detailTarget = detail::clampF (detailParam, 0.0f, 1.0f);

    for (int ch = 0; ch < 2; ++ch)
    {
        for (int sp = 0; sp < kMaxSeries; ++sp)
        {
            state.transistorDbgPre[sp][ch] = 0.0f;
            state.transistorDbgCoreIn[sp][ch] = 0.0f;
            state.transistorDbgCoreOut[sp][ch] = 0.0f;
            state.transistorDbgRailIn[sp][ch] = 0.0f;
            state.transistorDbgRailOut[sp][ch] = 0.0f;
            state.transistorDbgPost[sp][ch] = 0.0f;
            state.transistorDbgInputPad[sp][ch] = 0.0f;
            state.transistorDbgSatK[sp][ch] = 0.0f;
            state.transistorDbgRailThresh[sp][ch] = 0.0f;
        }
    }

    if (model == Model::Tape)
    {
        const bool tapeActive = driveCurved > 0.001f || modParam < 0.999f;
        if (tapeActive != state.tapeWasActive)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                for (int sp = 0; sp < kMaxSeries; ++sp)
                {
                    state.react[sp][ch].reset();
                    state.mbReact[sp][ch].reset();
                    state.sagEnvelope[sp][ch] = 0.0f;
                    state.emphasis[sp][ch].reset();
                    state.dynamicsComp[sp][ch].reset();
                    state.clipperPeak[sp][ch].reset();
                    state.transistorPeakCatch[sp][ch].reset();
                    state.triodeReact[sp][ch].reset();
                    state.dcX[sp][ch] = state.dcY[sp][ch] = 0.0f;
                    state.triodeAdaa[sp][ch].reset();
                    state.transistorCoreAdaa[sp][ch].reset();
                    state.tapeAdaa[sp][ch].reset();
                    state.clipperAdaa[sp][ch].reset();
                    state.interStageDCx[sp][ch] = 0.0f;
                    state.interStageDCy[sp][ch] = 0.0f;
                    state.interStageLPF[sp][ch] = 0.0f;
                    state.bumpZ1[sp][ch] = 0.0f;
                    state.bumpZ2[sp][ch] = 0.0f;
                    state.tapeFlux[sp][ch] = 0.0f;
                    state.tapeStressEnv[sp][ch] = 0.0f;
                }
            }
        }
        state.lastTapeDrive = driveCurved;
        state.tapeWasActive = tapeActive;
    }
    else if (model == Model::Transistor)
    {
        const float clampedGirth = detail::clampF (girthParam, 0.0f, 1.0f);
        const float clampedMod   = detail::clampF (modParam, 0.0f, 1.0f);
        const float clampedBias  = detail::clampF (biasParam, -1.0f, 1.0f);
        const float clampedReact = detail::clampF (reactParam, 0.0f, 1.0f);
        float& lastDrive = state.lastTransistorDrive;
        float& lastGirth = state.lastTransistorGirth;
        float& lastMod   = state.lastTransistorMod;
        float& lastBias  = state.lastTransistorBias;
        float& lastReact = state.lastTransistorReact;
        const float shapeSig = driveCurved
                             + 0.35f * clampedGirth
                             + 0.25f * clampedMod
                             + 0.15f * std::abs (clampedBias)
                             + 0.25f * clampedReact;
        const bool active = shapeSig > 0.001f;

        float& lastShape = state.lastTransistorShape;
        bool& wasActive = state.transistorWasActive;
        // These black boxes use parameter-dependent ADAA stages, but snapping
        // parameters can produce clicks. Model switches are handled above; here
        // we only reset when the transistor core enters/leaves its active state.
        if (active != wasActive)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                for (int sp = 0; sp < kMaxSeries; ++sp)
                {
                    state.react[sp][ch].reset();
                    state.mbReact[sp][ch].reset();
                    state.sagEnvelope[sp][ch] = 0.0f;
                    state.dynamicsComp[sp][ch].reset();
                    state.clipperPeak[sp][ch].reset();
                    state.transistorPeakCatch[sp][ch].reset();
                    state.emphasis[sp][ch].reset();
                    state.dcX[sp][ch] = state.dcY[sp][ch] = 0.0f;
                    state.transistorCoreAdaa[sp][ch].reset();
                    state.tapeAdaa[sp][ch].reset();
                    state.clipperAdaa[sp][ch].reset();
                    state.powerSag[sp][ch] = 0.0f;
                    state.transistorPreHP[sp][ch] = 0.0f;
                    state.transistorPreEdge[sp][ch] = 0.0f;
                    state.transistorPostLP[sp][ch] = 0.0f;
                    state.transistorDbgPre[sp][ch] = 0.0f;
                    state.transistorDbgCoreIn[sp][ch] = 0.0f;
                    state.transistorDbgCoreOut[sp][ch] = 0.0f;
                    state.transistorDbgRailIn[sp][ch] = 0.0f;
                    state.transistorDbgRailOut[sp][ch] = 0.0f;
                    state.transistorDbgPost[sp][ch] = 0.0f;
                    state.transistorDbgInputPad[sp][ch] = 0.0f;
                    state.transistorDbgSatK[sp][ch] = 0.0f;
                    state.transistorDbgRailThresh[sp][ch] = 0.0f;
                }
            }
        }

        lastDrive = driveCurved;
        lastGirth = clampedGirth;
        lastMod   = clampedMod;
        lastBias  = clampedBias;
        lastReact = clampedReact;
        lastShape = shapeSig;
        wasActive = active;
    }

    const int requestedSeriesCount = juce::jlimit (1, kMaxSeries, seriesCount);
    if (model == Model::Tube && requestedSeriesCount != state.currentSeriesCount)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            for (int sp = 0; sp < kMaxSeries; ++sp)
            {
                state.triodeReact[sp][ch].reset();
                state.triodeBodyPreLP[sp][ch] = 0.0f;
                state.triodeBodyPostLP[sp][ch] = 0.0f;
                state.triodeCouplingDc[sp][ch] = 0.0f;
                state.emphasis[sp][ch].reset();
                state.dcX[sp][ch] = state.dcY[sp][ch] = 0.0f;
                state.triodeAdaa[sp][ch].reset();
                state.girthAdaa[sp][ch].reset();
                state.sagEnvelope[sp][ch] = 0.0f;
            }
        }
    }
    state.currentSeriesCount = requestedSeriesCount;
    if (rawMode)
    {
        for (int ch = 0; ch < 2; ++ch)
            state.safetyLpf[ch].reset();

        // RAW strips wrapper colour/filter state only. Model dynamics such as
        // SAG/COMP/PEAK remain active when their control is enabled.
        for (int sp = 0; sp < state.currentSeriesCount; ++sp)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                state.emphasis[sp][ch].reset();
                state.dcX[sp][ch] = 0.0f;
                state.dcY[sp][ch] = 0.0f;
            }
        }
    }

    for (int i = 0; i < numSamples; ++i)
    {
        // -- Parameter smoothing (once per actual sample, NOT per series pass) --
        state.sDrive += (driveCurved - state.sDrive) * oneMinusSmooth;
        state.sDetailDrive += (detailDriveTarget - state.sDetailDrive) * oneMinusSmooth;
        state.sGirth += (girthParam  - state.sGirth) * oneMinusSmooth;
        state.sMod   += (modParam   - state.sMod)   * oneMinusSmooth;
        state.sBias  += (biasParam  - state.sBias)  * oneMinusSmooth;
        state.sReact += (reactParam - state.sReact) * oneMinusSmooth;
        state.sDetail += (detailTarget - state.sDetail) * oneMinusSmooth;
        state.sInstability   += (instabilityParam   - state.sInstability)   * oneMinusSmooth;

        const float drive = state.sDrive;
        const float detailDrive = state.sDetailDrive;
        const float girth = state.sGirth;
        const float mod   = state.sMod;
        const float bias  = state.sBias;
        const float react = state.sReact;
        const float detailAmount = state.sDetail;
        const float instabilityAmount = state.sInstability;

        // -- Instability: analog component tolerance + slow thermal drift --
        // Static tolerance (per-instance hash): each "unit" has unique character.
        // Thermal drift stays continuous and only touches AC-safe controls.
        float driveMod = 1.0f, shapeMod = 0.0f;
        if (instabilityAmount > 0.001f)
        {
            // Init static tolerances once per loader instance.
            if (! state.instability.tolerancesReady)
            {
                if (state.instabilitySeed == 0)
                    state.instabilitySeed = nextInstabilitySeed();

                state.instability.initTolerances (state.instabilitySeed);
            }

            // Very slow thermal drift: 0.03x0.15 Hz (one full cycle per 7x33 seconds)
            const float rate = 0.03f + instabilityAmount * 0.12f;
            state.instability.gainDrift.advance  (rate,         instabilityAmount, sampleRate);
            state.instability.shapeDrift.advance (rate * 1.43f, instabilityAmount, sampleRate);

            const float gainDynamic  = state.instability.gainDrift.dynamic;
            const float shapeDynamic = state.instability.shapeDrift.dynamic;
            constexpr float kInstabilityDynamicBase = 0.30f;
            constexpr float kInstabilityDynamicLift = 0.12f;
            const float dynamicBlend = detail::smoothStep01 (
                detail::clampF ((instabilityAmount - 0.15f) / 0.85f, 0.0f, 1.0f));
            const float dynamicWeight = kInstabilityDynamicBase + kInstabilityDynamicLift * dynamicBlend;
            const float staticWeight = 1.0f - dynamicWeight;
            const float gainInstability  = (state.instability.gainDrift.staticTol  * staticWeight + gainDynamic  * dynamicWeight) * instabilityAmount;
            const float shapeInstability = (state.instability.shapeDrift.staticTol * staticWeight + shapeDynamic * dynamicWeight) * instabilityAmount;

            const float ceilingScale = 1.0f + instabilityAmount;

            // Output already incorporates instability scaling (depth) -- no double-scaling.
            // Scale the whole range uniformly so INST remains subtle at low values
            // but reaches a clearly unstable unit at 100%.
            driveMod = 1.0f + gainInstability  * (0.08f  * ceilingScale); // +/-8..16% gain (tube gm + resistor dividers)
            shapeMod =        shapeInstability * (0.02f  * ceilingScale); // +/-2..4% shape (plate Rp instability)
        }

        const float detailChainInput[2] = { left[i], right[i] };

        // -- Per-sample series passes --
        // Series replicates the model's internal black-box stage N times.
        // Loader-level flow (ENV/FILTER/DELAY/etc.) stays outside SatEngine,
        // and there is no generic interstage colouring here. If a future
        // model needs explicit coupling between stages, that coupling should
        // live inside that model's own processFoo() implementation.
        for (int sp = 0; sp < seriesCount; ++sp)
        {
            state.currentSeriesPass = sp;
            const bool isFirst = (sp == 0);
            const bool isLast  = (sp == seriesCount - 1);

            for (int ch = 0; ch < 2; ++ch)
            {
                float& sample = (ch == 0) ? left[i] : right[i];
                float x = sample;
                auto& stageReact = state.react[sp][ch];
                auto& stageMbReact = state.mbReact[sp][ch];
                auto& stageSagEnvelope = state.sagEnvelope[sp][ch];
                auto& stageDynamicsComp = state.dynamicsComp[sp][ch];
                auto& stageClipperPeak = state.clipperPeak[sp][ch];
                auto& stageTriodeReact = state.triodeReact[sp][ch];
                auto& stageEmphasis = state.emphasis[sp][ch];
                auto& stageDcX = state.dcX[sp][ch];
                auto& stageDcY = state.dcY[sp][ch];

                // -- Safety LPF (first pass only, x1 mode) --
                if (isSafetyLpfOn && isFirst && !rawMode)
                    x = processSafetyLPF (state.safetyLpf[ch], x, safetyCoeffs);


                // Tube bias is DC-sensitive, so instability stays out of its core
                // operating point and is applied post-coupling instead.
                const bool tubeCoreInstabilitySafe = model == Model::Tube;
                float effDrive = tubeCoreInstabilitySafe ? drive : drive * driveMod;
                float effBias  = bias;
                float effMod   = tubeCoreInstabilitySafe ? mod : detail::clampF (mod + shapeMod, 0.0f, 1.0f);

                // -- INTERNAL PRE-EMPHASIS (per series pass, unless rawMode) --
                if (!rawMode && model != Model::Transistor)
                    x = preEmphasize (x, stageEmphasis, model, effDrive, effMod, emphCoeffs);

                // -- REACT: per-stage energy tracking + model processing --
                float sagPre  = 1.0f;
                float sagPost = 1.0f;
                float sagBias = 0.0f;
                float diodeReactColor = 0.0f;
                MultibandSagResult mbSag;
                bool useMbSag = false;

                if (react > 0.001f && model != Model::Tube && model != Model::Transistor)
                {
                    const int window = std::min (
                        (int) ((float) reactBaseWindow * (1.0f + react * 3.0f) * sampleRate / 44100.0f),
                        kReactBufSize - 1);

                    reactTrackEnergy (stageReact, x, window);
                    const float depletion = reactGetDepletion (stageReact, window);

                    if (depletion > stageSagEnvelope)
                        stageSagEnvelope += (depletion - stageSagEnvelope) * reactAttCoeff;
                    else
                        stageSagEnvelope += (depletion - stageSagEnvelope) * reactRelCoeff;

                    const float sagEnv = stageSagEnvelope;

                    switch (model)
                    {
                        case Model::Diode:
                        {
                            const float compDry = x;
                            const float program = detail::clampF (sagEnv, 0.0f, 1.0f);
                            const DynamicsCompResult comp = processDiodeComp (
                                x, stageDynamicsComp, react, effDrive, program, sampleRate);
                            const float compMix = detail::compressionBlendMix (react);
                            x = juce::jmap (compMix, compDry, comp.sample);
                            diodeReactColor = detail::clampF (comp.amount * compMix, 0.0f, 1.0f);
                            sagPre = 1.0f;
                            sagPost = 1.0f;
                            stageSagEnvelope = comp.amount;
                            break;
                        }
                        case Model::Tape:
                        {
                            const float compDry = x;
                            const float program = detail::clampF (sagEnv, 0.0f, 1.0f);
                            const DynamicsCompResult comp = processTapeComp (
                                x, stageDynamicsComp, react, effDrive, program, sampleRate);
                            const float compMix = detail::compressionBlendMix (react);
                            x = juce::jmap (compMix, compDry, comp.sample);
                            sagPre = 1.0f;
                            sagPost = 1.0f;
                            effDrive = std::min (effDrive * juce::jmap (compMix, 1.0f, comp.driveLift), 1.0f);
                            stageSagEnvelope = comp.amount;
                            break;
                        }
                        case Model::Clipper:
                        {
                            const float program = detail::clampF (sagEnv, 0.0f, 1.0f);
                            const ClipperPeakResult peak = processClipperPeak (
                                x, stageClipperPeak, react, effDrive, program, sampleRate);
                            x = peak.sample;
                            sagPre = 1.0f;
                            sagPost = 1.0f;
                            stageSagEnvelope = peak.amount;
                            break;
                        }
                        default: break;
                    }
                }
                else if (model == Model::Tape)
                {
                    stageDynamicsComp.gain = 1.0f;
                    stageDynamicsComp.env *= 0.5f;
                    stageDynamicsComp.hfEnv *= 0.5f;
                    stageDynamicsComp.bodyEnv *= 0.5f;
                    stageSagEnvelope = 0.0f;
                }
                else if (model == Model::Diode)
                {
                    stageDynamicsComp.gain = 1.0f;
                    stageDynamicsComp.env *= 0.5f;
                    stageDynamicsComp.hfEnv *= 0.5f;
                    stageDynamicsComp.bodyEnv *= 0.5f;
                    stageSagEnvelope = 0.0f;
                }
                else if (model == Model::Clipper)
                {
                    stageClipperPeak.peakEnv *= 0.5f;
                    stageClipperPeak.bodyEnv *= 0.5f;
                    stageClipperPeak.gain += (1.0f - stageClipperPeak.gain) * 0.25f;
                    stageSagEnvelope = 0.0f;
                }
                else if (model == Model::Transistor)
                {
                    stageSagEnvelope = 0.0f;
                }
                else if (model == Model::Tube && react <= 0.001f)
                {
                    stageTriodeReact.control *= 0.5f;
                    stageTriodeReact.lastSag *= 0.5f;
                    stageTriodeReact.lastSupply += (1.0f - stageTriodeReact.lastSupply) * 0.25f;
                    stageTriodeReact.supplyEnv *= 0.5f;
                    stageTriodeReact.supplyDrop *= 0.5f;
                    stageTriodeReact.strikeEnv *= 0.5f;
                    stageTriodeReact.bloomEnv *= 0.5f;
                    stageTriodeReact.bloomFastDemandEnv *= 0.5f;
                    stageTriodeReact.bloomDemandEnv *= 0.5f;
                    stageTriodeReact.burnFast *= 0.5f;
                    stageTriodeReact.burnSlow *= 0.5f;
                    stageTriodeReact.burnEnv *= 0.5f;
                    stageTriodeReact.atrophyEnv *= 0.5f;
                    stageTriodeReact.reservoirDrainEnv *= 0.5f;
                    stageTriodeReact.bloomSum *= 0.5f;
                    stageTriodeReact.bloomSlotSum = 0.0f;
                    stageTriodeReact.bloomSlotSamples = 0;
                    stageTriodeReact.bloomWindowSlots = 0;
                    stageTriodeReact.bloomActive = false;
                    stageSagEnvelope = 0.0f;
                }

                // Apply stage-local pre-sag boost + bias drift.
                {
                    if (useMbSag)
                    {
                        // Apply multiband pre-sag: split->boost per band->recombine
                        float subSig, midSig, airSig;
                        multibandReactSplit (stageMbReact, x, mbSubCoeff, mbAirCoeff,
                                            subSig, midSig, airSig);
                        x = subSig * mbSag.sagPreSub + midSig * mbSag.sagPreMid + airSig * mbSag.sagPreAir;
                    }
                    else
                    {
                        x *= sagPre;
                    }
                    effBias += sagBias;
                }

                // -- WAVESHAPER (all passes, with per-pass ADAA state) --
                if (diagCollector != nullptr && isLast && ch == 0)
                {
                    diagCollector->feedLastPassIn (x);
                    if (model == Model::Tube)
                        diagCollector->feedTriodeBlock (state.triodeBlock[sp][ch]);
                }

                switch (model)
                {
                    case Model::Tube:
                    {
                        x = processTriode (x, effDrive, girth, effBias, effMod, react, rawMode,
                                           state, ch, sampleRate,
                                           triodeBloomSamplesPerSlot,
                                           state.triodeAdaa[sp][ch]);
                        break;
                    }
                    case Model::Transistor:
                    {
                        x = processTransistorStage (x, effDrive, girth, effBias, effMod,
                                                    react, rawMode,
                                                    state, ch, sampleRate,
                                                    state.clipperAdaa[sp][ch]);
                        break;
                    }
                    case Model::Diode:
                    {
                        x = processDiodeStage (x, effDrive, girth, effBias, effMod,
                                               diodeReactColor,
                                               state.clipperAdaa[sp][ch]);
                        break;
                    }
                    case Model::Tape:
                    {
                        x = processTape (x, effDrive, effBias, effMod, rawMode,
                                         state, ch, sampleRate,
                                         state.tapeAdaa[sp][ch], isFirst);
                        break;
                    }
                    case Model::Clipper:
                    {
                        x = processClipper (x, effDrive, girth, effBias, effMod,
                                            rawMode, state, ch, sampleRate,
                                            state.clipperAdaa[sp][ch]);
                        break;
                    }
                    default: break;
                }

                if (diagCollector != nullptr && isLast && ch == 0)
                    diagCollector->feedCore (x);

                // -- Intermediate safety: prevent extreme values entering girth/filters --
                // Soft clip: transparent below +/-2, asymptotic to +/-3
                {
                    const float absX = std::abs (x);
                    if (absX > 2.0f)
                    {
                        const float sign = (x >= 0.0f) ? 1.0f : -1.0f;
                        x = sign * (2.0f + std::tanh (absX - 2.0f));
                    }
                }

                if (diagCollector != nullptr && isLast && ch == 0)
                    diagCollector->feedClip (x);

                // -- Post-sag ceiling (per series pass) --
                {
                    if (useMbSag)
                    {
                        // Multiband post-sag: split->attenuate per band->recombine
                        float subSig, midSig, airSig;
                        multibandReactSplit (stageMbReact, x, mbSubCoeff, mbAirCoeff,
                                            subSig, midSig, airSig);
                        x = subSig * mbSag.sagPostSub + midSig * mbSag.sagPostMid + airSig * mbSag.sagPostAir;
                    }
                    else
                    {
                        x *= sagPost;
                    }
                }


                // -- CHAR (all passes) --
                if (model == Model::Tape)
                {
                    x = applyTapeGirth (x, girth);
                }
                else if (model == Model::Transistor || model == Model::Clipper
                      || model == Model::Diode)
                {
                    // CHAR/COLOR is already encoded inside these cores.
                }
                else if (model == Model::Tube)
                {
                    x = applyTriodeGirth (x, girth);
                }
                else
                    x = applyGirth (x, girth, state.girthAdaa[sp][ch]);

                // -- INTERNAL DE-EMPHASIS (per series pass, unless rawMode) --
                if (!rawMode && model != Model::Transistor)
                    x = deEmphasize (x, stageEmphasis, model, effDrive, effMod, emphCoeffs);

                // -- DC BLOCKER (1st-order HPF at 5Hz, per series pass) --
                if (!rawMode)
                {
                    const float dcOut = x - stageDcX + dcR * stageDcY;
                    stageDcX = x;
                    stageDcY = dcOut;
                    x = dcOut;
                }

                if (model == Model::Tube && isLast)
                    x *= driveMod;

                if (diagCollector != nullptr && isLast && ch == 0)
                    diagCollector->feedDc (x);

                // -- Model level trim --
                // Transistor is calibrated per black-box stage so SERIES feeds
                // each repeated stage at a stable nominal level. Tape/Tube keep
                // their existing final-chain trim because their stages are
                // internally level-calibrated.
                if (model == Model::Transistor)
                {
                    x *= getTransistorLevelTrim (drive, mod, girth, react);
                    if (isLast)
                        x *= getTransistorLevelCorrection (detailDrive, girth, mod, state.currentSeriesCount)
                           * getHotInputReferenceCorrection (model, detailDrive, girth, mod, state.currentSeriesCount);
                }
                else if (isLast)
                {
                    if (model == Model::Tape)
                        x *= getTapeLevelTrim (drive, mod, girth, react)
                           * getTapeLevelCorrection (detailDrive, girth, mod, state.currentSeriesCount)
                           * getHotInputReferenceCorrection (model, detailDrive, girth, mod, state.currentSeriesCount);
                    else if (model == Model::Tube)
                        x *= getTriodeLevelTrim (drive, mod, state.currentSeriesCount)
                           * getTriodeLevelCorrection (detailDrive, girth, mod, state.currentSeriesCount)
                           * getHotInputReferenceCorrection (model, detailDrive, girth, mod, state.currentSeriesCount);
                    else if (model == Model::Diode)
                        x *= getDiodeLevelTrim (detailDrive, girth, mod, state.currentSeriesCount)
                           * getHotInputReferenceCorrection (model, detailDrive, girth, mod, state.currentSeriesCount);
                    else if (model == Model::Clipper)
                        x *= getClipperLevelCorrection (detailDrive, girth, mod, state.currentSeriesCount);
                }

                // -- Final safety soft-limiter --
                // Transparent below +/-1.5, smooth compression above, max +/-2.5
                // Eliminates hard-clip discontinuities that cause audible clicks.
                if (isLast)
                {
                    const float absX = std::abs (x);
                    if (absX > 1.5f)
                    {
                        const float sign = (x >= 0.0f) ? 1.0f : -1.0f;
                        x = sign * (1.5f + std::tanh (absX - 1.5f));
                    }
                }

                if (diagCollector != nullptr && isLast && ch == 0)
                    diagCollector->feedLim (x);

                sample = x;
            }
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            float& sample = (ch == 0) ? left[i] : right[i];
            const float detailDelta = makeDetailHardClipDelta (detailChainInput[ch], detailDrive);
            sample = applyDetailPreservation (sample, detailDelta, detailAmount,
                                              state.detailState[ch], detailCoeffs);
        }
    }
}

} // namespace SatEngine

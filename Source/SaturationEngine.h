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
//    GIRTH (post-waveshaper wavefolding + sharpen)
//    Instability (analog drift via Hermite S&H)
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
    float scLP  = 0.0f;
    float env   = 0.0f;
    float hfEnv = 0.0f;
    float gain  = 1.0f;

    void reset() noexcept
    {
        scLP = env = hfEnv = 0.0f;
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
//  Micro-wander: very small high-range movement for extra analog instability
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

struct MicroWanderOsc
{
    float phase1 = 0.0f;
    float phase2 = 0.0f;
    float basePhase1 = 0.0f;
    float basePhase2 = 0.0f;
    float output = 0.0f;

    void initPhase (uint32_t seed, int paramIdx) noexcept
    {
        uint32_t h = seed ^ (uint32_t (paramIdx) * 2246822519u);
        h = ((h >> 15) ^ h) * 0x2c1b3c6du;
        h = ((h >> 12) ^ h) * 0x297a2d39u;
        h = (h >> 15) ^ h;

        basePhase1 = float (h & 0xFFFFu) / 65536.0f;
        basePhase2 = float ((h >> 16) & 0xFFFFu) / 65536.0f;
        phase1 = basePhase1;
        phase2 = basePhase2;
    }

    void advance (float rate, float depth, float sampleRate) noexcept
    {
        const float sr = juce::jmax (1.0f, sampleRate);
        phase1 += rate / sr;
        phase2 += rate * 1.618034f / sr;

        if (phase1 >= 1.0f) phase1 -= std::floor (phase1);
        if (phase2 >= 1.0f) phase2 -= std::floor (phase2);

        const float wander = std::sin (phase1 * kTwoPi) * 0.6f
                           + std::sin (phase2 * kTwoPi) * 0.4f;
        output = wander * depth;
    }

    void reset() noexcept
    {
        phase1 = basePhase1;
        phase2 = basePhase2;
        output = 0.0f;
    }
};

struct SmoothSampleHoldOsc
{
    float curr = 0.0f;
    float next = 0.0f;
    float phase = 0.0f;
    float output = 0.0f;
    uint32_t baseSeed = 1u;
    uint32_t state = 1u;

    static uint32_t scramble (uint32_t v) noexcept
    {
        v ^= v >> 16;
        v *= 0x7feb352du;
        v ^= v >> 15;
        v *= 0x846ca68bu;
        v ^= v >> 16;
        return v == 0u ? 1u : v;
    }

    static float toBipolar (uint32_t v) noexcept
    {
        return (float ((scramble (v) >> 8) & 0x00FFFFFFu) * (2.0f / 16777216.0f)) - 1.0f;
    }

    uint32_t nextHash() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return scramble (state);
    }

    void initSeed (uint32_t seed, int paramIdx) noexcept
    {
        baseSeed = scramble (seed ^ (uint32_t (paramIdx) * 3266489917u));
        reset();
    }

    void reset() noexcept
    {
        state = baseSeed;
        phase = 0.0f;
        curr = toBipolar (nextHash());
        next = toBipolar (nextHash());
        output = curr;
    }

    void advance (float rate, float depth, float sampleRate) noexcept
    {
        const float sr = juce::jmax (1.0f, sampleRate);
        phase += juce::jmax (0.0f, rate) / sr;

        while (phase >= 1.0f)
        {
            phase -= 1.0f;
            curr = next;
            next = toBipolar (nextHash());
        }

        const float t = juce::jlimit (0.0f, 1.0f, phase);
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float smooth = t3 * (t * (t * 6.0f - 15.0f) + 10.0f);
        output = (curr + (next - curr) * smooth) * depth;
    }
};

struct InstabilityState
{
    DriftOsc gainDrift;
    DriftOsc biasDrift;
    DriftOsc shapeDrift;
    DriftOsc asymDrift;
    SmoothSampleHoldOsc gainSH;
    SmoothSampleHoldOsc shapeSH;
    MicroWanderOsc driveWander;
    MicroWanderOsc shapeWander;
    float shMix = 0.0f;
    bool     tolerancesReady = false;

    void initTolerances (uint32_t seed) noexcept
    {
        gainDrift.initTolerance  (seed, 0);
        biasDrift.initTolerance  (seed, 1);
        shapeDrift.initTolerance (seed, 2);
        asymDrift.initTolerance  (seed, 3);
        gainSH.initSeed          (seed, 6);
        shapeSH.initSeed         (seed, 8);
        driveWander.initPhase    (seed, 4);
        shapeWander.initPhase    (seed, 5);
        tolerancesReady = true;
    }

    void reset() noexcept
    {
        gainDrift.reset();
        biasDrift.reset();
        shapeDrift.reset();
        asymDrift.reset();
        gainSH.reset();
        shapeSH.reset();
        driveWander.reset();
        shapeWander.reset();
        shMix = 0.0f;
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

    // Internal emphasis/de-emphasis (per-stage / per-channel)
    EmphasisState emphasis[kMaxSeries][2];

    // ADAA states -- main waveshaper [series pass][channel]
    adaa::StableTanhADAA triodeAdaa[kMaxSeries][2];
    adaa::StableTanhADAA transistorCoreAdaa[kMaxSeries][2];
    adaa::TapeTanhADAA tapeAdaa[kMaxSeries][2];
    adaa::ClipperADAA clipperAdaa[kMaxSeries][2];
    // GIRTH wavefolder ADAA [series pass][channel]
    adaa::SinFoldADAA girthAdaa[kMaxSeries][2];

    // Instability drift
    InstabilityState instability;
    uint32_t instabilitySeed = 0;
    float instabilitySignalEnv = 0.0f;

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
    float sGirth = 0.0f;
    float sBias  = 0.0f;
    float sReact = 0.0f;
    float sMod   = 0.0f;
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
            for (int sp = 0; sp < kMaxSeries; ++sp)
            {
                react[sp][ch].reset();
                mbReact[sp][ch].reset();
                sagEnvelope[sp][ch] = 0.0f;
                dynamicsComp[sp][ch].reset();
                clipperPeak[sp][ch].reset();
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
        instabilitySignalEnv = 0.0f;
        sDrive = sGirth = sBias = sReact = sMod = sInstability = 0.0f;
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
            }
        }
        fl (instabilitySignalEnv);
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
    const float detSq = low * low * (0.42f + react * 0.10f)
                      + high * high * highWeight;
    const float detector = std::sqrt (std::max (detSq, 0.0f) + 1.0e-12f);

    const float attackHz  = 55.0f + react * 180.0f + drive * 70.0f;
    const float releaseHz = 1.9f + program * (3.5f + react * 6.0f);
    const float atk = detail::onePoleCoeff (attackHz, sr);
    const float rel = detail::onePoleCoeff (releaseHz, sr);

    if (detector > st.env)
        st.env += (detector - st.env) * atk;
    else
        st.env += (detector - st.env) * rel;

    const float hfDet = std::abs (high) * (1.0f + react * 1.2f);
    const float hfAtk = std::min (1.0f, atk * 1.35f);
    const float hfRel = detail::onePoleCoeff (releaseHz * 1.7f + 1.0f, sr);
    if (hfDet > st.hfEnv)
        st.hfEnv += (hfDet - st.hfEnv) * hfAtk;
    else
        st.hfEnv += (hfDet - st.hfEnv) * hfRel;

    const float threshold = juce::jmap (react, 0.34f, 0.14f)
                          * juce::jmap (drive, 1.04f, 0.90f);
    const float ratio = juce::jmap (react, 1.25f, 3.8f);
    const float over = st.env / std::max (threshold, 1.0e-4f);
    const float knee = detail::smoothStep01 ((over - 0.85f) / 0.75f);

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
                                   * (0.08f + 0.10f * program + 0.05f * react);
    const float targetGain = juce::jlimit (0.35f, 1.0f, compGain * makeup);
    const float gainAtk = detail::onePoleCoeff (90.0f + react * 170.0f, sr);
    const float gainRel = detail::onePoleCoeff (4.0f + program * (4.0f + react * 5.0f), sr);

    if (targetGain < st.gain)
        st.gain += (targetGain - st.gain) * gainAtk;
    else
        st.gain += (targetGain - st.gain) * gainRel;

    r.sample = (low + high * hfGain) * st.gain;
    r.driveLift = 1.0f + (1.0f - compGain) * react * (0.06f + 0.06f * program);
    r.amount = 1.0f - compGain;
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

    const float det = std::abs (x);
    const float detectorTilt = juce::jmap (type, 1.05f, 1.10f);
    const float detector = det * detectorTilt;

    const float attackHz = juce::jmap (type,
                                       1200.0f + react * 2500.0f,
                                       1350.0f + react * 2800.0f);
    const float releaseHz = juce::jmap (type,
                                        1.8f + react * 6.8f,
                                        1.9f + react * 7.4f)
                          + detector * juce::jmap (type, 2.8f, 3.2f);
    const float atk = detail::onePoleCoeff (attackHz, sr);
    const float rel = detail::onePoleCoeff (releaseHz, sr);

    if (detector > st.env)
        st.env += (detector - st.env) * atk;
    else
        st.env += (detector - st.env) * rel;

    const float threshold = juce::jmap (type,
                                        juce::jmap (react, 0.36f, 0.095f),
                                        juce::jmap (react, 0.35f, 0.090f))
                          * juce::jmap (drive, 1.02f, 0.78f);
    const float ratio = juce::jmap (type,
                                    juce::jmap (react, 7.0f, 13.5f),
                                    juce::jmap (react, 7.5f, 14.0f));
    const float over = st.env / std::max (threshold, 1.0e-4f);

    float compGain = 1.0f;
    if (over > 1.0f)
        compGain = std::pow (over, -(ratio - 1.0f) / ratio);

    const float allButtons = detail::smoothStep01 ((react - 0.82f) / 0.18f) * type;
    compGain *= 1.0f - allButtons * detail::smoothStep01 ((over - 1.0f) / 1.8f) * 0.10f;
    compGain = juce::jlimit (0.18f, 1.0f, compGain);

    const float makeup = 1.0f + (1.0f - compGain) * juce::jmap (type, 0.00f, 0.02f);
    const float targetGain = juce::jlimit (0.18f, 1.0f, compGain * makeup);
    const float gainAtk = detail::onePoleCoeff (attackHz * juce::jmap (type, 1.35f, 1.55f), sr);
    const float gainRel = detail::onePoleCoeff (releaseHz, sr);

    if (targetGain < st.gain)
        st.gain += (targetGain - st.gain) * gainAtk;
    else
        st.gain += (targetGain - st.gain) * gainRel;

    st.scLP = detector;
    st.hfEnv += (detector - st.hfEnv) * detail::onePoleCoeff (releaseHz * 0.7f + 0.5f, sr);

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
    const float absX = std::abs (detectorInput);

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

    const float absX = std::abs (x);

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
    const int offset = juce::jlimit (1, kTriodeSagBufSize - 2,
                                     (int) std::round (2.42f * overallscale));

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

    // Bloom remembers both hot input and sustained supply collapse.
    const float hotOverDb = std::max (0.0f, senseDb);
    const float hotTarget = (1.0f - adaa::fastExp (-hotOverDb / (10.0f - reactDepth * 3.0f)))
                          * reactDepth;
    const float bloomInputTarget = juce::jlimit (
        0.0f, 1.0f,
        std::max (hotTarget, supplyTarget * (0.25f + reactDepth * 0.50f)));
    const float strikeAttackHz = 40.0f + reactDepth * 45.0f;     // ~4 ms -> ~2 ms
    const float strikeReleaseHz = 2.3f + reactDepth * 1.8f;      // ~70 ms -> ~39 ms
    const float strikeCoeff = detail::onePoleCoeff (
        hotTarget > st.strikeEnv ? strikeAttackHz : strikeReleaseHz, sr);
    st.strikeEnv += (hotTarget - st.strikeEnv) * strikeCoeff;
    st.strikeEnv = juce::jlimit (0.0f, 1.0f, st.strikeEnv);

    const float bloomWindowMs = 35.0f + (reactDepth * reactDepth) * 515.0f;
    const int targetBloomWindowSlots = juce::jlimit (1, kTriodeBloomSlotCount - 2,
                                                     (int) std::round (bloomWindowMs));

    // Airwindows-style bloom memory: a real time window, stored in 1 ms slots
    // so the musical recovery length stays consistent at any host SR or
    // oversampling factor. React 100% now reaches a long ~500 ms bloom tail.
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
    const float bloomRiseHz = 8.0f + reactDepth * 6.0f;
    const float bloomFallHz = 1.6f - reactDepth * 1.0f;
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

// ----------------------------------------------------------------
//  GIRTH -- post-waveshaper fold + sharpen
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
            const float lowRetain = 0.28f + drive * 0.12f;
            const float klon = juce::jmap (lowRetain, x, hp)
                             + edge * (0.012f + drive * 0.035f);
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
            const float klonBase = y + (st.postLP - y) * (0.08f + drive * 0.16f);
            const float bright = y - st.postLP;
            const float klon = klonBase + bright * (0.010f + (1.0f - drive) * 0.015f);
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
    const float signalPresence = detail::smoothStep01 (
        juce::jlimit (0.0f, 1.0f, state.instabilitySignalEnv));
    auto& triodeSag = state.triodeReact[sp][ch];
    auto& bodyPreLp = state.triodeBodyPreLP[sp][ch];
    auto& bodyPostLp = state.triodeBodyPostLP[sp][ch];
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

    if (!rawMode && react > 0.0001f)
    {
        const float sagSense = getTriodeSagSenseInput (xStage);
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
    const float burnCore = juce::jlimit (-1.0f, 1.0f, triodeSag.burnEnv);
    const float burnPress = std::max (0.0f, burnCore);
    const float atrophyCore = detail::smoothStep01 (
        juce::jlimit (0.0f, 1.0f, triodeSag.atrophyEnv)) * (0.45f + supplyCore * 0.55f);
    const float atrophyDb = atrophyCore * juce::jmap (tubeMorph, 4.5f, 7.5f);
    const float reservoirCore = detail::smoothStep01 (
        juce::jlimit (0.0f, 1.0f, triodeSag.reservoirDrainEnv))
        * (0.35f + supplyCore * 0.65f);
    const float reservoirDb = reservoirCore * juce::jmap (tubeMorph, 1.6f, 2.5f);
    const float atrophyGain = adaa::fastExp (-(atrophyDb + reservoirDb) * 0.11512925465f);

    // Tube2ustyle stage inside the black box. MOD/GIRTH stay mostly outside
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
    const float stageBias = juce::jmap (tubeMorph, stageBias12AX7, stageBiasPower)
                          * signalPresence;
    const float cathodeDepth12AX7 = bodyCurve * (0.040f + d * 0.050f);
    const float cathodeDepthPower = bodyCurve * (0.026f + d * 0.032f);
    const float cathodeDepth = juce::jmap (tubeMorph, cathodeDepth12AX7, cathodeDepthPower);
    const float bloomHeadroomRecovery = bloomCore * supplyCore * (0.066f + tubeMorph * 0.154f);

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
                                               bloomCore * supplyCore * (0.65f + tubeMorph * 1.05f));
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
    s = detail::clampF (s, -ceiling, ceiling);
    s *= 1.0f / ceiling;

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
// MOD morphs BJT punch into softer FET behaviour while GIRTH/BODY relaxes
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
                                adaa::ClipperADAA& adaaState) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float c = detail::clampF (girth, 0.0f, 1.0f);
    const float s = detail::clampF (bias, -1.0f, 1.0f);
    const float t = detail::clampF (mod, 0.0f, 1.0f);

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

    const float threshold = condThreshold * thresholdMul;
    const float knee = std::max (1.0e-4f, condKnee * kneeMul);

    const float thresholdPos = detail::clampF (threshold * (1.0f + s * symRange), 0.22f, 1.65f);
    const float thresholdNeg = detail::clampF (threshold * (1.0f - s * symRange), 0.22f, 1.65f);
    const float kneePos = std::max (1.0e-4f, knee * (1.0f - s * 0.18f));
    const float kneeNeg = std::max (1.0e-4f, knee * (1.0f + s * 0.18f));

    float clipIn = x * driveGain * condDrive;
    clipIn += x * std::abs (x) * edgeShape;

    float clipped = adaaState.process (clipIn, thresholdPos, thresholdNeg,
                                       kneePos, kneeNeg);

    clipped *= 2.0f / (thresholdPos + thresholdNeg);

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
    float cleanBlend = 0.0f;
    float voiceLift = 1.0f;
    if (m <= 0.5f)
    {
        const float t = detail::smoothStep01 (m * 2.0f);
        voiceScale = juce::jmap (t, 1.00f, 1.08f); // TS gets slightly tighter
    }
    else
    {
        const float u = detail::smoothStep01 ((m - 0.5f) * 2.0f);
        voiceScale = juce::jmap (u, 1.08f, 0.92f); // Klon opens back up
        cleanBlend = 0.18f * u;
        voiceLift = juce::jmap (u, 1.0f, 1.12f);
    }

    const float clipIn = x * (voiceScale / std::max (threshold, 0.05f));

    // BIAS becomes symmetry / mismatch: shifts positive and negative clip
    // thresholds independently, but keep their mean around unity.
    const float thresholdPos = detail::clampF (1.0f + b * 0.45f, 0.45f, 1.55f);
    const float thresholdNeg = detail::clampF (1.0f - b * 0.45f, 0.45f, 1.55f);
    const float kneeSoft = 0.01f + (1.0f - k) * 0.56f;
    const float kneePos = std::max (1.0e-4f, thresholdPos * kneeSoft);
    const float kneeNeg = std::max (1.0e-4f, thresholdNeg * kneeSoft);

    float clipped = adaaState.process (clipIn, thresholdPos, thresholdNeg,
                                       kneePos, kneeNeg);

    // Preserve average ceiling when asymmetry moves thresholds apart.
    clipped *= 2.0f / (thresholdPos + thresholdNeg);

    if (cleanBlend > 0.0001f)
    {
        const float clean = detail::clampF (x * (0.90f + 0.10f * voiceScale), -1.25f, 1.25f);
        clipped = juce::jmap (cleanBlend, clipped, clean);
    }

    return clipped * voiceLift * juce::jmap (d, 0.98f, 0.92f);
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

    float out = raw * (totalGain / pregain);
    const float stressDrag = 1.0f / (1.0f + stressCore * (0.040f + d * 0.070f));
    out *= stressDrag;

    if (rabbitMod > 0.0001f)
    {
        const float rabbit = rabbitMod * rabbitMod;
        const float rabbitMakeup = detail::interpDrive5 (d,
                                                         1.24f, 1.20f, 1.16f, 1.13f, 1.10f);
        out *= juce::jmap (rabbit, 1.0f, rabbitMakeup);
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
                          float instabilityParam,       // 0..1
                          float sampleRate,
                          int   seriesCount = 1,
                          bool  isSafetyLpfOn = false,
                          bool  rawMode = false,
                          SatDiag::Collector* diagCollector = nullptr) noexcept
{

    // CLEAN model: 1:1 pass-through -- no saturation processing at all
    if (model == Model::Clean)
        return;

    // -- Model-switch detection: reset filters & feedback to prevent transient explosions --
    if (model != state.lastModel)
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
        state.instabilitySignalEnv = 0.0f;
    }

    // Sample-rate-aware parameter smoothing (~15ms time constant at any SR).
    // Using onePoleCoeff(11Hz) -> 63% in ~15ms, 95% in ~43ms. Consistent
    // whether running at 44.1kHz native or 176.4kHz (4x oversampled).
    const float oneMinusSmooth = detail::onePoleCoeff (11.0f, sampleRate);

    // DC blocker coefficient
    const float dcR = 1.0f - (kTwoPi * 5.0f / sampleRate);
    const float instabilityPresenceAttack = detail::onePoleCoeff (400.0f, sampleRate);
    const float instabilityPresenceRelease = detail::onePoleCoeff (8.0f, sampleRate);

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
                state.emphasis[sp][ch].reset();
                state.dcX[sp][ch] = state.dcY[sp][ch] = 0.0f;
                state.triodeAdaa[sp][ch].reset();
                state.girthAdaa[sp][ch].reset();
                state.sagEnvelope[sp][ch] = 0.0f;
            }
        }
    }
    state.currentSeriesCount = requestedSeriesCount;
    const bool rawStripFilters = rawMode && (model == Model::Tape || model == Model::Tube
                                          || model == Model::Transistor
                                          || model == Model::Diode);

    if (rawMode)
    {
        for (int ch = 0; ch < 2; ++ch)
            state.safetyLpf[ch].reset();

        for (int sp = 0; sp < state.currentSeriesCount; ++sp)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                state.emphasis[sp][ch].reset();
                state.dcX[sp][ch] = 0.0f;
                state.dcY[sp][ch] = 0.0f;
                if (!(rawMode && model == Model::Tape))
                    state.sagEnvelope[sp][ch] = 0.0f;

                if (model == Model::Tape)
                {
                    if (!rawMode)
                    {
                        state.dynamicsComp[sp][ch].reset();
                        state.tapeFlux[sp][ch] = 0.0f;
                        state.tapeStressEnv[sp][ch] = 0.0f;
                    }
                }

                if (model == Model::Diode)
                    state.dynamicsComp[sp][ch].reset();

                if (model == Model::Tube)
                    state.triodeReact[sp][ch].reset();
            }
        }
    }

    for (int i = 0; i < numSamples; ++i)
    {
        // -- Parameter smoothing (once per actual sample, NOT per series pass) --
        state.sDrive += (driveCurved - state.sDrive) * oneMinusSmooth;
        state.sGirth += (girthParam  - state.sGirth) * oneMinusSmooth;
        state.sMod   += (modParam   - state.sMod)   * oneMinusSmooth;
        state.sBias  += (biasParam  - state.sBias)  * oneMinusSmooth;
        state.sReact += (reactParam - state.sReact) * oneMinusSmooth;
        state.sInstability   += (instabilityParam   - state.sInstability)   * oneMinusSmooth;

        const float drive = state.sDrive;
        const float girth = state.sGirth;
        const float mod   = state.sMod;
        const float bias  = state.sBias;
        const float react = state.sReact;
        const float instabilityAmount = state.sInstability;
        const float inputPeak = std::max (std::abs (left[i]), std::abs (right[i]));
        const float presenceTarget = detail::smoothStep01 (
            detail::clampF ((inputPeak - 1.0e-5f) / (5.0e-4f - 1.0e-5f), 0.0f, 1.0f));
        const float presenceCoeff = presenceTarget > state.instabilitySignalEnv
                                  ? instabilityPresenceAttack
                                  : instabilityPresenceRelease;
        state.instabilitySignalEnv += (presenceTarget - state.instabilitySignalEnv) * presenceCoeff;
        const float instabilitySignalScale = state.instabilitySignalEnv;

        // -- Instability: analog component tolerance + slow thermal drift --
        // Static tolerance (per-instance hash): each "unit" has unique character.
        // Thermal drift (sub-Hz sines): very slow cathode/plate temp changes.
        // Micro-wander only appears at high instability and stays intentionally subtle.
        float driveMod = 1.0f, biasMod = 0.0f, shapeMod = 0.0f, asymMod = 0.0f;
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
            state.instability.biasDrift.advance  (rate * 0.37f, instabilityAmount, sampleRate);
            state.instability.shapeDrift.advance (rate * 1.43f, instabilityAmount, sampleRate);
            state.instability.asymDrift.advance  (rate * 0.61f, instabilityAmount, sampleRate);

            // Smooth S&H adds a small non-periodic thermal/mechanical wrinkle
            // inside the existing dynamic component. It remains deterministic
            // and much smaller than the fixed component tolerance.
            state.instability.gainSH.advance  (rate * 2.10f, 1.0f, sampleRate);
            state.instability.shapeSH.advance (rate * 2.80f, 1.0f, sampleRate);
            const float shTarget = detail::smoothStep01 (
                detail::clampF ((instabilityAmount - 0.20f) / 0.80f, 0.0f, 1.0f));
            state.instability.shMix += (shTarget - state.instability.shMix)
                                   * detail::onePoleCoeff (5.0f, sampleRate);
            const float shWeight = state.instability.shMix * 0.08f;
            const float oscWeight = 1.0f - shWeight;
            const float gainDynamic  = state.instability.gainDrift.dynamic  * oscWeight
                                     + state.instability.gainSH.output      * shWeight;
            const float biasDynamic  = state.instability.biasDrift.dynamic;
            const float shapeDynamic = state.instability.shapeDrift.dynamic * oscWeight
                                     + state.instability.shapeSH.output     * shWeight;
            const float asymDynamic  = state.instability.asymDrift.dynamic;
            const float gainInstability  = (state.instability.gainDrift.staticTol  * 0.70f + gainDynamic  * 0.30f) * instabilityAmount;
            const float biasInstability  = (state.instability.biasDrift.staticTol  * 0.70f + biasDynamic  * 0.30f) * instabilityAmount;
            const float shapeInstability = (state.instability.shapeDrift.staticTol * 0.70f + shapeDynamic * 0.30f) * instabilityAmount;
            const float asymInstability  = (state.instability.asymDrift.staticTol  * 0.70f + asymDynamic  * 0.30f) * instabilityAmount;

            const float wanderNorm = detail::smoothStep01 (
                detail::clampF ((instabilityAmount - 0.70f) / 0.30f, 0.0f, 1.0f));
            const float wanderDepth = wanderNorm * wanderNorm;
            if (wanderDepth > 0.0f)
            {
                const float wanderRate = 0.45f + wanderDepth * 3.55f;
                state.instability.driveWander.advance (wanderRate,         wanderDepth, sampleRate);
                state.instability.shapeWander.advance (wanderRate * 1.37f, wanderDepth, sampleRate);
            }

            // Output already incorporates instability scaling (depth) -- no double-scaling
            driveMod = 1.0f + gainInstability  * 0.08f;  // +/-8% gain (tube gm + resistor dividers)
            biasMod  =        biasInstability  * 0.04f;  // +/-4% bias (cathode R + grid leak)
            shapeMod =        shapeInstability * 0.02f;  // +/-2% shape (plate Rp instability)
            asymMod  =        asymInstability  * 0.025f; // +/-2.5% L/R (matched pair mismatch)
            driveMod += state.instability.driveWander.output * 0.015f;      // high-instability +/-1.5% drive wander
            shapeMod += state.instability.shapeWander.output * 0.005f;      // high-instability +/-0.5% shape wander
        }
        else
        {
            state.instability.shMix = 0.0f;
        }

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


                // -- Apply Instability per-channel (L/R asymmetry decorrelation) --
                const float scaledDriveMod = 1.0f + (driveMod - 1.0f) * instabilitySignalScale;
                float effDrive = drive * scaledDriveMod;
                const float instabilityBias = (biasMod + (ch == 0 ? asymMod : -asymMod)) * instabilitySignalScale;
                float effBias  = bias + instabilityBias;
                float effMod   = detail::clampF (mod + shapeMod * instabilitySignalScale, 0.0f, 1.0f);

                // -- INTERNAL PRE-EMPHASIS (per series pass, unless rawMode) --
                if (!rawMode && model != Model::Transistor)
                    x = preEmphasize (x, stageEmphasis, model, effDrive, effMod, emphCoeffs);

                // -- REACT: per-stage energy tracking + model processing --
                float sagPre  = 1.0f;
                float sagPost = 1.0f;
                float sagBias = 0.0f;
                MultibandSagResult mbSag;
                bool useMbSag = false;

                if (react > 0.001f && model != Model::Tube && model != Model::Transistor
                    && !(rawStripFilters && (model == Model::Transistor
                                          || model == Model::Clipper || model == Model::Diode)))
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
                            const DynamicsCompResult comp = processTapeComp (
                                x, stageDynamicsComp, react, effDrive, program, sampleRate);
                            const float compMix = detail::compressionBlendMix (react);
                            x = juce::jmap (compMix, compDry, comp.sample);
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
                    stageSagEnvelope = 0.0f;
                }
                else if (model == Model::Diode)
                {
                    stageDynamicsComp.gain = 1.0f;
                    stageDynamicsComp.env *= 0.5f;
                    stageDynamicsComp.hfEnv *= 0.5f;
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
                        x = processTriode (x, effDrive, girth, effBias, effMod, react, rawMode,
                                           state, ch, sampleRate,
                                           triodeBloomSamplesPerSlot,
                                           state.triodeAdaa[sp][ch]);
                        break;
                    case Model::Transistor:
                        x = processTransistorStage (x, effDrive, girth, effBias, effMod,
                                                    react, rawMode,
                                                    state, ch, sampleRate,
                                                    state.clipperAdaa[sp][ch]);
                        break;
                    case Model::Diode:
                        x = processDiodeStage (x, effDrive, girth, effBias, effMod,
                                               state.clipperAdaa[sp][ch]);
                        break;
                    case Model::Tape:
                        x = processTape (x, effDrive, effBias, effMod, rawMode,
                                         state, ch, sampleRate,
                                         state.tapeAdaa[sp][ch], isFirst);
                        break;
                    case Model::Clipper:
                        x = processClipper (x, effDrive, girth, effBias, effMod,
                                            state.clipperAdaa[sp][ch]);
                        break;
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


                // -- GIRTH (all passes) --
                if (model == Model::Tape)
                {
                    x = applyTapeGirth (x, girth);
                }
                else if (model == Model::Transistor || model == Model::Clipper
                      || model == Model::Diode)
                {
                    // GIRTH/COLOR is already encoded inside these cores.
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

                if (diagCollector != nullptr && isLast && ch == 0)
                    diagCollector->feedDc (x);

                // -- Final level trim (per series pass) --
                if (isLast)
                {
                    if (model == Model::Tape)
                        x *= getTapeLevelTrim (drive, mod, girth, react);
                    else if (model == Model::Tube)
                        x *= getTriodeLevelTrim (drive, mod, state.currentSeriesCount);
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
    }
}

} // namespace SatEngine

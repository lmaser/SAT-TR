#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>
#include <atomic>
#include "SatDspDiag.h"

// ══════════════════════════════════════════════════════════════
//  SaturationEngine — header-only DSP for SAT-TR
//  6 physically-modeled saturation algorithms with:
//    ADAA (1st-order antiderivative anti-aliasing)
//    REACT (Airwindows-inspired energy tracking → parameter modulation)
//    GIRTH (post-waveshaper wavefolding + sharpen)
//    VARIATION (analog drift via Hermite S&H)
//    MOD (input-domain power warp + model-specific secondary)
//    Internal emphasis / de-emphasis EQ per model
//    Auto-gain compensation
//    Safety LPF for ×1 (no oversampling) mode
// ══════════════════════════════════════════════════════════════

namespace SatEngine
{

// ──────────────────────────────────────────────────────────────
//  Model enum
// ──────────────────────────────────────────────────────────────
enum class Model : int
{
    Clean     = 0,   // Bypass — 1:1 pass-through (no saturation)
    Tape      = 1,   // Tape stage — soft magnetic compression + losses
    Triode    = 2,   // TUBE — 12AX7 -> EL34/6L6-inspired morph via MOD
    PushPull  = 3,   // Legacy power stage — retained for compatibility
    Cascade   = 4,   // Cascaded triode stages (1-4, fractional crossfade)
    Diode     = 5,   // Shockley diode clipper — Ge/Si blend
    Tundra    = 6,   // Metal Zone→Modern morph — dual-stage cascaded clipping
    Fuzz      = 7,   // Transistor fuzz — Ge/Si stages with bias starving
    Doom      = 8,   // Multi-stage doom fuzz — Big Muff inspired wall-of-sound
    Destroy   = 9,   // Ring-fuzz — pitch-tracked ring modulation + feedback
    Clipper   = 10,  // Broadband/pedal clipper — classic -> TS -> Klon voice
    NumModels = 11
};

inline constexpr Model canonicalizeModel (Model model) noexcept
{
    return model == Model::PushPull ? Model::Triode : model;
}

// ──────────────────────────────────────────────────────────────
//  Constants
// ──────────────────────────────────────────────────────────────
static constexpr float kPi         = 3.14159265358979323846f;
static constexpr float kHalfPi     = 1.57079632679489661923f;
static constexpr float kTwoPi      = 6.28318530717958647692f;
static constexpr float kInvPi      = 0.31830988618379067154f;
static constexpr float kLn2        = 0.69314718055994530942f;
static constexpr float kSmoothCoeff = 0.995f;   // ~10ms @ 48kHz
static constexpr int   kReactBufSize = 8192;
static constexpr int   kTriodeSagBufSize = 512;
static constexpr int   kMaxCascade   = 4;
static constexpr int   kMaxSeries    = 4;

// ──────────────────────────────────────────────────────────────
//  ADAA1 helpers  (1st-order antiderivative anti-aliasing)
// ──────────────────────────────────────────────────────────────
namespace adaa
{
    // ── Fast math helpers (avoid expensive std::exp/log1p in hot path) ──
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
        // Pade [2/2]: log(1+x) ≈ x(6+x) / (6+4x)
        return x * (6.0f + x) / (6.0f + 4.0f * x);
    }

    // Antiderivative of tanh(k·x):  F1(x) = (1/k)·ln(cosh(k·x))
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

    // Generic ADAA1 processor for tanh(k·x)
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

        inline float process (float x,
                              float thresholdPos, float thresholdNeg,
                              float kneePos, float kneeNeg) noexcept
        {
            constexpr float kTol = 1.0e-5f;

            const float direct = clip (x, thresholdPos, thresholdNeg, kneePos, kneeNeg);

            if (! initialised || ! std::isfinite (prev) || ! std::isfinite (ad1Prev))
            {
                prev = x;
                ad1Prev = clipAD1 (x, thresholdPos, thresholdNeg, kneePos, kneeNeg);
                initialised = true;
                return direct;
            }

            const float ad1 = clipAD1 (x, thresholdPos, thresholdNeg, kneePos, kneeNeg);
            const float dx = x - prev;
            const float mid = clip (0.5f * (x + prev), thresholdPos, thresholdNeg, kneePos, kneeNeg);
            float y = (std::abs (dx) < kTol) ? mid : (ad1 - ad1Prev) / dx;

            if (! std::isfinite (y))
                y = direct;

            prev = x;
            ad1Prev = ad1;
            return y;
        }

        void reset() noexcept
        {
            prev = 0.0f;
            ad1Prev = 0.0f;
            initialised = false;
        }
    };

    // ──────────────────────────────────────────────────────────────
    //  ADAA-2 (2nd-order antiderivative anti-aliasing) for hard clippers
    //  Used by Doom, Destroy, Fuzz — models with steep waveshaper slopes
    //  F2(x) = second antiderivative of tanh(k·x)
    // ──────────────────────────────────────────────────────────────

    // Second antiderivative of tanh(k·x):
    //   F2(x) = (1/k²) · [ Li₂(-e^{-2kx}) + kx·ln(1+e^{-2kx}) + (kx)²/2 ]
    //   Stable approx: use direct integration of F1
    //   F2(x) ≈ x·F1(x) - (1/(2k²))·ln²(cosh(k·x)) ... complex.
    //   Practical: numerical F2 via Simpson integration of F1.
    //   Better: closed-form  F2(x) = x·F1(x) − (1/k²)·( Li₂(−e^{2kx}) + kx·ln(1+e^{−2kx}) )
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

// ──────────────────────────────────────────────────────────────
//  REACT  (Airwindows-inspired circular buffer energy tracker)
// ──────────────────────────────────────────────────────────────
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

// ──────────────────────────────────────────────────────────────
//  Multiband REACT  (3-band energy tracker: sub / mid / air)
//  Crossover at ~200Hz (sub/mid) and ~4kHz (mid/air).
//  Each band has its own energy window → frequency-dependent sag.
// ──────────────────────────────────────────────────────────────
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

struct TapeCompState
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

struct TriodeReactState
{
    float sagBuf[kTriodeSagBufSize] = {};
    float control = 0.0f;
    int   gcount = 0;
    float prevIn = 0.0f;
    float prevOut = 0.0f;
    float prevHyst = 0.0f;
    float lastSag = 0.0f;
    float lastSupply = 1.0f;

    void reset() noexcept
    {
        std::memset (sagBuf, 0, sizeof (sagBuf));
        control = 0.0f;
        gcount = 0;
        prevIn = prevOut = prevHyst = 0.0f;
        lastSag = 0.0f;
        lastSupply = 1.0f;
    }
};

inline void multibandReactSplit (MultibandReactState& mb, float x,
                                 float coeffSub, float coeffAir,
                                 float& outSub, float& outMid, float& outAir) noexcept
{
    // 1st-order LP at ~200Hz → sub
    mb.lpSub += (x - mb.lpSub) * coeffSub;
    const float subBand = mb.lpSub;
    const float hiPass  = x - mb.lpSub;

    // 1st-order LP at ~4kHz → mid (of hi-passed signal)
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

// ──────────────────────────────────────────────────────────────
//  VARIATION — analog component tolerance + slow thermal drift
//  Static tolerance (dominant, 70%): per-instance hash → fixed offset
//  Thermal drift (secondary, 30%): 3 incommensurate sub-Hz sines
// ──────────────────────────────────────────────────────────────
struct DriftOsc
{
    float phase1 = 0.0f;
    float phase2 = 0.0f;
    float phase3 = 0.0f;
    float staticTol = 0.0f;     // per-component tolerance (fixed per instance)
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
        // Very slow non-periodic thermal drift (~0.03-0.15 Hz)
        phase1 += rate / sampleRate;
        phase2 += rate * 0.7937f / sampleRate;   // cube-root of 2
        phase3 += rate * 0.6180f / sampleRate;   // 1/phi (golden ratio inverse)

        if (phase1 >= 1.0f) phase1 -= 1.0f;
        if (phase2 >= 1.0f) phase2 -= 1.0f;
        if (phase3 >= 1.0f) phase3 -= 1.0f;

        const float drift = std::sin (phase1 * kTwoPi) * 0.5f
                          + std::sin (phase2 * kTwoPi) * 0.3f
                          + std::sin (phase3 * kTwoPi) * 0.2f;

        // Static tolerance (70%) + slow thermal drift (30%)
        output = (staticTol * 0.7f + drift * 0.3f) * depth;
    }

    void reset() noexcept { phase1 = phase2 = phase3 = 0.0f; output = 0.0f; }
};

struct VariationState
{
    DriftOsc gainDrift;
    DriftOsc biasDrift;
    DriftOsc shapeDrift;
    DriftOsc asymDrift;
    bool     tolerancesReady = false;

    void initTolerances (uint32_t seed) noexcept
    {
        gainDrift.initTolerance  (seed, 0);
        biasDrift.initTolerance  (seed, 1);
        shapeDrift.initTolerance (seed, 2);
        asymDrift.initTolerance  (seed, 3);
        tolerancesReady = true;
    }

    void reset() noexcept
    {
        gainDrift.reset();
        biasDrift.reset();
        shapeDrift.reset();
        asymDrift.reset();
    }
};

inline uint32_t nextVariationSeed() noexcept
{
    static std::atomic<uint32_t> counter { 0x5A54'5231u };
    return counter.fetch_add (0x9E37'79B9u, std::memory_order_relaxed);
}

// ──────────────────────────────────────────────────────────────
//  Internal emphasis/de-emphasis EQ state (1st-order filters)
// ──────────────────────────────────────────────────────────────
struct EmphasisState
{
    float preHP  = 0.0f;
    float preSh  = 0.0f;
    float postLP = 0.0f;
    float postHP = 0.0f;

    void reset() noexcept { preHP = 0.0f; preSh = 0.0f; postLP = 0.0f; postHP = 0.0f; }
};

// ──────────────────────────────────────────────────────────────
//  Safety LPF for ×1 mode (2nd-order Butterworth at 0.4×fs)
// ──────────────────────────────────────────────────────────────
struct SafetyLPF
{
    float x1 = 0.0f, x2 = 0.0f;
    float y1 = 0.0f, y2 = 0.0f;

    void reset() noexcept { x1 = x2 = y1 = y2 = 0.0f; }
};

// ──────────────────────────────────────────────────────────────
//  Full per-channel / per-loader state
// ──────────────────────────────────────────────────────────────
struct State
{
    // Per-stage dynamic state. Series should replicate the internal stage
    // black box, so these states must not be shared across passes.
    ReactState react[kMaxSeries][2];
    float sagEnvelope[kMaxSeries][2] = {};
    TapeCompState tapeComp[kMaxSeries][2];
    TriodeReactState triodeReact[kMaxSeries][2];

    // CASCADE per-stage DC accumulation (coupling cap model)
    // [series pass][stage][channel]
    float cascadeDC[kMaxSeries][kMaxCascade][2] = {};

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

    // Internal emphasis/de-emphasis (per-stage / per-channel)
    EmphasisState emphasis[kMaxSeries][2];

    // ADAA states — main waveshaper [series pass][channel]
    adaa::TanhADAA wsAdaa[kMaxSeries][2];
    adaa::StableTanhADAA triodeAdaa[kMaxSeries][2];
    adaa::TapeTanhADAA tapeAdaa[kMaxSeries][2];
    adaa::ClipperADAA clipperAdaa[kMaxSeries][2];
    // ADAA-2 states — hard clippers (Fuzz/Doom/Destroy) [series pass][channel]
    adaa::TanhADAA2 wsAdaa2[kMaxSeries][2];
    // FUZZ Q2 second ADAA-2 stage [series pass][channel]
    adaa::TanhADAA2 fuzzAdaa2Q2[kMaxSeries][2];
    // DOOM S2 second ADAA-2 stage [series pass][channel]
    adaa::TanhADAA2 doomAdaa2S2[kMaxSeries][2];
    // CASCADE per-stage ADAA [series pass][stage][channel]
    adaa::TanhADAA cascadeAdaa[kMaxSeries][kMaxCascade][2];
    // GIRTH wavefolder ADAA [series pass][channel]
    adaa::SinFoldADAA girthAdaa[kMaxSeries][2];

    // VARIATION drift
    VariationState variation;
    uint32_t variationSeed = 0;

    // Multiband REACT (per-stage / per-channel)
    MultibandReactState mbReact[kMaxSeries][2];

    // Safety LPF (per-channel, for ×1 mode)
    SafetyLPF safetyLpf[2];

    // DOOM (Big Muff-style) state [series pass][filter][channel]
    // [0]=coupling HPF S1, [1]=coupling HPF S2, [2]=Miller LPF S1, [3]=Miller LPF S2, [4]=tone LP, [5]=tone HP
    float doomDC[kMaxSeries][6][2] = {};
    float doomFeedback[kMaxSeries][2] = {};

    // FUZZ feedback + coupling caps [series pass][channel]
    float fuzzFeedback[kMaxSeries][2] = {};     // global feedback (R4 path)
    float fuzzCoupDC[kMaxSeries][2] = {};       // coupling cap HPF between Q1→Q2 (~50Hz)
    float fuzzToneLPF[kMaxSeries][2] = {};      // post-clip tone LPF (MOD-controlled)

    // Shared YIN pitch tracker state (used by sub-osc + DESTROY ring mod)
    static constexpr int kYinBufSize = 2048; // analysis window (~46ms @ 44.1k, supports down to ~43Hz)
    float yinBuf[kYinBufSize] = {};          // circular input buffer for autocorrelation
    int   yinWritePos = 0;                   // write position
    float yinSmoothedFreq = 220.0f;          // smoothed detected frequency (Hz)
    float yinRawFreq = 220.0f;               // last detected frequency (unsmoothed, for onset snap)
    int   yinCounter = 0;                    // samples since last YIN analysis

    // DESTROY (Plasma discharge) state
    float destroyRingPhase = 0.0f;           // ring mod oscillator phase (mono)
    float destroyFeedback[kMaxSeries][2] = {};  // feedback [series pass][channel]
    float destroyXfmrLP[kMaxSeries][2] = {};    // transformer BW limit LPF
    float destroyRectHP[kMaxSeries][2] = {};     // post-rectifier coupling cap HPF

    // TUNDRA inter-stage coupling cap + tightness HPF
    float tundraInterHP[kMaxSeries][2] = {};
    float tundraTightHP[kMaxSeries][2] = {};
    // TUNDRA presence bump (2-pole BP state per-channel)
    float tundraPresZ1[kMaxSeries][2] = {};
    float tundraPresZ2[kMaxSeries][2] = {};

    // Inter-stage LPF for series chaining (one-pole per pass per channel)
    float interStageLPF[kMaxSeries][2] = {};

    // Inter-stage DC blocker (coupling cap between series passes)
    float interStageDCx[kMaxSeries][2] = {};
    float interStageDCy[kMaxSeries][2] = {};

    // Sub-octave synthesizer (pitch-tracked sine at f/2)
    float subOscPhase = 0.0f;       // phase accumulator (mono, both ch use same)
    float subOscEnv[2] = {};        // per-channel envelope follower
    float subOscLPF[2] = {};        // per-channel output LPF (~250 Hz)

    // Current series pass / total series count (set by processBlock before waveshapers)
    int currentSeriesPass = 0;
    int currentSeriesCount = 1;

    // Model-switch detection (reset filters/feedback on change to prevent transients)
    Model lastModel = Model::Clean;

    // Per-block precomputed constant filter coefficients
    // Avoids recomputing onePoleCoeff per-sample for fixed frequencies
    struct BlockCoeffs {
        float fuzzCoupC    = 0;  // 50Hz coupling cap
        float doomCoupC1   = 0;  // 55Hz coupling cap S1
        float doomCoupC2   = 0;  // 94Hz coupling cap S2
        float doomToneLP   = 0;  // 1200Hz tone LP
        float doomToneHP   = 0;  // 723Hz tone HP
        float destroyRectC = 0;  // 70Hz post-rectifier HPF
        float tundraCoupC  = 0;  // 80Hz inter-stage coupling
        float yinFastC     = 0;  // 60Hz adaptive smoothing (fast)
        float yinSlowC     = 0;  // 6Hz adaptive smoothing (slow)
        float autoGain     = 1;  // precomputed auto-gain compensation
    } blockCoeffs;

    // Parameter smoothing (one-pole IIR)
    float sDrive = 0.0f;
    float sGirth = 0.0f;
    float sBias  = 0.0f;
    float sReact = 0.0f;
    float sMod   = 0.0f;
    float sVar   = 0.0f;
    float lastTapeDrive = 0.0f;
    bool tapeWasActive = false;

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
                tapeComp[sp][ch].reset();
                triodeReact[sp][ch].reset();
                dcX[sp][ch] = dcY[sp][ch] = 0.0f;
                emphasis[sp][ch].reset();
                bumpZ1[sp][ch] = bumpZ2[sp][ch] = 0.0f;
                triodeBlock[sp][ch] = 0.0f;
                powerSag[sp][ch] = 0.0f;
                tapeFlux[sp][ch] = 0.0f;
                wsAdaa[sp][ch].reset();
                triodeAdaa[sp][ch].reset();
                tapeAdaa[sp][ch].reset();
                clipperAdaa[sp][ch].reset();
                wsAdaa2[sp][ch].reset();
                fuzzAdaa2Q2[sp][ch].reset();
                doomAdaa2S2[sp][ch].reset();
                girthAdaa[sp][ch].reset();
                doomDC[sp][0][ch] = doomDC[sp][1][ch] = doomDC[sp][2][ch] = 0.0f;
                doomDC[sp][3][ch] = doomDC[sp][4][ch] = doomDC[sp][5][ch] = 0.0f;
                doomAdaa2S2[sp][ch].reset();
                doomFeedback[sp][ch] = 0.0f;
                fuzzFeedback[sp][ch] = 0.0f;
                fuzzCoupDC[sp][ch] = 0.0f;
                fuzzToneLPF[sp][ch] = 0.0f;
                destroyFeedback[sp][ch] = 0.0f;
                destroyXfmrLP[sp][ch] = 0.0f;
                destroyRectHP[sp][ch] = 0.0f;
                interStageLPF[sp][ch] = 0.0f;
                interStageDCx[sp][ch] = 0.0f;
                interStageDCy[sp][ch] = 0.0f;
                tundraInterHP[sp][ch] = 0.0f;
                tundraTightHP[sp][ch] = 0.0f;
                tundraPresZ1[sp][ch] = 0.0f;
                tundraPresZ2[sp][ch] = 0.0f;
                for (int s = 0; s < kMaxCascade; ++s)
                {
                    cascadeDC[sp][s][ch] = 0.0f;
                    cascadeAdaa[sp][s][ch].reset();
                }
            }
        }
        flutterPhase = 0.0f;
        std::memset (yinBuf, 0, sizeof (yinBuf));
        yinWritePos = 0;
        yinCounter = 0;
        yinSmoothedFreq = 220.0f;
        yinRawFreq = 220.0f;
        destroyRingPhase = 0.0f;
        subOscPhase = 0.0f;
        subOscEnv[0] = subOscEnv[1] = 0.0f;
        subOscLPF[0] = subOscLPF[1] = 0.0f;
        currentSeriesPass = 0;
        currentSeriesCount = 1;
        lastModel = Model::Clean;
        variation.reset();
        sDrive = sGirth = sBias = sReact = sMod = sVar = 0.0f;
        lastTapeDrive = 0.0f;
        tapeWasActive = false;
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
                fl (tapeComp[sp][ch].scLP);
                fl (tapeComp[sp][ch].env);
                fl (tapeComp[sp][ch].hfEnv);
                fl (tapeComp[sp][ch].gain);
                fl (triodeReact[sp][ch].control);
                fl (triodeReact[sp][ch].prevIn);
                fl (triodeReact[sp][ch].prevOut);
                fl (triodeReact[sp][ch].prevHyst);
                fl (triodeReact[sp][ch].lastSag);
                fl (triodeReact[sp][ch].lastSupply);
                for (int f = 0; f < 6; ++f) fl (doomDC[sp][f][ch]);
                fl (doomFeedback[sp][ch]);
                fl (fuzzFeedback[sp][ch]);
                fl (fuzzCoupDC[sp][ch]);
                fl (fuzzToneLPF[sp][ch]);
                fl (destroyFeedback[sp][ch]);
                fl (destroyXfmrLP[sp][ch]);
                fl (destroyRectHP[sp][ch]);
                fl (tundraInterHP[sp][ch]);
                fl (tundraTightHP[sp][ch]);
                fl (tundraPresZ1[sp][ch]);
                fl (tundraPresZ2[sp][ch]);
                fl (interStageLPF[sp][ch]);
                fl (interStageDCx[sp][ch]);
                fl (interStageDCy[sp][ch]);
                fl (bumpZ1[sp][ch]);
                fl (bumpZ2[sp][ch]);
                fl (triodeBlock[sp][ch]);
                fl (powerSag[sp][ch]);
                fl (tapeFlux[sp][ch]);
            }
            fl (subOscEnv[ch]);
            fl (subOscLPF[ch]);
        }
    }
};

// ──────────────────────────────────────────────────────────────
//  Internal helpers
// ──────────────────────────────────────────────────────────────
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

struct TapeCompResult
{
    float sample    = 0.0f;
    float driveLift = 1.0f;
    float amount    = 0.0f;
};

struct TriodeReactResult
{
    float sample    = 0.0f;
    float supply    = 1.0f;
    float biasShift = 0.0f;
    float amount    = 0.0f;
};

inline TapeCompResult processTapeComp (float x, TapeCompState& st,
                                       float react, float drive, float program,
                                       float sr) noexcept
{
    TapeCompResult r;
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

inline float getTriodeSagSenseInput (float x) noexcept
{
    // Sense the actual signal arriving at the stage. Keep headroom for
    // already-hot material so sag follows real input energy.
    return juce::jlimit (-12.0f, 12.0f, x);
}

inline TriodeReactResult processTriodeReact (float sample, float sense,
                                             TriodeReactState& st,
                                             float react,
                                             float sr) noexcept
{
    TriodeReactResult r;
    r.sample = sample;

    if (react <= 0.0001f)
        return r;

    // Airwindows-style power sag: short energy memory that directly deforms
    // the sample before it hits the Tube2-style stage. In this plugin the
    // SAG control is the depth control, so we have to map it much more
    // assertively than the fixed-intensity desk context Chris uses.
    const float depth = detail::clampF (react, 0.0f, 1.0f);
    const float depth2 = depth * depth;
    // Pro-style sag controls are usually subtle in the lower half and get
    // much steeper near the top. Keep 0-30% gentle, but make 100% clearly
    // more extreme than the old near-linear mapping.
    const float depthCurve = juce::jlimit (0.0f, 1.5f,
                                           depth * (0.18f + depth * 1.18f));
    const float overallscale = sr / 44100.0f;
    const int offset = juce::jlimit (1, kTriodeSagBufSize - 2,
                                     (int) std::round (2.42f * overallscale));

    if (st.gcount < 0 || st.gcount >= kTriodeSagBufSize)
        st.gcount = kTriodeSagBufSize - 1;

    const int idx = st.gcount;
    int oldIdx = idx + offset;
    if (oldIdx >= kTriodeSagBufSize) oldIdx -= kTriodeSagBufSize;

    // Current draw must rise faster than linearly with hot input, otherwise
    // +6 dB or +12 dB material does not feel dramatically more sagged.
    const float senseMag = std::abs (sense);
    const float senseNorm = juce::jlimit (0.0f, 4.0f, senseMag);
    const float currentDraw = senseNorm * (0.55f + 0.30f * std::min (1.0f, senseNorm))
                            + (senseNorm * senseNorm) * (0.18f + 0.22f * depth);

    const float intensity = 0.0445556f * (0.12f + depthCurve * 1.65f);
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
    float control = st.control * (0.45f + depthCurve * 7.50f + depth2 * 4.00f);
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

    const float wet = juce::jlimit (0.0f, 1.0f,
                                    depth * (0.35f + 0.85f * depth));
    const float supply = juce::jlimit (0.5f, 1.0f, clamp);
    r.sample = sample + (sagged - sample) * wet;
    r.amount = effectiveControl * wet;
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

// ══════════════════════════════════════════════════════════════
//  GIRTH — post-waveshaper fold + sharpen
// ══════════════════════════════════════════════════════════════
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

// ══════════════════════════════════════════════════════════════
//  Internal Emphasis / De-emphasis
// ══════════════════════════════════════════════════════════════
struct EmphCoeffs {
    float preHP = 0, preSh = 0, postLP = 0, postHP = 0;
};

inline float preEmphasize (float x, EmphasisState& st, Model model,
                           float drive, float mod, const EmphCoeffs& ec) noexcept
{
    model = canonicalizeModel (model);
    switch (model)
    {
        case Model::Triode:
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
        case Model::Cascade:
        {
            st.preHP += (x - st.preHP) * ec.preHP;
            float hp = x - st.preHP;
            st.preSh += (hp - st.preSh) * ec.preSh;
            const float edge = hp - st.preSh;
            return hp + edge * (0.01f + drive * mod * 0.06f);
        }
        case Model::PushPull:
            return x;
        case Model::Diode:
        {
            st.preHP += (x - st.preHP) * ec.preHP;
            return x - st.preHP;
        }
        case Model::Clipper:
        {
            st.preHP += (x - st.preHP) * ec.preHP;
            const float hp = x - st.preHP;
            st.preSh += (hp - st.preSh) * ec.preSh;
            const float edge = hp - st.preSh;
            const float voice = detail::clampF (mod, 0.0f, 1.0f);

            const float tsEdge = 0.050f + drive * 0.080f;
            const float ts = hp + edge * tsEdge;

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
            // MOD morphs Rabbit-style Tape B at 0% into the smoother Tape A at
            // 100%. Keep the path unified and only morph the voicing.
            st.preHP += (x - st.preHP) * ec.preHP;
            float hp = x - st.preHP;
            st.preSh += (hp - st.preSh) * ec.preSh;
            const float edge = hp - st.preSh;
            const float rabbitMod = 1.0f - mod;
            const float rabbit = rabbitMod * rabbitMod;
            const float gritMix = rabbitMod * (0.048f + drive * 0.145f + rabbit * (0.010f + drive * 0.040f));
            const float bodyLift = rabbit * (0.022f + drive * 0.055f);
            return hp * (1.0f + bodyLift) + edge * gritMix;
        }
        case Model::Fuzz:
        {
            // C1 = 2.2µF input cap HPF @ ~14Hz (real Fuzz Face)
            st.preHP += (x - st.preHP) * ec.preHP;
            return x - st.preHP;
        }
        case Model::Doom:
        {
            // BMP input cap: HPF ~55Hz (C1=0.1µF coupling into first stage)
            st.preHP += (x - st.preHP) * ec.preHP;
            return x - st.preHP;
        }
        case Model::Destroy:
        {
            st.preHP += (x - st.preHP) * ec.preHP;
            return x - st.preHP;
        }
        case Model::Tundra:
        {
            st.preHP += (x - st.preHP) * ec.preHP;
            float hp = x - st.preHP;
            st.preSh += (hp - st.preSh) * ec.preSh;
            float treble = hp - st.preSh;
            return hp + treble * 0.3f;
        }
        default: return x;
    }
}

inline float deEmphasize (float y, EmphasisState& st, Model model,
                          float drive, float mod, const EmphCoeffs& ec) noexcept
{
    model = canonicalizeModel (model);
    switch (model)
    {
        case Model::Triode:
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
        case Model::Cascade:
        {
            st.postLP += (y - st.postLP) * ec.postLP;
            const float lpMix = 0.05f + drive * (0.18f + mod * 0.10f);
            return y + (st.postLP - y) * lpMix;
        }
        case Model::PushPull:
        {
            st.postLP += (y - st.postLP) * ec.postLP;
            const float lpMix = 0.08f + drive * (0.22f + mod * 0.10f);
            return y + (st.postLP - y) * lpMix;
        }
        case Model::Diode:
        {
            st.postLP += (y - st.postLP) * ec.postLP;
            return st.postLP;
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
            // Rabbit needs to stay more open and more harmonically forward than
            // Tape A, especially as drive rises.
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
        case Model::Fuzz:
        {
            // C3 = 0.01µF output cap HPF @ ~31Hz (with RVOL=500kΩ)
            st.postHP += (y - st.postHP) * ec.postHP;
            float hp = y - st.postHP;
            // Gentle HF rolloff (speaker/cabinet interaction)
            st.postLP += (hp - st.postLP) * ec.postLP;
            return st.postLP;
        }
        case Model::Doom:
        {
            st.postHP += (y - st.postHP) * ec.postHP;
            float hp = y - st.postHP;
            st.postLP += (hp - st.postLP) * ec.postLP;
            return st.postLP;
        }
        case Model::Destroy:
        {
            st.postHP += (y - st.postHP) * ec.postHP;
            float hp = y - st.postHP;
            st.postLP += (hp - st.postLP) * ec.postLP;
            return st.postLP;
        }
        case Model::Tundra:
        {
            st.postHP += (y - st.postHP) * ec.postHP;
            float hp = y - st.postHP;
            st.postLP += (hp - st.postLP) * ec.postLP;
            return st.postLP;
        }
        default: return y;
    }
}

// ══════════════════════════════════════════════════════════════
//  Per-sample waveshaper functions (new models)
// ══════════════════════════════════════════════════════════════

// TUBE: 12AX7 -> EL34/6L6-inspired stage morph with Tube2-style core
inline float processTriode (float x, float drive, float girth, float bias, float mod,
                            float react, bool rawMode,
                            State& state, int ch, float sr,
                            adaa::StableTanhADAA& adaaState) noexcept
{
    const int sp = state.currentSeriesPass;
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float m = detail::clampF (mod,   0.0f, 1.0f);
    const float b = detail::clampF (bias, -1.0f, 1.0f);
    const float tubeMorph = detail::smoothStep01 (m);
    const float tubeMorph2 = tubeMorph * tubeMorph;
    juce::ignoreUnused (g);
    auto& triodeSag = state.triodeReact[sp][ch];
    float xStage = x;
    float bEff = b;

    if (!rawMode && react > 0.0001f)
    {
        const float sagSense = getTriodeSagSenseInput (xStage);
        const TriodeReactResult triodeComp = processTriodeReact (
            xStage, sagSense, triodeSag, react, sr);
        xStage = triodeComp.sample;
        state.sagEnvelope[sp][ch] = triodeComp.amount;
    }
    else
    {
        triodeSag.control *= 0.5f;
        triodeSag.lastSag *= 0.5f;
        triodeSag.lastSupply += (1.0f - triodeSag.lastSupply) * 0.25f;
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

    // Tube2-style stage inside the black box. MOD/GIRTH stay mostly outside
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
    // tightens headroom and shifts the operating point of the Tube2-style core.
    const float biasPos = std::max (0.0f, bEff);
    const float biasNeg = std::max (0.0f, -bEff);
    const float stageBias12AX7 = bEff * 0.050f - sagCore * 0.095f;
    const float stageBiasPower = bEff * 0.028f - sagCore * 0.072f;
    const float stageBias = juce::jmap (tubeMorph, stageBias12AX7, stageBiasPower);

    const float headroom12AX7 = juce::jlimit (0.54f, 1.0f,
                                              1.0f - sagCore * 0.40f
                                                    - biasPos * 0.05f + biasNeg * 0.02f);
    const float headroomPower = juce::jlimit (0.58f, 1.08f,
                                              1.04f - sagCore * 0.28f
                                                     - biasPos * 0.08f + biasNeg * 0.05f);
    const float stageHeadroom = juce::jmap (tubeMorph, headroom12AX7, headroomPower);
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
    const float asymAmt = juce::jmap (tubeMorph, asymAmt12AX7, asymAmtPower);
    s = detail::tube2AsymSection (s, asymPad, asymAmt);
    // Original Tube curve.
    s = detail::airwindowsTubeCurve (s, powerFactor);

    if (tubeMorph > 0.001f)
    {
        const float hotness = detail::clampF (0.55f + bEff * 0.35f, 0.0f, 1.0f);
        const float idleBias = 0.010f + hotness * (0.012f + d * 0.010f);
        const float crossover = 0.010f + (1.0f - hotness) * (0.012f + d * 0.008f);
        const float powerGain = 1.0f + d * (0.70f + tubeMorph * 0.55f);

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

    state.triodeBlock[sp][ch] = 0.0f;
    return s;
}

// POWER: Legacy hybrid 6L6/EL34 class-AB output stage
inline float processPushPull (float x, float drive, float bias, float mod,
                              State& state, int ch, float sr,
                              adaa::TanhADAA& adaaState) noexcept
{
    const int sp = state.currentSeriesPass;
    const float hotness = detail::clampF (0.5f - bias * 0.5f, 0.0f, 1.0f);
    const float envTarget = std::abs (x) * (0.18f + drive * 0.55f);
    const float sagCoeff = detail::onePoleCoeff (12.0f + mod * 18.0f, sr);
    state.powerSag[sp][ch] += (envTarget - state.powerSag[sp][ch]) * sagCoeff;
    const float sag = state.powerSag[sp][ch];

    float gain = 1.0f + drive * (3.8f + mod * 1.2f);
    gain *= 1.0f - sag * (0.08f + drive * 0.10f);

    const float idleBias = 0.022f + hotness * (0.050f + drive * 0.010f);
    const float crossover = 0.010f + (1.0f - hotness) * (0.012f + drive * 0.010f);
    const float satK = 1.0f + drive * (2.6f + mod * 0.9f);

    const float posV = x * gain + idleBias;
    const float negV = -x * gain + idleBias;
    const float posC = detail::smoothRect (posV, crossover);
    const float negC = detail::smoothRect (negV, crossover);

    float conduction = posC - negC;
    conduction += x * std::abs (x) * gain * (0.020f + mod * 0.035f);

    const float raw = adaaState.process (conduction, satK);
    const float derivRect = detail::smoothRectDeriv (idleBias, crossover);
    const float slope0 = satK * (2.0f * gain * derivRect);
    return detail::normalizeSmallSignal (raw, 0.0f, slope0);
}

// CASCADE: single-stage helper
inline float processCascadeSingle (float x, float drivePerStage, float bias,
                                   float dcAccum[][2], int ch,
                                   adaa::TanhADAA cascAdaa[][2],
                                   int numStages, float capBleed) noexcept
{
    float s = x;
    const float k = 1.0f + drivePerStage * 3.0f;

    for (int stage = 0; stage < numStages; ++stage)
    {
        s *= (1.0f + drivePerStage * 12.0f);
        s += bias * 0.1f + dcAccum[stage][ch];
        s = cascAdaa[stage][ch].process (s, k);
        dcAccum[stage][ch] = dcAccum[stage][ch] * capBleed + s * (1.0f - capBleed) * 0.1f;
        s *= 0.8f;
    }
    return s;
}

// CASCADE: N cascaded triode stages with fractional crossfade
inline float processCascade (float x, float drive, float bias, float mod,
                             State& state, int ch, float capBleed) noexcept
{
    // MOD maps to continuous stage range: MOD=0 → 4 stages, MOD=1 → 1 stage
    const float stagesF = 4.0f - mod * 3.0f;
    const int nLo = std::max (1, (int) stagesF);
    const int nHi = std::min (nLo + 1, kMaxCascade);
    const float frac = stagesF - (float) nLo;

    const float drivePerStage = drive / std::sqrt ((float) nHi);

    const int sp = state.currentSeriesPass;

    float outLo = processCascadeSingle (x, drivePerStage, bias,
                                        state.cascadeDC[sp], ch,
                                        state.cascadeAdaa[sp],
                                        nLo, capBleed);

    float outHi = outLo;
    if (nHi > nLo)
    {
        float sExtra = outLo * (1.0f + drivePerStage * 12.0f);
        sExtra += bias * 0.1f;
        const float k = 1.0f + drivePerStage * 3.0f;
        sExtra = state.cascadeAdaa[sp][nLo][ch].process (sExtra, k);
        sExtra *= 0.8f;
        outHi = sExtra;
    }

    return outLo * (1.0f - frac) + outHi * frac;
}

// DIODE: Shockley equation diode clipper — Ge/Si blend
inline float processDiode (float x, float drive, float bias, float mod,
                           adaa::TanhADAA& adaaState) noexcept
{
    const float gain = 1.0f + drive * 40.0f;
    float s = x * gain;

    // Bias blends Ge↔Si:  -1 = Germanium, +1 = Silicon
    const float geRatio = 0.5f - bias * 0.5f;
    const float siRatio = 1.0f - geRatio;
    const float Vf  = geRatio * 0.3f + siRatio * 0.6f;
    const float nFactor = geRatio * 1.3f + siRatio * 1.8f;

    // MOD: clipping topology (0 = feedback/soft, 1 = shunt/hard)
    const float hardness = 1.0f + mod * mod * 4.0f;

    const float k = hardness / (Vf * nFactor + 0.01f);
    float shaped = adaaState.process (s * Vf, k);

    const float peakEst = std::tanh (k * Vf);
    if (peakEst > 0.01f)
        shaped /= peakEst;

    return shaped;
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
                                                  1.00f, 0.86f, 0.68f, 0.46f, 0.24f);

    float voiceScale = 1.0f;
    float cleanBlend = 0.0f;
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
    }

    const float clipIn = x * (voiceScale / std::max (threshold, 0.05f));

    // BIAS becomes symmetry / mismatch: shifts positive and negative clip
    // thresholds independently, but keep their mean around unity.
    const float thresholdPos = detail::clampF (1.0f + b * 0.45f, 0.45f, 1.55f);
    const float thresholdNeg = detail::clampF (1.0f - b * 0.45f, 0.45f, 1.55f);
    const float kneeSoft = 0.01f + (1.0f - k) * 0.98f;
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

    return clipped * juce::jmap (d, 0.98f, 0.92f);
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

    if (!rawMode && rabbitMod > 0.0001f)
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

    if (rabbitMod > 0.0001f)
    {
        const float rabbit = rabbitMod * rabbitMod;
        const float rabbitMakeup = detail::interpDrive5 (d,
                                                         1.24f, 1.20f, 1.16f, 1.13f, 1.10f);
        out *= juce::jmap (rabbit, 1.0f, rabbitMakeup);
    }

    return out;
}

// FUZZ: Fuzz Face — 2-stage CE topology with global feedback
// Models the Arbiter Fuzz Face circuit:
//   Q1 (common emitter, biased off-center for asymmetric clip)
//   Q2 (common emitter with emitter degeneration via FUZZ pot)
//   R4 feedback from Q2 emitter → Q1 base (100kΩ)
//   Coupling caps between stages (~50Hz HPF) and output (~31Hz HPF)
//
// DRIVE = FUZZ pot: controls gain + inversely controls feedback amount
// BIAS  = Ge/Si character: -1 = PNP Germanium (sputtery), +1 = NPN Silicon (sustained)
// MOD   = TONE: post-distortion tilt EQ (0 = dark/warm, 1 = bright/cutting)
inline float processFuzz (float x, float drive, float bias, float mod,
                          State& state, int ch, float sr,
                          adaa::TanhADAA2& adaaQ1,
                          adaa::TanhADAA2& adaaQ2) noexcept
{
    const int sp = state.currentSeriesPass;

    // ── Fuzz Face feedback loop (R4 = 100kΩ path) ──
    // Real circuit: R4 (100kΩ) provides ~20dB attenuation from Q2 emitter to Q1 base.
    // High FUZZ pot → C2 shorts feedback to ground → less feedback → more gain.
    // Low FUZZ → full feedback → reduced gain, cleaner, more compressed.
    // Feedback is from PRE-TONE Q2 output, heavily attenuated (real R4/Rin ratio ≈ 0.1).
    const float fbAmount = (1.0f - drive) * 0.15f;  // attenuated like real R4 divider
    x += state.fuzzFeedback[sp][ch] * fbAmount;

    // ── Bias character: Ge vs Si ──
    // Ge: lower β, more leakage, sputtery gating at low levels
    // Si: higher β, cleaner sustain, tighter
    const float geChar = 0.5f - bias * 0.5f;  // 0 = Si, 1 = Ge

    // ── Q1: Input stage (common emitter, biased off-center) ──
    // Real FF: Q1 VC ≈ -1.6V (vs ideal -4.5V) → asymmetric clipping
    // Gain from R1=33kΩ collector load, reduced by feedback
    // Open loop ~49dB, closed loop ~18.6dB — we model the net effect
    const float q1Gain = 2.0f + drive * 8.0f;   // ~6dB to ~20dB
    float s = x * q1Gain;

    // Q1 asymmetric soft clip (bias off-center: clips negative first)
    // k_pos > k_neg models the VC=-1.6V bias point
    const float k1_pos = 1.5f + drive * 3.0f;   // positive headroom (more)
    const float k1_neg = 2.5f + drive * 5.0f;   // negative clips sooner
    // Split signal for asymmetric ADAA clipping
    {
        // Asymmetric tanh approximation: different k for +/- half-cycles
        // Process through ADAA with average k, then apply asymmetry correction
        const float k1_avg = 0.5f * (k1_pos + k1_neg);
        s = adaaQ1.process (s, k1_avg);
        // Add even-harmonic asymmetry (models off-center bias)
        const float asym = 0.15f + drive * 0.1f;  // subtle asymmetry
        s += asym * s * s;  // 2nd harmonic from asymmetry
        s = detail::clampF (s, -1.5f, 1.5f);  // safety
    }

    // ── Coupling cap HPF between Q1 → Q2 (~50Hz) ──
    // C1 = 2.2µF, interstage coupling removes DC + bass buildup
    {
        const float c = state.blockCoeffs.fuzzCoupC;
        state.fuzzCoupDC[sp][ch] += (s - state.fuzzCoupDC[sp][ch]) * c;
        s -= state.fuzzCoupDC[sp][ch];
    }

    // ── Q2: Output stage (CE with emitter degeneration) ──
    // FUZZ pot controls emitter degeneration → gain range
    // With degeneration (low FUZZ): gain ≈ RC/RE = 8.2 (~18dB)
    // Without (high FUZZ): gain → β (transistor limit) → hard clip
    const float q2Gain = 1.0f + drive * 4.0f;
    s *= q2Gain;

    // Q2 clipping: transitions from soft (low FUZZ) to hard (max FUZZ)
    // ADAA-2 for clean anti-aliased clipping
    const float k2 = 2.0f + drive * 6.0f;
    s = adaaQ2.process (s, k2);

    // ── Germanium gating (voltage starving at low levels) ──
    // Ge transistors have leakage + thermal instability → gated/velcro at low signal
    if (geChar > 0.1f)
    {
        const float gate = geChar * 0.04f;
        const float absS = std::abs (s);
        if (absS < gate)
            s *= absS / (gate + 1.0e-6f);
    }

    // ── Store feedback BEFORE tone (Q2 emitter → R4 → Q1 base) ──
    // Real FF: R4 (100kΩ) takes signal from Q2 emitter, NOT post-tone.
    // Heavy attenuation: R4/Rin ratio + voltage divider ≈ 0.15.
    // Clamped to ±0.5 to prevent self-oscillation (real circuit can't exceed ~0.6Vpp at this node).
    state.fuzzFeedback[sp][ch] = detail::clampF (s * 0.15f, -0.5f, 0.5f);

    // ── Post-distortion TONE (MOD parameter) ──
    // mod=0: dark/warm (LP dominated, ~800Hz cutoff) — Hendrix neck pickup character
    // mod=0.5: neutral (balanced)
    // mod=1: bright/cutting (HP dominated, treble boost)
    {
        const float toneCutoff = 500.0f + (1.0f - mod) * 3500.0f;  // 500Hz–4kHz
        const float tc = detail::onePoleCoeff (toneCutoff, sr);
        state.fuzzToneLPF[sp][ch] += (s - state.fuzzToneLPF[sp][ch]) * tc;
        // Tilt: crossfade between LP output and full signal
        const float tilt = mod * mod;  // quadratic: stays warm longer, then opens up
        s = state.fuzzToneLPF[sp][ch] * (1.0f - tilt) + s * tilt;
    }

    return s;
}

// DOOM: Big Muff Pi-style 2-stage diode-in-feedback clipper
// Real BMP topology: booster → Miller LPF → diode-feedback clip (×2) → tone stack
// DRIVE = sustain (pre-clip level, like BMP SUSTAIN pot)
// MOD   = tone scoop position (0=bass, 0.5=mid scoop @1kHz, 1=treble)
// BIAS  = voltage starve (-1=gated/velcro, 0=normal, +1=wall/sustain boost)
inline float processDoom (float x, float drive, float bias, float mod,
                          State& state, int ch, float sr,
                          adaa::TanhADAA2& adaaS1,
                          adaa::TanhADAA2& adaaS2) noexcept
{
    const int sp = state.currentSeriesPass;

    // Bias: -1 = starved/gated, 0 = normal 9V, +1 = boosted headroom
    // starve: 0 = clean supply (bias=+1), 1 = fully starved (bias=-1)
    const float starve = 0.5f - bias * 0.5f;
    const float headroom = 1.0f - starve * 0.55f;

    // Sustain (pre-clip level): drive controls how hard we push into clippers
    // BMP SUSTAIN pot is level into clip stages, not clipper gain
    const float sustain = 1.0f + drive * 40.0f;

    // ── Input booster (BMP Q1 CE stage) ──
    float s = x * sustain;

    // ── Stage 1: Miller cap LPF → diode-in-feedback clip (BMP Q2+D1D2) ──
    // Miller cap 470pF pre-filters before clipping (~1.2kHz)
    // This is THE BMP character: narrow BW entering the clipper
    {
        const float millerF = 1200.0f + (1.0f - drive) * 600.0f; // 1.2-1.8kHz
        const float cM = detail::onePoleCoeff (millerF, sr);
        state.doomDC[sp][2][ch] += (s - state.doomDC[sp][2][ch]) * cM;
        s = state.doomDC[sp][2][ch];
    }
    // Diode-in-feedback clip: softer than series clip, compresses at ±0.6V
    // BMP uses Si diodes (1N914) in collector-base feedback → k ≈ 2-3
    {
        const float k1 = 2.0f + drive * 2.5f;
        s = adaaS1.process (s, k1);
    }
    // Coupling cap HPF ~55Hz (C5=100nF, R=~30kΩ in BMP)
    {
        const float c = state.blockCoeffs.doomCoupC1;
        state.doomDC[sp][0][ch] += (s - state.doomDC[sp][0][ch]) * c;
        s -= state.doomDC[sp][0][ch];
    }
    s *= headroom;

    // ── Stage 2: Miller cap LPF → diode-in-feedback clip (BMP Q3+D3D4) ──
    // Second identical clipping stage — cascading creates the massive sustain
    {
        const float millerF2 = 1200.0f + (1.0f - drive) * 400.0f; // 1.2-1.6kHz
        const float cM2 = detail::onePoleCoeff (millerF2, sr);
        state.doomDC[sp][3][ch] += (s - state.doomDC[sp][3][ch]) * cM2;
        s = state.doomDC[sp][3][ch];
    }
    {
        const float k2 = 2.0f + drive * 3.0f;
        s = adaaS2.process (s, k2);
    }
    // Coupling cap HPF ~94Hz (C8=100nF, R=~17kΩ)
    {
        const float c = state.blockCoeffs.doomCoupC2;
        state.doomDC[sp][1][ch] += (s - state.doomDC[sp][1][ch]) * c;
        s -= state.doomDC[sp][1][ch];
    }
    s *= headroom;

    // ── Passive tone stack (BMP mid-scoop EQ) ──
    // MOD controls tone position: 0=bass, 0.5=mid scoop @1kHz, 1=treble
    // Real BMP: R=33k/C=4nF (LP ~1.2kHz) + R=22k/C=10nF (HP ~723Hz)
    // The tone pot crossfades between LP and HP paths → mid scoop at center
    {
        const float cLP = state.blockCoeffs.doomToneLP;
        const float cHP = state.blockCoeffs.doomToneHP;
        // LP path (bass)
        state.doomDC[sp][4][ch] += (s - state.doomDC[sp][4][ch]) * cLP;
        float lpPath = state.doomDC[sp][4][ch];
        // HP path (treble)
        state.doomDC[sp][5][ch] += (s - state.doomDC[sp][5][ch]) * cHP;
        float hpPath = s - state.doomDC[sp][5][ch];
        // Crossfade: mod=0 → full LP (bass), mod=1 → full HP (treble)
        // mod=0.5 → equal mix → classic mid-scoop
        s = lpPath * (1.0f - mod) + hpPath * mod;
    }

    // ── Low-level gating when starved (voltage sag → velcro/stutter) ──
    if (starve > 0.15f)
    {
        const float gate = starve * 0.05f;
        const float absS = std::abs (s);
        if (absS < gate)
            s *= absS / (gate + 1.0e-6f);
    }

    // ── Store for diagnostic feedback tracking (BMP has no real feedback loop) ──
    state.doomFeedback[sp][ch] = detail::clampF (s, -1.0f, 1.0f);

    return s;
}

// DESTROY: Plasma Discharge — inspired by Gamechanger Plasma Pedal
// Core: high-gain transformer boost → xenon discharge threshold gate → ultra-hard
// ADAA-2 clip → full-wave rectifier → REACT-driven ring modulation
// The gas discharge tube clips to near-square wave, rectifier adds octave-up.
// Ring mod uses pitch-tracked sub-octave carrier with REACT-controlled depth.
//
// DRIVE = discharge intensity (gain into clip stages)
// BIAS  = discharge threshold: -1=high threshold (sputtery/gated), +1=wide open
// MOD   = ring mod depth: 0=pure plasma, 1=full REACT-modulated ring mod
inline float processDestroy (float x, float drive, float bias, float mod,
                             State& state, int ch, float sr,
                             adaa::TanhADAA2& adaaState,
                             bool advanceOsc = true) noexcept
{
    const int sp = state.currentSeriesPass;

    // ── Feedback DISABLED ──
    // Plasma Pedal has no signal feedback path — the xenon discharge tube is
    // a one-way device. Previous feedback caused self-oscillation at silence
    // (dsFB hitting ±0.5 clamp, 57+ clicks/block with pkI=0.0004).
    // Ring mod carrier was also disabled (uses YIN pitch tracker).
    (void) state.destroyFeedback[sp][ch];  // keep state for diagnostics

    // ── Transformer boost (step-up for plasma discharge) ──
    // Real Plasma: signal boosted to >3kV via resonant transformer
    // Higher gain than Fuzz/Doom — plasma sustains at extreme levels
    const float gain = 1.0f + drive * 50.0f;
    float s = x * gain;

    // ── Transformer bandwidth limit (~4kHz pre-clip) ──
    // The step-up transformer has limited BW — no HF above ~4-6kHz
    // This shapes the harmonic content entering the discharge tube
    {
        const float xfmrFreq = 3500.0f + (1.0f - drive) * 2500.0f; // 3.5-6kHz
        const float cX = detail::onePoleCoeff (xfmrFreq, sr);
        state.destroyXfmrLP[sp][ch] += (s - state.destroyXfmrLP[sp][ch]) * cX;
        s = state.destroyXfmrLP[sp][ch];
    }

    // ── Discharge threshold gate (xenon tube ionization model) ──
    // Gas doesn't conduct until voltage exceeds ionization threshold.
    // Below threshold: signal is heavily attenuated (tube not firing).
    // Above threshold: gas ionizes → hard conduction → near-square output.
    // BIAS controls threshold: -1 = very high (sputtery), +1 = very low (open)
    {
        // threshold: bias=-1 → 0.15 (very gated), bias=0 → 0.05, bias=+1 → 0.0 (open)
        const float threshold = detail::clampF ((1.0f - bias) * 0.075f, 0.0f, 0.2f);
        if (threshold > 0.001f)
        {
            const float absS = std::abs (s);
            if (absS < threshold)
            {
                // Smooth cubic gate (softer than Fuzz/Doom linear gate)
                const float ratio = absS / (threshold + 1.0e-6f);
                s *= ratio * ratio;  // cubic gating: natural sputtery decay
            }
        }
    }

    // ── ADAA-2 ultra-hard clip (plasma discharge → near-square wave) ──
    // Plasma clips much harder than Fuzz Face (k ≈ 2-6) or BMP (k ≈ 2-5).
    // At full drive, this approaches a sign() function — near-perfect square.
    {
        const float k = 3.0f + drive * 14.0f;  // k up to 17: extremely steep
        s = adaaState.process (s, k);
    }

    // ── Full-wave rectifier blend (analog rectifier → octave-up artifacts) ──
    // The Plasma's specialized rectifier converts HV discharge back to audio.
    // Full-wave rectification folds negative half-cycles → octave-up effect.
    // Drive increases rectifier blend (more aggressive at higher settings).
    {
        const float rectified = std::abs (s);
        const float rectMix = 0.2f + drive * 0.35f;  // 20-55% rectifier
        s = s * (1.0f - rectMix) + rectified * rectMix;
    }

    // ── Post-rectifier coupling cap HPF (~70Hz) ──
    // Removes DC offset from rectification + transformer coupling
    {
        const float c = state.blockCoeffs.destroyRectC;
        state.destroyRectHP[sp][ch] += (s - state.destroyRectHP[sp][ch]) * c;
        s -= state.destroyRectHP[sp][ch];
    }

    // ── Ring modulation — DISABLED for pure distortion testing ──
    // (was: REACT-driven ring mod with pitch-tracked sub-octave carrier)
    // To re-enable: restore ring mod block and set needsYin = true for Destroy

    // ── Diagnostic feedback state (no actual feedback loop) ──
    state.destroyFeedback[sp][ch] = detail::clampF (s * 0.1f, -0.5f, 0.5f);

    return s;
}

// TUNDRA: Metal Zone → Modern high‐gain morph (dual‐stage cascaded clipping)
// Stage 1: op‐amp gain + ADAA tanh soft clip (MT-2 first diode stage)
// Stage 2: exponential→algebraic hard clip morph (MT-2→modern tight)
// MOD = Metal Zone classic (0) ↔ Modern tight (1)
// BIAS = clipping asymmetry (even‐harmonic injection)
inline float processTundra (float x, float drive, float bias, float mod,
                            State& state, int ch, float sr,
                            adaa::TanhADAA& adaaState) noexcept
{
    const int sp = state.currentSeriesPass;

    // ── Input tightness filter: MOD‐dependent HPF (50Hz@MZ → 120Hz@Modern) ──
    const float tightFreq = 50.0f + mod * 70.0f;
    const float tightC = detail::onePoleCoeff (tightFreq, sr);
    state.tundraTightHP[sp][ch] += (x - state.tundraTightHP[sp][ch]) * tightC;
    x -= state.tundraTightHP[sp][ch];

    // ── Stage 1: Op‐amp gain + soft clip (ADAA tanh) ──
    const float gain1 = 1.0f + drive * (20.0f + mod * 8.0f);
    float s = x * gain1;
    const float k1 = 1.5f + drive * 4.0f + mod * 2.0f;
    s = adaaState.process (s, k1);

    // Asymmetry: even‐harmonic injection via bias
    s += bias * 0.15f * s * std::abs (s);

    // ── Inter‐stage coupling cap HPF ~80Hz ──
    const float coupC = state.blockCoeffs.tundraCoupC;
    state.tundraInterHP[sp][ch] += (s - state.tundraInterHP[sp][ch]) * coupC;
    s -= state.tundraInterHP[sp][ch];

    // ── Stage 2: Hard clip — exponential (MT-2) ↔ algebraic (modern) morph ──
    const float gain2 = 1.0f + drive * (12.0f + mod * 5.0f);
    s *= gain2;
    const float k2 = 1.5f + drive * 6.0f;

    // Exponential clip (MT-2 diode character)
    {
        const float a = s * k2;
        const float sgn = a >= 0.0f ? 1.0f : -1.0f;
        const float expClip = sgn * (1.0f - std::exp (-std::abs (a)));

        // Algebraic clip (modern tight character)
        const float algClip = s / (1.0f + std::abs (s) * k2);

        // Morph between the two clipping styles
        s = expClip * (1.0f - mod) + algClip * mod;
    }

    // ── Presence spike: 2.5kHz resonant bump (MT-2 gyrator emulation) ──
    // Decreases with MOD for cleaner modern tone
    const float presAmount = (1.0f - mod) * 0.3f;
    if (presAmount > 0.01f)
    {
        const float presFreq = 2500.0f;
        const float presQ = 1.5f;
        const float w0 = kTwoPi * presFreq / sr;
        const float alpha = std::sin (w0) / (2.0f * presQ);
        const float cosW = std::cos (w0);
        const float a0 = 1.0f + alpha;
        const float b0 =  alpha / a0;
        const float b2 = -alpha / a0;
        const float a1n = -2.0f * cosW / a0;
        const float a2n = (1.0f - alpha) / a0;

        const float pres = b0 * s + b2 * state.tundraPresZ2[sp][ch]
                         - a1n * state.tundraPresZ1[sp][ch] - a2n * state.tundraPresZ2[sp][ch];
        state.tundraPresZ2[sp][ch] = state.tundraPresZ1[sp][ch];
        state.tundraPresZ1[sp][ch] = pres;

        s += pres * presAmount;
    }

    return s;
}

// ══════════════════════════════════════════════════════════════
//  Per-model drive curve — normalizes perceived distortion range
//  so that drive=50% feels similar across all models.
//  Higher exponent = more gradual ramp (for cascaded/high-gain models).
// ══════════════════════════════════════════════════════════════
inline float applyDriveCurve (float driveParam, Model model) noexcept
{
    model = canonicalizeModel (model);
    // Per-model exponent: gentle models use lower exponent, aggressive
    // cascaded models use higher exponent to spread usable range.
    float exp;
    switch (model)
    {
        case Model::Triode:     exp = 1.85f; break; // TUBE should wake up slightly earlier than legacy power/cascade
        case Model::PushPull:   exp = 2.0f; break;  // power stage should stay cleaner early on
        case Model::Cascade:    exp = 2.0f; break;  // multi-stage, gain compounds
        case Model::Diode:      exp = 1.8f; break;  // single stage, high gain (×40)
        case Model::Clipper:    exp = 1.0f; break;  // threshold should track the UI directly
        case Model::Tape:       exp = 1.0f; break;  // Tape fitting is done directly in UI-drive space
        case Model::Fuzz:       exp = 2.5f; break;  // feedback controls gain range naturally
        case Model::Doom:       exp = 3.0f; break;  // 2 cascaded BMP clipping stages
        case Model::Destroy:    exp = 2.5f; break;  // plasma: fast attack, wide sustain range
        case Model::Tundra:     exp = 3.0f; break;  // 2 stages, high combined gain
        default:                exp = 1.5f; break;
    }
    return std::pow (driveParam, exp);
}

// ══════════════════════════════════════════════════════════════
//  Auto-Gain Compensation
// ══════════════════════════════════════════════════════════════
// Estimates the peak output of each model for a unity-amplitude input signal
// and returns 1/peakOut so that output level stays roughly constant across
// the drive range.  The estimates MUST match the actual gain×k products
// inside each processFoo() function.
inline float getAutoGain (Model model, float drive) noexcept
{
    model = canonicalizeModel (model);
    float peakOut = 1.0f;
    switch (model)
    {
        case Model::Triode:
        {
            peakOut = 1.0f;
            break;
        }
        case Model::Clipper:
        {
            peakOut = 1.0f;
            break;
        }
        case Model::PushPull:
        {
            const float peak = 1.0f + drive * 0.24f;
            peakOut = 1.0f + (peak - 1.0f) * detail::clampF (drive * 2.5f, 0.0f, 1.0f);
            break;
        }
        case Model::Cascade:
        {
            // processCascade: per-stage gain = 1 + dps*12, k = 1 + dps*3
            // Each stage independently clips to tanh*0.8 ≈ 0.8.
            // The cascade does NOT multiply outputs — each stage re-clips.
            // Peak output ≈ single stage output = tanh(k*gain)*0.8.
            const float dps = drive * 0.5f;
            const float stageGain = 1.0f + dps * 12.0f;
            const float k = 1.0f + dps * 3.0f;
            peakOut = std::tanh (k * std::min (stageGain, 8.0f)) * 0.8f;
            peakOut = 1.0f + (peakOut - 1.0f) * detail::clampF (drive * 5.0f, 0.0f, 1.0f);
            break;
        }
        case Model::Diode:
            peakOut = 0.5f + drive * 0.3f;
            break;
        case Model::Tape:
        {
            // Keep tape level largely user-driven. A strong auto-gain here
            // makes low-drive passages feel unnaturally hyped and can invert
            // the expected "more drive = at least similar loudness" behavior.
            peakOut = 1.0f;
            break;
        }
        case Model::Fuzz:
        {
            // processFuzz: Q1 gain = 2+drive*8, k1_avg ≈ 2+drive*4
            //              Q2 gain = 1+drive*4, k2 = 2+drive*6
            // Two cascaded tanh stages, each near ±1 at moderate drive.
            // The feedback REDUCES effective output at low drive (negative feedback).
            const float k2 = 2.0f + drive * 6.0f;
            const float q2out = std::tanh (k2);
            // At low drive, negative feedback compresses output below unity
            const float fbReduction = 1.0f - (1.0f - drive) * 0.10f;
            peakOut = q2out * fbReduction;
            break;
        }
        case Model::Doom:
        {
            // processDoom: sustain = 1+drive*40, two ADAA stages
            // Miller LPFs attenuate before each clip stage.
            // At moderate drive, Miller cuts ~50% → effective input to clip is reduced.
            const float s1out = std::tanh (2.0f + drive * 2.5f);
            const float s2out = std::tanh (2.0f + drive * 3.0f);
            peakOut = s1out * s2out;
            peakOut = 1.0f + (peakOut - 1.0f) * detail::clampF (drive * 5.0f, 0.0f, 1.0f);
            break;
        }
        case Model::Destroy:
        {
            // processDestroy: gain = 1+drive*50, k = 3+drive*14
            // Ultra-hard clip → near ±1, plus partial rectification.
            const float k = 3.0f + drive * 14.0f;
            const float clipPeak = std::tanh (k);
            const float rectMix = 0.2f + drive * 0.35f;
            peakOut = clipPeak * (1.0f + rectMix * 0.3f);
            peakOut = 1.0f + (peakOut - 1.0f) * detail::clampF (drive * 8.0f, 0.0f, 1.0f);
            break;
        }
        case Model::Tundra:
        {
            // processTundra: stage1 gain = 1+drive*(20+mod*8), stage2 gain = 1+drive*(12+mod*5)
            // k1 = 1.5+drive*4+mod²*2, k2 = 1.5+drive*6
            // Two cascaded clip stages (exp + algebraic morph).
            const float k1 = 1.5f + drive * 4.0f;
            const float k2 = 1.5f + drive * 6.0f;
            peakOut = std::tanh (k1) * std::tanh (k2);
            peakOut = 1.0f + (peakOut - 1.0f) * detail::clampF (drive * 10.0f, 0.0f, 1.0f);
            break;
        }
        default: break;
    }
    return (peakOut > 0.01f) ? 1.0f / peakOut : 1.0f;
}

// ══════════════════════════════════════════════════════════════
//  Safety LPF processing (Butterworth 2nd-order at 0.4×fs)
//  Coefficients precomputed per-block; struct just holds state.
// ══════════════════════════════════════════════════════════════
struct SafetyLPFCoeffs { float b0=0, b1=0, b2=0, a1=0, a2=0; };

inline float processSafetyLPF (SafetyLPF& st, float x, const SafetyLPFCoeffs& c) noexcept
{
    const float y = c.b0 * x + c.b1 * st.x1 + c.b2 * st.x2
                  - c.a1 * st.y1 - c.a2 * st.y2;

    st.x2 = st.x1; st.x1 = x;
    st.y2 = st.y1; st.y1 = y;
    return y;
}

// ══════════════════════════════════════════════════════════════
//  Main block processor
// ══════════════════════════════════════════════════════════════
inline void processBlock (State& state,
                          float* left, float* right,
                          int numSamples,
                          Model model,
                          float driveParam,     // 0..1
                          float girthParam,     // 0..1
                          float modParam,       // 0..1
                          float biasParam,      // -1..1
                          float reactParam,     // 0..1
                          float varParam,       // 0..1
                          float sampleRate,
                          int   seriesCount = 1,
                          bool  isSafetyLpfOn = false,
                          bool  skipAutoGain = false,
                          bool  rawMode = false,
                          SatDiag::Collector* diagCollector = nullptr) noexcept
{
    model = canonicalizeModel (model);

    // CLEAN model: 1:1 pass-through — no saturation processing at all
    if (model == Model::Clean)
        return;

    // ── Model-switch detection: reset filters & feedback to prevent transient explosions ──
    if (model != state.lastModel)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            for (int sp = 0; sp < kMaxSeries; ++sp)
            {
                state.react[sp][ch].reset();
                state.mbReact[sp][ch].reset();
                state.sagEnvelope[sp][ch] = 0.0f;
                state.tapeComp[sp][ch].reset();
                state.triodeReact[sp][ch].reset();
                state.emphasis[sp][ch].reset();
                state.dcX[sp][ch] = state.dcY[sp][ch] = 0.0f;
                state.wsAdaa[sp][ch].reset();
                state.triodeAdaa[sp][ch].reset();
                state.tapeAdaa[sp][ch].reset();
                state.clipperAdaa[sp][ch].reset();
                state.wsAdaa2[sp][ch].reset();
                state.fuzzAdaa2Q2[sp][ch].reset();
                state.doomAdaa2S2[sp][ch].reset();
                state.girthAdaa[sp][ch].reset();
                state.triodeBlock[sp][ch] = 0.0f;
                state.powerSag[sp][ch] = 0.0f;
                state.tapeFlux[sp][ch] = 0.0f;
                state.fuzzFeedback[sp][ch] = 0.0f;
                state.fuzzCoupDC[sp][ch] = 0.0f;
                state.fuzzToneLPF[sp][ch] = 0.0f;
                state.doomFeedback[sp][ch] = 0.0f;
                state.destroyFeedback[sp][ch] = 0.0f;
                state.destroyXfmrLP[sp][ch] = 0.0f;
                state.destroyRectHP[sp][ch] = 0.0f;
                state.interStageLPF[sp][ch] = 0.0f;
                state.interStageDCx[sp][ch] = 0.0f;
                state.interStageDCy[sp][ch] = 0.0f;
                state.tundraInterHP[sp][ch] = 0.0f;
                state.tundraTightHP[sp][ch] = 0.0f;
                state.tundraPresZ1[sp][ch] = 0.0f;
                state.tundraPresZ2[sp][ch] = 0.0f;
                state.bumpZ1[sp][ch] = state.bumpZ2[sp][ch] = 0.0f;
                for (int s = 0; s < kMaxCascade; ++s)
                    state.doomDC[sp][s][ch] = 0.0f;
            }
        }
        state.lastModel = model;
    }

    // Sample-rate-aware parameter smoothing (~15ms time constant at any SR).
    // Using onePoleCoeff(11Hz) → 63% in ~15ms, 95% in ~43ms. Consistent
    // whether running at 44.1kHz native or 176.4kHz (4× oversampled).
    const float oneMinusSmooth = detail::onePoleCoeff (11.0f, sampleRate);

    // DC blocker coefficient
    const float dcR = 1.0f - (kTwoPi * 5.0f / sampleRate);

    // REACT window size (model-dependent base, scaled by react amount)
    int reactBaseWindow = 1024;
    switch (model)
    {
        case Model::Triode:   reactBaseWindow = 1024; break;
        case Model::PushPull: reactBaseWindow = 4096; break;
        case Model::Cascade:  reactBaseWindow = 512;  break;
        case Model::Diode:    reactBaseWindow = 4096; break;
        case Model::Clipper:  reactBaseWindow = 2048; break;
        case Model::Tape:     reactBaseWindow = 2048; break;
        case Model::Fuzz:     reactBaseWindow = 4096; break;
        case Model::Doom:     reactBaseWindow = 4096; break;
        case Model::Destroy:  reactBaseWindow = 2048; break;
        case Model::Tundra:   reactBaseWindow = 2048; break;
        default: break;
    }

    // Precomputed emphasis/de-emphasis coefficients (hoisted from per-sample)
    EmphCoeffs emphCoeffs;
    switch (model)
    {
        case Model::Triode:
            emphCoeffs.preHP  = detail::onePoleCoeff (20.0f,   sampleRate);
            emphCoeffs.preSh  = detail::onePoleCoeff (3800.0f, sampleRate);
            emphCoeffs.postLP = detail::onePoleCoeff (9500.0f, sampleRate);
            emphCoeffs.postHP = detail::onePoleCoeff (30.0f,   sampleRate);
            break;
        case Model::Cascade:
            emphCoeffs.preHP  = detail::onePoleCoeff (22.0f,   sampleRate);
            emphCoeffs.preSh  = detail::onePoleCoeff (1600.0f, sampleRate);
            emphCoeffs.postLP = detail::onePoleCoeff (6500.0f, sampleRate);
            emphCoeffs.postHP = detail::onePoleCoeff (40.0f,   sampleRate);
            break;
        case Model::PushPull:
            emphCoeffs.postLP = detail::onePoleCoeff (5500.0f, sampleRate);
            break;
        case Model::Diode:
            emphCoeffs.preHP  = detail::onePoleCoeff (720.0f,  sampleRate);
            emphCoeffs.postLP = detail::onePoleCoeff (723.0f,  sampleRate);
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
        case Model::Fuzz:
            emphCoeffs.preHP  = detail::onePoleCoeff (14.0f,   sampleRate);  // C1 HPF
            emphCoeffs.postHP = detail::onePoleCoeff (31.0f,   sampleRate);  // C3 HPF
            emphCoeffs.postLP = detail::onePoleCoeff (6000.0f, sampleRate);  // gentle HF rolloff
            break;
        case Model::Doom:
            emphCoeffs.preHP  = detail::onePoleCoeff (55.0f,   sampleRate);  // BMP input HPF ~55Hz
            emphCoeffs.postHP = detail::onePoleCoeff (55.0f,   sampleRate);  // DC block post
            emphCoeffs.postLP = detail::onePoleCoeff (2000.0f, sampleRate);  // narrow BMP BW
            break;
        case Model::Destroy:
            emphCoeffs.preHP  = detail::onePoleCoeff (30.0f,   sampleRate);   // transformer coupling
            emphCoeffs.postHP = detail::onePoleCoeff (30.0f,   sampleRate);   // output coupling
            emphCoeffs.postLP = detail::onePoleCoeff (7000.0f, sampleRate);   // transformer BW post
            break;
        case Model::Tundra:
            emphCoeffs.preHP  = detail::onePoleCoeff (80.0f,   sampleRate);
            emphCoeffs.preSh  = detail::onePoleCoeff (2000.0f, sampleRate);
            emphCoeffs.postHP = detail::onePoleCoeff (35.0f,   sampleRate);
            emphCoeffs.postLP = detail::onePoleCoeff (6000.0f, sampleRate);
            break;
        default: break;
    }

    // Precomputed REACT envelope coefficients (hoisted from per-sample)
    const float reactAttCoeff  = 1.0f - std::exp (-kTwoPi * 1000.0f / sampleRate);
    const float reactRelCoeff  = 1.0f - std::exp (-kTwoPi * 2.0f / sampleRate);

    // Multiband REACT crossover coefficients (~200Hz sub/mid, ~4kHz mid/air)
    const float mbSubCoeff = detail::onePoleCoeff (200.0f, sampleRate);
    const float mbAirCoeff = detail::onePoleCoeff (4000.0f, sampleRate);

    // Precomputed Cascade coupling-cap bleed coefficient
    const float cascadeCapBleed = 1.0f - detail::onePoleCoeff (70.0f, sampleRate);

    // Precomputed safety LPF coefficients (constant since fc = 0.4×sr)
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

    // ── Per-block hoisted computations (avoid per-sample transcendentals) ──
    // Drive curve: std::pow only once per block (driveParam is constant within a block)
    const float driveCurved = applyDriveCurve (driveParam, model);

    if (model == Model::Tape)
    {
        const bool tapeActive = driveCurved > 0.001f || modParam < 0.999f;
        const bool driveJump = std::abs (driveCurved - state.lastTapeDrive) > 0.15f;
        if (driveJump || tapeActive != state.tapeWasActive)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                for (int sp = 0; sp < kMaxSeries; ++sp)
                {
                    state.react[sp][ch].reset();
                    state.mbReact[sp][ch].reset();
                    state.sagEnvelope[sp][ch] = 0.0f;
                    state.emphasis[sp][ch].reset();
                    state.tapeComp[sp][ch].reset();
                    state.triodeReact[sp][ch].reset();
                    state.dcX[sp][ch] = state.dcY[sp][ch] = 0.0f;
                    state.triodeAdaa[sp][ch].reset();
                    state.tapeAdaa[sp][ch].reset();
                    state.clipperAdaa[sp][ch].reset();
                    state.interStageDCx[sp][ch] = 0.0f;
                    state.interStageDCy[sp][ch] = 0.0f;
                    state.interStageLPF[sp][ch] = 0.0f;
                    state.bumpZ1[sp][ch] = 0.0f;
                    state.bumpZ2[sp][ch] = 0.0f;
                    state.tapeFlux[sp][ch] = 0.0f;
                }
            }
        }
        state.lastTapeDrive = driveCurved;
        state.tapeWasActive = tapeActive;
    }

    // Auto-gain: precompute with target drive (std::tanh per-block, not per-sample)
    const float preAutoGain = skipAutoGain ? 1.0f : getAutoGain (model, driveCurved);

    // Constant filter coefficients inside waveshaper functions
    state.blockCoeffs.fuzzCoupC    = detail::onePoleCoeff (50.0f,   sampleRate);
    state.blockCoeffs.doomCoupC1   = detail::onePoleCoeff (55.0f,   sampleRate);
    state.blockCoeffs.doomCoupC2   = detail::onePoleCoeff (94.0f,   sampleRate);
    state.blockCoeffs.doomToneLP   = detail::onePoleCoeff (1200.0f, sampleRate);
    state.blockCoeffs.doomToneHP   = detail::onePoleCoeff (723.0f,  sampleRate);
    state.blockCoeffs.destroyRectC = detail::onePoleCoeff (70.0f,   sampleRate);
    state.blockCoeffs.tundraCoupC  = detail::onePoleCoeff (80.0f,   sampleRate);
    state.blockCoeffs.autoGain     = preAutoGain;

    // YIN smoothing coefficients (constant per block)
    const float yinHopRate = sampleRate / 96.0f;
    state.blockCoeffs.yinFastC = detail::onePoleCoeff (60.0f, yinHopRate);
    state.blockCoeffs.yinSlowC = detail::onePoleCoeff (6.0f,  yinHopRate);

    // YIN only needed for models that use sub-osc or ring mod, AND only with react>0
    // (sub-osc and ring mod are currently disabled for testing)
    const bool needsYin = false;  // disabled: sub-osc + ring mod removed for testing

    state.currentSeriesCount = juce::jlimit (1, kMaxSeries, seriesCount);
    const bool rawStripFilters = rawMode && (model == Model::Tape || model == Model::Triode || model == Model::Clipper);

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
                state.sagEnvelope[sp][ch] = 0.0f;

                if (model == Model::Tape)
                {
                    state.tapeComp[sp][ch].reset();
                    state.tapeFlux[sp][ch] = 0.0f;
                }

                if (model == Model::Clipper)
                    state.tapeComp[sp][ch].reset();

                if (model == Model::Triode)
                    state.triodeReact[sp][ch].reset();
            }
        }
    }

    for (int i = 0; i < numSamples; ++i)
    {
        // ── Parameter smoothing (once per actual sample, NOT per series pass) ──
        state.sDrive += (driveCurved - state.sDrive) * oneMinusSmooth;
        state.sGirth += (girthParam  - state.sGirth) * oneMinusSmooth;
        state.sMod   += (modParam   - state.sMod)   * oneMinusSmooth;
        state.sBias  += (biasParam  - state.sBias)  * oneMinusSmooth;
        state.sReact += (reactParam - state.sReact) * oneMinusSmooth;
        state.sVar   += (varParam   - state.sVar)   * oneMinusSmooth;

        const float drive = state.sDrive;
        const float girth = state.sGirth;
        const float mod   = state.sMod;
        const float bias  = state.sBias;
        const float react = state.sReact;
        const float var   = state.sVar;

        // ── VARIATION: analog component tolerance + slow thermal drift ──
        // Static tolerance (per-instance hash): each "unit" has unique character.
        // Thermal drift (sub-Hz sines): very slow cathode/plate temp changes.
        // NO fast oscillation — real components don't modulate at Hz rates.
        float driveMod = 1.0f, biasMod = 0.0f, shapeMod = 0.0f, asymMod = 0.0f;
        if (var > 0.001f)
        {
            // Init static tolerances once per loader instance.
            if (! state.variation.tolerancesReady)
            {
                if (state.variationSeed == 0)
                    state.variationSeed = nextVariationSeed();

                state.variation.initTolerances (state.variationSeed);
            }

            // Very slow thermal drift: 0.03-0.15 Hz (one full cycle per 7-33 seconds)
            const float rate = 0.03f + var * 0.12f;
            state.variation.gainDrift.advance  (rate,         var, sampleRate);
            state.variation.biasDrift.advance  (rate * 0.37f, var, sampleRate);
            state.variation.shapeDrift.advance (rate * 1.43f, var, sampleRate);
            state.variation.asymDrift.advance  (rate * 0.61f, var, sampleRate);

            // Output already incorporates var scaling (depth) — no double-scaling
            driveMod = 1.0f + state.variation.gainDrift.output  * 0.08f;  // ±8% gain (tube gm + resistor dividers)
            biasMod  =        state.variation.biasDrift.output  * 0.04f;  // ±4% bias (cathode R + grid leak)
            shapeMod =        state.variation.shapeDrift.output * 0.02f;  // ±2% shape (plate Rp variation)
            asymMod  =        state.variation.asymDrift.output  * 0.025f; // ±2.5% L/R (matched pair mismatch)
        }

        // ── Per-sample series passes ──
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
                auto& stageTapeComp = state.tapeComp[sp][ch];
                auto& stageTriodeReact = state.triodeReact[sp][ch];
                auto& stageEmphasis = state.emphasis[sp][ch];
                auto& stageDcX = state.dcX[sp][ch];
                auto& stageDcY = state.dcY[sp][ch];

                // ── Safety LPF (first pass only, ×1 mode) ──
                if (isSafetyLpfOn && isFirst && !rawMode)
                    x = processSafetyLPF (state.safetyLpf[ch], x, safetyCoeffs);

                // ── YIN pitch tracker on CLEAN input (ch0, first pass only) ──
                // Feeds both sub-octave synthesizer and DESTROY ring mod.
                // Gated by needsYin: only runs for models that need pitch tracking.
                if (needsYin && ch == 0 && isFirst)
                {
                    state.yinBuf[state.yinWritePos] = x;
                    state.yinWritePos = (state.yinWritePos + 1) & (State::kYinBufSize - 1);

                    if (++state.yinCounter >= 96)
                    {
                        state.yinCounter = 0;

                        // kMaxLag=512 → lowest freq = sr/512 ≈ 86Hz @ 44.1k (covers low E guitar).
                        // kHalfW=512 for the difference function.
                        // Cost: 512×512 = 262k ops every 96 samples ≈ 120M ops/s — acceptable.
                        constexpr int kHalfW   = 512;
                        constexpr int kMinLag  = 16;    // ~2756 Hz max
                        constexpr int kMaxLag  = kHalfW;  // ~86 Hz min @ 44.1k
                        constexpr float kYinThreshold = 0.20f;

                        // Step 1+2: Difference function + CMND (store for interpolation)
                        float cmndfBuf[kMaxLag + 1];
                        cmndfBuf[0] = 1.0f;
                        float runningSum = 0.0f;

                        for (int tau = 1; tau <= kMaxLag; ++tau)
                        {
                            float d = 0.0f;
                            for (int n = 0; n < kHalfW; ++n)
                            {
                                const int idx1 = (state.yinWritePos - 1 - n) & (State::kYinBufSize - 1);
                                const int idx2 = (state.yinWritePos - 1 - n - tau) & (State::kYinBufSize - 1);
                                const float diff = state.yinBuf[idx1] - state.yinBuf[idx2];
                                d += diff * diff;
                            }
                            runningSum += d;
                            cmndfBuf[tau] = (runningSum > 1e-10f)
                                          ? (d * (float) tau / runningSum) : 1.0f;
                        }

                        // Step 3: Absolute threshold + walk to local minimum
                        int bestLag = -1;
                        for (int tau = kMinLag; tau <= kMaxLag; ++tau)
                        {
                            if (cmndfBuf[tau] < kYinThreshold)
                            {
                                while (tau + 1 <= kMaxLag && cmndfBuf[tau + 1] < cmndfBuf[tau])
                                    ++tau;
                                bestLag = tau;
                                break;
                            }
                        }

                        if (bestLag > 0)
                        {
                            // Step 5: Parabolic interpolation for sub-sample accuracy
                            float betterLag = (float) bestLag;
                            if (bestLag > kMinLag && bestLag < kMaxLag)
                            {
                                const float s0 = cmndfBuf[bestLag - 1];
                                const float s1 = cmndfBuf[bestLag];
                                const float s2 = cmndfBuf[bestLag + 1];
                                const float denom = 2.0f * (2.0f * s1 - s2 - s0);
                                if (std::abs (denom) > 1e-10f)
                                    betterLag = (float) bestLag + (s2 - s0) / denom;
                            }

                            const float detFreq = sampleRate / betterLag;
                            const float clamped = detail::clampF (detFreq, 30.0f, 4000.0f);
                            state.yinRawFreq = clamped;

                            // Adaptive smoothing: fast snap on large jumps (new note),
                            // slow smooth on small changes (vibrato/sustain)
                            const float jumpRatio = clamped / (state.yinSmoothedFreq + 1e-6f);
                            const float absJump = std::abs (jumpRatio - 1.0f);
                            // >10% change = new note → snap fast; <5% = vibrato → smooth
                            const float fastC = state.blockCoeffs.yinFastC;
                            const float slowC = state.blockCoeffs.yinSlowC;
                            const float blend = detail::clampF ((absJump - 0.05f) * 20.0f, 0.0f, 1.0f);
                            const float sC = slowC + (fastC - slowC) * blend;
                            state.yinSmoothedFreq += (clamped - state.yinSmoothedFreq) * sC;
                        }
                    }
                }

                // ── Apply VARIATION per-channel (L/R asymmetry decorrelation) ──
                float effDrive = drive * driveMod;
                float effBias  = bias  + biasMod + (ch == 0 ? asymMod : -asymMod);
                float effMod   = detail::clampF (mod + shapeMod, 0.0f, 1.0f);

                // ── INTERNAL PRE-EMPHASIS (per series pass, unless rawMode) ──
                if (!rawMode)
                    x = preEmphasize (x, stageEmphasis, model, effDrive, effMod, emphCoeffs);

                // ── REACT: per-stage energy tracking + model processing ──
                float sagPre  = 1.0f;
                float sagPost = 1.0f;
                float sagBias = 0.0f;
                bool  doSubOctave = false;
                MultibandSagResult mbSag;
                bool useMbSag = false;

                if (react > 0.001f && model != Model::Triode
                    && !(rawStripFilters && (model == Model::Tape || model == Model::Clipper)))
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
                        case Model::PushPull:
                        case Model::Cascade:
                        {
                            // Multiband REACT: frequency-dependent sag (bass sags more)
                            mbSag = multibandReactProcess (
                                stageMbReact, x, mbSubCoeff, mbAirCoeff,
                                window, react, reactAttCoeff, reactRelCoeff);
                            useMbSag = true;

                            // Broadband sag bias + drive boost (same as before)
                            const float sagSq = sagEnv * sagEnv;
                            sagBias = -sagSq * react * 0.8f;
                            effDrive = std::min (effDrive * (1.0f + sagSq * react * 2.0f), 1.0f);
                            break;
                        }
                        case Model::Diode:
                            sagBias = sagEnv * react * 0.6f;
                            sagPre  = 1.0f + sagEnv * react * 0.5f;
                            break;
                        case Model::Tape:
                        {
                            const float program = detail::clampF (sagEnv, 0.0f, 1.0f);
                            const TapeCompResult comp = processTapeComp (
                                x, stageTapeComp, react, effDrive, program, sampleRate);
                            x = comp.sample;
                            sagPre = 1.0f;
                            sagPost = 1.0f;
                            effDrive = std::min (effDrive * comp.driveLift, 1.0f);
                            stageSagEnvelope = comp.amount;
                            break;
                        }
                        case Model::Clipper:
                        {
                            const float program = detail::clampF (sagEnv, 0.0f, 1.0f);
                            const TapeCompResult comp = processTapeComp (
                                x, stageTapeComp, react, effDrive, program, sampleRate);
                            x = comp.sample;
                            sagPre = 1.0f;
                            sagPost = 1.0f;
                            stageSagEnvelope = comp.amount;
                            break;
                        }
                        case Model::Fuzz:
                        case Model::Doom:
                        case Model::Destroy:
                        case Model::Tundra:
                            doSubOctave = true;
                            break;
                        default: break;
                    }
                }
                else if (model == Model::Tape)
                {
                    stageTapeComp.gain = 1.0f;
                    stageTapeComp.env *= 0.5f;
                    stageTapeComp.hfEnv *= 0.5f;
                    stageSagEnvelope = 0.0f;
                }
                else if (model == Model::Clipper)
                {
                    stageTapeComp.gain = 1.0f;
                    stageTapeComp.env *= 0.5f;
                    stageTapeComp.hfEnv *= 0.5f;
                    stageSagEnvelope = 0.0f;
                }
                else if (model == Model::Triode && react <= 0.001f)
                {
                    stageTriodeReact.control *= 0.5f;
                    stageTriodeReact.lastSag *= 0.5f;
                    stageTriodeReact.lastSupply += (1.0f - stageTriodeReact.lastSupply) * 0.25f;
                    stageSagEnvelope = 0.0f;
                }

                // Apply stage-local pre-sag boost + bias drift.
                {
                    if (useMbSag)
                    {
                        // Apply multiband pre-sag: split→boost per band→recombine
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

                if (model == Model::Triode && girth > 0.001f)
                {
                    const float preGirth = girth * (0.28f + effDrive * 0.16f);
                    x = applyTriodePreGirth (x, juce::jlimit (0.0f, 1.0f, preGirth), effDrive);
                }

                // ── WAVESHAPER (all passes, with per-pass ADAA state) ──
                if (diagCollector != nullptr && isLast && ch == 0)
                {
                    diagCollector->feedLastPassIn (x);
                    if (model == Model::Triode)
                        diagCollector->feedTriodeBlock (state.triodeBlock[sp][ch]);
                }

                switch (model)
                {
                    case Model::Triode:
                        x = processTriode (x, effDrive, girth, effBias, effMod, react, rawMode,
                                           state, ch, sampleRate,
                                           state.triodeAdaa[sp][ch]);
                        break;
                    case Model::PushPull:
                        x = processPushPull (x, effDrive, effBias, effMod,
                                             state, ch, sampleRate,
                                             state.wsAdaa[sp][ch]);
                        break;
                    case Model::Cascade:
                        x = processCascade (x, effDrive, effBias, effMod,
                                            state, ch, cascadeCapBleed);
                        break;
                    case Model::Diode:
                        x = processDiode (x, effDrive, effBias, effMod,
                                          state.wsAdaa[sp][ch]);
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
                    case Model::Fuzz:
                        x = processFuzz (x, effDrive, effBias, effMod,
                                         state, ch, sampleRate,
                                         state.wsAdaa2[sp][ch],
                                         state.fuzzAdaa2Q2[sp][ch]);
                        break;
                    case Model::Doom:
                        x = processDoom (x, effDrive, effBias, effMod,
                                         state, ch, sampleRate,
                                         state.wsAdaa2[sp][ch],
                                         state.doomAdaa2S2[sp][ch]);
                        break;
                    case Model::Destroy:
                        x = processDestroy (x, effDrive, effBias, effMod,
                                             state, ch, sampleRate,
                                             state.wsAdaa2[sp][ch], isFirst);
                        break;
                    case Model::Tundra:
                        x = processTundra (x, effDrive, effBias, effMod,
                                           state, ch, sampleRate,
                                           state.wsAdaa[sp][ch]);
                        break;
                    default: break;
                }

                if (diagCollector != nullptr && isLast && ch == 0)
                    diagCollector->feedCore (x);

                // ── Intermediate safety: prevent extreme values entering girth/filters ──
                // Soft clip: transparent below ±2, asymptotic to ±3
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

                // ── Post-sag ceiling (per series pass) ──
                {
                    if (useMbSag)
                    {
                        // Multiband post-sag: split→attenuate per band→recombine
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

                // ── Sub-octave synthesizer — DISABLED for pure distortion testing ──
                // (was: pitch-tracked sine at f/2, gated by react + Fuzz/Doom/Destroy/Tundra)
                // To re-enable: restore doSubOctave logic and set needsYin = true for these models

                // ── GIRTH (all passes) ──
                if (model == Model::Tape)
                {
                    x = applyTapeGirth (x, girth);
                }
                else if (model == Model::Triode)
                {
                    x = applyTriodeGirth (x, girth);
                }
                else if (model == Model::Clipper)
                {
                    // GIRTH is already the clipper knee.
                }
                else
                    x = applyGirth (x, girth, state.girthAdaa[sp][ch]);

                // ── INTERNAL DE-EMPHASIS (per series pass, unless rawMode) ──
                if (!rawMode)
                    x = deEmphasize (x, stageEmphasis, model, effDrive, effMod, emphCoeffs);

                // ── DC BLOCKER (1st-order HPF at 5Hz, per series pass) ──
                if (!rawMode)
                {
                    const float dcOut = x - stageDcX + dcR * stageDcY;
                    stageDcX = x;
                    stageDcY = dcOut;
                    x = dcOut;
                }

                if (diagCollector != nullptr && isLast && ch == 0)
                    diagCollector->feedDc (x);

                // ── AUTO-GAIN COMPENSATION (per series pass, unless skipped) ──
                if (isLast)
                {
                    if (model == Model::Tape)
                        x *= getTapeLevelTrim (drive, mod, girth, react);
                    else if (model == Model::Triode)
                        x *= getTriodeLevelTrim (drive, mod, state.currentSeriesCount);

                    if (!skipAutoGain)
                        x *= state.blockCoeffs.autoGain;
                }

                // ── Final safety soft-limiter (AFTER auto-gain) ──
                // Transparent below ±1.5, smooth compression above, max ±2.5
                // Prevents auto-gain from amplifying clamped signal past safe levels
                // and eliminates hard-clip discontinuities that cause audible clicks.
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

#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <vector>
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
    OverdriveA  = 5,   // OVERDRIVE A - TS808-style feedback diode overdrive
    Clipper     = 6,   // CLIPPER - transparent mastering-style sample clipper
    NAM         = 7,   // External Neural Amp Modeler black box
    OverdriveB  = 8,   // OVERDRIVE B - Klon-style split clean/dirty overdrive
    NumModels   = 9
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
static constexpr float kRawCeiling  = 1.0f;       // 0 dBFS internal sample ceiling for RAW mode
static constexpr int   kReactBufSize = 8192;
static constexpr int   kTriodeSagBufSize = 512;
static constexpr int   kTriodeBloomSlotCount = 1024;
static constexpr int   kMaxSeries    = 4;
static constexpr bool  kOverdriveResidualMatchingEnabled = false;


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
            const float W = juce::jlimit (1.0e-5f, T, knee);
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
            const float W = juce::jlimit (1.0e-5f, T, knee);
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

struct KlonBiquadState
{
    float z1 = 0.0f;
    float z2 = 0.0f;
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float cachedSr = -1.0f;
    float cachedFreq = -1.0f;
    float cachedQ = -1.0f;
    float cachedGain = 9999.0f;
    int cachedType = -1;
    bool cachedHighShelf = false;

    void reset() noexcept
    {
        z1 = 0.0f;
        z2 = 0.0f;
        cachedType = -1;
    }
};

enum class KlonEqKind
{
    Peak,
    LowShelf,
    HighShelf,
    LowPass,
    HighPass,
    TiltShelf
};

enum class KlonEqAmount
{
    Fixed,
    Reference,
    Classic,
    ClassicDrive
};

struct KlonEqBandSpec
{
    KlonEqKind kind;
    float freqHz;
    float q;
    float gainDb;
    int stages;
    KlonEqAmount amount;
};

constexpr int kKlonPreEqBands = 64;
constexpr int kKlonPostEqBands = 64;
constexpr int kClassicPreEqBands = 32;
constexpr int kClassicPostEqBands = 64;
constexpr int kMaxKlonEqStages = 4;
constexpr int kComponentVoicingBands = 3;

struct ComponentVoicingBand
{
    bool enabled = false;
    KlonEqKind kind = KlonEqKind::Peak;
    float freqHz = 1000.0f;
    float q = 0.707f;
    float gainDb = 0.0f;
    int stages = 1;
};

struct ComponentVoicingSet
{
    ComponentVoicingBand pre[kComponentVoicingBands];
    ComponentVoicingBand post[kComponentVoicingBands];
};

struct ComponentVoicingState
{
    KlonBiquadState pre[kComponentVoicingBands][kMaxKlonEqStages];
    KlonBiquadState post[kComponentVoicingBands][kMaxKlonEqStages];

    void reset() noexcept
    {
        for (auto& band : pre)
            for (auto& s : band)
                s.reset();
        for (auto& band : post)
            for (auto& s : band)
                s.reset();
    }
};
#include "OverdriveVoicingData.h"

#ifndef SAT_TS808_RUNTIME_TUNING
#define SAT_TS808_RUNTIME_TUNING 0
#endif

#if SAT_TS808_RUNTIME_TUNING
namespace OverdriveVoicing
{
inline Ts808CoreTuning& mutableTs808CoreForAnalysis() noexcept
{
    static Ts808CoreTuning runtimeCore = kTs808Core;
    return runtimeCore;
}

inline KlonCoreTuning& mutableKlonCoreForAnalysis() noexcept
{
    static KlonCoreTuning runtimeCore = kKlonCore;
    return runtimeCore;
}

template <size_t N>
inline std::vector<KlonEqBandSpec> makeRuntimeLayer (const KlonEqBandSpec (&src)[N])
{
    return std::vector<KlonEqBandSpec> (src, src + N);
}

inline std::vector<KlonEqBandSpec>& mutableTs808PreAForAnalysis() { static auto layer = makeRuntimeLayer (kTs808PreA); return layer; }
inline std::vector<KlonEqBandSpec>& mutableTs808PreNdspForAnalysis() { static auto layer = makeRuntimeLayer (kTs808PreNdsp); return layer; }
inline std::vector<KlonEqBandSpec>& mutableTs808PreBForAnalysis() { static auto layer = makeRuntimeLayer (kTs808PreB); return layer; }
inline std::vector<KlonEqBandSpec>& mutableTs808PostAForAnalysis() { static auto layer = makeRuntimeLayer (kTs808PostA); return layer; }
inline std::vector<KlonEqBandSpec>& mutableTs808PostNdspForAnalysis() { static auto layer = makeRuntimeLayer (kTs808PostNdsp); return layer; }
inline std::vector<KlonEqBandSpec>& mutableTs808PostBForAnalysis() { static auto layer = makeRuntimeLayer (kTs808PostB); return layer; }
inline std::vector<KlonEqBandSpec>& mutableKlonPreAForAnalysis() { static auto layer = makeRuntimeLayer (kKlonPreA); return layer; }
inline std::vector<KlonEqBandSpec>& mutableKlonPreNdspForAnalysis() { static auto layer = makeRuntimeLayer (kKlonPreNdsp); return layer; }
inline std::vector<KlonEqBandSpec>& mutableKlonPreBForAnalysis() { static auto layer = makeRuntimeLayer (kKlonPreB); return layer; }
inline std::vector<KlonEqBandSpec>& mutableKlonPostAForAnalysis() { static auto layer = makeRuntimeLayer (kKlonPostA); return layer; }
inline std::vector<KlonEqBandSpec>& mutableKlonPostNdspForAnalysis() { static auto layer = makeRuntimeLayer (kKlonPostNdsp); return layer; }
inline std::vector<KlonEqBandSpec>& mutableKlonPostBForAnalysis() { static auto layer = makeRuntimeLayer (kKlonPostB); return layer; }

inline void resetTs808RuntimeTuningForAnalysis()
{
    mutableTs808CoreForAnalysis() = kTs808Core;
    mutableKlonCoreForAnalysis() = kKlonCore;
    mutableTs808PreAForAnalysis() = makeRuntimeLayer (kTs808PreA);
    mutableTs808PreNdspForAnalysis() = makeRuntimeLayer (kTs808PreNdsp);
    mutableTs808PreBForAnalysis() = makeRuntimeLayer (kTs808PreB);
    mutableTs808PostAForAnalysis() = makeRuntimeLayer (kTs808PostA);
    mutableTs808PostNdspForAnalysis() = makeRuntimeLayer (kTs808PostNdsp);
    mutableTs808PostBForAnalysis() = makeRuntimeLayer (kTs808PostB);
    mutableKlonPreAForAnalysis() = makeRuntimeLayer (kKlonPreA);
    mutableKlonPreNdspForAnalysis() = makeRuntimeLayer (kKlonPreNdsp);
    mutableKlonPreBForAnalysis() = makeRuntimeLayer (kKlonPreB);
    mutableKlonPostAForAnalysis() = makeRuntimeLayer (kKlonPostA);
    mutableKlonPostNdspForAnalysis() = makeRuntimeLayer (kKlonPostNdsp);
    mutableKlonPostBForAnalysis() = makeRuntimeLayer (kKlonPostB);
}
}
#endif

inline const OverdriveVoicing::Ts808CoreTuning& getTs808CoreTuning() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableTs808CoreForAnalysis();
#else
    return OverdriveVoicing::kTs808Core;
#endif
}

inline const OverdriveVoicing::KlonCoreTuning& getKlonCoreTuning() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableKlonCoreForAnalysis();
#else
    return OverdriveVoicing::kKlonCore;
#endif
}


inline const auto& getTs808PreAForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableTs808PreAForAnalysis();
#else
    return OverdriveVoicing::kTs808PreA;
#endif
}

inline const auto& getTs808PreNdspForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableTs808PreNdspForAnalysis();
#else
    return OverdriveVoicing::kTs808PreNdsp;
#endif
}

inline const auto& getTs808PreBForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableTs808PreBForAnalysis();
#else
    return OverdriveVoicing::kTs808PreB;
#endif
}

inline const auto& getTs808PostAForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableTs808PostAForAnalysis();
#else
    return OverdriveVoicing::kTs808PostA;
#endif
}

inline const auto& getTs808PostNdspForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableTs808PostNdspForAnalysis();
#else
    return OverdriveVoicing::kTs808PostNdsp;
#endif
}

inline const auto& getTs808PostBForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableTs808PostBForAnalysis();
#else
    return OverdriveVoicing::kTs808PostB;
#endif
}

inline const auto& getKlonPreAForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableKlonPreAForAnalysis();
#else
    return OverdriveVoicing::kKlonPreA;
#endif
}

inline const auto& getKlonPreNdspForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableKlonPreNdspForAnalysis();
#else
    return OverdriveVoicing::kKlonPreNdsp;
#endif
}

inline const auto& getKlonPreBForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableKlonPreBForAnalysis();
#else
    return OverdriveVoicing::kKlonPreB;
#endif
}

inline const auto& getKlonPostAForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableKlonPostAForAnalysis();
#else
    return OverdriveVoicing::kKlonPostA;
#endif
}

inline const auto& getKlonPostNdspForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableKlonPostNdspForAnalysis();
#else
    return OverdriveVoicing::kKlonPostNdsp;
#endif
}

inline const auto& getKlonPostBForAnalysis() noexcept
{
#if SAT_TS808_RUNTIME_TUNING
    return OverdriveVoicing::mutableKlonPostBForAnalysis();
#else
    return OverdriveVoicing::kKlonPostB;
#endif
}

using KlonPostEqBank = KlonBiquadState[kKlonPostEqBands][kMaxKlonEqStages];

struct KlonPostEqState
{
    KlonPostEqBank klonPostEq;

    void reset() noexcept
    {
        for (auto& band : klonPostEq)
            for (auto& s : band)
                s.reset();
    }
};

struct OverdriveAPostEqState
{
    KlonBiquadState overdriveAPostEq[kClassicPostEqBands][kMaxKlonEqStages];
    float overdriveAPostHiCutLP = 0.0f;

    void reset() noexcept
    {
        for (auto& band : overdriveAPostEq)
            for (auto& s : band)
                s.reset();
        overdriveAPostHiCutLP = 0.0f;
    }
};

struct OverdriveToneState
{
    float cleanLP = 0.0f;
    float dirtyLowLP = 0.0f;
    float dirtyLP = 0.0f;
    KlonBiquadState klonPreEq[kKlonPreEqBands][kMaxKlonEqStages];
    KlonPostEqBank klonPostEq;
    KlonBiquadState overdriveAPreEq[kClassicPreEqBands][kMaxKlonEqStages];
    KlonBiquadState overdriveAPostEq[kClassicPostEqBands][kMaxKlonEqStages];
    float overdriveAPostHiCutLP = 0.0f;
    float tsFeedbackLP = 0.0f;
    float tsDiodeMemory = 0.0f;
    float tsOpAmpRecovery = 0.0f;
    bool tsFeedbackInitialised = false;
    adaa::StableTanhADAA softAdaa;

    void reset() noexcept
    {
        cleanLP = 0.0f;
        dirtyLowLP = 0.0f;
        dirtyLP = 0.0f;
        for (auto& band : klonPreEq)
            for (auto& s : band)
                s.reset();
        for (auto& band : klonPostEq)
            for (auto& s : band)
                s.reset();
        for (auto& band : overdriveAPreEq)
            for (auto& s : band)
                s.reset();
        for (auto& band : overdriveAPostEq)
            for (auto& s : band)
                s.reset();
        overdriveAPostHiCutLP = 0.0f;
        tsFeedbackLP = 0.0f;
        tsDiodeMemory = 0.0f;
        tsOpAmpRecovery = 0.0f;
        tsFeedbackInitialised = false;
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
    DriftOsc inputDrift;
    DriftOsc shapeDrift;
    bool     tolerancesReady = false;

    void initTolerances (uint32_t seed) noexcept
    {
        gainDrift.initTolerance  (seed, 0);
        inputDrift.initTolerance (seed, 1);
        shapeDrift.initTolerance (seed, 2);
        tolerancesReady = true;
    }

    void reset() noexcept
    {
        gainDrift.reset();
        inputDrift.reset();
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
    OverdriveToneState overdriveTone[kMaxSeries][2];
    KlonPostEqState overdriveBNativePost[kMaxSeries][2];
    OverdriveAPostEqState overdriveANativePost[kMaxSeries][2];
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
    float tubeBiasPostDcX[kMaxSeries][2] = {};
    float tubeBiasPostDcY[kMaxSeries][2] = {};

    // Internal emphasis/de-emphasis (per-stage / per-channel)
    EmphasisState emphasis[kMaxSeries][2];
    ComponentVoicingState componentVoicing[kMaxSeries][2];

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
    bool deferOverdriveAPostEq = false;
    bool deferFullKlonPostEq = false;

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
                overdriveBNativePost[sp][ch].reset();
                overdriveANativePost[sp][ch].reset();
                react[sp][ch].reset();
                mbReact[sp][ch].reset();
                sagEnvelope[sp][ch] = 0.0f;
                dynamicsComp[sp][ch].reset();
                clipperPeak[sp][ch].reset();
                overdriveTone[sp][ch].reset();
                transistorPeakCatch[sp][ch].reset();
                triodeReact[sp][ch].reset();
                dcX[sp][ch] = dcY[sp][ch] = 0.0f;
                emphasis[sp][ch].reset();
                componentVoicing[sp][ch].reset();
                bumpZ1[sp][ch] = bumpZ2[sp][ch] = 0.0f;
                triodeBlock[sp][ch] = 0.0f;
                powerSag[sp][ch] = 0.0f;
                tapeFlux[sp][ch] = 0.0f;
                tapeStressEnv[sp][ch] = 0.0f;
                triodeBodyPreLP[sp][ch] = 0.0f;
                triodeBodyPostLP[sp][ch] = 0.0f;
                triodeCouplingDc[sp][ch] = 0.0f;
                tubeBiasPostDcX[sp][ch] = 0.0f;
                tubeBiasPostDcY[sp][ch] = 0.0f;
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
        deferOverdriveAPostEq = false;
        deferFullKlonPostEq = false;
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
                for (auto& band : overdriveBNativePost[sp][ch].klonPostEq)
                    for (auto& s : band)
                    {
                        fl (s.z1);
                        fl (s.z2);
                    }
                for (auto& band : overdriveANativePost[sp][ch].overdriveAPostEq)
                    for (auto& s : band)
                    {
                        fl (s.z1);
                        fl (s.z2);
                    }
                fl (overdriveANativePost[sp][ch].overdriveAPostHiCutLP);
                fl (sagEnvelope[sp][ch]);
                fl (dcX[sp][ch]);
                fl (dcY[sp][ch]);
                fl (emphasis[sp][ch].preHP);
                fl (emphasis[sp][ch].preSh);
                fl (emphasis[sp][ch].postHP);
                fl (emphasis[sp][ch].postLP);
                for (auto& band : componentVoicing[sp][ch].pre)
                    for (auto& s : band)
                    {
                        fl (s.z1);
                        fl (s.z2);
                    }
                for (auto& band : componentVoicing[sp][ch].post)
                    for (auto& s : band)
                    {
                        fl (s.z1);
                        fl (s.z2);
                    }
                fl (dynamicsComp[sp][ch].scLP);
                fl (dynamicsComp[sp][ch].env);
                fl (dynamicsComp[sp][ch].hfEnv);
                fl (dynamicsComp[sp][ch].gain);
                fl (clipperPeak[sp][ch].peakEnv);
                fl (clipperPeak[sp][ch].bodyEnv);
                fl (overdriveTone[sp][ch].cleanLP);
                fl (overdriveTone[sp][ch].dirtyLowLP);
                fl (overdriveTone[sp][ch].dirtyLP);
                for (auto& band : overdriveTone[sp][ch].klonPreEq)
                    for (auto& s : band)
                    {
                        fl (s.z1);
                        fl (s.z2);
                    }
                for (auto& band : overdriveTone[sp][ch].klonPostEq)
                    for (auto& s : band)
                    {
                        fl (s.z1);
                        fl (s.z2);
                    }
                for (auto& band : overdriveTone[sp][ch].overdriveAPreEq)
                    for (auto& s : band)
                    {
                        fl (s.z1);
                        fl (s.z2);
                    }
                for (auto& band : overdriveTone[sp][ch].overdriveAPostEq)
                    for (auto& s : band)
                    {
                        fl (s.z1);
                        fl (s.z2);
                    }
                fl (overdriveTone[sp][ch].overdriveAPostHiCutLP);
                fl (overdriveTone[sp][ch].tsFeedbackLP);
                fl (overdriveTone[sp][ch].tsDiodeMemory);
                fl (overdriveTone[sp][ch].tsOpAmpRecovery);
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
                fl (tubeBiasPostDcX[sp][ch]);
                fl (tubeBiasPostDcY[sp][ch]);
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

    inline float modulateUnitAtEdges (float base, float delta) noexcept
    {
        float y = clampF (base, 0.0f, 1.0f) + delta;
        if (y > 1.0f)
            y = 1.0f - (y - 1.0f);
        else if (y < 0.0f)
            y = -y;

        return clampF (y, 0.0f, 1.0f);
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

    inline float clipperADrive (float drive, float type) noexcept
    {
        const float typeT = smoothStep01 (clampF (type, 0.0f, 1.0f));
        const float typeDriveScale = juce::jmap (typeT, 1.1765f, 0.95f);
        return clampF (drive, 0.0f, 1.0f) * 0.68f * typeDriveScale;
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
                { 0.5070f, 0.5754f, 1.5487f, 2.4091f, 2.8021f },
                { 0.6019f, 0.7650f, 1.5354f, 1.9830f, 2.3674f },
                { 1.0305f, 1.1990f, 1.4901f, 2.0575f, 2.5000f },
            },
            {
                { 0.5605f, 0.5457f, 1.4064f, 2.2068f, 2.6081f },
                { 0.5009f, 0.6336f, 1.4207f, 1.8634f, 2.2230f },
                { 0.7659f, 0.9595f, 1.4464f, 1.9999f, 2.4271f },
            },
            {
                { 0.4758f, 0.4054f, 0.9766f, 1.5621f, 1.9496f },
                { 0.3173f, 0.4132f, 1.0028f, 1.4221f, 1.7007f },
                { 0.4483f, 0.6486f, 1.1248f, 1.5867f, 1.9283f },
            },
        },
        {
            {
                { 0.1734f, 0.3161f, 1.2342f, 2.0484f, 2.6059f },
                { 0.2306f, 0.4571f, 1.3097f, 1.9390f, 2.3745f },
                { 0.6716f, 1.0383f, 1.4003f, 2.0646f, 2.5465f },
            },
            {
                { 0.2600f, 0.3357f, 1.1455f, 1.8806f, 2.3840f },
                { 0.2297f, 0.3943f, 1.2057f, 1.8111f, 2.2181f },
                { 0.4283f, 0.7478f, 1.3461f, 1.9995f, 2.4648f },
            },
            {
                { 0.3313f, 0.2985f, 0.8545f, 1.3889f, 1.7230f },
                { 0.1918f, 0.2810f, 0.8494f, 1.3466f, 1.6572f },
                { 0.2316f, 0.4555f, 0.9951f, 1.5472f, 1.9329f },
            },
        },
        {
            {
                { 0.1261f, 0.2660f, 1.1208f, 1.8615f, 2.3876f },
                { 0.1306f, 0.3640f, 1.2113f, 1.8882f, 2.3331f },
                { 0.4576f, 0.9224f, 1.3279f, 2.0038f, 2.4844f },
            },
            {
                { 0.2065f, 0.2964f, 1.0567f, 1.7350f, 2.1960f },
                { 0.1645f, 0.3361f, 1.1233f, 1.7685f, 2.1841f },
                { 0.2916f, 0.6427f, 1.2761f, 1.9490f, 2.4149f },
            },
            {
                { 0.3001f, 0.2826f, 0.8188f, 1.3354f, 1.6531f },
                { 0.1730f, 0.2622f, 0.8161f, 1.3264f, 1.6402f },
                { 0.1840f, 0.4028f, 0.9462f, 1.5215f, 1.8910f },
            },
        },
        {
            {
                { 0.1202f, 0.2587f, 1.0968f, 1.8138f, 2.2875f },
                { 0.1071f, 0.3384f, 1.1670f, 1.8694f, 2.3182f },
                { 0.3317f, 0.8372f, 1.2992f, 1.9929f, 2.4729f },
            },
            {
                { 0.1990f, 0.2901f, 1.0394f, 1.7076f, 2.1419f },
                { 0.1485f, 0.3213f, 1.0878f, 1.7549f, 2.1738f },
                { 0.2351f, 0.5930f, 1.2488f, 1.9454f, 2.4111f },
            },
            {
                { 0.2969f, 0.2830f, 0.8216f, 1.3381f, 1.6532f },
                { 0.1707f, 0.2604f, 0.8117f, 1.3282f, 1.6408f },
                { 0.1736f, 0.3899f, 0.9367f, 1.5266f, 1.8908f },
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

    // Static per-stage trim. Series-dependent correction is handled separately
    // at the end of the chain.
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
                { 1.4278f, 1.0453f, 0.2993f, 0.2399f, 0.2578f },
                { 1.5200f, 1.0561f, 0.2851f, 0.2259f, 0.2536f },
                { 1.7760f, 1.1450f, 0.3191f, 0.2560f, 0.2894f },
            },
            {
                { 1.2384f, 0.8842f, 0.2748f, 0.2266f, 0.2487f },
                { 1.3256f, 0.9012f, 0.2612f, 0.2125f, 0.2433f },
                { 1.5626f, 0.9888f, 0.2882f, 0.2353f, 0.2707f },
            },
            {
                { 0.8970f, 0.6445f, 0.2494f, 0.2160f, 0.2504f },
                { 0.9587f, 0.6579f, 0.2391f, 0.2056f, 0.2466f },
                { 1.1363f, 0.7260f, 0.2556f, 0.2195f, 0.2652f },
            },
        },
        {
            {
                { 1.5433f, 0.8702f, 0.2278f, 0.1597f, 0.2052f },
                { 1.7093f, 0.8700f, 0.2092f, 0.1512f, 0.2010f },
                { 2.2420f, 0.9856f, 0.2313f, 0.1785f, 0.2290f },
            },
            {
                { 1.1779f, 0.6382f, 0.2200f, 0.1597f, 0.2057f },
                { 1.3206f, 0.6511f, 0.2015f, 0.1500f, 0.1999f },
                { 1.7668f, 0.7571f, 0.2135f, 0.1717f, 0.2207f },
            },
            {
                { 0.6559f, 0.3840f, 0.2000f, 0.1645f, 0.2174f },
                { 0.7298f, 0.3933f, 0.1886f, 0.1578f, 0.2134f },
                { 0.9771f, 0.4565f, 0.1946f, 0.1720f, 0.2270f },
            },
        },
        {
            {
                { 1.6002f, 0.7002f, 0.2164f, 0.1503f, 0.2051f },
                { 1.8324f, 0.6906f, 0.1928f, 0.1442f, 0.2012f },
                { 2.6788f, 0.8135f, 0.2139f, 0.1635f, 0.2271f },
            },
            {
                { 1.0842f, 0.4589f, 0.2106f, 0.1505f, 0.2063f },
                { 1.2644f, 0.4669f, 0.1890f, 0.1438f, 0.2006f },
                { 1.9022f, 0.5680f, 0.1991f, 0.1586f, 0.2197f },
            },
            {
                { 0.4930f, 0.2698f, 0.1859f, 0.1572f, 0.2189f },
                { 0.5608f, 0.2741f, 0.1746f, 0.1525f, 0.2160f },
                { 0.8219f, 0.3180f, 0.1803f, 0.1632f, 0.2278f },
            },
        },
        {
            {
                { 1.6183f, 0.5551f, 0.2070f, 0.1486f, 0.2090f },
                { 1.9178f, 0.5433f, 0.1798f, 0.1438f, 0.2049f },
                { 3.1288f, 0.6679f, 0.2020f, 0.1619f, 0.2313f },
            },
            {
                { 0.9771f, 0.3422f, 0.2046f, 0.1494f, 0.2113f },
                { 1.1857f, 0.3481f, 0.1801f, 0.1433f, 0.2049f },
                { 2.0056f, 0.4359f, 0.1894f, 0.1567f, 0.2238f },
            },
            {
                { 0.3849f, 0.2220f, 0.1778f, 0.1574f, 0.2242f },
                { 0.4431f, 0.2247f, 0.1667f, 0.1533f, 0.2208f },
                { 0.6950f, 0.2577f, 0.1722f, 0.1624f, 0.2332f },
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
                { 1.8412f, 1.1754f, 0.5667f, 0.5761f, 0.5324f },
                { 1.5580f, 1.1826f, 0.5929f, 0.5774f, 0.5258f },
                { 1.3951f, 1.2365f, 0.6724f, 0.6199f, 0.5509f },
            },
            {
                { 1.2925f, 0.9136f, 0.6327f, 0.6746f, 0.6328f },
                { 1.1620f, 0.9448f, 0.6316f, 0.6495f, 0.6018f },
                { 1.1209f, 1.0506f, 0.6921f, 0.6719f, 0.6095f },
            },
            {
                { 1.1643f, 0.8780f, 0.6630f, 0.7168f, 0.6758f },
                { 1.0583f, 0.8992f, 0.6600f, 0.6916f, 0.6445f },
                { 1.0388f, 1.0034f, 0.7184f, 0.7128f, 0.6513f },
            },
        },
        {
            {
                { 2.1441f, 0.8661f, 0.5029f, 0.5405f, 0.5134f },
                { 1.7422f, 0.9402f, 0.5101f, 0.5326f, 0.4998f },
                { 1.5269f, 1.0669f, 0.5586f, 0.5591f, 0.5140f },
            },
            {
                { 1.0565f, 0.6428f, 0.5890f, 0.6477f, 0.6183f },
                { 0.9696f, 0.6834f, 0.5730f, 0.6160f, 0.5830f },
                { 0.9860f, 0.8147f, 0.6040f, 0.6256f, 0.5809f },
            },
            {
                { 0.8575f, 0.6403f, 0.6251f, 0.6925f, 0.6619f },
                { 0.8050f, 0.6563f, 0.6093f, 0.6615f, 0.6275f },
                { 0.8474f, 0.7662f, 0.6406f, 0.6717f, 0.6258f },
            },
        },
        {
            {
                { 2.3282f, 0.6659f, 0.4786f, 0.5306f, 0.5085f },
                { 1.8353f, 0.7566f, 0.4790f, 0.5174f, 0.4932f },
                { 1.5977f, 0.9185f, 0.5181f, 0.5359f, 0.5021f },
            },
            {
                { 0.8046f, 0.5206f, 0.5677f, 0.6389f, 0.6134f },
                { 0.7618f, 0.5451f, 0.5459f, 0.6025f, 0.5767f },
                { 0.8291f, 0.6606f, 0.5662f, 0.6030f, 0.5693f },
            },
            {
                { 0.5883f, 0.5360f, 0.6049f, 0.6841f, 0.6571f },
                { 0.5767f, 0.5356f, 0.5837f, 0.6485f, 0.6209f },
                { 0.6610f, 0.6220f, 0.6035f, 0.6487f, 0.6141f },
            },
        },
        {
            {
                { 2.4634f, 0.5518f, 0.4682f, 0.5274f, 0.5063f },
                { 1.8944f, 0.6370f, 0.4643f, 0.5124f, 0.4910f },
                { 1.6362f, 0.7999f, 0.4947f, 0.5255f, 0.4980f },
            },
            {
                { 0.5966f, 0.4634f, 0.5587f, 0.6360f, 0.6113f },
                { 0.5865f, 0.4744f, 0.5331f, 0.5987f, 0.5745f },
                { 0.6822f, 0.5633f, 0.5456f, 0.5931f, 0.5651f },
            },
            {
                { 0.3930f, 0.4868f, 0.5970f, 0.6812f, 0.6547f },
                { 0.4049f, 0.4750f, 0.5716f, 0.6446f, 0.6187f },
                { 0.5047f, 0.5360f, 0.5838f, 0.6390f, 0.6098f },
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
                { 1.4598f, 0.6594f, 0.2075f, 0.2082f, 0.2167f },
                { 1.1489f, 0.2633f, 0.1624f, 0.1893f, 0.2074f },
                { 1.0023f, 0.3567f, 0.1606f, 0.1789f, 0.1917f },
            },
            {
                { 2.1910f, 1.0128f, 0.2324f, 0.2160f, 0.2212f },
                { 1.7754f, 0.3400f, 0.1677f, 0.1920f, 0.2090f },
                { 1.5163f, 0.5376f, 0.1703f, 0.1835f, 0.1944f },
            },
            {
                { 2.2680f, 1.1000f, 0.2435f, 0.2178f, 0.2221f },
                { 1.8797f, 0.3695f, 0.1692f, 0.1926f, 0.2093f },
                { 1.5930f, 0.5875f, 0.1727f, 0.1838f, 0.1942f },
            },
        },
        {
            {
                { 0.8525f, 0.1888f, 0.1413f, 0.1688f, 0.1870f },
                { 0.5893f, 0.1131f, 0.1423f, 0.1747f, 0.1921f },
                { 0.5608f, 0.1241f, 0.1263f, 0.1510f, 0.1671f },
            },
            {
                { 1.9729f, 0.3299f, 0.1434f, 0.1689f, 0.1869f },
                { 1.4046f, 0.1262f, 0.1430f, 0.1747f, 0.1921f },
                { 1.2671f, 0.1599f, 0.1276f, 0.1507f, 0.1682f },
            },
            {
                { 2.0907f, 0.3949f, 0.1449f, 0.1689f, 0.1867f },
                { 1.5783f, 0.1300f, 0.1433f, 0.1747f, 0.1921f },
                { 1.4292f, 0.1725f, 0.1275f, 0.1498f, 0.1669f },
            },
        },
        {
            {
                { 0.4706f, 0.1150f, 0.1361f, 0.1667f, 0.1855f },
                { 0.3289f, 0.1030f, 0.1405f, 0.1717f, 0.1905f },
                { 0.3302f, 0.0952f, 0.1219f, 0.1483f, 0.1646f },
            },
            {
                { 1.6943f, 0.1586f, 0.1348f, 0.1667f, 0.1853f },
                { 1.0537f, 0.1062f, 0.1405f, 0.1716f, 0.1904f },
                { 0.9508f, 0.1035f, 0.1220f, 0.1475f, 0.1639f },
            },
            {
                { 1.8255f, 0.2054f, 0.1341f, 0.1666f, 0.1852f },
                { 1.2678f, 0.1076f, 0.1404f, 0.1714f, 0.1903f },
                { 1.1681f, 0.1067f, 0.1215f, 0.1463f, 0.1627f },
            },
        },
        {
            {
                { 0.2774f, 0.1016f, 0.1358f, 0.1663f, 0.1850f },
                { 0.1976f, 0.1010f, 0.1400f, 0.1707f, 0.1898f },
                { 0.2096f, 0.0903f, 0.1214f, 0.1477f, 0.1640f },
            },
            {
                { 1.4230f, 0.1108f, 0.1363f, 0.1662f, 0.1848f },
                { 0.7706f, 0.1027f, 0.1399f, 0.1706f, 0.1897f },
                { 0.6927f, 0.0943f, 0.1206f, 0.1466f, 0.1629f },
            },
            {
                { 1.5564f, 0.1468f, 0.1363f, 0.1662f, 0.1846f },
                { 0.9997f, 0.1034f, 0.1398f, 0.1706f, 0.1896f },
                { 0.9319f, 0.0956f, 0.1197f, 0.1454f, 0.1616f },
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

inline float getClipperLevelTrim (float drive, float girth, float mod) noexcept
{
    const float d = detail::clipperADrive (drive, mod);
    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float m = detail::clampF (mod, 0.0f, 1.0f);

    static constexpr float trims[3][3][5] =
    {
        {
            { 4.7562f, 4.2937f, 1.9506f, 2.0236f, 2.1185f },
            { 2.3205f, 3.1355f, 2.0069f, 2.0759f, 2.1820f },
            { 4.7665f, 3.6648f, 2.6112f, 2.4270f, 2.4201f },
        },
        {
            { 4.4815f, 4.0448f, 1.9772f, 2.0381f, 2.1195f },
            { 2.4018f, 3.1549f, 2.0133f, 2.0787f, 2.1822f },
            { 4.6200f, 4.2327f, 3.1330f, 2.5604f, 2.4790f },
        },
        {
            { 4.3951f, 3.9026f, 2.0472f, 2.0763f, 2.1221f },
            { 2.5654f, 3.2097f, 2.0311f, 2.0863f, 2.1829f },
            { 4.6161f, 4.1841f, 3.6594f, 3.3946f, 2.9365f },
        },
    };

    auto interpDrive = [d] (const float (&v)[5]) noexcept
    {
        return detail::interpDrive5 (d, v[0], v[1], v[2], v[3], v[4]);
    };

    float charTrim[3];
    for (int charIndex = 0; charIndex < 3; ++charIndex)
    {
        const float trimType0 = interpDrive (trims[charIndex][0]);
        const float trimType1 = interpDrive (trims[charIndex][1]);
        const float trimType2 = interpDrive (trims[charIndex][2]);
        charTrim[charIndex] = detail::morphThreeWay (m, trimType0, trimType1, trimType2);
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
                { 0.4299f, 0.1584f, 0.2066f, 0.1969f, 0.1888f },
                { 2.9580f, 1.8556f, 0.9628f, 0.8238f, 0.7749f },
                { 0.6833f, 0.4841f, 0.3444f, 0.3807f, 0.4106f },
            },
            {
                { 0.6144f, 0.2128f, 0.2061f, 0.1959f, 0.1890f },
                { 2.8578f, 1.8432f, 0.9712f, 0.8254f, 0.7748f },
                { 0.9396f, 0.6508f, 0.3895f, 0.3654f, 0.4038f },
            },
            {
                { 0.9107f, 0.3164f, 0.2049f, 0.1931f, 0.1891f },
                { 2.6756f, 1.8100f, 0.9948f, 0.8296f, 0.7745f },
                { 1.5638f, 1.8014f, 0.9099f, 0.4670f, 0.3837f },
            },
        },
        {
            {
                { 0.1869f, 0.0812f, 0.1761f, 0.1706f, 0.1642f },
                { 2.1234f, 0.5301f, 0.7415f, 0.7744f, 0.7664f },
                { 0.3495f, 0.2470f, 0.2718f, 0.3241f, 0.3439f },
            },
            {
                { 0.3100f, 0.0867f, 0.1738f, 0.1693f, 0.1641f },
                { 1.9820f, 0.5312f, 0.7396f, 0.7735f, 0.7662f },
                { 0.6819f, 0.3156f, 0.2446f, 0.2884f, 0.3332f },
            },
            {
                { 0.6448f, 0.0922f, 0.1689f, 0.1662f, 0.1638f },
                { 1.7373f, 0.5366f, 0.7339f, 0.7707f, 0.7657f },
                { 2.0380f, 2.0910f, 0.4230f, 0.2807f, 0.2965f },
            },
        },
        {
            {
                { 0.1130f, 0.0785f, 0.1702f, 0.1650f, 0.1589f },
                { 1.2813f, 0.3173f, 0.7264f, 0.7713f, 0.7661f },
                { 0.1850f, 0.1954f, 0.2561f, 0.3072f, 0.3211f },
            },
            {
                { 0.1568f, 0.0835f, 0.1681f, 0.1639f, 0.1588f },
                { 1.1555f, 0.3162f, 0.7241f, 0.7704f, 0.7660f },
                { 0.4092f, 0.2007f, 0.2212f, 0.2735f, 0.3110f },
            },
            {
                { 0.3749f, 0.0871f, 0.1635f, 0.1608f, 0.1586f },
                { 0.9499f, 0.3137f, 0.7170f, 0.7675f, 0.7653f },
                { 2.4097f, 1.7070f, 0.2673f, 0.2610f, 0.2777f },
            },
        },
        {
            {
                { 0.0989f, 0.0776f, 0.1681f, 0.1630f, 0.1570f },
                { 0.7356f, 0.2960f, 0.7248f, 0.7712f, 0.7661f },
                { 0.1246f, 0.1797f, 0.2513f, 0.3006f, 0.3131f },
            },
            {
                { 0.1052f, 0.0824f, 0.1661f, 0.1619f, 0.1569f },
                { 0.6435f, 0.2944f, 0.7224f, 0.7702f, 0.7660f },
                { 0.2397f, 0.1653f, 0.2156f, 0.2674f, 0.3028f },
            },
            {
                { 0.1930f, 0.0859f, 0.1617f, 0.1589f, 0.1567f },
                { 0.5153f, 0.2901f, 0.7152f, 0.7674f, 0.7653f },
                { 2.5476f, 1.2056f, 0.2375f, 0.2554f, 0.2711f },
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

    const float baseTrim = detail::morphThreeWay (g, charTrim[0], charTrim[1], charTrim[2]);
    const float midType = std::pow (detail::clampF (1.0f - std::abs (m - 0.5f) * 2.0f, 0.0f, 1.0f), 1.5f);
    const float driveTrim = detail::smoothStep01 (d * 2.0f);
    const float midAttenuationDb = -9.0f * midType * driveTrim;
    const float klonVoice = m <= 0.5f ? 0.0f : detail::smoothStep01 ((m - 0.5f) * 2.0f);
    const float klonHotDrive = detail::smoothStep01 ((d - 0.45f) / 0.55f);
    static constexpr float klonSeriesDb[4] = { -1.65f, -0.85f, -0.40f, -0.20f };
    const float klonHotTrimDb = klonSeriesDb[seriesIndex] * klonVoice * klonHotDrive;

    const float overdriveAVoiceAmount = 1.0f - detail::smoothStep01 (detail::clampF (m * 2.0f, 0.0f, 1.0f));
    const float measuredOverdriveAOutputDb = -13.5f * overdriveAVoiceAmount;
    return baseTrim * std::pow (10.0f, (midAttenuationDb + klonHotTrimDb + measuredOverdriveAOutputDb) / 20.0f);
}

inline float getStageTrimMigrationCorrection (Model model, int seriesCount) noexcept
{
    const int seriesIndex = juce::jlimit (1, kMaxSeries, seriesCount) - 1;

    // Brown/white peak-normalized migration trim: keeps the output envelope
    // close to the pre-migration calibration while the static trim now feeds
    // every repeated stage at a stable nominal level.
    static constexpr float tape[4] = { 1.0f, 1.0310f, 1.0351f, 1.0292f };
    static constexpr float tube[4] = { 1.0f, 0.9611f, 0.9273f, 0.8995f };

    switch (model)
    {
        case Model::Tape: return tape[seriesIndex];
        case Model::Tube: return tube[seriesIndex];
        default:          return 1.0f;
    }
}

inline float getRawModeLevelCorrection (Model model, float drive, float girth, float mod, int seriesCount) noexcept
{
    int modelIndex = -1;
    switch (model)
    {
        case Model::Tape:       modelIndex = 0; break;
        case Model::Tube:       modelIndex = 1; break;
        case Model::Transistor: modelIndex = 2; break;
        case Model::Diode:      modelIndex = 3; break;
        case Model::OverdriveA:
        case Model::OverdriveB: modelIndex = 4; break;
        default: return 1.0f;
    }

    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float m = detail::clampF (mod, 0.0f, 1.0f);
    const int seriesIndex = juce::jlimit (1, kMaxSeries, seriesCount) - 1;

    // Brown/white RAW calibration. Each point uses the worst measured
    // peak/RMS excess against the normal route, so RAW does not jump in
    // output while keeping the raw core tone unchanged.
    static constexpr float trims[5][4][3][3][5] =
    {
        {
            {
                {
                    { 0.8701f, 0.9063f, 0.9371f, 0.9524f, 0.9623f },
                    { 0.8419f, 0.8761f, 0.9061f, 0.9358f, 0.9619f },
                    { 0.8321f, 0.8790f, 0.9156f, 0.9519f, 0.9685f },
                },
                {
                    { 0.8015f, 0.8436f, 0.8858f, 0.9081f, 0.9154f },
                    { 0.7693f, 0.8099f, 0.8476f, 0.8944f, 0.9367f },
                    { 0.7528f, 0.8116f, 0.8614f, 0.9248f, 0.9527f },
                },
                {
                    { 0.8682f, 0.8886f, 0.9005f, 0.9058f, 0.9030f },
                    { 0.8037f, 0.8161f, 0.8295f, 0.8616f, 0.9083f },
                    { 0.7432f, 0.7751f, 0.8160f, 0.8945f, 0.9309f },
                },
            },
            {
                {
                    { 0.8700f, 0.9166f, 0.9397f, 0.9568f, 0.9658f },
                    { 0.8260f, 0.8713f, 0.8967f, 0.9408f, 0.9560f },
                    { 0.7965f, 0.8466f, 0.8723f, 0.9519f, 0.9761f },
                },
                {
                    { 0.7968f, 0.8537f, 0.8895f, 0.9079f, 0.9198f },
                    { 0.7528f, 0.8046f, 0.8374f, 0.9170f, 0.9529f },
                    { 0.7100f, 0.7712f, 0.8087f, 0.9445f, 0.9865f },
                },
                {
                    { 0.8886f, 0.9126f, 0.9188f, 0.9045f, 0.8965f },
                    { 0.8477f, 0.8555f, 0.8563f, 0.9390f, 0.8635f },
                    { 0.7591f, 0.7800f, 0.8033f, 0.9747f, 0.8777f },
                },
            },
            {
                {
                    { 0.8826f, 0.9244f, 0.9349f, 0.9455f, 0.9509f },
                    { 0.8354f, 0.8785f, 0.8962f, 0.9320f, 0.9485f },
                    { 0.7811f, 0.8286f, 0.8461f, 0.9410f, 0.9739f },
                },
                {
                    { 0.8065f, 0.8647f, 0.8875f, 0.8957f, 0.9061f },
                    { 0.7657f, 0.8184f, 0.8427f, 0.9114f, 0.9433f },
                    { 0.6949f, 0.7524f, 0.7805f, 0.9329f, 0.9684f },
                },
                {
                    { 0.9018f, 0.9261f, 0.9313f, 0.9097f, 0.8935f },
                    { 0.8950f, 0.8989f, 0.8905f, 0.9371f, 0.8751f },
                    { 0.8097f, 0.8139f, 0.8199f, 0.9559f, 0.8884f },
                },
            },
            {
                {
                    { 0.8925f, 0.9231f, 0.9251f, 0.9314f, 0.9315f },
                    { 0.8512f, 0.8858f, 0.8966f, 0.9273f, 0.9403f },
                    { 0.7737f, 0.8168f, 0.8281f, 0.9371f, 0.9700f },
                },
                {
                    { 0.8170f, 0.8684f, 0.8826f, 0.8845f, 0.8909f },
                    { 0.7874f, 0.8348f, 0.8507f, 0.9114f, 0.9396f },
                    { 0.6924f, 0.7438f, 0.7637f, 0.9324f, 0.9739f },
                },
                {
                    { 0.9213f, 0.9428f, 0.9455f, 0.9192f, 0.8925f },
                    { 0.9258f, 0.9256f, 0.9123f, 0.9403f, 0.8833f },
                    { 0.8585f, 0.8482f, 0.8415f, 0.9524f, 0.8906f },
                },
            },
        },
        {
            {
                {
                    { 0.7585f, 0.7546f, 0.7466f, 0.7706f, 0.8113f },
                    { 0.7437f, 0.7428f, 0.7410f, 0.7654f, 0.8049f },
                    { 0.7388f, 0.7408f, 0.7480f, 0.7755f, 0.8116f },
                },
                {
                    { 0.7655f, 0.7618f, 0.7570f, 0.7839f, 0.8190f },
                    { 0.7493f, 0.7486f, 0.7493f, 0.7755f, 0.8093f },
                    { 0.7430f, 0.7453f, 0.7538f, 0.7819f, 0.8139f },
                },
                {
                    { 0.7862f, 0.7847f, 0.7853f, 0.8119f, 0.8453f },
                    { 0.7689f, 0.7698f, 0.7756f, 0.8025f, 0.8347f },
                    { 0.7601f, 0.7640f, 0.7772f, 0.8067f, 0.8369f },
                },
            },
            {
                {
                    { 0.6847f, 0.6784f, 0.6976f, 0.7974f, 0.8618f },
                    { 0.6611f, 0.6601f, 0.6783f, 0.7762f, 0.8642f },
                    { 0.6743f, 0.6774f, 0.6958f, 0.7907f, 0.8520f },
                },
                {
                    { 0.6988f, 0.6952f, 0.7200f, 0.8203f, 0.8630f },
                    { 0.6689f, 0.6696f, 0.6942f, 0.7916f, 0.8591f },
                    { 0.6769f, 0.6804f, 0.7035f, 0.7964f, 0.8460f },
                },
                {
                    { 0.7445f, 0.7452f, 0.7692f, 0.8499f, 0.8803f },
                    { 0.7091f, 0.7138f, 0.7423f, 0.8224f, 0.8733f },
                    { 0.7092f, 0.7174f, 0.7500f, 0.8268f, 0.8588f },
                },
            },
            {
                {
                    { 0.6333f, 0.6278f, 0.6867f, 0.8472f, 0.8796f },
                    { 0.6212f, 0.6116f, 0.6621f, 0.8240f, 0.8863f },
                    { 0.7464f, 0.7289f, 0.7107f, 0.8699f, 0.8817f },
                },
                {
                    { 0.6544f, 0.6529f, 0.7246f, 0.8697f, 0.8791f },
                    { 0.6193f, 0.6221f, 0.6842f, 0.8335f, 0.8783f },
                    { 0.6833f, 0.6734f, 0.7162f, 0.8621f, 0.8728f },
                },
                {
                    { 0.7267f, 0.7318f, 0.7877f, 0.8923f, 0.8949f },
                    { 0.6796f, 0.6895f, 0.7486f, 0.8563f, 0.8893f },
                    { 0.7166f, 0.7285f, 0.7802f, 0.8780f, 0.8794f },
                },
            },
            {
                {
                    { 0.5921f, 0.5913f, 0.6923f, 0.8839f, 0.8842f },
                    { 0.6123f, 0.5857f, 0.6743f, 0.8723f, 0.8925f },
                    { 0.8519f, 0.8266f, 0.7789f, 0.9564f, 0.9013f },
                },
                {
                    { 0.6201f, 0.6229f, 0.7435f, 0.9013f, 0.8819f },
                    { 0.5880f, 0.5939f, 0.6983f, 0.8689f, 0.8824f },
                    { 0.7485f, 0.7161f, 0.7798f, 0.9325f, 0.8912f },
                },
                {
                    { 0.7185f, 0.7289f, 0.8128f, 0.9176f, 0.8976f },
                    { 0.6676f, 0.6839f, 0.7719f, 0.8810f, 0.8925f },
                    { 0.7773f, 0.7906f, 0.8487f, 0.9262f, 0.8937f },
                },
            },
        },
        {
            {
                {
                    { 0.8867f, 0.9054f, 0.9442f, 0.9790f, 0.9865f },
                    { 0.8674f, 0.8817f, 0.9130f, 0.9475f, 0.9557f },
                    { 0.8527f, 0.8589f, 0.8782f, 0.9101f, 0.9180f },
                },
                {
                    { 0.8534f, 0.8889f, 0.9470f, 0.9824f, 0.9896f },
                    { 0.8301f, 0.8579f, 0.9089f, 0.9478f, 0.9574f },
                    { 0.8136f, 0.8296f, 0.8661f, 0.9059f, 0.9171f },
                },
                {
                    { 0.8454f, 0.8881f, 0.9523f, 0.9863f, 0.9924f },
                    { 0.8208f, 0.8539f, 0.9114f, 0.9507f, 0.9603f },
                    { 0.8040f, 0.8233f, 0.8654f, 0.9066f, 0.9185f },
                },
            },
            {
                {
                    { 0.8072f, 0.8454f, 0.8948f, 0.9465f, 0.9547f },
                    { 0.7784f, 0.8115f, 0.8549f, 0.9151f, 0.9307f },
                    { 0.7595f, 0.7808f, 0.8101f, 0.8682f, 0.8977f },
                },
                {
                    { 0.7553f, 0.8225f, 0.8943f, 0.9518f, 0.9606f },
                    { 0.7248f, 0.7814f, 0.8463f, 0.9191f, 0.9352f },
                    { 0.7083f, 0.7469f, 0.7936f, 0.8667f, 0.8994f },
                },
                {
                    { 0.7521f, 0.8287f, 0.9034f, 0.9577f, 0.9654f },
                    { 0.7228f, 0.7862f, 0.8544f, 0.9247f, 0.9400f },
                    { 0.7083f, 0.7510f, 0.8007f, 0.8708f, 0.9026f },
                },
            },
            {
                {
                    { 0.7449f, 0.8016f, 0.8644f, 0.9328f, 0.9395f },
                    { 0.7107f, 0.7620f, 0.8185f, 0.9016f, 0.9151f },
                    { 0.6909f, 0.7278f, 0.7660f, 0.8520f, 0.8859f },
                },
                {
                    { 0.6832f, 0.7799f, 0.8675f, 0.9385f, 0.9435f },
                    { 0.6576f, 0.7354f, 0.8151f, 0.9070f, 0.9184f },
                    { 0.6397f, 0.6993f, 0.7552f, 0.8534f, 0.8870f },
                },
                {
                    { 0.7076f, 0.8019f, 0.8837f, 0.9448f, 0.9470f },
                    { 0.7005f, 0.7596f, 0.8345f, 0.9120f, 0.9223f },
                    { 0.6926f, 0.7244f, 0.7782f, 0.8589f, 0.8895f },
                },
            },
            {
                {
                    { 0.6929f, 0.7672f, 0.8503f, 0.9383f, 0.9355f },
                    { 0.6554f, 0.7243f, 0.7988f, 0.9097f, 0.9122f },
                    { 0.6359f, 0.6880f, 0.7374f, 0.8587f, 0.8874f },
                },
                {
                    { 0.6730f, 0.7535f, 0.8612f, 0.9429f, 0.9393f },
                    { 0.6643f, 0.7087f, 0.8057f, 0.9129f, 0.9152f },
                    { 0.6424f, 0.6714f, 0.7379f, 0.8619f, 0.8870f },
                },
                {
                    { 0.7592f, 0.8012f, 0.8841f, 0.9491f, 0.9431f },
                    { 0.7506f, 0.7630f, 0.8376f, 0.9191f, 0.9191f },
                    { 0.7303f, 0.7271f, 0.7812f, 0.8677f, 0.8907f },
                },
            },
        },
        {
            {
                {
                    { 0.2889f, 0.2749f, 0.2820f, 0.3629f, 0.4409f },
                    { 0.4049f, 0.5004f, 0.6180f, 0.6488f, 0.6829f },
                    { 0.5752f, 0.6331f, 0.7645f, 0.8521f, 0.9251f },
                },
                {
                    { 0.3000f, 0.2788f, 0.2576f, 0.3283f, 0.4351f },
                    { 0.4012f, 0.4458f, 0.5765f, 0.6361f, 0.6804f },
                    { 0.6390f, 0.5770f, 0.7001f, 0.8053f, 0.9140f },
                },
                {
                    { 0.3106f, 0.2821f, 0.2588f, 0.3158f, 0.4327f },
                    { 0.4196f, 0.4315f, 0.5606f, 0.6316f, 0.6795f },
                    { 0.6717f, 0.5652f, 0.6798f, 0.7875f, 0.9093f },
                },
            },
            {
                {
                    { 0.1503f, 0.1673f, 0.2431f, 0.3886f, 0.4515f },
                    { 0.2802f, 0.4798f, 0.6479f, 0.6803f, 0.6930f },
                    { 0.5909f, 0.7645f, 0.9809f, 1.0082f, 0.9890f },
                },
                {
                    { 0.1369f, 0.1546f, 0.2036f, 0.3519f, 0.4505f },
                    { 0.2614f, 0.3574f, 0.6151f, 0.6798f, 0.6928f },
                    { 0.6231f, 0.6117f, 0.9165f, 1.0026f, 0.9893f },
                },
                {
                    { 0.1361f, 0.1589f, 0.2047f, 0.3334f, 0.4505f },
                    { 0.2541f, 0.3256f, 0.5978f, 0.6795f, 0.6927f },
                    { 0.6973f, 0.5778f, 0.8838f, 0.9970f, 0.9891f },
                },
            },
            {
                {
                    { 0.1074f, 0.1287f, 0.2573f, 0.4024f, 0.4615f },
                    { 0.2475f, 0.5542f, 0.6687f, 0.6926f, 0.6942f },
                    { 0.7131f, 0.9852f, 1.0867f, 1.0686f, 1.0321f },
                },
                {
                    { 0.1110f, 0.1350f, 0.1893f, 0.3824f, 0.4619f },
                    { 0.1851f, 0.3687f, 0.6552f, 0.6941f, 0.6944f },
                    { 0.6113f, 0.7655f, 1.0756f, 1.0713f, 1.0369f },
                },
                {
                    { 0.1006f, 0.1395f, 0.1883f, 0.3702f, 0.4623f },
                    { 0.1680f, 0.3163f, 0.6420f, 0.6947f, 0.6943f },
                    { 0.7029f, 0.6930f, 1.0639f, 1.0707f, 1.0376f },
                },
            },
            {
                {
                    { 0.0991f, 0.1265f, 0.2817f, 0.4083f, 0.4636f },
                    { 0.2270f, 0.6181f, 0.6723f, 0.6986f, 0.6942f },
                    { 0.8889f, 1.1004f, 1.1157f, 1.1005f, 1.0517f },
                },
                {
                    { 0.0962f, 0.1242f, 0.1901f, 0.3976f, 0.4640f },
                    { 0.1241f, 0.4147f, 0.6697f, 0.7013f, 0.6945f },
                    { 0.6202f, 0.9584f, 1.1188f, 1.1039f, 1.0587f },
                },
                {
                    { 0.0810f, 0.1268f, 0.1857f, 0.3914f, 0.4646f },
                    { 0.1192f, 0.3380f, 0.6608f, 0.7023f, 0.6944f },
                    { 0.7447f, 0.8693f, 1.1170f, 1.1025f, 1.0610f },
                },
            },
        },
        {
        {
            {
                { 0.1424f, 0.1395f, 0.2716f, 0.2631f, 0.2526f },
                { 0.4120f, 0.3966f, 0.7425f, 0.7223f, 0.6912f },
                { 0.1341f, 0.1379f, 0.1567f, 0.1471f, 0.1375f },
            },
            {
                { 0.1494f, 0.1455f, 0.2684f, 0.2615f, 0.2525f },
                { 0.4081f, 0.3988f, 0.7406f, 0.7216f, 0.6911f },
                { 0.1556f, 0.1299f, 0.1433f, 0.1443f, 0.1374f },
            },
            {
                { 0.1638f, 0.1552f, 0.2606f, 0.2574f, 0.2523f },
                { 0.4059f, 0.4048f, 0.7354f, 0.7196f, 0.6910f },
                { 0.3568f, 0.1500f, 0.1113f, 0.1323f, 0.1364f },
            },
        },
        {
            {
                { 0.0583f, 0.1563f, 0.3231f, 0.3044f, 0.2914f },
                { 0.3040f, 0.6412f, 0.7724f, 0.7318f, 0.6905f },
                { 0.1414f, 0.2057f, 0.2202f, 0.2129f, 0.2063f },
            },
            {
                { 0.0607f, 0.1495f, 0.3234f, 0.3046f, 0.2912f },
                { 0.3027f, 0.6382f, 0.7720f, 0.7316f, 0.6905f },
                { 0.0954f, 0.1487f, 0.2079f, 0.2118f, 0.2061f },
            },
            {
                { 0.0627f, 0.1281f, 0.3242f, 0.3053f, 0.2909f },
                { 0.3145f, 0.6299f, 0.7710f, 0.7311f, 0.6904f },
                { 0.2349f, 0.0529f, 0.1284f, 0.2077f, 0.2060f },
            },
        },
        {
            {
                { 0.0548f, 0.2485f, 0.3375f, 0.3183f, 0.3057f },
                { 0.3577f, 0.7772f, 0.7789f, 0.7354f, 0.6907f },
                { 0.2215f, 0.2767f, 0.2547f, 0.2427f, 0.2317f },
            },
            {
                { 0.0545f, 0.2288f, 0.3387f, 0.3186f, 0.3056f },
                { 0.3699f, 0.7763f, 0.7786f, 0.7352f, 0.6906f },
                { 0.1199f, 0.2278f, 0.2527f, 0.2423f, 0.2340f },
            },
            {
                { 0.0508f, 0.1843f, 0.3418f, 0.3197f, 0.3056f },
                { 0.3915f, 0.7734f, 0.7779f, 0.7348f, 0.6906f },
                { 0.1420f, 0.0367f, 0.2033f, 0.2421f, 0.2339f },
            },
        },
        {
            {
                { 0.0661f, 0.3335f, 0.3427f, 0.3280f, 0.3143f },
                { 0.4671f, 0.8044f, 0.7793f, 0.7354f, 0.6906f },
                { 0.2555f, 0.3153f, 0.2712f, 0.2536f, 0.2439f },
            },
            {
                { 0.0574f, 0.3135f, 0.3436f, 0.3296f, 0.3144f },
                { 0.4894f, 0.8045f, 0.7791f, 0.7352f, 0.6906f },
                { 0.1760f, 0.2814f, 0.2724f, 0.2557f, 0.2436f },
            },
            {
                { 0.0516f, 0.2574f, 0.3501f, 0.3318f, 0.3144f },
                { 0.5188f, 0.8044f, 0.7785f, 0.7348f, 0.6905f },
                { 0.1110f, 0.0351f, 0.2553f, 0.2557f, 0.2456f },
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


inline float getBroadbandDriveReferenceTrim (Model model, float drive) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);

    switch (model)
    {
        case Model::Tape:
            return detail::interpDrive5 (d, 0.8794f, 0.6841f, 0.5592f, 0.5027f, 0.5375f);

        case Model::Tube:
            return detail::interpDrive5 (d, 0.8907f, 0.9079f, 0.7788f, 0.7045f, 0.4813f);

        case Model::Transistor:
            return detail::interpDrive5 (d, 0.9118f, 0.7075f, 0.5081f, 0.4440f, 0.4627f);

        case Model::Diode:
            return detail::interpDrive5 (d, 1.0612f, 0.8826f, 0.6549f, 0.5420f, 0.4897f);

        default:
            return 1.0f;
    }
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

    return detail::morphThreeWay (g, charTrim[0], charTrim[1], charTrim[2])
         * getBroadbandDriveReferenceTrim (model, d);
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

    const float girthCurve = 1.0f - std::pow (1.0f - detail::clampF (girth, 0.0f, 1.0f), 1.35f);
    const float density = detail::fastTanh (shaped * (1.0f + girth * 1.55f));
    const float body = shaped + shaped * (1.0f - std::min (1.0f, std::abs (shaped)))
                                 * girth * 0.18f;
    const float tapeLike = density * 0.82f + body * 0.18f;
    return juce::jmap (girthCurve, shaped, tapeLike);
}

inline float applyTriodeGirth (float shaped, float girth) noexcept
{
    if (girth < 0.01f) return shaped;

    const float g = detail::clampF (girth, 0.0f, 1.0f);
    const float g2 = g * g;
    const float density = detail::fastTanh (shaped * (1.0f + g * 0.70f));
    const float body = shaped + shaped * (1.0f - std::min (1.0f, std::abs (shaped)))
                                 * (0.045f + g * 0.065f);
    const float oddDensity = shaped + shaped * std::abs (shaped) * (0.016f + g * 0.045f);
    const float thick = density * 0.70f + body * 0.18f + oddDensity * 0.12f;
    const float out = juce::jmap (g2 * 0.82f, shaped, thick);
    return out * (1.0f - g2 * 0.10f);
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
        case Model::OverdriveA:
        {
            // OVERDRIVE A keeps the TS808 analysis contract: no TYPE-driven
            // pre-emphasis morph. TYPE is reserved for diode smoothing inside
            // the TS feedback core.
            return x;
        }
        case Model::OverdriveB:
        {
            st.preHP += (x - st.preHP) * ec.preHP;
            const float hp = x - st.preHP;
            st.preSh += (hp - st.preSh) * ec.preSh;
            const float edge = hp - st.preSh;
            const float lowRetain = 0.74f + drive * 0.20f;
            return juce::jmap (lowRetain, x, hp)
                 + edge * (0.0008f + drive * 0.0014f);
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
        case Model::OverdriveA:
        {
            return y;
        }
        case Model::OverdriveB:
        {
            st.postLP += (y - st.postLP) * ec.postLP;
            const float klonBase = y + (st.postLP - y) * (0.42f + drive * 0.45f);
            const float bright = y - st.postLP;
            return klonBase + bright * ((1.0f - drive) * 0.002f);
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

inline bool hasExtendedDriveRange (Model model) noexcept
{
    return model == Model::Tape
        || model == Model::Tube
        || model == Model::Transistor
        || model == Model::Diode;
}

inline float applyDriveCurve (float driveParam, Model model) noexcept
{
    float exp;
    switch (model)
    {
        case Model::Tube:        exp = 1.85f; break;
        case Model::Transistor:  exp = 0.62f; break;
        case Model::Diode:       exp = 1.3f; break;
        case Model::OverdriveA:
        case Model::OverdriveB:
        case Model::Clipper:    exp = 1.0f; break;
        case Model::Tape:        exp = 1.0f; break;
        default:                 exp = 1.5f; break;
    }
    return std::pow (driveParam, exp);
}

inline float mapDriveParamToEffective (float driveParam, Model model) noexcept
{
    const float scaled = detail::clampF (driveParam, 0.0f, 1.0f)
                       * (hasExtendedDriveRange (model) ? 2.0f : 1.0f);
    const float base = applyDriveCurve (detail::clampF (scaled, 0.0f, 1.0f), model);
    const float extra = std::max (0.0f, scaled - 1.0f);
    return base + extra;
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
    const float driveOver = std::max (0.0f, drive - 1.0f);
    const float driveExtraGain = 1.0f + driveOver;
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
    // Treat BIAS as the tube operating-point control. Keep the full UI range
    // useful instead of hard-clamping by ~33%, otherwise the polarity extremes
    // stop behaving like mirrored bias points.
    const float bEff = detail::clampF (b * 1.25f, -1.0f, 1.0f);
    float bodyControl = g;
    constexpr float bodyUpperPivot = 2.0f / 3.0f;
    if (bodyControl > bodyUpperPivot)
    {
        const float t = (bodyControl - bodyUpperPivot) / (1.0f - bodyUpperPivot);
        bodyControl = bodyUpperPivot + (1.0f - bodyUpperPivot) * std::pow (t, 1.55f);
    }
    const float bodyCurve = 1.0f - std::pow (1.0f - bodyControl, 1.80f);
    const float bodyImpact = 1.0f + bodyCurve * 0.78f;

    {
        const float bodyPreHz12AX7 = 210.0f - d * 45.0f;
        const float bodyPreHzPower = 170.0f - d * 35.0f;
        const float bodyPreHz = juce::jmap (tubeMorph, bodyPreHz12AX7, bodyPreHzPower);
        const float bodyPreCoeff = detail::onePoleCoeff (bodyPreHz, sr);
        bodyPreLp += (xStage - bodyPreLp) * bodyPreCoeff;

        const float lfFeedAmt12AX7 = 0.08f + d * 0.10f;
        const float lfFeedAmtPower = 0.06f + d * 0.08f;
        const float lfFeedAmt = juce::jmap (tubeMorph, lfFeedAmt12AX7, lfFeedAmtPower)
                              * bodyImpact;
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
                                                      0.42f, 0.54f, 0.72f, 1.02f, 2.95f);
    const float inputPadPower = detail::interpDrive5 (d,
                                                      0.48f, 0.61f, 0.79f, 1.10f, 3.10f);
    const float inputPad = juce::jmap (tubeMorph, inputPad12AX7, inputPadPower) * driveExtraGain;
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
    const float biasAbs = std::abs (bEff);
    const float biasDrive = biasAbs * (0.62f + d * 0.38f);
    const float burnBiasShift = burnCore * (0.004f + tubeMorph * 0.006f);
    const float userStageBias12AX7 = bEff * (0.250f + d * 0.055f);
    const float userStageBiasPower = bEff * (0.245f + d * 0.065f + tubeMorph * 0.030f);
    const float sagStageBias12AX7 = -sagCore * 0.095f - supplyCore * 0.040f - burnBiasShift;
    const float sagStageBiasPower = -sagCore * 0.072f - supplyCore * 0.055f - burnBiasShift * 1.25f;
    const float userStageBias = juce::jmap (tubeMorph, userStageBias12AX7, userStageBiasPower);
    const float stageBias = userStageBias + juce::jmap (tubeMorph, sagStageBias12AX7, sagStageBiasPower);

    const float cathodeDepth12AX7 = bodyCurve * (0.040f + d * 0.050f);
    const float cathodeDepthPower = bodyCurve * (0.026f + d * 0.032f);
    const float cathodeDepth = juce::jmap (tubeMorph, cathodeDepth12AX7, cathodeDepthPower)
                             * (1.0f + bodyCurve * 0.32f);
    const float bloomHeadroomRecovery = bloomRecoveryCore * (0.066f + tubeMorph * 0.154f);

    const float headroom12AX7 = juce::jlimit (0.54f, 1.0f,
                                              1.0f - sagCore * 0.40f
                                                    - supplyCore * 0.16f
                                                    - biasAbs * 0.075f);
    const float headroomPower = juce::jlimit (0.58f, 1.08f,
                                              1.04f - sagCore * 0.28f
                                                     - supplyCore * 0.18f
                                                     - biasAbs * 0.105f);
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
    const float userBiasReference = detail::clampF (userStageBias / stageHeadroom, -1.0f, 1.0f);


    const float iterations12AX7 = 1.0f - d;
    const float iterationsPower = juce::jlimit (0.0f, 1.0f, 1.0f - d * 0.82f);
    const float iterations = juce::jmap (tubeMorph, iterations12AX7, iterationsPower);
    const int powerFactorBase = juce::jlimit (1, 10, 1 + (int) std::floor (9.0f * iterations));
    const int sagPowerDrop = juce::jlimit (0, 4, (int) std::floor (sagCore * juce::jmap (tubeMorph, 4.0f, 2.5f) + 0.35f));
    const int powerFactor = juce::jlimit (1, 10, powerFactorBase - sagPowerDrop);
    const float asymPad = (float) powerFactor;
    const float gainScaling = 1.0f / (float) (powerFactor + 1);

    // First Tube2 asymmetry section.
    const float asymAmt12AX7 = 0.25f + sagCore * 0.36f + biasDrive * 0.64f;
    const float asymAmtPower = 0.18f + sagCore * 0.24f + biasDrive * 0.78f;
    const float asymAmt = juce::jmap (tubeMorph, asymAmt12AX7, asymAmtPower)
                        + juce::jmap (tubeMorph,
                                      bodyCurve * (0.026f + d * 0.018f),
                                      bodyCurve * (0.015f + d * 0.012f));
    s = detail::tube2AsymSection (s, asymPad, asymAmt);
    // Original Tube curve.
    s = detail::airwindowsTubeCurve (s, powerFactor);
    if (biasAbs > 0.0001f)
    {
        float biasReference = detail::tube2AsymSection (userBiasReference, asymPad, asymAmt);
        biasReference = detail::airwindowsTubeCurve (biasReference, powerFactor);

        const auto addSignedEven = [bEff, d, tubeMorph] (float v) noexcept
        {
            const float signedEven = bEff * (v * v) / (0.85f + std::abs (v))
                                   * (0.045f + d * 0.090f + tubeMorph * 0.075f);
            return detail::clampF (v + signedEven, -1.24f, 1.24f);
        };

        s = addSignedEven (s);
        biasReference = addSignedEven (biasReference);
        s = detail::clampF (s - biasReference, -1.24f, 1.24f);
    }


    if (tubeMorph > 0.001f)
    {
        const float hotness = detail::clampF (0.55f + biasAbs * 0.18f, 0.0f, 1.0f);
        const float idleBias = bEff * (0.010f + hotness * (0.012f + d * 0.010f));
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

        const auto makePowerShape = [powerGain, idleBias, crossover, bEff, d, tubeMorph, hotness] (float v) noexcept
        {
            const float posV = v * powerGain + idleBias;
            const float negV = -v * powerGain + idleBias;
            const float posC = detail::smoothRect (posV, crossover);
            const float negC = detail::smoothRect (negV, crossover);

            float y = posC - negC;
            y += bEff * (v * v) * (0.060f + d * 0.115f + tubeMorph * 0.045f);
            y += v * std::abs (v) * (0.010f + tubeMorph * 0.035f);
            return detail::clampF (y * (0.92f + hotness * 0.08f), -1.20f, 1.20f);
        };

        float powerShape = makePowerShape (s);
        if (biasAbs > 0.0001f)
            powerShape = detail::clampF (powerShape - makePowerShape (0.0f), -1.20f, 1.20f);

        // Local power-stage negative feedback. This belongs mostly to the
        // power/tube side of TYPE: it trades some open-loop dirt for tighter
        // damping and lower nonlinear residue, without making the 12AX7 end
        // of the morph feel hi-fi or sterile.
        const float nfbAmt = tubeMorph2 * (0.035f + d * 0.075f)
                           * (1.0f - biasAbs * 0.10f)
                           * (1.0f + react * 0.10f);
        if (nfbAmt > 0.0001f)
        {
            const float forwardDelta = powerShape - s;
            powerShape = detail::clampF (powerShape - forwardDelta * nfbAmt, -1.20f, 1.20f);
        }

        const float satDrive = 1.0f + tubeMorph * (0.10f + 0.18f * d);
        const float satK = 0.85f + d * (0.55f + 0.25f * tubeMorph);
        const float satRaw = adaaState.process (powerShape * satDrive, satK);
        const float satNorm = detail::normalizeSmallSignal (satRaw, 0.0f, satK * satDrive);
        const float powerMix = tubeMorph * (0.35f + 0.35f * d) * (1.0f - nfbAmt * 0.16f);
        s = juce::jmap (powerMix, s, satNorm);
    }

    if (bodyCurve > 0.0001f)
    {
        const float grit = detail::smoothStep01 (bodyCurve);
        const float absS = std::abs (s);
        const float oddEdge = s * absS * grit * (0.020f + d * 0.060f + tubeMorph * 0.018f);
        const float cubicEdge = s * s * s * grit * (0.006f + d * 0.026f);
        const float evenEdge = bEff * (s * s) / (1.0f + absS)
                           * grit * (0.055f + d * 0.135f + tubeMorph * 0.042f);
        s = detail::clampF (s + oddEdge + cubicEdge + evenEdge, -1.30f, 1.30f);
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
        const float depthAmt = juce::jmap (tubeMorph, depthAmt12AX7, depthAmtPower)
                             * (1.0f + bodyCurve * 0.28f);
        const float depth = bodyPostLp * bodyCurve * depthAmt;
        s = (s + depth) / (1.0f + bodyCurve * depthAmt * 0.35f);
    }

    s *= atrophyGain;

    if (biasAbs > 0.0001f)
    {
        // Small residual even-order curvature only. The main bias behaviour is
        // already generated by the operating-point shift and its zero reference;
        // keeping this low avoids a second post-coupling bias stage.
        const float signedCurve = bEff * (0.026f + d * 0.052f + tubeMorph * 0.022f);
        const float evenCurve = (s * s) / (0.42f + std::abs (s));
        s = detail::clampF (s + evenCurve * signedCurve, -1.34f, 1.34f);
    }



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
    auto& compState = state.dynamicsComp[sp][ch];
    auto& peakCatchState = state.transistorPeakCatch[sp][ch];
    auto& coreAdaa = state.transistorCoreAdaa[sp][ch];
    juce::ignoreUnused (rawMode);

    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float driveOver = std::max (0.0f, drive - 1.0f);
    const float driveExtraGain = 1.0f + driveOver;
    const float body = detail::clampF (girth, 0.0f, 1.0f);
    const float b = detail::clampF (bias, -1.0f, 1.0f);
    const float type = detail::smoothStep01 (detail::clampF (0.5f + (mod - 0.5f) * 1.35f, 0.0f, 1.0f));
    const float bodyToneCurve = 1.0f - std::pow (1.0f - body, 2.15f);
    const float bodyClipCurve = 1.0f - std::pow (1.0f - body, 1.35f);
    const float degenerationLift = bodyToneCurve * juce::jmap (type, 0.34f, 0.22f);
    const float emitterGain = 1.0f + degenerationLift * (0.72f + d * 0.24f);
    const float coreDegeneration = 1.0f - degenerationLift * (0.28f + d * 0.10f);
    auto trackDbg = [] (float& dst, float v) noexcept
    {
        dst = std::max (dst, std::abs (v));
    };

    const float inputPadBjt = detail::interpDrive5 (d,
                                                    0.14f, 0.30f, 0.68f, 1.32f, 5.50f)
                            * juce::jmap (bodyToneCurve, 1.00f, 1.22f) * emitterGain;
    const float inputPadFet = detail::interpDrive5 (d,
                                                    0.18f, 0.36f, 0.74f, 1.26f, 4.40f)
                            * juce::jmap (bodyToneCurve, 1.00f, 1.16f) * juce::jmap (type, emitterGain, 1.0f + degenerationLift * 0.42f);
    const float inputPad = juce::jmap (type, inputPadBjt, inputPadFet) * driveExtraGain;

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
                                                  - bodyClipCurve * 0.085f
                                                  - juce::jmax (0.0f, b) * 0.055f
                                                  + juce::jmax (0.0f, -b) * 0.040f);
    const float headroomFet = juce::jlimit (0.50f, 1.16f,
                                            1.10f - d * 0.44f
                                                  - bodyClipCurve * 0.060f
                                                  - juce::jmax (0.0f, b) * 0.040f
                                                  + juce::jmax (0.0f, -b) * 0.026f);
    const float headroom = juce::jmap (type, headroomBjt, headroomFet);

    const float requestedOpBias = juce::jmap (type, b * 0.34f, b * 0.28f);
    const float biasLimit = headroom * juce::jmap (type, 0.44f, 0.36f) * (1.0f - d * 0.08f);
    const float opBias = detail::clampF (requestedOpBias, -biasLimit, biasLimit);
    const float biasNormScale = juce::jmax (1.0e-4f, juce::jmap (type, 0.34f, 0.28f));
    const float bEff = detail::clampF (opBias / biasNormScale, -1.0f, 1.0f);
    const float evenAmt = bEff * (0.090f + d * 0.155f + bodyClipCurve * 0.120f);
    const float oddAmt = juce::jmap (type,
                                     0.026f + bodyClipCurve * 0.032f + d * 0.052f,
                                    -0.010f - bodyClipCurve * 0.014f - d * 0.018f) * coreDegeneration;
    const float cubicAmt = juce::jmap (type,
                                       0.012f + bodyClipCurve * 0.012f + d * 0.052f,
                                       0.020f + bodyClipCurve * 0.010f + d * 0.030f)
                         * (1.0f + degenerationLift * juce::jmap (type, 0.48f, 0.24f));

    auto applyCoreShape = [oddAmt, cubicAmt, evenAmt] (float v) noexcept
    {
        return v
             + v * v * evenAmt
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
                          + 2.0f * biasNorm * evenAmt
                          + 2.0f * std::abs (biasNorm) * oddAmt
                          + 3.0f * biasNorm * biasNorm * cubicAmt;

    const float satK = juce::jmap (type,
                                   0.98f + d * (2.24f + bodyClipCurve * 0.24f),
                                   0.84f + d * (1.55f + bodyClipCurve * 0.14f))
                    * (1.0f + degenerationLift * juce::jmap (type, 0.26f, 0.12f));
    state.transistorDbgSatK[sp][ch] = satK;
    const float raw = coreAdaa.process (satK * shifted, 1.0f);
    const float raw0 = std::tanh (satK * z0);
    const float slopeRef = satK * (1.0f - raw0 * raw0);
    const float slope0 = std::max (1.0e-4f, slopeRef * preDeriv0 * (1.0f / headroom));
    float core = detail::normalizeSmallSignal (raw, raw0, slope0);
    if (std::abs (bEff) > 0.0001f)
    {
        const float symAbs = std::pow (std::abs (bEff), 0.62f);
        const float emitterAsym = juce::jmap (type, 1.08f, 0.70f);
        const float coreEven = (core * core) / (0.36f + std::abs (core));
        core += bEff * symAbs * coreEven
             * (0.115f + d * 0.235f + bodyClipCurve * 0.150f)
             * emitterAsym;
    }
    trackDbg (state.transistorDbgCoreOut[sp][ch], core);

    const float railDrive = juce::jmap (type,
                                        1.02f + d * 1.02f,
                                        0.96f + d * 0.68f)
                          * juce::jmap (bodyClipCurve, 1.00f, 1.080f);
    const float posThresh = juce::jmap (type, 1.24f - d * 0.24f,
                                              1.34f - d * 0.14f)
                          * (1.0f - juce::jmax (0.0f, bEff) * 0.090f
                                   + juce::jmax (0.0f, -bEff) * 0.030f);
    const float negThresh = juce::jmap (type, 1.16f - d * 0.22f,
                                              1.28f - d * 0.12f)
                          * (1.0f - juce::jmax (0.0f, -bEff) * 0.090f
                                   + juce::jmax (0.0f,  bEff) * 0.030f);
    const float kneeBase = juce::jmap (type,
                                       juce::jmap (bodyClipCurve, 0.24f, 0.17f),
                                       juce::jmap (bodyClipCurve, 0.30f, 0.22f));
    const float kneePos = std::max (1.0e-4f, kneeBase * (1.0f - d * 0.10f));
    const float kneeNeg = std::max (1.0e-4f, kneeBase * juce::jmap (type, 0.95f, 1.08f) * (1.0f - d * 0.08f));
    state.transistorDbgRailThresh[sp][ch] = 0.5f * (posThresh + negThresh);

    const float railSignal = core * railDrive;
    const float railBias = bEff * (0.052f + d * 0.095f + bodyClipCurve * 0.074f)
                         * juce::jmap (type, 1.00f, 0.66f);
    const float railIn = railSignal + railBias;
    trackDbg (state.transistorDbgRailIn[sp][ch], railIn);
    float out = clipAdaa.process (railIn, posThresh, negThresh, kneePos, kneeNeg);
    if (std::abs (railBias) > 1.0e-6f)
    {
        constexpr float eps = 1.0e-3f;
        const float railRaw0 = adaa::ClipperADAA::clip (railBias, posThresh, negThresh, kneePos, kneeNeg);
        const float yp = adaa::ClipperADAA::clip (railBias + eps, posThresh, negThresh, kneePos, kneeNeg);
        const float yn = adaa::ClipperADAA::clip (railBias - eps, posThresh, negThresh, kneePos, kneeNeg);
        const float slope = (yp - yn) / (2.0f * eps);
        const float slopeComp = juce::jlimit (0.72f, 1.35f, 1.0f / std::max (std::abs (slope), 0.55f));
        out = (out - railRaw0) * slopeComp;
    }
    if (std::abs (bEff) > 0.0001f)
    {
        const float symAbs = std::pow (std::abs (bEff), 0.62f);
        const float transistorSymTone = juce::jmap (type, 1.16f, 0.78f);
        const float evenWave = (out * out) / (0.30f + std::abs (out));
        out += bEff * symAbs * evenWave
             * (0.155f + d * 0.305f + bodyClipCurve * 0.170f)
             * transistorSymTone;
    }

    trackDbg (state.transistorDbgRailOut[sp][ch], out);

    trackDbg (state.transistorDbgPost[sp][ch], out);
    return out;
}

inline float processDiodeStage (float x, float drive, float girth, float bias, float mod,
                                float reactColor,
                                adaa::ClipperADAA& adaaState) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float driveOver = std::max (0.0f, drive - 1.0f);
    const float driveExtraGain = 1.0f + driveOver;
    const float c = detail::clampF (girth, 0.0f, 1.0f);
    const float s = detail::clampF (bias, -1.0f, 1.0f);
    const float symUser = detail::clampF (s, -1.0f, 1.0f);
    const float biasSign = std::abs (s) > 0.015f ? (s > 0.0f ? 1.0f : -1.0f) : 0.0f;
    const float t = detail::clampF (0.5f + (mod - 0.5f) * 1.32f, 0.0f, 1.0f);
    const float harmonic = detail::smoothStep01 (c);
    const float percolatorVoice = detail::smoothStep01 (detail::clampF ((t - 0.35f) / 0.65f, 0.0f, 1.0f));
    const float bridgeWork = detail::smoothStep01 (detail::clampF (reactColor * 1.55f, 0.0f, 1.0f));
    const float percolatorAmt = detail::clampF (
        harmonic * (0.58f + 0.88f * percolatorVoice)
            + bridgeWork * (0.22f + percolatorVoice * 0.40f + harmonic * 0.30f),
        0.0f, 1.0f);
    const float reactPolarity = biasSign * bridgeWork * (0.060f + d * 0.095f)
                              * (0.45f + percolatorVoice * 0.55f);
    const float percolatorIntrinsic = percolatorVoice * percolatorAmt * (0.055f + d * 0.045f);
    const float topologyPolarity = reactPolarity + percolatorIntrinsic;

    const float condCurve = 1.0f - std::pow (1.0f - c, 2.00f);
    const float condDriveCurve = 1.0f - std::pow (1.0f - c, 1.65f);
    const float condThreshold = juce::jmap (condCurve, 0.68f, 1.18f);
    const float condKnee = juce::jmap (condCurve, 0.25f, 0.055f);
    const float condDrive = juce::jmap (condDriveCurve, 1.00f, 1.34f);

    const float driveFb = detail::interpDrive5 (d, 1.00f, 1.45f, 2.80f, 5.80f, 19.00f) * driveExtraGain;
    const float driveHard = detail::interpDrive5 (d, 1.00f, 2.20f, 5.20f, 10.80f, 36.00f) * driveExtraGain;
    const float driveOpen = detail::interpDrive5 (d, 1.00f, 1.65f, 3.40f, 6.10f, 19.20f) * driveExtraGain;

    float driveGain = driveHard;
    float thresholdMul = 1.0f;
    float kneeMul = 1.0f;
    float cleanBlend = 0.0f;
    float voiceTrim = 1.0f;
    float symRange = 0.34f;
    float edgeShape = 0.0f;
    float evenShape = 0.0f;

    if (t <= 0.5f)
    {
        const float u = detail::smoothStep01 (t * 2.0f);
        driveGain = juce::jmap (u, driveFb, driveHard);
        thresholdMul = juce::jmap (u, 0.92f, 0.82f);
        kneeMul = juce::jmap (u, 1.35f, 0.58f);
        cleanBlend = juce::jmap (u, 0.0f, 0.02f);
        voiceTrim = juce::jmap (u, 1.00f, 0.96f);
        symRange = juce::jmap (u, 0.72f, 0.94f);
        edgeShape = juce::jmap (u, 0.10f, 0.03f);
        evenShape = juce::jmap (u, 0.12f, 0.20f);
    }
    else
    {
        const float u = detail::smoothStep01 ((t - 0.5f) * 2.0f);
        driveGain = juce::jmap (u, driveHard, driveOpen);
        thresholdMul = juce::jmap (u, 0.82f, 1.02f);
        kneeMul = juce::jmap (u, 0.58f, 1.00f);
        cleanBlend = juce::jmap (u, 0.02f, 0.07f);
        voiceTrim = juce::jmap (u, 0.96f, 1.07f);
        symRange = juce::jmap (u, 0.94f, 0.82f);
        edgeShape = juce::jmap (u, 0.03f, 0.06f);
        evenShape = juce::jmap (u, 0.20f, 0.38f);
    }

    thresholdMul *= 1.0f - bridgeWork * (0.085f + percolatorVoice * 0.040f);
    kneeMul      *= 1.0f + bridgeWork * (0.30f + harmonic * 0.22f);
    cleanBlend   *= 1.0f - bridgeWork * 0.45f;
    evenShape    *= 1.0f + harmonic * 0.60f + bridgeWork * (1.60f + harmonic * 1.20f);

    const float threshold = condThreshold * thresholdMul;
    const float knee = std::max (1.0e-4f, condKnee * kneeMul);

    const float diodeMismatch = detail::clampF (symUser * symRange + topologyPolarity, -0.86f, 0.86f);
    const float thresholdPos = detail::clampF (threshold * (1.0f + diodeMismatch), 0.20f, 1.72f);
    const float thresholdNeg = detail::clampF (threshold * (1.0f - diodeMismatch), 0.20f, 1.72f);
    const float kneeSkew = detail::clampF (symUser * (0.160f + percolatorVoice * 0.075f), -0.26f, 0.26f);
    const float kneePos = std::max (1.0e-4f, knee * (1.0f - kneeSkew));
    const float kneeNeg = std::max (1.0e-4f, knee * (1.0f + kneeSkew));

    float clipIn = x * driveGain * condDrive;
    clipIn *= 1.0f + bridgeWork * (0.12f + d * 0.22f);
    clipIn += x * std::abs (x) * edgeShape;
    const float evenPolarity = detail::clampF (symUser + topologyPolarity, -1.0f, 1.0f);
    const float evenIn = (clipIn * clipIn) / (1.0f + std::abs (clipIn));
    const float pairImbalance = detail::clampF (
        symUser * (0.42f + d * 0.62f + harmonic * 0.20f)
      + topologyPolarity * (0.48f + harmonic * 0.22f),
        -0.82f, 0.82f);
    clipIn += evenPolarity * evenIn * evenShape * (0.18f + percolatorAmt)
            + pairImbalance * evenIn * (0.48f + percolatorVoice * 0.42f);

    // SYM must bend diode conduction before the ADAA clipper, not only change
    // post level. Keep the neutral point bit-identical: explicitSym is zero at
    // SYM=0 and the branch is skipped.
    const float explicitSymPre = std::pow (std::abs (symUser), 0.58f);
    if (explicitSymPre > 1.0e-5f)
    {
        const float clipAbs = std::abs (clipIn);
        const float bendFocus = detail::smoothStep01 (
            clipAbs / (threshold * (0.72f + percolatorVoice * 0.22f) + 1.0e-4f));
        const float clipPolarity = clipIn >= 0.0f ? 1.0f : -1.0f;
        const float bendAmt = symUser * explicitSymPre
                            * (0.22f + d * 0.42f + harmonic * 0.18f + bridgeWork * 0.10f)
                            * juce::jmap (percolatorVoice, 1.12f, 0.96f);
        const float bendGain = juce::jlimit (0.42f, 1.76f,
                                             1.0f + bendAmt * clipPolarity * (0.28f + bendFocus * 0.72f));
        clipIn *= bendGain;

        const float bentEven = (clipIn * clipIn) / (1.0f + std::abs (clipIn));
        clipIn += symUser * explicitSymPre * bentEven
                * (0.085f + d * 0.185f + harmonic * 0.080f)
                * juce::jmap (percolatorVoice, 1.06f, 0.88f);
    }

    const float diodeBiasOffset = symUser * (0.070f + d * 0.150f + harmonic * 0.060f + percolatorAmt * 0.075f)
                                * (1.0f + bridgeWork * 0.18f);
    float clipped = adaaState.process (clipIn + diodeBiasOffset, thresholdPos, thresholdNeg,
                                       kneePos, kneeNeg);
    if (std::abs (diodeBiasOffset) > 1.0e-6f)
    {
        constexpr float eps = 1.0e-3f;
        const float raw0 = adaa::ClipperADAA::clip (diodeBiasOffset, thresholdPos, thresholdNeg, kneePos, kneeNeg);
        const float yp = adaa::ClipperADAA::clip (diodeBiasOffset + eps, thresholdPos, thresholdNeg, kneePos, kneeNeg);
        const float yn = adaa::ClipperADAA::clip (diodeBiasOffset - eps, thresholdPos, thresholdNeg, kneePos, kneeNeg);
        const float slope = (yp - yn) / (2.0f * eps);
        const float slopeComp = juce::jlimit (0.72f, 1.38f, 1.0f / std::max (std::abs (slope), 0.52f));
        clipped = (clipped - raw0) * slopeComp;
    }

    const float outputScale = (2.0f / (thresholdPos + thresholdNeg))
                            * (1.0f + std::abs (evenPolarity) * percolatorAmt * 0.06f);
    clipped *= outputScale;
    const float postEven = (clipped * clipped) / (1.0f + std::abs (clipped));

    // SYM is modelled as diode-pair threshold/conduction mismatch before ADAA.
    // This post-even term restores visible waveform bending after zero-reference
    // compensation, without turning the control into plain output DC.
    const float explicitSym = std::pow (std::abs (symUser), 0.62f);
    const float topologySym = juce::jmap (percolatorVoice, 1.0f, 0.72f);
    clipped += evenPolarity * postEven * explicitSym * topologySym
             * (0.200f + d * 0.315f + harmonic * 0.110f + bridgeWork * 0.085f);
    clipped += evenPolarity * postEven * percolatorAmt
             * (0.100f + d * 0.180f + harmonic * 0.060f + bridgeWork * (0.070f + harmonic * 0.105f));

    // At the open/percolator end, SYM should feel like one diode branch reaching
    // conduction earlier and bending that half-cycle, not like simple level tilt.
    const float openFoldAmt = percolatorVoice * explicitSym
                            * (0.22f + d * 0.46f + harmonic * 0.18f + bridgeWork * 0.12f);
    if (openFoldAmt > 0.0001f)
    {
        const float foldPolarity = evenPolarity >= 0.0f ? 1.0f : -1.0f;
        const float selectedHalf = clipped * foldPolarity;
        const float foldKnee = juce::jmap (d, 0.82f, 0.48f)
                            * juce::jmap (harmonic, 1.06f, 0.88f);
        if (selectedHalf > foldKnee)
        {
            const float over = selectedHalf - foldKnee;
            const float foldedHalf = foldKnee
                                   + over * (1.0f - openFoldAmt * 1.20f)
                                   - (over * over) * openFoldAmt / (0.30f + over);
            clipped = foldPolarity * foldedHalf;
        }
    }

    if (cleanBlend > 0.0001f)
    {
        const float clean = detail::clampF (x * juce::jmap (c, 0.96f, 1.06f), -1.30f, 1.30f);
        clipped = juce::jmap (cleanBlend, clipped, clean);
    }

    return clipped * voiceTrim;
}

inline bool klonBiquadNeedsUpdate (const KlonBiquadState& st, int type, float sr,
                                   float freqHz, float q, float gainDb, bool highShelf) noexcept
{
    return st.cachedType != type
        || st.cachedSr != sr
        || st.cachedFreq != freqHz
        || st.cachedQ != q
        || st.cachedGain != gainDb
        || st.cachedHighShelf != highShelf;
}

inline void cacheKlonBiquad (KlonBiquadState& st, int type, float sr,
                             float freqHz, float q, float gainDb, bool highShelf) noexcept
{
    st.cachedType = type;
    st.cachedSr = sr;
    st.cachedFreq = freqHz;
    st.cachedQ = q;
    st.cachedGain = gainDb;
    st.cachedHighShelf = highShelf;
}

inline float processCachedKlonBiquad (float x, KlonBiquadState& st) noexcept
{
    const float y = st.b0 * x + st.z1;
    st.z1 = st.b1 * x - st.a1 * y + st.z2;
    st.z2 = st.b2 * x - st.a2 * y;
    return std::isfinite (y) ? y : x;
}

inline void updateKlonPeakEqCoeffs (KlonBiquadState& st, float sr,
                                    float freqHz, float q, float gainDb) noexcept
{
    const float safeSr = std::max (sr, 1000.0f);
    const float f0 = detail::clampF (freqHz, 5.0f, safeSr * 0.45f);
    const float safeQ = std::max (q, 0.025f);
    const float w0 = kTwoPi * f0 / safeSr;
    const float cosW = std::cos (w0);
    const float sinW = std::sin (w0);
    const float alpha = sinW / (2.0f * safeQ);
    const float a = std::pow (10.0f, gainDb / 40.0f);
    const float a0 = 1.0f + alpha / a;

    st.b0 = (1.0f + alpha * a) / a0;
    st.b1 = (-2.0f * cosW) / a0;
    st.b2 = (1.0f - alpha * a) / a0;
    st.a1 = (-2.0f * cosW) / a0;
    st.a2 = (1.0f - alpha / a) / a0;
    cacheKlonBiquad (st, 1, sr, freqHz, q, gainDb, false);
}

inline float processKlonPeakEq (float x, KlonBiquadState& st, float sr,
                                float freqHz, float q, float gainDb) noexcept
{
    if (klonBiquadNeedsUpdate (st, 1, sr, freqHz, q, gainDb, false))
        updateKlonPeakEqCoeffs (st, sr, freqHz, q, gainDb);

    return processCachedKlonBiquad (x, st);
}

inline void updateKlonShelfEqCoeffs (KlonBiquadState& st, float sr,
                                     float freqHz, float q, float gainDb, bool highShelf) noexcept
{
    const float safeSr = std::max (sr, 1000.0f);
    const float f0 = detail::clampF (freqHz, 5.0f, safeSr * 0.45f);
    const float safeQ = std::max (q, 0.025f);
    const float w0 = kTwoPi * f0 / safeSr;
    const float cosW = std::cos (w0);
    const float sinW = std::sin (w0);
    const float a = std::pow (10.0f, gainDb / 40.0f);
    const float alpha = sinW / (2.0f * safeQ);
    const float beta = 2.0f * std::sqrt (a) * alpha;

    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a0 = 1.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;

    if (highShelf)
    {
        b0 = a * ((a + 1.0f) + (a - 1.0f) * cosW + beta);
        b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cosW);
        b2 = a * ((a + 1.0f) + (a - 1.0f) * cosW - beta);
        a0 = (a + 1.0f) - (a - 1.0f) * cosW + beta;
        a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cosW);
        a2 = (a + 1.0f) - (a - 1.0f) * cosW - beta;
    }
    else
    {
        b0 = a * ((a + 1.0f) - (a - 1.0f) * cosW + beta);
        b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cosW);
        b2 = a * ((a + 1.0f) - (a - 1.0f) * cosW - beta);
        a0 = (a + 1.0f) + (a - 1.0f) * cosW + beta;
        a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cosW);
        a2 = (a + 1.0f) + (a - 1.0f) * cosW - beta;
    }

    const float invA0 = 1.0f / std::max (a0, 1.0e-12f);
    st.b0 = b0 * invA0;
    st.b1 = b1 * invA0;
    st.b2 = b2 * invA0;
    st.a1 = a1 * invA0;
    st.a2 = a2 * invA0;
    cacheKlonBiquad (st, 2, sr, freqHz, q, gainDb, highShelf);
}

inline float processKlonShelfEq (float x, KlonBiquadState& st, float sr,
                                 float freqHz, float q, float gainDb, bool highShelf) noexcept
{
    if (klonBiquadNeedsUpdate (st, 2, sr, freqHz, q, gainDb, highShelf))
        updateKlonShelfEqCoeffs (st, sr, freqHz, q, gainDb, highShelf);

    return processCachedKlonBiquad (x, st);
}

inline void updateKlonLowPassEqCoeffs (KlonBiquadState& st, float sr,
                                       float freqHz, float q) noexcept
{
    const float safeSr = std::max (sr, 1000.0f);
    const float f0 = detail::clampF (freqHz, 20.0f, safeSr * 0.45f);
    const float safeQ = std::max (q, 0.025f);
    const float w0 = kTwoPi * f0 / safeSr;
    const float cosW = std::cos (w0);
    const float sinW = std::sin (w0);
    const float alpha = sinW / (2.0f * safeQ);

    float b0 = (1.0f - cosW) * 0.5f;
    float b1 = 1.0f - cosW;
    float b2 = (1.0f - cosW) * 0.5f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cosW;
    float a2 = 1.0f - alpha;

    const float invA0 = 1.0f / std::max (a0, 1.0e-12f);
    st.b0 = b0 * invA0;
    st.b1 = b1 * invA0;
    st.b2 = b2 * invA0;
    st.a1 = a1 * invA0;
    st.a2 = a2 * invA0;
    cacheKlonBiquad (st, 3, sr, freqHz, q, 0.0f, false);
}

inline float processKlonLowPassEq (float x, KlonBiquadState& st, float sr,
                                   float freqHz, float q) noexcept
{
    if (klonBiquadNeedsUpdate (st, 3, sr, freqHz, q, 0.0f, false))
        updateKlonLowPassEqCoeffs (st, sr, freqHz, q);

    return processCachedKlonBiquad (x, st);
}

inline void updateKlonHighPassEqCoeffs (KlonBiquadState& st, float sr,
                                        float freqHz, float q) noexcept
{
    const float safeSr = std::max (sr, 1000.0f);
    const float f0 = detail::clampF (freqHz, 20.0f, safeSr * 0.45f);
    const float safeQ = std::max (q, 0.025f);
    const float w0 = kTwoPi * f0 / safeSr;
    const float cosW = std::cos (w0);
    const float sinW = std::sin (w0);
    const float alpha = sinW / (2.0f * safeQ);

    float b0 = (1.0f + cosW) * 0.5f;
    float b1 = -(1.0f + cosW);
    float b2 = (1.0f + cosW) * 0.5f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * cosW;
    float a2 = 1.0f - alpha;

    const float invA0 = 1.0f / std::max (a0, 1.0e-12f);
    st.b0 = b0 * invA0;
    st.b1 = b1 * invA0;
    st.b2 = b2 * invA0;
    st.a1 = a1 * invA0;
    st.a2 = a2 * invA0;
    cacheKlonBiquad (st, 4, sr, freqHz, q, 0.0f, false);
}

inline float processKlonHighPassEq (float x, KlonBiquadState& st, float sr,
                                    float freqHz, float q) noexcept
{
    if (klonBiquadNeedsUpdate (st, 4, sr, freqHz, q, 0.0f, false))
        updateKlonHighPassEqCoeffs (st, sr, freqHz, q);

    return processCachedKlonBiquad (x, st);
}

inline bool klonFixedBiquadNeedsUpdate (const KlonBiquadState& st, int type, float sr) noexcept
{
    return st.cachedType != type || st.cachedSr != sr;
}

inline float processKlonFixedPeakEq (float x, KlonBiquadState& st, float sr,
                                     float freqHz, float q, float gainDb) noexcept
{
    if (klonFixedBiquadNeedsUpdate (st, 1, sr))
        updateKlonPeakEqCoeffs (st, sr, freqHz, q, gainDb);

    return processCachedKlonBiquad (x, st);
}

inline float processKlonFixedShelfEq (float x, KlonBiquadState& st, float sr,
                                      float freqHz, float q, float gainDb, bool highShelf) noexcept
{
    if (klonFixedBiquadNeedsUpdate (st, 2, sr))
        updateKlonShelfEqCoeffs (st, sr, freqHz, q, gainDb, highShelf);

    return processCachedKlonBiquad (x, st);
}

inline float klonEqAmountFor (KlonEqAmount amount, float reference, float classic, float drive) noexcept
{
    switch (amount)
    {
        case KlonEqAmount::Reference:    return reference;
        case KlonEqAmount::Classic:      return classic;
        case KlonEqAmount::ClassicDrive: return classic * detail::clampF (drive, 0.0f, 1.0f);
        case KlonEqAmount::Fixed:
        default:                         return 1.0f;
    }
}

inline float processKlonEqBand (float x, KlonBiquadState* states, const KlonEqBandSpec& spec,
                                float sr, float reference, float classic, float drive) noexcept
{
    const int stages = juce::jlimit (1, kMaxKlonEqStages, spec.stages);
    const float amount = klonEqAmountFor (spec.amount, reference, classic, drive);
    const float gainDb = spec.gainDb * amount;
    const float stageGainDb = gainDb / (float) stages;
    const bool fixed = spec.amount == KlonEqAmount::Fixed;

    switch (spec.kind)
    {
        case KlonEqKind::Peak:
        {
            for (int i = 0; i < stages; ++i)
                x = fixed ? processKlonFixedPeakEq (x, states[i], sr, spec.freqHz, spec.q, stageGainDb)
                          : processKlonPeakEq      (x, states[i], sr, spec.freqHz, spec.q, stageGainDb);
            break;
        }

        case KlonEqKind::LowShelf:
        case KlonEqKind::HighShelf:
        {
            const bool highShelf = spec.kind == KlonEqKind::HighShelf;
            for (int i = 0; i < stages; ++i)
                x = fixed ? processKlonFixedShelfEq (x, states[i], sr, spec.freqHz, spec.q, stageGainDb, highShelf)
                          : processKlonShelfEq      (x, states[i], sr, spec.freqHz, spec.q, stageGainDb, highShelf);
            break;
        }

        case KlonEqKind::LowPass:
        {
            for (int i = 0; i < stages; ++i)
                x = processKlonLowPassEq (x, states[i], sr, spec.freqHz, spec.q);
            break;
        }

        case KlonEqKind::HighPass:
        {
            for (int i = 0; i < stages; ++i)
                x = processKlonHighPassEq (x, states[i], sr, spec.freqHz, spec.q);
            break;
        }

        case KlonEqKind::TiltShelf:
        {
            const int tiltStages = juce::jlimit (1, kMaxKlonEqStages / 2, spec.stages);
            const float tiltStageGainDb = gainDb / (float) tiltStages;
            for (int i = 0; i < tiltStages; ++i)
            {
                x = fixed ? processKlonFixedShelfEq (x, states[i * 2],     sr, spec.freqHz, spec.q, -tiltStageGainDb * 0.5f, false)
                          : processKlonShelfEq      (x, states[i * 2],     sr, spec.freqHz, spec.q, -tiltStageGainDb * 0.5f, false);
                x = fixed ? processKlonFixedShelfEq (x, states[i * 2 + 1], sr, spec.freqHz, spec.q,  tiltStageGainDb * 0.5f, true)
                          : processKlonShelfEq      (x, states[i * 2 + 1], sr, spec.freqHz, spec.q,  tiltStageGainDb * 0.5f, true);
            }
            break;
        }
    }

    return x;
}

inline bool isComponentVoicingModel (Model model) noexcept
{
    return model == Model::Tube
        || model == Model::Tape
        || model == Model::Diode
        || model == Model::Transistor;
}

inline ComponentVoicingBand makeComponentBand (KlonEqKind kind, float freqHz, float q,
                                               float gainDb, int stages = 1) noexcept
{
    ComponentVoicingBand band;
    band.enabled = true;
    band.kind = kind;
    band.freqHz = freqHz;
    band.q = q;
    band.gainDb = gainDb;
    band.stages = stages;
    return band;
}

inline ComponentVoicingBand lerpComponentBand (const ComponentVoicingBand& a,
                                               const ComponentVoicingBand& b,
                                               float t) noexcept
{
    if (!a.enabled) return b;
    if (!b.enabled) return a;

    ComponentVoicingBand out;
    out.enabled = true;
    out.kind = a.kind;
    out.freqHz = juce::jmap (t, a.freqHz, b.freqHz);
    out.q = juce::jmap (t, a.q, b.q);
    out.gainDb = juce::jmap (t, a.gainDb, b.gainDb);
    out.stages = t < 0.5f ? a.stages : b.stages;
    return out;
}

inline ComponentVoicingBand interpComponentBand3 (const ComponentVoicingBand& lo,
                                                  const ComponentVoicingBand& mid,
                                                  const ComponentVoicingBand& hi,
                                                  float type) noexcept
{
    const float t = detail::smoothStep01 (detail::clampF (type, 0.0f, 1.0f));
    return t <= 0.5f ? lerpComponentBand (lo, mid, t * 2.0f)
                     : lerpComponentBand (mid, hi, (t - 0.5f) * 2.0f);
}

inline ComponentVoicingSet makeComponentVoicingSet (Model model, float drive, float character,
                                                    float type, float bias = 0.0f) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float c = detail::smoothStep01 (detail::clampF (character, 0.0f, 1.0f));
    const float b = detail::clampF (bias, -1.0f, 1.0f);
    ComponentVoicingSet set;
    ComponentVoicingSet a0, a1, a2;

    switch (model)
    {
        case Model::Tube:
        {
            a0.pre[0] = makeComponentBand (KlonEqKind::HighPass, 24.0f, 0.707f, 0.0f);
            a0.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 175.0f, 0.70f, 0.25f + c * 0.20f);
            a0.pre[2] = makeComponentBand (KlonEqKind::Peak, 1850.0f, 1.05f, 0.35f + c * 0.45f + d * 0.18f);
            a0.post[0] = makeComponentBand (KlonEqKind::LowPass, 15500.0f, 0.707f, 0.0f);
            a0.post[1] = makeComponentBand (KlonEqKind::HighShelf, 8800.0f, 0.72f, -0.45f - d * 0.25f);
            a0.post[2] = makeComponentBand (KlonEqKind::Peak, 1350.0f, 0.85f, 0.12f + c * 0.18f);
            a1.pre[0] = makeComponentBand (KlonEqKind::HighPass, 26.0f, 0.707f, 0.0f);
            a1.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 165.0f, 0.70f, 0.18f + c * 0.28f);
            a1.pre[2] = makeComponentBand (KlonEqKind::Peak, 1450.0f, 1.05f, 0.25f + c * 0.55f + d * 0.14f);
            a1.post[0] = makeComponentBand (KlonEqKind::LowPass, 12200.0f, 0.707f, 0.0f);
            a1.post[1] = makeComponentBand (KlonEqKind::HighShelf, 7600.0f, 0.72f, -0.70f - d * 0.30f);
            a1.post[2] = makeComponentBand (KlonEqKind::Peak, 980.0f, 0.85f, 0.16f + c * 0.22f);
            a2.pre[0] = makeComponentBand (KlonEqKind::HighPass, 30.0f, 0.707f, 0.0f);
            a2.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 145.0f, 0.70f, 0.30f + c * 0.34f);
            a2.pre[2] = makeComponentBand (KlonEqKind::Peak, 1100.0f, 1.00f, 0.15f + c * 0.60f + d * 0.10f);
            a2.post[0] = makeComponentBand (KlonEqKind::LowPass, 9300.0f, 0.707f, 0.0f);
            a2.post[1] = makeComponentBand (KlonEqKind::HighShelf, 6200.0f, 0.72f, -0.95f - d * 0.36f);
            a2.post[2] = makeComponentBand (KlonEqKind::Peak, 760.0f, 0.90f, 0.20f + c * 0.25f);
            break;
        }
        case Model::Tape:
        {
            const float underBias = std::max (-b, 0.0f);
            const float overBias = std::max (b, 0.0f);

            // TYPE 0: Rabbit-style dense tape. The measured reference has a
            // clear presence/head resonance around 1.1-1.4 kHz plus resonant
            // band-limited edges, not a low-frequency 200 Hz bump.
            a0.pre[0] = makeComponentBand (KlonEqKind::HighPass, 25.0f, 1.05f, 0.0f, 2);
            a0.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 88.0f, 0.70f, 0.56f + c * 0.22f);
            a0.pre[2] = makeComponentBand (KlonEqKind::Peak, 1250.0f, 1.26f, 1.35f + c * 0.46f + d * 0.30f);
            a0.post[0] = makeComponentBand (KlonEqKind::LowPass, 17200.0f, 1.05f, 0.0f, 2);
            a0.post[1] = makeComponentBand (KlonEqKind::HighShelf, 7200.0f, 0.86f,
                                            -0.58f - d * 0.95f - overBias * 0.55f + underBias * 0.18f);
            a0.post[2] = makeComponentBand (KlonEqKind::Peak, 1250.0f, 1.04f, 0.42f + c * 0.16f + d * 0.05f);

            // TYPE 50: transition, still recognisably tape but less Rabbit-forward.
            a1.pre[0] = makeComponentBand (KlonEqKind::HighPass, 22.0f, 0.95f, 0.0f, 2);
            a1.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 120.0f, 0.72f, 0.04f + c * 0.14f);
            a1.pre[2] = makeComponentBand (KlonEqKind::Peak, 1250.0f, 1.12f, 1.20f + c * 0.34f + d * 0.20f);
            a1.post[0] = makeComponentBand (KlonEqKind::LowPass, 14800.0f, 0.95f, 0.0f, 2);
            a1.post[1] = makeComponentBand (KlonEqKind::HighShelf, 6500.0f, 0.84f,
                                            -0.98f - d * 1.04f - overBias * 0.72f + underBias * 0.12f);
            a1.post[2] = makeComponentBand (KlonEqKind::Peak, 1250.0f, 0.98f, 0.36f + c * 0.12f + d * 0.04f);

            // TYPE 100: smoother/warmer tape, less presence bump and stronger HF loss.
            a2.pre[0] = makeComponentBand (KlonEqKind::HighPass, 18.0f, 0.90f, 0.0f, 2);
            a2.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 175.0f, 0.74f, -0.48f + c * 0.10f);
            a2.pre[2] = makeComponentBand (KlonEqKind::Peak, 1250.0f, 1.00f, 1.05f + c * 0.28f + d * 0.15f);
            a2.post[0] = makeComponentBand (KlonEqKind::LowPass, 12200.0f, 0.90f, 0.0f, 2);
            a2.post[1] = makeComponentBand (KlonEqKind::HighShelf, 5200.0f, 0.82f,
                                            -1.45f - d * 1.18f - overBias * 0.90f + underBias * 0.06f);
            a2.post[2] = makeComponentBand (KlonEqKind::Peak, 1250.0f, 0.94f, 0.30f + c * 0.10f);
            break;
        }

        case Model::Diode:
        {
            a0.pre[0] = makeComponentBand (KlonEqKind::HighPass, 400.0f, 0.707f, 0.0f);
            a0.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 135.0f, 0.72f, -0.55f - d * 0.10f);
            a0.pre[2] = makeComponentBand (KlonEqKind::Peak, 850.0f, 0.90f, 0.28f + c * 0.22f);
            a0.post[0] = makeComponentBand (KlonEqKind::LowPass, 5000.0f, 0.707f, 0.0f);
            a0.post[1] = makeComponentBand (KlonEqKind::HighShelf, 3600.0f, 0.72f, -0.90f - d * 0.18f);
            a0.post[2] = makeComponentBand (KlonEqKind::Peak, 1600.0f, 0.95f, -0.08f - d * 0.04f);
            a1.pre[0] = makeComponentBand (KlonEqKind::HighPass, 220.0f, 0.707f, 0.0f);
            a1.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 120.0f, 0.72f, -0.40f - d * 0.08f);
            a1.pre[2] = makeComponentBand (KlonEqKind::Peak, 1350.0f, 0.95f, 0.12f + c * 0.20f);
            a1.post[0] = makeComponentBand (KlonEqKind::LowPass, 4200.0f, 0.707f, 0.0f);
            a1.post[1] = makeComponentBand (KlonEqKind::HighShelf, 3500.0f, 0.72f, -1.10f - d * 0.22f);
            a1.post[2] = makeComponentBand (KlonEqKind::Peak, 2400.0f, 0.95f, -0.36f - d * 0.06f);
            a2.pre[0] = makeComponentBand (KlonEqKind::HighPass, 115.0f, 0.707f, 0.0f);
            a2.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 105.0f, 0.72f, -0.15f - d * 0.04f);
            a2.pre[2] = makeComponentBand (KlonEqKind::Peak, 1850.0f, 1.00f, 0.36f + c * 0.26f);
            a2.post[0] = makeComponentBand (KlonEqKind::LowPass, 7000.0f, 0.707f, 0.0f);
            a2.post[1] = makeComponentBand (KlonEqKind::HighShelf, 5000.0f, 0.72f, -0.42f - d * 0.12f);
            a2.post[2] = makeComponentBand (KlonEqKind::Peak, 2100.0f, 0.95f, 0.22f + c * 0.16f);
            break;
        }
        case Model::Transistor:
        {
            a0.pre[0] = makeComponentBand (KlonEqKind::HighPass, 46.0f, 0.707f, 0.0f);
            a0.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 180.0f, 0.70f, -0.65f + c * 0.20f);
            a0.pre[2] = makeComponentBand (KlonEqKind::Peak, 2400.0f, 0.95f, 0.60f + d * 0.55f + c * 0.15f);
            a0.post[0] = makeComponentBand (KlonEqKind::LowPass, 9800.0f - d * 2000.0f, 0.707f, 0.0f);
            a0.post[1] = makeComponentBand (KlonEqKind::HighShelf, 7200.0f, 0.72f, -0.30f - d * 0.28f);
            a0.post[2] = makeComponentBand (KlonEqKind::Peak, 320.0f, 0.85f, 0.16f + c * 0.14f);
            a1.pre[0] = makeComponentBand (KlonEqKind::HighPass, 34.0f, 0.707f, 0.0f);
            a1.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 165.0f, 0.70f, -0.22f + c * 0.24f);
            a1.pre[2] = makeComponentBand (KlonEqKind::Peak, 1750.0f, 0.95f, 0.42f + d * 0.35f + c * 0.12f);
            a1.post[0] = makeComponentBand (KlonEqKind::LowPass, 8200.0f - d * 1500.0f, 0.707f, 0.0f);
            a1.post[1] = makeComponentBand (KlonEqKind::HighShelf, 6400.0f, 0.72f, -0.46f - d * 0.30f);
            a1.post[2] = makeComponentBand (KlonEqKind::Peak, 420.0f, 0.85f, 0.12f + c * 0.16f);
            a2.pre[0] = makeComponentBand (KlonEqKind::HighPass, 26.0f, 0.707f, 0.0f);
            a2.pre[1] = makeComponentBand (KlonEqKind::LowShelf, 150.0f, 0.70f, 0.08f + c * 0.24f);
            a2.pre[2] = makeComponentBand (KlonEqKind::Peak, 1300.0f, 0.90f, 0.25f + d * 0.22f + c * 0.16f);
            a2.post[0] = makeComponentBand (KlonEqKind::LowPass, 6500.0f - d * 1200.0f, 0.707f, 0.0f);
            a2.post[1] = makeComponentBand (KlonEqKind::HighShelf, 5600.0f, 0.72f, -0.62f - d * 0.30f);
            a2.post[2] = makeComponentBand (KlonEqKind::Peak, 520.0f, 0.85f, 0.08f + c * 0.18f);
            break;
        }
        default:
            break;
    }

    for (int i = 0; i < kComponentVoicingBands; ++i)
    {
        set.pre[i] = interpComponentBand3 (a0.pre[i], a1.pre[i], a2.pre[i], type);
        set.post[i] = interpComponentBand3 (a0.post[i], a1.post[i], a2.post[i], type);
    }

    return set;
}

inline float processComponentVoicingBand (float x, KlonBiquadState* states,
                                          const ComponentVoicingBand& band,
                                          float sr) noexcept
{
    if (!band.enabled)
        return x;

    KlonEqBandSpec spec { band.kind, band.freqHz, band.q, band.gainDb,
                          band.stages, KlonEqAmount::Fixed };
    return processKlonEqBand (x, states, spec, sr, 1.0f, 1.0f, 0.0f);
}

inline float processComponentPreVoicing (float x, ComponentVoicingState& st,
                                         const ComponentVoicingSet& spec,
                                         float sr) noexcept
{
    for (int i = 0; i < kComponentVoicingBands; ++i)
        x = processComponentVoicingBand (x, st.pre[i], spec.pre[i], sr);
    return x;
}

inline float processComponentPostVoicing (float x, ComponentVoicingState& st,
                                          const ComponentVoicingSet& spec,
                                          float sr) noexcept
{
    for (int i = 0; i < kComponentVoicingBands; ++i)
        x = processComponentVoicingBand (x, st.post[i], spec.post[i], sr);
    return x;
}
inline float klonReferenceWeight (float drive) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float driveOpen = detail::smoothStep01 ((d - 0.15f) / 0.35f);
    const float topGuard = detail::smoothStep01 ((d - 0.90f) / 0.10f);
    return 0.25f * driveOpen * (1.0f - 0.25f * topGuard);
}

inline float processKlonPreEq (float x, OverdriveToneState& st, float sr,
                               float peak, float bias, float drive) noexcept
{
    (void) peak;
    const float sym = detail::clampF (bias, -1.0f, 1.0f);

    float y = x;
    y += sym * (x * x) / (1.0f + std::abs (x)) * (0.012f + drive * 0.030f);

    if constexpr (OverdriveVoicing::kKlonResidualMatchingEnabled)
    {
        int band = 0;
        const float ref = klonReferenceWeight (drive);
        for (const auto& spec : getKlonPreAForAnalysis())
            if (band < kKlonPreEqBands)
                y = processKlonEqBand (y, st.klonPreEq[band++], spec, sr, ref, 1.0f, drive);
        for (const auto& spec : getKlonPreNdspForAnalysis())
            if (band < kKlonPreEqBands)
                y = processKlonEqBand (y, st.klonPreEq[band++], spec, sr, ref, 1.0f, drive);
        for (const auto& spec : getKlonPreBForAnalysis())
            if (band < kKlonPreEqBands)
                y = processKlonEqBand (y, st.klonPreEq[band++], spec, sr, ref, 1.0f, drive);
    }

    return y;
}

inline float processKlonPostEqBank (float x, KlonPostEqBank& bank,
                                    float sr, float drive) noexcept
{
    float y = x;

    if constexpr (OverdriveVoicing::kKlonResidualMatchingEnabled)
    {
        int band = 0;
        const float ref = klonReferenceWeight (drive);
        for (const auto& spec : getKlonPostAForAnalysis())
            if (band < kKlonPostEqBands)
                y = processKlonEqBand (y, bank[band++], spec, sr, ref, 1.0f, drive);
        for (const auto& spec : getKlonPostNdspForAnalysis())
            if (band < kKlonPostEqBands)
                y = processKlonEqBand (y, bank[band++], spec, sr, ref, 1.0f, drive);
        for (const auto& spec : getKlonPostBForAnalysis())
            if (band < kKlonPostEqBands)
                y = processKlonEqBand (y, bank[band++], spec, sr, ref, 1.0f, drive);
    }

    return y;
}

inline float processKlonPostEq (float x, OverdriveToneState& st, float sr,
                                float peak, float bias, float drive) noexcept
{
    juce::ignoreUnused (peak, bias);
    return processKlonPostEqBank (x, st.klonPostEq, sr, drive);
}

inline float overdriveAVoice (float mod) noexcept
{
    return 1.0f - detail::smoothStep01 (detail::clampF (mod * 2.0f, 0.0f, 1.0f));
}

inline float solveTs808FeedbackDiode (float target, float limit, float feedback,
                                      float hardness, float asymmetry,
                                      float diodeBiasOffset = 0.0f,
                                      float asymmetryStrength = 0.0f) noexcept
{
    const float safeLimit = juce::jmax (limit, 1.0e-4f);
    const float safeFeedback = juce::jlimit (0.0f, 8.0f, feedback);
    const float safeHardness = juce::jlimit (0.10f, 8.0f, hardness);
    const float safeAsymmetry = detail::clampF (asymmetry, -0.60f, 0.60f);
    const float safeBiasOffset = detail::clampF (diodeBiasOffset, -1.25f, 1.25f);
    const float sideShape = detail::smoothStep01 (detail::clampF (asymmetryStrength, 0.0f, 1.0f));

    auto solveUncompensated = [&] (float input) noexcept
    {
        const float safeTarget = detail::clampF (input, -48.0f, 48.0f);
        const float sign = safeTarget < 0.0f ? -1.0f : 1.0f;
        const float mag = std::abs (safeTarget);
        const float sideLimit = safeLimit * (sign > 0.0f ? 1.0f + safeAsymmetry
                                                         : 1.0f - safeAsymmetry);
        const float norm = mag / juce::jmax (sideLimit, 1.0e-5f);

        // TS feedback diodes are not an ideal on/off switch. Keep the first part
        // almost linear, then add a soft pre-conduction knee before hard feedback
        // compression. This preserves low-level signal while restoring presence.
        const float kneeStart = getTs808CoreTuning().solverKneeStart;
        if (norm <= kneeStart)
            return safeTarget;

        const float sidePolarity = sign > 0.0f ? 1.0f : -1.0f;
        const float sideSkew = safeAsymmetry * sideShape * sidePolarity;
        const float sideFeedback = safeFeedback * detail::clampF (1.0f + sideSkew * 0.72f, 0.38f, 1.85f);
        const float sideHardness = safeHardness * detail::clampF (1.0f + sideSkew * 0.58f, 0.42f, 1.70f);

        const float kneeWidth = 1.0f - kneeStart;
        const float kneeT = detail::smoothStep01 ((norm - kneeStart) / kneeWidth);
        const float mainOver = juce::jmax (0.0f, norm - 1.0f);

        const float preConduct = kneeT * kneeT * getTs808CoreTuning().solverPreConduct;
        const float mainConduct = mainOver / (1.0f + mainOver * sideHardness);
        const float conduct = preConduct + mainConduct;
        const float loopResistance = 1.0f + sideFeedback * sideHardness * conduct;
        const float solvedNorm = norm / juce::jmax (1.0f, loopResistance);
        const float y = sign * sideLimit * solvedNorm;

        return detail::clampF (y, -sideLimit * (1.0f + sideFeedback),
                               sideLimit * (1.0f + sideFeedback));
    };

    if (std::abs (safeBiasOffset) <= 1.0e-7f && sideShape <= 1.0e-7f)
        return solveUncompensated (target);

    // A real biased feedback-diode pair changes where the loop conducts, not
    // just the final waveform ceiling. Subtract the zero-input operating point
    // so the control adds asymmetry without leaking DC into the plugin output.
    return solveUncompensated (target + safeBiasOffset) - solveUncompensated (safeBiasOffset);
}

inline float processTs808FeedbackDiodeCore (float x, OverdriveToneState& st, float sr,
                                            float amount, float drive,
                                            float upperSmoothAmount,
                                            float kneeAmount,
                                            float symmetryAmount) noexcept
{
    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const auto& ts = getTs808CoreTuning();
    const float a = detail::clampF (amount, 0.0f, 1.0f);
    const float upperSmooth = detail::smoothStep01 (detail::clampF (upperSmoothAmount, 0.0f, 1.0f));
    const float kneeRange = detail::clampF (kneeAmount, 0.0f, 2.0f);
    const float kneeSmooth = kneeRange <= 1.0f
        ? detail::smoothStep01 (kneeRange)
        : 1.0f + detail::smoothStep01 (kneeRange - 1.0f);
    const float sym = detail::clampF (symmetryAmount, -1.0f, 1.0f);
    if (a <= 0.0001f)
        return x;

    float pre = x;
    if (OverdriveVoicing::kTs808ResidualMatchingEnabled)
    {
        int preBand = 2;
        for (const auto& spec : getTs808PreAForAnalysis())
            pre = processKlonEqBand (pre, st.overdriveAPreEq[preBand++], spec, sr, 1.0f, 1.0f, d);
        for (const auto& spec : getTs808PreNdspForAnalysis())
            pre = processKlonEqBand (pre, st.overdriveAPreEq[preBand++], spec, sr, 1.0f, 1.0f, d);
        for (const auto& spec : getTs808PreBForAnalysis())
            pre = processKlonEqBand (pre, st.overdriveAPreEq[preBand++], spec, sr, 1.0f, 1.0f, d);
    }

    // TS808 clipping op-amp: 4.7k + 47nF sets the ~720 Hz rising gain corner.
    const float low = processKlonLowPassEq (pre, st.overdriveAPreEq[0][0], sr, ts.inputHighPassHz, 0.707f);
    const float high = pre - low;

    const float driveT = detail::smoothStep01 (d);
    const float feedbackOhms = 51000.0f + 500000.0f * driveT;
    const float schematicHighGain = 1.0f + feedbackOhms / 4700.0f;
    const float loopGain = 1.0f + driveT * (std::pow (schematicHighGain, 0.70f) - 1.0f);
    const float feedbackPole = 1.0f / (kTwoPi * feedbackOhms * 51.0e-12f);
    const float cappedPole = detail::clampF (feedbackPole, ts.feedbackPoleMinHz,
                                             std::max (sr * 0.45f, ts.feedbackPoleMinHz));

    const float capped = processKlonLowPassEq (high, st.overdriveAPreEq[1][0], sr, cappedPole, 0.707f);
    const float air = high - capped;
    const float lowTrim = 1.0f - driveT * ts.lowTrimAtMaxDrive;
    const float tsLoopDrive = 1.0f + driveT * driveT * driveT * ts.loopDriveMax;
    const float loopTarget = (capped * loopGain * (1.0f + driveT * ts.loopCappedGainAtMaxDrive)
                           + air * (1.0f + driveT * ts.airGainAtMaxDrive)) * tsLoopDrive;

    const float loopCoeff = detail::onePoleCoeff (juce::jmap (driveT, ts.loopCoeffHzLo, ts.loopCoeffHzHi), sr);
    const bool initialiseFeedback = ! st.tsFeedbackInitialised || ! std::isfinite (st.tsFeedbackLP);
    if (initialiseFeedback)
    {
        st.tsFeedbackLP = loopTarget;
        st.tsFeedbackInitialised = true;
    }

    st.tsFeedbackLP += (loopTarget - st.tsFeedbackLP) * loopCoeff;
    const float loopBody = st.tsFeedbackLP;
    const float loopUpper = loopTarget - loopBody;
    const float upperMid = processKlonLowPassEq (loopUpper, st.overdriveAPreEq[1][1],
                                                 sr, ts.upperMidSplitHz, 0.707f);
    const float upperAir = loopUpper - upperMid;

    // Dynamic diode conductance memory: in the real feedback network, diode
    // capacitance and op-amp recovery make hot passages smoother than a purely
    // static transfer curve. TYPE now damps the upper loop instead of exciting
    // extra fizz; KNEE changes how early the pre-conduction region wakes up.
    const float diodeSense = std::abs (loopTarget) / (1.0f + std::abs (loopTarget));
    const float memoryStart = juce::jlimit (0.08f, 0.42f, 0.30f - kneeSmooth * 0.14f);
    const float memoryTarget = detail::smoothStep01 ((diodeSense - memoryStart)
                                                   / juce::jmax (0.08f, 0.70f - memoryStart));
    const float memoryAttackHz = 180.0f + driveT * 560.0f + upperSmooth * 920.0f;
    const float memoryReleaseHz = 12.0f + upperSmooth * 34.0f + kneeSmooth * 38.0f;
    const float memoryCoeff = detail::onePoleCoeff (
        memoryTarget > st.tsDiodeMemory ? memoryAttackHz : memoryReleaseHz, sr);
    st.tsDiodeMemory += (memoryTarget - st.tsDiodeMemory) * memoryCoeff;
    st.tsDiodeMemory = juce::jlimit (0.0f, 1.0f, st.tsDiodeMemory);
    const float diodeMemory = st.tsDiodeMemory
                            * (0.040f + driveT * 0.150f + upperSmooth * 0.125f + kneeSmooth * 0.120f);
    const float upperDynamicFeedback = detail::clampF (ts.upperDynamicFeedback, 0.35f, 2.20f);
    const float upperMidDynamicFeedback = detail::clampF (ts.upperMidDynamicFeedback, 0.35f, 2.35f);
    const float upperAirDynamicFeedback = detail::clampF (ts.upperAirDynamicFeedback, 0.35f, 2.35f);
    const float upperMidDiodeMemory = diodeMemory * upperDynamicFeedback * upperMidDynamicFeedback;
    const float upperAirDiodeMemory = diodeMemory * upperDynamicFeedback * upperAirDynamicFeedback;

    const float bodyLimit = juce::jmap (driveT, ts.bodyLimitLo, ts.bodyLimitHi)
                          * juce::jmap (kneeSmooth, 1.0f, 1.24f)
                          * (1.0f + diodeMemory * 0.18f);
    const float bodyFeedback = juce::jmap (driveT, ts.bodyFeedbackLo, ts.bodyFeedbackHi)
                             * juce::jmap (kneeSmooth, 1.0f, 0.64f)
                             * (1.0f + diodeMemory * 0.30f);
    const float bodyHardness = juce::jmap (driveT, ts.bodyHardnessLo, ts.bodyHardnessHi)
                             * juce::jmap (kneeSmooth, 1.0f, 0.50f)
                             * (1.0f - diodeMemory * 0.22f);
    const float symAbs = detail::smoothStep01 (std::abs (sym));
    const float typeSymLift = 1.0f + (1.0f - upperSmooth) * 0.52f;
    const float asymControl = symAbs * (0.36f + driveT * 0.78f) * typeSymLift;
    const float chainsawMismatchLift = 1.0f + symAbs * (0.20f + driveT * 0.30f)
                                             * juce::jmap (upperSmooth, 1.18f, 0.72f);
    const float bodyBaseAsymmetry = juce::jmap (driveT, ts.bodyAsymmetryLo, ts.bodyAsymmetryHi);
    const float bodyAsymmetry = detail::clampF (bodyBaseAsymmetry
                                              + sym * (0.56f + driveT * 0.72f + kneeSmooth * 0.20f)
                                                    * typeSymLift * chainsawMismatchLift,
                                              -0.84f, 0.84f);
    const float bodyDiodeBias = sym * asymControl * bodyLimit
                              * (0.32f + driveT * 0.66f + kneeSmooth * 0.24f)
                              * chainsawMismatchLift;

    const float solvedBody = solveTs808FeedbackDiode (loopBody, bodyLimit,
                                                      bodyFeedback, bodyHardness,
                                                      bodyAsymmetry,
                                                      bodyDiodeBias,
                                                      asymControl);
    const float upperLimit = juce::jmap (driveT, ts.upperLimitLo, ts.upperLimitHi);
    const float upperFeedback = (ts.upperFeedbackLo + driveT * ts.upperFeedbackHi)
                              * juce::jmap (upperSmooth, 1.0f, 0.72f)
                              * juce::jmap (kneeSmooth, 1.0f, 0.56f)
                              * (1.0f + upperMidDiodeMemory * 0.42f);
    const float upperHardness = (ts.upperHardnessLo + driveT * ts.upperHardnessHi)
                              * juce::jmap (upperSmooth, 1.0f, 0.62f)
                              * juce::jmap (kneeSmooth, 1.0f, 0.42f)
                              * (1.0f - upperMidDiodeMemory * 0.28f);
    const float upperAsymmetry = detail::clampF (bodyAsymmetry * ts.upperAsymmetryScale
                                               + sym * asymControl * (0.42f + driveT * 0.72f + upperSmooth * 0.22f)
                                                     * chainsawMismatchLift,
                                               -0.82f, 0.82f);
    const float upperDiodeBias = sym * asymControl * upperLimit
                               * (0.38f + driveT * 0.74f + upperSmooth * 0.22f)
                               * chainsawMismatchLift;
    const float upperSoft = solveTs808FeedbackDiode (upperMid, upperLimit,
                                                     upperFeedback,
                                                     upperHardness,
                                                     upperAsymmetry,
                                                     upperDiodeBias,
                                                     asymControl);
    const float upperBlend = detail::clampF (juce::jmap (driveT, ts.upperBlendLo, ts.upperBlendHi)
                                           * juce::jmap (upperSmooth, 1.0f, 0.78f)
                                           * juce::jmap (kneeSmooth, 1.0f, 0.82f)
                                           * (1.0f + upperMidDiodeMemory * 0.38f), 0.0f, 1.0f);
    const float upperAirTrim = juce::jmap (driveT, ts.upperAirTrimLo, ts.upperAirTrimHi)
                             * juce::jmap (upperSmooth, 1.0f, 0.46f)
                             * juce::jmap (kneeSmooth, 1.0f, 0.74f)
                             * (1.0f - upperAirDiodeMemory * 0.34f);
    float loopOut = solvedBody
                  + juce::jmap (upperBlend, upperMid, upperSoft)
                  + upperAir * upperAirTrim;

    if (symAbs > 0.0001f)
    {
        const float asymGrit = sym * symAbs * typeSymLift
                             * (0.245f + driveT * 0.650f + upperSmooth * 0.135f + kneeSmooth * 0.190f);
        const float folded = (loopOut * loopOut) / (0.19f + std::abs (loopOut));
        loopOut += folded * asymGrit;

        // HM-2-inspired TS variant: SYM now changes diode-loop behaviour before
        // the TS post EQ, instead of adding a final output tilt. Negative SYM is
        // tighter/upper-mid focused; positive SYM keeps more low-mid grind.
        const float sawAmt = detail::smoothStep01 ((symAbs - 0.045f) / 0.955f)
                           * (0.32f + driveT * 0.78f)
                           * juce::jmap (upperSmooth, 1.16f, 0.72f)
                           * juce::jmap (kneeSmooth, 0.96f, 0.72f);
        if (sawAmt > 0.0001f)
        {
            const float neg = sym < 0.0f ? 1.0f : 0.0f;
            const float pos = sym > 0.0f ? 1.0f : 0.0f;
            const float signedAmt = sym * sawAmt;
            const float focus = detail::clampF (upperSoft * (0.72f + driveT * 0.18f)
                                              + upperMid  * (0.28f + upperSmooth * 0.16f)
                                              - upperAir  * (0.04f + neg * 0.10f),
                                              -8.0f, 8.0f);
            const float bodyGrind = detail::clampF (solvedBody - loopBody * (0.16f + neg * 0.18f),
                                                    -8.0f, 8.0f);
            const float rectIn = loopOut * (1.0f + sawAmt * (0.26f + driveT * 0.46f))
                               + focus * signedAmt * (0.12f + driveT * 0.12f)
                               + bodyGrind * sawAmt * pos * (0.06f + driveT * 0.10f);
            const float rectBias = signedAmt * (0.045f + driveT * 0.105f);
            const float rectK = 1.10f + driveT * 1.65f + neg * 0.34f;
            const float zero = detail::fastTanh (rectBias * rectK) / rectK;
            const float rect = detail::fastTanh ((rectIn + rectBias) * rectK) / rectK - zero;
            const float chainsaw = rect - loopOut;
            const float upperPush = focus * sawAmt * (neg > 0.5f ? 0.18f : 0.10f)
                                  + bodyGrind * sawAmt * (pos > 0.5f ? 0.16f : -0.05f);
            loopOut += chainsaw * (0.28f + driveT * 0.30f)
                     + upperPush
                     - loopBody * sawAmt * neg * (0.025f + driveT * 0.045f);
        }
    }

    const float opSense = std::abs (loopOut) / (1.0f + std::abs (loopOut));
    const float opTarget = detail::smoothStep01 ((opSense - 0.58f) / 0.38f) * driveT;
    const float opCoeff = detail::onePoleCoeff (
        opTarget > st.tsOpAmpRecovery ? 680.0f + upperSmooth * 920.0f : 36.0f + kneeSmooth * 44.0f, sr);
    st.tsOpAmpRecovery += (opTarget - st.tsOpAmpRecovery) * opCoeff;
    st.tsOpAmpRecovery = juce::jlimit (0.0f, 1.0f, st.tsOpAmpRecovery);
    const float recoveryBlend = st.tsOpAmpRecovery * (0.030f + driveT * 0.060f + kneeSmooth * 0.040f);
    const float recoveryK = 1.0f + driveT * 0.75f;
    const float recovered = detail::fastTanh (loopOut * recoveryK) / recoveryK;
    loopOut = juce::jmap (recoveryBlend, loopOut, recovered);

    return juce::jmap (a, x, low * lowTrim + loopOut);
}

inline float processOverdriveAPostEqBank (float x,
                                              KlonBiquadState (&overdriveAPostEq)[kClassicPostEqBands][kMaxKlonEqStages],
                                              float& overdriveAPostHiCutLP,
                                              float sr,
                                              float amount,
                                              float drive) noexcept
{
    float y = x;
    int band = 0;
    if constexpr (OverdriveVoicing::kTs808BaseVoicingEnabled)
        for (const auto& spec : OverdriveVoicing::kTs808PostCore)
            y = processKlonEqBand (y, overdriveAPostEq[band++], spec, sr, 1.0f, amount, drive);

    if (OverdriveVoicing::kTs808ResidualMatchingEnabled)
    {
        for (const auto& spec : getTs808PostAForAnalysis())
            y = processKlonEqBand (y, overdriveAPostEq[band++], spec, sr, 1.0f, amount, drive);
        for (const auto& spec : getTs808PostNdspForAnalysis())
            y = processKlonEqBand (y, overdriveAPostEq[band++], spec, sr, 1.0f, amount, drive);
        for (const auto& spec : getTs808PostBForAnalysis())
            y = processKlonEqBand (y, overdriveAPostEq[band++], spec, sr, 1.0f, amount, drive);
    }

    if constexpr (OverdriveVoicing::kTs808PostHiCutEnabled)
    {
        const float biquadCut = processKlonEqBand (y, overdriveAPostEq[kClassicPostEqBands - 1],
                                                   OverdriveVoicing::kTs808PostHiCut, sr, 1.0f, amount, drive);
        const float onePoleCoeff = detail::onePoleCoeff (18000.0f, sr);
        overdriveAPostHiCutLP += (biquadCut - overdriveAPostHiCutLP) * onePoleCoeff;
        return juce::jmap (amount, y, overdriveAPostHiCutLP);
    }

    overdriveAPostHiCutLP = y;
    return y;
}

inline float processOverdriveAPostEq (float x, OverdriveToneState& st, float sr,
                                          float amount, float drive) noexcept
{
    return processOverdriveAPostEqBank (x, st.overdriveAPostEq, st.overdriveAPostHiCutLP,
                                            sr, amount, drive);
}

// OVERDRIVE A/B share the overdrive core shell but no longer morph between pedals.
// A is the TS808-style feedback diode core; B is the Klon-style split path.
inline float processOverdriveCore (float x, float drive, float girth, float bias, float mod, float peak,
                             Model model,
                             bool rawMode, State& state, int ch, float sr,
                             adaa::ClipperADAA& adaaState) noexcept
{
    const bool klonMode = model == Model::OverdriveB;
    const float voiceDriveParam = klonMode ? 1.0f : 0.0f;
    const float baseRawDrive = detail::clipperADrive (drive, voiceDriveParam);
    const float rawDrive = klonMode
        ? detail::clampF (juce::jmap (detail::smoothStep01 ((detail::clampF (drive, 0.0f, 1.0f) - 0.58f) / 0.42f),
                                      baseRawDrive, 0.86f),
                          0.0f, 1.0f)
        : baseRawDrive;
    const float k = detail::clampF (girth, 0.0f, 1.0f);
    const float userSym = detail::clampF (bias, -1.0f, 1.0f);
    const float b = detail::clampF (userSym * (klonMode ? 1.0f : 0.98f), -1.0f, 1.0f);
    const float m = detail::clampF (mod, 0.0f, 1.0f);
    const float typeTone = detail::smoothStep01 (m);
    const auto& klonCore = getKlonCoreTuning();
    const float overdriveAVoiceAmount = klonMode ? 0.0f : 1.0f;
    const float klonVoice = klonMode ? 1.0f : 0.0f;
    const float tsVoiceAmount = rawMode ? 0.0f : overdriveAVoiceAmount;
    const float tsDriveScale = juce::jmap (tsVoiceAmount, 1.0f, getTs808CoreTuning().driveScale);
    const float tsDriveHeadroom = juce::jlimit (1.0f, 2.5f, getTs808CoreTuning().driveHeadroom);
    const float tsDriveStress = rawDrive * tsDriveScale * juce::jmap (tsVoiceAmount, 1.0f, tsDriveHeadroom);
    const float d = detail::clampF (tsDriveStress, 0.0f, 1.0f);
    const float tsInputGain = std::pow (10.0f, getTs808CoreTuning().inputGainDb * tsVoiceAmount / 20.0f);
    const float tsInput = x * tsInputGain;
    const float overdriveADriveVoice = 1.0f - 0.08f * klonVoice;

    // DRIVE sets the clipping threshold, but we keep a fixed clip ceiling.
    // This makes the control behave like a real threshold while preserving a
    // practical output range similar to pro clippers and pedal stages.
    float threshold = 1.08f;
    constexpr float kLegacyMaxAtNewDrive = 1.0f / 3.0f;
    if (d <= kLegacyMaxAtNewDrive)
    {
        threshold = detail::interpDrive5 (d / kLegacyMaxAtNewDrive,
                                          1.08f, 0.92f, 0.72f, 0.48f, 0.24f);
    }
    else
    {
        constexpr float kCurrentMaxAtNewDrive = 0.5f;
        const float currentMaxThreshold = juce::jmap (overdriveADriveVoice, 0.08f, 0.04f);
        const float extendedMaxThreshold = juce::jmap (overdriveADriveVoice, 0.08f, 0.0025f);

        if (d <= kCurrentMaxAtNewDrive)
        {
            const float extraDrive = detail::smoothStep01 ((d - kLegacyMaxAtNewDrive)
                                                        / (kCurrentMaxAtNewDrive - kLegacyMaxAtNewDrive));
            threshold = juce::jmap (extraDrive, 0.24f, currentMaxThreshold);
        }
        else
        {
            const float extraDrive = detail::smoothStep01 ((d - kCurrentMaxAtNewDrive)
                                                        / (1.0f - kCurrentMaxAtNewDrive));
            threshold = juce::jmap (extraDrive, currentMaxThreshold, extendedMaxThreshold);
        }
    }

    const float voiceScale = klonMode ? 0.92f : 1.0f;
    const float voiceLift = klonMode ? 1.12f : 1.0f;

    auto& klonState = state.overdriveTone[state.currentSeriesPass][ch];
    float overdriveAInput = tsInput;
    float tsCoreOutput = tsInput;
    bool tsCoreActive = false;
    if (overdriveAVoiceAmount > 0.0001f && ! rawMode)
    {
        tsCoreOutput = processTs808FeedbackDiodeCore (tsInput, klonState, sr, overdriveAVoiceAmount, d, typeTone, k * 2.0f, b);
        tsCoreActive = true;
    }
    else if (klonState.tsFeedbackInitialised)
    {
        klonState.tsFeedbackLP = 0.0f;
        klonState.tsDiodeMemory = 0.0f;
        klonState.tsOpAmpRecovery = 0.0f;
        klonState.tsFeedbackInitialised = false;
    }

    const float thresholdFloor = juce::jmap (overdriveADriveVoice, 0.05f, 0.0015625f);
    const float overdriveAThresholdGain = voiceScale / std::max (threshold, thresholdFloor);
    const float overdriveAInputGain = (! rawMode && overdriveAVoiceAmount > 0.0001f)
                                ? juce::jmap (overdriveAVoiceAmount, overdriveAThresholdGain, 1.0f)
                                : overdriveAThresholdGain;
    const float overdriveAClipIn = overdriveAInput * overdriveAInputGain;
    const float klonEffectiveDrive = detail::clampF (d * (klonMode ? klonCore.driveScale : 1.0f), 0.0f, 1.0f);
    float klonDriveGain = detail::interpDrive5 (klonEffectiveDrive,
                                                0.90f, 2.15f, 7.80f, 35.00f,
                                                160.0f * (klonMode ? klonCore.driveGainMaxScale : 1.0f));
    const float klonDriveOpen = detail::smoothStep01 (klonEffectiveDrive);
    klonDriveGain *= 1.0f + klonDriveOpen;
    klonDriveGain *= klonMode ? klonCore.driveGainScale : 1.0f;
    klonDriveGain *= juce::jmap (typeTone, 0.94f, 1.34f);
    float klonInput = x;
    if (klonVoice > 0.0001f && ! rawMode)
    {
        const float klonInputGain = std::pow (10.0f, klonCore.inputGainDb / 20.0f);
        klonInput = juce::jmap (klonVoice, x, processKlonPreEq (x * klonInputGain, klonState, sr, peak, b, d));
    }

    const float clipIn = juce::jmap (klonVoice, overdriveAClipIn, klonInput * klonDriveGain);
    const float rawUnityComp = rawMode
        ? 1.0f / std::max (1.0e-6f, juce::jmap (klonVoice, overdriveAInputGain, klonDriveGain))
        : 1.0f;

    // BIAS becomes symmetry / mismatch: shifts positive and negative clip
    // thresholds independently, but keep their mean around unity.
    const float klonSymAbs = detail::smoothStep01 (std::abs (b));
    const float klonSymNeg = klonMode && b < 0.0f ? klonSymAbs : 0.0f;
    const float klonSymPos = klonMode && b > 0.0f ? klonSymAbs : 0.0f;
    const float klonDiodeHeadroom = juce::jmap (k, 0.60f, 0.74f)
                                   * juce::jmap (typeTone, 1.03f, 0.86f)
                                   * (klonMode ? klonCore.diodeHeadroomScale : 1.0f)
                                   * (klonMode ? detail::clampF (1.0f - klonSymNeg * (0.13f + d * 0.10f)
                                                                      + klonSymPos * (0.045f + d * 0.035f),
                                                                 0.74f, 1.12f)
                                               : 1.0f);
    const float klonBiasRange = klonMode
        ? (1.08f + k * 0.34f + typeTone * 0.22f) * (1.0f + klonSymAbs * (0.26f + d * 0.24f))
        : 1.08f + k * 0.34f + typeTone * 0.22f;
    const float overdriveAThresholdPos = detail::clampF (1.0f + b * 0.62f, 0.34f, 1.72f);
    const float overdriveAThresholdNeg = detail::clampF (1.0f - b * 0.62f, 0.34f, 1.72f);
    const float klonThresholdPos = detail::clampF (klonDiodeHeadroom * (1.0f + b * klonBiasRange), 0.24f, 1.45f);
    const float klonThresholdNeg = detail::clampF (klonDiodeHeadroom * (1.0f - b * klonBiasRange), 0.24f, 1.45f);
    const float tsKnee = overdriveAVoiceAmount * k;
    const float tsHeadroomScale = juce::jmap (tsKnee, 1.0f, 1.36f);
    const float tsInputComp = juce::jmap (tsKnee, 1.0f, 0.76f);
    const float thresholdPos = juce::jmap (klonVoice, overdriveAThresholdPos * tsHeadroomScale, klonThresholdPos);
    const float thresholdNeg = juce::jmap (klonVoice, overdriveAThresholdNeg * tsHeadroomScale, klonThresholdNeg);
    const float overdriveAKneeRange = juce::jmap (overdriveAVoiceAmount, getTs808CoreTuning().legacyKneeRangeLo, getTs808CoreTuning().legacyKneeRangeHi);
    const float overdriveAKneeSoft = 0.01f + k * overdriveAKneeRange;
    const float klonKneeSoft = (0.045f + k * 0.34f)
                             * (klonMode ? detail::clampF (1.0f - klonSymNeg * 0.28f + klonSymPos * 0.20f,
                                                           0.68f, 1.24f)
                                         : 1.0f);
    const float kneeSoft = juce::jmap (klonVoice, overdriveAKneeSoft, klonKneeSoft);
    const float kneePos = std::max (1.0e-4f, thresholdPos * kneeSoft);
    const float kneeNeg = std::max (1.0e-4f, thresholdNeg * kneeSoft);

    const float clipInForKnee = juce::jmap (klonVoice, clipIn * tsInputComp, clipIn);
    float clipped = adaaState.process (clipInForKnee, thresholdPos, thresholdNeg,
                                       kneePos, kneeNeg);

    // Preserve average ceiling when asymmetry moves thresholds apart.
    const float outputScale = 2.0f / (thresholdPos + thresholdNeg);
    clipped *= outputScale;

    if (tsCoreActive)
        clipped = juce::jmap (overdriveAVoiceAmount, clipped, tsCoreOutput);

    if (overdriveAVoiceAmount > 0.0001f && ! rawMode && ! state.deferOverdriveAPostEq)
        clipped = processOverdriveAPostEq (clipped, klonState, sr, overdriveAVoiceAmount, d);


    if (klonVoice > 0.0001f)
    {
        const float kneeT = k <= 0.5f ? k * 2.0f : (k - 0.5f) * 2.0f;
        const float klonSoftInGain = (k <= 0.5f ? juce::jmap (kneeT, 1.08f, 0.91f)
                                                : juce::jmap (kneeT, 0.91f, 0.78f))
                                  * (1.0f + klonSymNeg * 0.10f - klonSymPos * 0.035f);
        const float klonSoftK = (k <= 0.5f ? juce::jmap (kneeT, 0.92f, 0.67f)
                                           : juce::jmap (kneeT, 0.67f, 0.42f))
                              * (1.0f + klonSymNeg * 0.18f - klonSymPos * 0.14f);
        const float klonSoftOut = (k <= 0.5f ? juce::jmap (kneeT, 1.10f, 0.96f)
                                             : juce::jmap (kneeT, 0.96f, 0.84f))
                                * (1.0f - klonSymNeg * 0.045f + klonSymPos * 0.025f);
        const float klonSoftBlend = klonVoice * detail::clampF ((detail::smoothStep01 (k) + typeTone * 0.22f)
                                                              * klonCore.softBlendScale
                                                              * (1.0f + klonSymNeg * 0.20f + klonSymPos * 0.11f),
                                                              0.0f, 1.0f);
        const float klonSoftIn = klonInput * klonDriveGain * klonSoftInGain;
        float klonSoft = klonState.softAdaa.process (klonSoftIn, klonSoftK)
                       * klonSoftOut;
        if (klonSymAbs > 0.0001f)
        {
            const float klonDriveSym = 0.20f + detail::smoothStep01 (d) * 0.80f;
            const float softAsym = b * klonSymAbs * klonDriveSym
                                * (0.145f + d * 0.980f + k * 0.260f + typeTone * 0.220f)
                                * (1.0f + klonSymNeg * 0.46f + klonSymPos * 0.24f);
            klonSoft += (klonSoft * klonSoft) / (0.48f + std::abs (klonSoft)) * softAsym;

            // Klon-style SYM is dirty-path diode mismatch. Make it visible as
            // one-sided conduction bend before the clean/dirty summing stage,
            // rather than as a final output tilt or DC offset.
            const float bendAmt = klonSymAbs
                                * (0.115f + d * 0.440f + k * 0.170f + typeTone * 0.180f)
                                * (1.0f + klonSymNeg * 0.34f + klonSymPos * 0.18f);
            const float bendPolarity = b >= 0.0f ? 1.0f : -1.0f;
            const float selectedHalf = klonSoft * bendPolarity;
            const float bendStart = juce::jmap (d, 0.76f, 0.38f)
                                  * juce::jmap (k, 1.04f, 0.84f);
            if (selectedHalf > bendStart)
            {
                const float over = selectedHalf - bendStart;
                const float bentHalf = bendStart
                                      + over * (1.0f - bendAmt * 0.82f)
                                      - (over * over) * bendAmt / (0.42f + over);
                klonSoft = bendPolarity * bentHalf;
            }
        }
        clipped = juce::jmap (klonSoftBlend, clipped, klonSoft);
    }


    if (klonVoice > 0.0001f && ! rawMode)
    {
        const float cleanHz = (452.0f + d * 78.0f) * klonCore.cleanFreqScale;
        const float cleanCoeff = detail::onePoleCoeff (cleanHz, sr);
        klonState.cleanLP += (klonInput - klonState.cleanLP) * cleanCoeff;

        const float dirtyLowHz = (205.0f + d * 105.0f) * klonCore.dirtyLowFreqScale;
        const float dirtyLowCoeff = detail::onePoleCoeff (dirtyLowHz, sr);
        klonState.dirtyLowLP += (clipped - klonState.dirtyLowLP) * dirtyLowCoeff;

        const float peakOpen = detail::smoothStep01 (detail::clampF (peak, 0.0f, 1.0f));
        const float dirtyHz = (1450.0f + d * 1050.0f + peakOpen * (420.0f + d * 460.0f))
                            * klonCore.dirtyFreqScale;
        const float dirtyCoeff = detail::onePoleCoeff (dirtyHz, sr);
        klonState.dirtyLP += (clipped - klonState.dirtyLP) * dirtyCoeff;

        const float cleanPath = klonState.cleanLP * juce::jmap (d, 1.10f, 0.86f);
        const float dirtyToneMix = detail::clampF (0.86f + (1.0f - d) * 0.04f
                                                - typeTone * 0.025f
                                                - klonSymNeg * (0.045f + d * 0.035f)
                                                + klonSymPos * (0.018f + d * 0.018f)
                                                + klonCore.dirtyToneOffset, 0.36f, 1.24f);
        const float dirtyFiltered = clipped + (klonState.dirtyLP - clipped) * dirtyToneMix;
        const float dirtyPath = dirtyFiltered - klonState.dirtyLowLP * (0.30f + d * 0.20f)
                                                    * klonCore.dirtyLowMixScale;

        const float cleanFloor = juce::jmap (typeTone, 0.030f, 0.014f)
                               * detail::smoothStep01 (d);
        const float cleanSymMask = 1.0f - klonSymNeg * (0.46f + d * 0.24f + typeTone * 0.18f)
                                        - klonSymPos * (0.22f + d * 0.12f + typeTone * 0.08f);
        const float cleanAmount = detail::clampF (cleanFloor
                                + 0.46f * std::pow (1.0f - d, 2.65f)
                                * juce::jmap (typeTone, 1.0f, 0.72f)
                                * detail::clampF (cleanSymMask, 0.45f, 1.0f)
                                * klonCore.cleanAmountScale, 0.0f, 0.90f);
        const float dirtyAmount = (1.0f - cleanAmount * 0.42f)
                                * juce::jmap (typeTone, 0.96f, 1.16f)
                                * (1.0f + klonSymNeg * 0.16f + klonSymPos * 0.08f)
                                * klonCore.dirtyAmountScale;
        const float klonPrePost = dirtyPath * dirtyAmount + cleanPath * cleanAmount;
        float klonSum = state.deferFullKlonPostEq
            ? klonPrePost
            : processKlonPostEq (klonPrePost, klonState, sr, peak, b, d);

        clipped = juce::jmap (klonVoice, clipped, klonSum);
    }

    const float klonMakeup = juce::jmap (klonVoice, 1.0f, 1.0f + d * 0.44f);
    const float finalTrim = rawMode ? rawUnityComp
                                    : voiceLift * juce::jmap (d, 0.98f, 0.92f) * klonMakeup;

    return clipped * finalTrim;
}

// CLIPPER: transparent mastering-style sample clipper. No voicing EQ and no
// RAW variant; DRIVE lowers the threshold, KNEE controls edge/fold hold, SYM
// offsets positive/negative thresholds, TYPE morphs from sample clip to arc fold,
// and PEAK keeps the existing transient-shaving semantics from OVERDRIVE A/B.
inline float processClipper (float x, float drive, float knee, float type, float symmetry,
                              adaa::ClipperADAA& adaaState) noexcept
{
    juce::ignoreUnused (adaaState);

    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float k = detail::clampF (knee, 0.0f, 1.0f);
    const float t = detail::smoothStep01 (detail::clampF (type, 0.0f, 1.0f));
    const float b = detail::clampF (symmetry * 2.00f, -1.0f, 1.0f);

    const float threshold = detail::interpDrive5 (d, 1.0f, 0.76f, 0.48f, 0.22f, 0.035f);
    const float asym = b * 0.45f;
    const float thresholdPos = detail::clampF (threshold * (1.0f + asym), 0.01f, 1.60f);
    const float thresholdNeg = detail::clampF (threshold * (1.0f - asym), 0.01f, 1.60f);

    const float kneeScale = k * k;
    const float kneePos = juce::jlimit (1.0e-5f, thresholdPos, thresholdPos * (0.002f + 0.55f * kneeScale));
    const float kneeNeg = juce::jlimit (1.0e-5f, thresholdNeg, thresholdNeg * (0.002f + 0.55f * kneeScale));

    const auto asymmetricBend = [] (float v, float amount) noexcept
    {
        if (std::abs (amount) < 1.0e-6f)
            return v;

        const float mag = detail::clampF (std::abs (v) / kRawCeiling, 0.0f, 1.0f);
        const float weight = 0.35f + 0.65f * detail::smoothStep01 (mag);
        const float polarity = v >= 0.0f ? 1.0f : -1.0f;
        const float gain = juce::jlimit (0.35f, 1.65f, 1.0f + amount * polarity * weight);
        return v * gain;
    };

    const float bendAmount = b * (0.18f + 0.10f * t);
    const float xb = asymmetricBend (x, bendAmount);

    const float hard = adaa::ClipperADAA::clip (xb, thresholdPos, thresholdNeg, kneePos, kneeNeg);

    const auto arcFoldBranch = [] (float v, float threshold, float driveAmt, float kneeAmt) noexcept
    {
        const float T = juce::jmax (threshold, 1.0e-4f);
        // Drive in the arc mode must push the signal into the fold, not just
        // change the fold shape. Use the same threshold that drives the hard
        // clip branch so TYPE 2 tracks TYPE 1 drive instead of cancelling it.
        const float mag = std::abs (v) / T;
        const float driveCurve = std::pow (detail::clampF (driveAmt, 0.0f, 1.0f), 1.45f);
        const float phase = 0.5f * kPi + driveCurve * kPi;

        float folded = std::abs (std::sin (mag * phase));

        // High fold drive creates intentional internal zero-crossings. Without
        // density recovery inside the transfer curve, TYPE 2 can measure and
        // feel less driven than TYPE 1 even though it is folding more. This is
        // not output makeup: it is part of the arc/fold transfer itself.
        const float foldDensityExp = juce::jmap (driveCurve, 1.0f, 0.35f);
        folded = std::pow (folded, foldDensityExp);
        const float clipDensity = detail::smoothStep01 (std::min (mag, 1.0f));
        const float driveFill = 0.65f * driveCurve;
        folded = juce::jmap (driveFill, folded, clipDensity);

        const float hold = 0.10f * detail::smoothStep01 (detail::clampF (kneeAmt, 0.0f, 1.0f));
        const float held = 1.0f - std::pow (1.0f - folded, 1.0f + 12.0f * hold);
        const float shaped = juce::jmap (hold, folded, held);
        const float kneeComp = 1.0f / (1.0f + 0.65f * hold);
        return T * shaped * kneeComp;
    };

    const float soft = xb >= 0.0f ? arcFoldBranch (xb, thresholdPos, d, k)
                                  : -arcFoldBranch (-xb, thresholdNeg, d, k);
    float y = juce::jmap (t, hard, soft);

    const float outputScale = 2.0f / juce::jmax (0.02f, thresholdPos + thresholdNeg);
    y *= outputScale;
    return detail::clampF (y, -kRawCeiling, kRawCeiling);
}

inline void processDeferredFullKlonPostEq (State& state,
                                           float* left, float* right,
                                           int numSamples,
                                           float drive, float mod,
                                           int seriesCount,
                                           float sampleRate,
                                           bool rawMode) noexcept
{
    if (rawMode || numSamples <= 0 || left == nullptr)
        return;

    const float m = detail::clampF (mod, 0.0f, 1.0f);
    const float klonVoice = m <= 0.5f ? 0.0f : detail::smoothStep01 ((m - 0.5f) * 2.0f);
    if (klonVoice <= 0.0001f)
        return;

    const float effectiveDrive = detail::clipperADrive (drive, mod);
    const int passes = juce::jlimit (1, kMaxSeries, seriesCount);
    auto processChannel = [&] (float* data, int ch)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float dry = data[i];
            float wet = dry;
            for (int sp = 0; sp < passes; ++sp)
            {
                auto& post = state.overdriveBNativePost[sp][ch];
                wet = processKlonPostEqBank (wet, post.klonPostEq, sampleRate, effectiveDrive);
            }
            data[i] = juce::jmap (klonVoice, dry, wet);
        }
    };

    processChannel (left, 0);
    if (right != nullptr && right != left)
        processChannel (right, 1);
}

inline void processDeferredOverdriveAPostEq (State& state,
                                                 float* left, float* right,
                                                 int numSamples,
                                                 float drive, float mod,
                                                 int seriesCount,
                                                 float sampleRate,
                                                 bool rawMode) noexcept
{
    if (rawMode || numSamples <= 0 || left == nullptr)
        return;

    const float overdriveAVoiceAmount = overdriveAVoice (mod);
    if (overdriveAVoiceAmount <= 0.0001f)
        return;

    const float effectiveDrive = detail::clipperADrive (drive, mod);
    const int passes = juce::jlimit (1, kMaxSeries, seriesCount);
    auto processChannel = [&] (float* data, int ch)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float y = data[i];
            for (int sp = 0; sp < passes; ++sp)
            {
                auto& post = state.overdriveANativePost[sp][ch];
                y = processOverdriveAPostEqBank (y, post.overdriveAPostEq,
                                                     post.overdriveAPostHiCutLP,
                                                     sampleRate, overdriveAVoiceAmount, effectiveDrive);
            }
            data[i] = y;
        }
    };

    processChannel (left, 0);
    if (right != nullptr && right != left)
        processChannel (right, 1);
}

// TAPE: ADAA tape stage with two fitted families:
//   MOD=0   -> Rabbit-style grittier tape reference
//   MOD=1   -> current smoother tape reference
// Interpolate parameters, not outputs, so the mode remains a single cohesive
// nonlinear path instead of behaving like a parallel blend.
inline float processTape (float x, float drive, float girth, float bias, float mod,
                          bool rawMode, State& state, int ch, float sr,
                          adaa::TapeTanhADAA& adaaState,
                          bool advanceOsc = true) noexcept
{
    const int sp = state.currentSeriesPass;
    (void) sr;
    (void) advanceOsc;
    juce::ignoreUnused (rawMode);

    // Keep the old low-frequency head-bump path disabled. bumpZ1 is reused as
    // a tiny magnetic HF-loss memory, so Tape can darken with drive/over-bias
    // inside the core without adding another post-EQ layer.
    state.bumpZ2[sp][ch] = 0.0f;
    if (ch == 0)
        state.flutterPhase = 0.0f;


    const float d = detail::clampF (drive, 0.0f, 1.0f);
    const float driveOver = std::max (0.0f, drive - 1.0f);
    const float driveExtraGain = 1.0f + driveOver;
    const float character = detail::smoothStep01 (detail::clampF (girth, 0.0f, 1.0f));
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
                                                 0.90f, 1.32f, 2.35f, 4.70f, 8.20f);
    const float totalGainB = detail::interpDrive5 (d,
                                                   2.35f, 2.28f, 2.20f, 2.14f, 2.10f);

    const float pregain = juce::jmap (rabbitMod, pregainA, pregainB)
                          * driveExtraGain
                          * (1.0f + character * (0.34f + d * 0.44f));
    const float totalGain = juce::jmap (rabbitMod, totalGainA, totalGainB);

    // Tape bias is better treated as under/over-bias behaviour than as a plain
    // DC offset. Negative values under-bias the record stage (brighter, grittier);
    // positive values over-bias it (smoother, slightly softer, less HF aggression).
    const float biasClamped = detail::clampF (bias * 1.65f, -1.0f, 1.0f);
    const float underBias = std::max (-biasClamped, 0.0f);
    const float overBias  = std::max ( biasClamped, 0.0f);
    const float biasShift = biasClamped * (0.0035f + d * 0.0062f);
    float satIn = (x + biasShift) * pregain;
    satIn *= 1.0f + underBias * (0.065f + d * 0.120f)
                  - overBias  * (0.045f + d * 0.080f);
    if (underBias > 0.0001f)
    {
        const float oddBias = satIn * std::abs (satIn);
        satIn += oddBias * underBias * (0.008f + d * 0.022f);
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
            * (0.25f + driveStress * 0.75f)
            * (1.0f + character * 0.88f));
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

    if (character > 0.0001f)
    {
        const float density = character * (0.030f + d * 0.072f);
        const float fluxBody = raw * (1.0f - std::min (1.0f, std::abs (raw)))
                           * character * (0.026f + d * 0.066f);
        raw += raw * std::abs (raw) * density + fluxBody;
    }

    if (std::abs (biasClamped) > 0.0001f)
    {
        const float evenBias = biasClamped * (0.048f + d * 0.110f + character * 0.088f);
        raw += (raw * raw) / (1.0f + std::abs (raw)) * evenBias;
    }

    if (underBias > 0.0001f)
    {
        const float oddBias = raw * std::abs (raw);
        raw += oddBias * underBias * (0.018f + d * 0.040f);
    }
    if (overBias > 0.0001f)
    {
        const float oddBias = raw * std::abs (raw);
        raw -= oddBias * overBias * (0.009f + d * 0.018f);
        raw *= 1.0f - overBias * (0.025f + d * 0.040f);
    }

    if (stressCore > 0.0001f)
    {
        const float stressDensity = stressCore * (0.006f + d * 0.018f)
                                  * (0.65f + rabbitMod * 0.35f);
        raw += raw * std::abs (raw) * stressDensity;
    }

    {
        const float hfLoss = detail::clampF (
            overBias * (0.040f + d * 0.090f)
          + stressCore * (0.025f + d * 0.055f)
          + rabbitMod * d * (0.010f + character * 0.018f)
          - underBias * 0.018f,
            0.0f, 0.32f);
        if (hfLoss > 0.0001f)
        {
            const float hfTrackHz = juce::jmap (rabbitMod, 6800.0f, 4200.0f)
                                  * (1.0f - overBias * 0.18f);
            float& hfMemory = state.bumpZ1[sp][ch];
            hfMemory += (raw - hfMemory) * detail::onePoleCoeff (hfTrackHz, sr);
            raw = juce::jmap (hfLoss, raw, hfMemory);
        }
        else
        {
            state.bumpZ1[sp][ch] += (raw - state.bumpZ1[sp][ch])
                                  * detail::onePoleCoeff (9000.0f, sr);
        }
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
                          SatDiag::Collector* diagCollector = nullptr,
                          int channelsToProcessParam = 2) noexcept
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
                state.overdriveBNativePost[sp][ch].reset();
                state.overdriveANativePost[sp][ch].reset();
                state.react[sp][ch].reset();
                state.mbReact[sp][ch].reset();
                state.sagEnvelope[sp][ch] = 0.0f;
                state.dynamicsComp[sp][ch].reset();
                state.clipperPeak[sp][ch].reset();
                state.transistorPeakCatch[sp][ch].reset();
                state.triodeReact[sp][ch].reset();
                state.emphasis[sp][ch].reset();
                state.componentVoicing[sp][ch].reset();
                state.dcX[sp][ch] = state.dcY[sp][ch] = 0.0f;
                state.triodeAdaa[sp][ch].reset();
                state.transistorCoreAdaa[sp][ch].reset();
                state.tapeAdaa[sp][ch].reset();
                state.clipperAdaa[sp][ch].reset();
                state.overdriveTone[sp][ch].reset();
                state.girthAdaa[sp][ch].reset();
                state.triodeBlock[sp][ch] = 0.0f;
                state.powerSag[sp][ch] = 0.0f;
                state.tapeFlux[sp][ch] = 0.0f;
                state.tapeStressEnv[sp][ch] = 0.0f;
                state.triodeBodyPreLP[sp][ch] = 0.0f;
                state.triodeBodyPostLP[sp][ch] = 0.0f;
                state.triodeCouplingDc[sp][ch] = 0.0f;
                state.tubeBiasPostDcX[sp][ch] = 0.0f;
                state.tubeBiasPostDcY[sp][ch] = 0.0f;
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
    const int channelsToProcess = juce::jlimit (1, 2, channelsToProcessParam);
    // REACT window size (model-dependent base, scaled by react amount)
    int reactBaseWindow = 1024;
    switch (model)
    {
        case Model::Tube:        reactBaseWindow = 1024; break;
        case Model::Diode:       reactBaseWindow = 2048; break;
        case Model::OverdriveA:
        case Model::OverdriveB:
        case Model::Clipper:    reactBaseWindow = 2048; break;
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
        case Model::OverdriveA:
        case Model::OverdriveB:
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
    const float driveCurved = mapDriveParamToEffective (driveParam, model);
    const float detailDriveTarget = detail::clampF (driveParam, 0.0f, 1.0f);
    const float detailTarget = detail::clampF (detailParam, 0.0f, 1.0f);

    const bool useComponentVoicing = isComponentVoicingModel (model) && !rawMode;
    const ComponentVoicingSet componentVoicingSpec = useComponentVoicing
        ? makeComponentVoicingSet (model, detailDriveTarget, girthParam, modParam, biasParam)
        : ComponentVoicingSet {};

    // Component models use declarative pre/post voicing. Overdrive A/B and
    // Clipper keep their own dedicated analogue/matching paths.
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
                    state.componentVoicing[sp][ch].reset();
                    state.dynamicsComp[sp][ch].reset();
                    state.clipperPeak[sp][ch].reset();
                    state.transistorPeakCatch[sp][ch].reset();
                    state.triodeReact[sp][ch].reset();
                    state.dcX[sp][ch] = state.dcY[sp][ch] = 0.0f;
                    state.triodeAdaa[sp][ch].reset();
                    state.transistorCoreAdaa[sp][ch].reset();
                    state.tapeAdaa[sp][ch].reset();
                    state.clipperAdaa[sp][ch].reset();
                    state.overdriveTone[sp][ch].reset();
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
                    state.componentVoicing[sp][ch].reset();
                    state.dcX[sp][ch] = state.dcY[sp][ch] = 0.0f;
                    state.transistorCoreAdaa[sp][ch].reset();
                    state.tapeAdaa[sp][ch].reset();
                    state.clipperAdaa[sp][ch].reset();
                    state.overdriveTone[sp][ch].reset();
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
                state.tubeBiasPostDcX[sp][ch] = 0.0f;
                state.tubeBiasPostDcY[sp][ch] = 0.0f;
                state.emphasis[sp][ch].reset();
                state.componentVoicing[sp][ch].reset();
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
                state.componentVoicing[sp][ch].reset();
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
        float driveMod = 1.0f, inputMod = 1.0f, shapeMod = 0.0f;
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
            state.instability.inputDrift.advance (rate * 0.57f, instabilityAmount, sampleRate);
            state.instability.shapeDrift.advance (rate * 1.43f, instabilityAmount, sampleRate);

            const float gainDynamic  = state.instability.gainDrift.dynamic;
            const float inputDynamic = state.instability.inputDrift.dynamic;
            const float shapeDynamic = state.instability.shapeDrift.dynamic;
            constexpr float kInstabilityDynamicBase = 0.30f;
            constexpr float kInstabilityDynamicLift = 0.12f;
            const float dynamicBlend = detail::smoothStep01 (
                detail::clampF ((instabilityAmount - 0.15f) / 0.85f, 0.0f, 1.0f));
            const float dynamicWeight = kInstabilityDynamicBase + kInstabilityDynamicLift * dynamicBlend;
            const float staticWeight = 1.0f - dynamicWeight;
            const float gainInstability  = (state.instability.gainDrift.staticTol  * staticWeight + gainDynamic  * dynamicWeight) * instabilityAmount;
            const float inputInstability = (state.instability.inputDrift.staticTol * staticWeight + inputDynamic * dynamicWeight) * instabilityAmount;
            const float shapeInstability = (state.instability.shapeDrift.staticTol * staticWeight + shapeDynamic * dynamicWeight) * instabilityAmount;

            const float ceilingScale = 1.0f + instabilityAmount;

            // Output already incorporates instability scaling (depth) -- no double-scaling.
            // Scale the whole range uniformly so INST remains subtle at low values
            // but reaches a clearly unstable unit at 100%.
            driveMod = 1.0f + gainInstability  * (0.08f  * ceilingScale); // +/-8..16% gain (tube gm + resistor dividers)
            shapeMod =        shapeInstability * (0.02f  * ceilingScale); // +/-2..4% shape (plate Rp instability)

            // Separate input tolerance/thermal wobble. This is intentionally small
            // and dB-based: at INST 100% it moves the level by up to about +/-1 dB
            // before the component, without changing the user drive mapping.
            const float inputDb = detail::clampF (inputInstability * 1.0f, -1.0f, 1.0f);
            inputMod = std::pow (10.0f, inputDb / 20.0f);
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

            for (int ch = 0; ch < channelsToProcess; ++ch)
            {
                float& sample = (ch == 0) ? left[i] : right[i];
                float x = sample * (isFirst ? inputMod : 1.0f);
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
                if (isSafetyLpfOn && isFirst && !rawMode && model != Model::Clipper)
                    x = processSafetyLPF (state.safetyLpf[ch], x, safetyCoeffs);


                // Tube bias is DC-sensitive, so instability stays out of its core
                // operating point and is applied post-coupling instead.
                const bool tubeCoreInstabilitySafe = model == Model::Tube;
                const float typeEdge = std::abs (mod - 0.5f) * 2.0f;
                const float edgeShapeMod = shapeMod * (1.0f + 1.5f * typeEdge);
                float effDrive = tubeCoreInstabilitySafe ? drive : drive * driveMod;
                float effBias  = bias;
                float effMod   = tubeCoreInstabilitySafe ? mod : detail::modulateUnitAtEdges (mod, edgeShapeMod);

                // -- INTERNAL PRE-EMPHASIS (per series pass, unless rawMode) --
                if (useComponentVoicing)
                    x = processComponentPreVoicing (x, state.componentVoicing[sp][ch],
                                                    componentVoicingSpec, sampleRate);
                else if (!rawMode && model != Model::Transistor)
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
                            const float reactDepth = detail::smoothStep01 (detail::clampF (react, 0.0f, 1.0f));
                            const float program = detail::smoothStep01 (detail::clampF (sagEnv, 0.0f, 1.0f));
                            // DIODE REACT is conductance memory, not a gain compressor.
                            // Keep level untouched here and let the diode core move threshold/knee/asymmetry.
                            diodeReactColor = detail::clampF (
                                reactDepth * (0.32f + program * 0.68f) * (0.82f + effDrive * 0.18f),
                                0.0f, 1.0f);
                            sagPre = 1.0f;
                            sagPost = 1.0f;
                            stageDynamicsComp.gain += (1.0f - stageDynamicsComp.gain) * 0.20f;
                            stageDynamicsComp.env *= 0.5f;
                            stageDynamicsComp.hfEnv *= 0.5f;
                            stageDynamicsComp.bodyEnv *= 0.5f;
                            stageSagEnvelope = diodeReactColor;
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
                        case Model::OverdriveA:
                        case Model::OverdriveB:
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
                else if (model == Model::OverdriveA || model == Model::OverdriveB || model == Model::Clipper)
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
                        x = processTape (x, effDrive, girth, effBias, effMod, rawMode,
                                         state, ch, sampleRate,
                                         state.tapeAdaa[sp][ch], isFirst);
                        break;
                    }
                    case Model::OverdriveA:
                    case Model::OverdriveB:
                    {
                        x = processOverdriveCore (x, effDrive, girth, effBias, effMod, react,
                                            model, rawMode, state, ch, sampleRate,
                                            state.clipperAdaa[sp][ch]);
                        break;
                    }
                    case Model::Clipper:
                    {
                        x = processClipper (x, effDrive, girth, effMod, effBias,
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
                else if (model == Model::Transistor || model == Model::OverdriveA
                      || model == Model::OverdriveB
                      || model == Model::Clipper
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
                if (useComponentVoicing)
                    x = processComponentPostVoicing (x, state.componentVoicing[sp][ch],
                                                     componentVoicingSpec, sampleRate);
                else if (!rawMode && model != Model::Transistor)
                    x = deEmphasize (x, stageEmphasis, model, effDrive, effMod, emphCoeffs);


                // -- DC BLOCKER (1st-order HPF at 5Hz, per series pass) --
                if (!rawMode && model != Model::Clipper)
                {
                    const float dcOut = x - stageDcX + dcR * stageDcY;
                    stageDcX = x;
                    stageDcY = dcOut;
                    x = dcOut;
                }

                if (model == Model::Tube)
                {
                    // Tube BIAS is handled inside processTriode as an operating
                    // point shift. Do not add another post-DC half-cycle tilt here:
                    // it turns bias into output imbalance and breaks +BIAS/-BIAS
                    // mirror behaviour. Keep the old dedicated states flushed.
                    state.tubeBiasPostDcX[sp][ch] = 0.0f;
                    state.tubeBiasPostDcY[sp][ch] = 0.0f;
                }


                if (model == Model::Tube && isLast)
                    x *= driveMod;

                if (diagCollector != nullptr && isLast && ch == 0)
                    diagCollector->feedDc (x);

                // -- Model level trim --
                // Static trims are applied per black-box stage so SERIES feeds
                // each repeated stage at a stable nominal level. The measured
                // series/hot-input corrections remain final-chain trims.
                if (model == Model::Transistor)
                {
                    x *= getTransistorLevelTrim (drive, mod, girth, react);
                    if (isLast)
                        x *= getTransistorLevelCorrection (detailDrive, girth, mod, state.currentSeriesCount)
                           * getHotInputReferenceCorrection (model, detailDrive, girth, mod, state.currentSeriesCount);
                }
                else if (model == Model::Tape)
                {
                    const float stageTrim = getTapeLevelTrim (drive, mod, girth, react);
                    x *= stageTrim;
                    if (isLast)
                        x *= getStageTrimMigrationCorrection (model, state.currentSeriesCount)
                           * getTapeLevelCorrection (detailDrive, girth, mod, state.currentSeriesCount)
                           * getHotInputReferenceCorrection (model, detailDrive, girth, mod, state.currentSeriesCount);
                }
                else if (model == Model::Tube)
                {
                    const float stageTrim = getTriodeLevelTrim (drive, mod, state.currentSeriesCount);
                    x *= stageTrim;
                    if (isLast)
                        x *= getStageTrimMigrationCorrection (model, state.currentSeriesCount)
                           * getTriodeLevelCorrection (detailDrive, girth, mod, state.currentSeriesCount)
                           * getHotInputReferenceCorrection (model, detailDrive, girth, mod, state.currentSeriesCount);
                }
                else if (model == Model::OverdriveA || model == Model::OverdriveB)
                {
                    if (!rawMode)
                    {
                        const float overdriveVoice = model == Model::OverdriveB ? 1.0f : 0.0f;
                        x *= getClipperLevelTrim (drive, girth, overdriveVoice);
                        if (isLast)
                            x *= getClipperLevelCorrection (detailDrive, girth, overdriveVoice, state.currentSeriesCount);
                    }
                }
                else if (isLast)
                {
                    if (model == Model::Diode)
                        x *= getDiodeLevelTrim (detailDrive, girth, mod, state.currentSeriesCount)
                           * getHotInputReferenceCorrection (model, detailDrive, girth, mod, state.currentSeriesCount);
                }

                if (rawMode && isLast && model != Model::OverdriveA && model != Model::OverdriveB)
                    x *= getRawModeLevelCorrection (model, detailDrive, girth, mod, state.currentSeriesCount);

                if (rawMode && isLast)
                    x = detail::clampF (x, -kRawCeiling, kRawCeiling);

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

        for (int ch = 0; ch < channelsToProcess; ++ch)
        {
            float& sample = (ch == 0) ? left[i] : right[i];
            const float detailDelta = makeDetailHardClipDelta (detailChainInput[ch], detailDrive);
            sample = applyDetailPreservation (sample, detailDelta, detailAmount,
                                               state.detailState[ch], detailCoeffs);

            if (rawMode)
                sample = detail::clampF (sample, -kRawCeiling, kRawCeiling);
        }
    }
}

} // namespace SatEngine

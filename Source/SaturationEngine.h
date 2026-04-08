#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>
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
    Triode    = 2,   // 12AX7-style preamp stage
    PushPull  = 3,   // Power stage — hybrid EL34/6L6 class AB feel
    Cascade   = 4,   // Cascaded triode stages (1-4, fractional crossfade)
    Diode     = 5,   // Shockley diode clipper — Ge/Si blend
    Tundra    = 6,   // Metal Zone→Modern morph — dual-stage cascaded clipping
    Fuzz      = 7,   // Transistor fuzz — Ge/Si stages with bias starving
    Doom      = 8,   // Multi-stage doom fuzz — Big Muff inspired wall-of-sound
    Destroy   = 9,   // Ring-fuzz — pitch-tracked ring modulation + feedback
    NumModels = 10
};

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
    // REACT energy tracker (per-channel)
    ReactState react[2];

    // REACT sag envelope follower (per-channel, asymmetric attack/release)
    float sagEnvelope[2] = {};

    // CASCADE per-stage DC accumulation (coupling cap model)
    // [series pass][stage][channel]
    float cascadeDC[kMaxSeries][kMaxCascade][2] = {};

    // DC blocker (1st-order HPF, post-saturation)
    float dcX[2] = {};
    float dcY[2] = {};

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

    // Internal emphasis/de-emphasis (per-channel)
    EmphasisState emphasis[2];

    // ADAA states — main waveshaper [series pass][channel]
    adaa::TanhADAA wsAdaa[kMaxSeries][2];
    adaa::TapeTanhADAA tapeAdaa[kMaxSeries][2];
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

    // Multiband REACT (per-channel)
    MultibandReactState mbReact[2];

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

    // Current series pass index (set by processBlock before waveshapers)
    int currentSeriesPass = 0;

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
            react[ch].reset();
            mbReact[ch].reset();
            sagEnvelope[ch] = 0.0f;
            dcX[ch] = dcY[ch] = 0.0f;
            emphasis[ch].reset();
            safetyLpf[ch].reset();
            for (int sp = 0; sp < kMaxSeries; ++sp)
            {
                bumpZ1[sp][ch] = bumpZ2[sp][ch] = 0.0f;
                triodeBlock[sp][ch] = 0.0f;
                powerSag[sp][ch] = 0.0f;
                tapeFlux[sp][ch] = 0.0f;
                wsAdaa[sp][ch].reset();
                tapeAdaa[sp][ch].reset();
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
            fl (emphasis[ch].preHP);
            fl (emphasis[ch].preSh);
            fl (emphasis[ch].postHP);
            fl (emphasis[ch].postLP);
            fl (subOscEnv[ch]);
            fl (subOscLPF[ch]);
            fl (sagEnvelope[ch]);
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
} // namespace detail

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

// ══════════════════════════════════════════════════════════════
//  Internal Emphasis / De-emphasis
// ══════════════════════════════════════════════════════════════
struct EmphCoeffs {
    float preHP = 0, preSh = 0, postLP = 0, postHP = 0;
};

inline float preEmphasize (float x, EmphasisState& st, Model model,
                           float drive, float mod, const EmphCoeffs& ec) noexcept
{
    switch (model)
    {
        case Model::Triode:
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
        case Model::Tape:
        {
            // Keep the record side almost transparent, but trim a little
            // sub-energy before the nonlinearity so the stage feels more like
            // a tape bus than a flat digital clipper.
            st.preHP += (x - st.preHP) * ec.preHP;
            float hp = x - st.preHP;
            (void) mod;
            return hp;
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
    switch (model)
    {
        case Model::Triode:
        case Model::Cascade:
        {
            // LPF at 3500Hz: undo the pre-emphasis treble boost.
            // NO HPF here — previous postLP-postHP bandpass caused 40-65%
            // overshoot at guitar frequencies due to slow postHP (70Hz)
            // accumulating DC from asymmetry, then releasing at zero crossings.
            // The 5Hz DC blocker downstream handles any DC from asymmetry.
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
        case Model::Tape:
        {
            // Airwindows-style polish: keep the post stage subtle and broad.
            // No heavy de-emphasis, only a gentle HF rounding that increases
            // with drive and remains bypassable via RAW mode upstream.
            st.postLP += (y - st.postLP) * ec.postLP;
            const float lpMix = 0.025f + drive * 0.095f;
            (void) mod;
            return y + (st.postLP - y) * lpMix;
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

// TRIODE: 12AX7-style preamp stage with normalized small-signal behavior
inline float processTriode (float x, float drive, float bias, float mod,
                            State& state, int ch, float sr,
                            adaa::TanhADAA& adaaState) noexcept
{
    const int sp = state.currentSeriesPass;
    const float gain = 1.0f + drive * (3.2f + mod * 1.3f);
    const float biasShift = bias * 0.055f + drive * (0.010f + mod * 0.015f);
    const float k = 0.95f + drive * (2.2f + mod * 1.1f);

    // Positive grid conduction charges the coupling network and gently moves
    // the operating point during hard hits, but it should stay invisible at
    // low drive and in the sustain tail.
    const float conduction = std::max (0.0f, x * gain + biasShift - (0.32f - drive * 0.08f));
    const float attack = detail::onePoleCoeff (250.0f + drive * 350.0f, sr);
    const float release = detail::onePoleCoeff (2.5f + drive * 4.0f, sr);
    const float targetBlock = conduction * (0.10f + drive * 0.18f);
    if (targetBlock > state.triodeBlock[sp][ch])
        state.triodeBlock[sp][ch] += (targetBlock - state.triodeBlock[sp][ch]) * attack;
    else
        state.triodeBlock[sp][ch] += (targetBlock - state.triodeBlock[sp][ch]) * release;

    const float blockBias = state.triodeBlock[sp][ch];
    const float opBias = biasShift - blockBias;
    const float s = x * gain + opBias;

    const float raw = adaaState.process (s, k);
    const float raw0 = std::tanh (k * opBias);
    const float raw0t = std::tanh (k * opBias);
    const float slope0 = gain * k * detail::sech2FromTanh (raw0t);
    float out = detail::normalizeSmallSignal (raw, raw0, slope0);

    // Plate load asymmetry / even harmonics, constructed so the derivative at
    // zero stays unchanged.
    const float driveDelta = x * gain;
    const float even = driveDelta * std::abs (driveDelta);
    out += even * (0.020f + drive * 0.060f) * (0.4f + 0.6f * (bias * 0.5f + 0.5f));

    return out;
}

// POWER: Hybrid 6L6/EL34 class-AB output stage
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

// TAPE: fitted ADAA tape stage based on the reference family.
// Base behavior:
//   0%   -> almost linear with ~+1.12 dB
//   50%  -> mild rounded saturation
//   100% -> dense, strongly compressed tanh-like stage
inline float processTape (float x, float drive, float bias, float mod,
                          State& state, int ch, float sr,
                          adaa::TapeTanhADAA& adaaState,
                          bool advanceOsc = true) noexcept
{
    const int sp = state.currentSeriesPass;
    (void) mod;
    (void) sr;
    (void) advanceOsc;

    // This base implementation intentionally ignores the legacy magnetic-memory
    // extras until the core transfer matches the reference family.
    state.tapeFlux[sp][ch] = 0.0f;
    state.bumpZ1[sp][ch] = 0.0f;
    state.bumpZ2[sp][ch] = 0.0f;
    if (ch == 0)
        state.flutterPhase = 0.0f;

    const float d = detail::clampF (drive, 0.0f, 1.0f);

    // Endpoint fits derived from comparison renders:
    //   50%  -> tanh(pre*x)/pre with pre ~= 3.184 and gain ~= 1.0625
    //   100% -> tanh(pre*x)/pre with pre ~= 24.649 and gain ~= 3.5497
    constexpr float baseGain = 1.1438f;       // matches the 0% reference lift
    constexpr float pre0     = 1.0f;          // keeps ADAA bounded and nearly linear
    constexpr float pre50    = 3.30f;
    constexpr float pre100   = 22.50f;
    constexpr float mu50     = 0.95f;         // gain50 / baseGain
    constexpr float mu100    = 3.00f;         // gain100 / baseGain

    float pregain = pre0;
    float makeup = 1.0f;

    if (d <= 0.5f)
    {
        const float t = detail::smoothStep01 (d * 2.0f);
        pregain = juce::jmap (t, pre0, pre50);
        makeup = juce::jmap (t, 1.0f, mu50);
    }
    else
    {
        const float u = (d - 0.5f) * 2.0f;
        const float t = detail::smoothStep01 (u);
        pregain = juce::jmap (t, pre50, pre100);
        makeup = juce::jmap (t, mu50, mu100);
    }

    // Keep bias subtle in this base fit so the comparison path remains mostly
    // symmetric with default controls.
    const float biasShift = bias * (0.0015f + d * 0.0025f);
    const float satIn = (x + biasShift) * pregain;

    // Use a fixed tanh ADAA kernel. Varying k inside the ADAA state was a
    // likely source of the pathological spikes seen in the diagnostics.
    const float raw = adaaState.process (satIn, 1.0f);
    return raw * (baseGain * makeup / pregain);
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
    // Per-model exponent: gentle models use lower exponent, aggressive
    // cascaded models use higher exponent to spread usable range.
    float exp;
    switch (model)
    {
        case Model::Triode:     exp = 2.0f; break;  // make low-drive range cleaner and wider
        case Model::PushPull:   exp = 2.0f; break;  // power stage should stay cleaner early on
        case Model::Cascade:    exp = 2.0f; break;  // multi-stage, gain compounds
        case Model::Diode:      exp = 1.8f; break;  // single stage, high gain (×40)
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
    float peakOut = 1.0f;
    switch (model)
    {
        case Model::Triode:
        {
            const float peak = 1.0f + drive * 0.28f;
            peakOut = 1.0f + (peak - 1.0f) * detail::clampF (drive * 2.5f, 0.0f, 1.0f);
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
    // CLEAN model: 1:1 pass-through — no saturation processing at all
    if (model == Model::Clean)
        return;

    // ── Model-switch detection: reset filters & feedback to prevent transient explosions ──
    if (model != state.lastModel)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            state.emphasis[ch].reset();
            state.dcX[ch] = state.dcY[ch] = 0.0f;
            for (int sp = 0; sp < kMaxSeries; ++sp)
            {
                state.wsAdaa[sp][ch].reset();
                state.tapeAdaa[sp][ch].reset();
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
        case Model::Tape:     reactBaseWindow = 2048; break;
        case Model::Fuzz:     reactBaseWindow = 4096; break;
        case Model::Doom:     reactBaseWindow = 4096; break;
        case Model::Destroy:  reactBaseWindow = 2048; break;
        case Model::Tundra:   reactBaseWindow = 2048; break;
        default: break;
    }

    // Inter-stage LPF: model-dependent Miller-cap roll-off between series passes
    float interStageFreq = 6000.0f;
    switch (model)
    {
        case Model::Triode:   interStageFreq = 6500.0f; break;
        case Model::PushPull: interStageFreq = 7500.0f; break;
        case Model::Cascade:  interStageFreq = 5000.0f; break;
        case Model::Diode:    interStageFreq = 5000.0f; break;
        case Model::Tape:     interStageFreq = 9000.0f; break;
        case Model::Fuzz:     interStageFreq = 6000.0f; break;
        case Model::Doom:     interStageFreq = 4000.0f; break;
        case Model::Destroy:  interStageFreq = 10000.0f; break;
        case Model::Tundra:   interStageFreq = 5000.0f; break;
        default: break;
    }
    const float interStageCoeff = detail::onePoleCoeff (interStageFreq, sampleRate);
    // Inter-stage DC blocker (coupling cap HPF, same as output DC blocker)
    const float interStageDCR = 1.0f - (kTwoPi * 30.0f / sampleRate);

    // Precomputed emphasis/de-emphasis coefficients (hoisted from per-sample)
    EmphCoeffs emphCoeffs;
    switch (model)
    {
        case Model::Triode:
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
        case Model::Tape:
            emphCoeffs.preHP  = detail::onePoleCoeff (28.0f,   sampleRate);
            emphCoeffs.postLP = detail::onePoleCoeff (12500.0f, sampleRate);
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
        const bool tapeActive = driveCurved > 0.001f;
        const bool driveJump = std::abs (driveCurved - state.lastTapeDrive) > 0.15f;
        if (driveJump || tapeActive != state.tapeWasActive)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                state.emphasis[ch].reset();
                state.dcX[ch] = state.dcY[ch] = 0.0f;
                for (int sp = 0; sp < kMaxSeries; ++sp)
                {
                    state.tapeAdaa[sp][ch].reset();
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
            // Init static tolerances once (deterministic seed → same "unit" every session)
            if (! state.variation.tolerancesReady)
                state.variation.initTolerances (0x5A54'5231u);  // fixed seed = one specific amp unit

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

        // ── Per-sample series passes (inner loop — proper cascading) ──
        for (int sp = 0; sp < seriesCount; ++sp)
        {
            state.currentSeriesPass = sp;
            const bool isFirst = (sp == 0);
            const bool isLast  = (sp == seriesCount - 1);

            for (int ch = 0; ch < 2; ++ch)
            {
                float& sample = (ch == 0) ? left[i] : right[i];
                float x = sample;

                // ── Safety LPF (first pass only, ×1 mode) ──
                if (isSafetyLpfOn && isFirst)
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

                // ── INTERNAL PRE-EMPHASIS (first pass only, unless rawMode) ──
                if (isFirst && !rawMode)
                    x = preEmphasize (x, state.emphasis[ch], model, effDrive, effMod, emphCoeffs);

                // ── REACT: energy tracking + per-model processing (first pass only) ──
                float sagPre  = 1.0f;
                float sagPost = 1.0f;
                float sagBias = 0.0f;
                bool  doSubOctave = false;
                MultibandSagResult mbSag;
                bool useMbSag = false;

                if (isFirst && react > 0.001f)
                {
                    const int window = std::min (
                        (int) ((float) reactBaseWindow * (1.0f + react * 3.0f) * sampleRate / 44100.0f),
                        kReactBufSize - 1);

                    reactTrackEnergy (state.react[ch], x, window);
                    const float depletion = reactGetDepletion (state.react[ch], window);

                    if (depletion > state.sagEnvelope[ch])
                        state.sagEnvelope[ch] += (depletion - state.sagEnvelope[ch]) * reactAttCoeff;
                    else
                        state.sagEnvelope[ch] += (depletion - state.sagEnvelope[ch]) * reactRelCoeff;

                    const float sagEnv = state.sagEnvelope[ch];

                    switch (model)
                    {
                        case Model::Triode:
                        case Model::PushPull:
                        case Model::Cascade:
                        {
                            // Multiband REACT: frequency-dependent sag (bass sags more)
                            mbSag = multibandReactProcess (
                                state.mbReact[ch], x, mbSubCoeff, mbAirCoeff,
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
                            sagPre  = 1.0f + sagEnv * react * 1.0f;
                            sagPost = 1.0f / (1.0f + sagEnv * react * 2.5f);
                            break;
                        case Model::Fuzz:
                        case Model::Doom:
                        case Model::Destroy:
                        case Model::Tundra:
                            doSubOctave = true;
                            break;
                        default: break;
                    }
                }

                // Apply pre-sag boost + bias drift (first pass only)
                if (isFirst)
                {
                    if (useMbSag)
                    {
                        // Apply multiband pre-sag: split→boost per band→recombine
                        float subSig, midSig, airSig;
                        multibandReactSplit (state.mbReact[ch], x, mbSubCoeff, mbAirCoeff,
                                            subSig, midSig, airSig);
                        x = subSig * mbSag.sagPreSub + midSig * mbSag.sagPreMid + airSig * mbSag.sagPreAir;
                    }
                    else
                    {
                        x *= sagPre;
                    }
                    effBias += sagBias;
                }

                // ── WAVESHAPER (all passes, with per-pass ADAA state) ──
                switch (model)
                {
                    case Model::Triode:
                        x = processTriode (x, effDrive, effBias, effMod,
                                           state, ch, sampleRate,
                                           state.wsAdaa[sp][ch]);
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
                        x = processTape (x, effDrive, effBias, effMod,
                                         state, ch, sampleRate,
                                         state.tapeAdaa[sp][ch], isFirst);
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

                if (diagCollector != nullptr && model == Model::Tape && isLast && ch == 0)
                    diagCollector->feedTapeCore (x);

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

                if (diagCollector != nullptr && model == Model::Tape && isLast && ch == 0)
                    diagCollector->feedTapeClip (x);

                // ── Post-sag ceiling (first pass only) ──
                if (isFirst)
                {
                    if (useMbSag)
                    {
                        // Multiband post-sag: split→attenuate per band→recombine
                        float subSig, midSig, airSig;
                        multibandReactSplit (state.mbReact[ch], x, mbSubCoeff, mbAirCoeff,
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

                // ── GIRTH (all passes, with per-pass ADAA) ──
                x = applyGirth (x, girth, state.girthAdaa[sp][ch]);

                // ── Inter-stage filtering (between series passes, always active) ──
                // Analogous to Miller capacitance + coupling caps between tube stages.
                // LPF prevents cascaded harmonic aliasing; DC blocker removes
                // accumulated DC offset from bias (prevents silence at high series).
                if (! isLast && seriesCount > 1)
                {
                    // Miller-cap LPF (model-dependent frequency)
                    state.interStageLPF[sp][ch] += (x - state.interStageLPF[sp][ch]) * interStageCoeff;
                    x = state.interStageLPF[sp][ch];

                    // Coupling-cap DC blocker (HPF ~30Hz)
                    const float dcOut = x - state.interStageDCx[sp][ch]
                                      + interStageDCR * state.interStageDCy[sp][ch];
                    state.interStageDCx[sp][ch] = x;
                    state.interStageDCy[sp][ch] = dcOut;
                    x = dcOut;
                }

                // ── INTERNAL DE-EMPHASIS (last pass only, unless rawMode) ──
                if (isLast && !rawMode)
                    x = deEmphasize (x, state.emphasis[ch], model, effDrive, effMod, emphCoeffs);

                // ── DC BLOCKER (1st-order HPF at 5Hz, last pass only) ──
                if (isLast)
                {
                    const float dcOut = x - state.dcX[ch] + dcR * state.dcY[ch];
                    state.dcX[ch] = x;
                    state.dcY[ch] = dcOut;
                    x = dcOut;
                }

                if (diagCollector != nullptr && model == Model::Tape && isLast && ch == 0)
                    diagCollector->feedTapeDc (x);

                // ── AUTO-GAIN COMPENSATION (last pass only, precomputed per-block) ──
                if (isLast && !skipAutoGain)
                    x *= state.blockCoeffs.autoGain;

                // ── Final safety soft-limiter (AFTER auto-gain) ──
                // Transparent below ±1.5, smooth compression above, max ±2.5
                // Prevents auto-gain from amplifying clamped signal past safe levels
                // and eliminates hard-clip discontinuities that cause audible clicks.
                {
                    const float absX = std::abs (x);
                    if (absX > 1.5f)
                    {
                        const float sign = (x >= 0.0f) ? 1.0f : -1.0f;
                        x = sign * (1.5f + std::tanh (absX - 1.5f));
                    }
                }

                if (diagCollector != nullptr && model == Model::Tape && isLast && ch == 0)
                    diagCollector->feedTapeLim (x);

                sample = x;
            }
        }
    }
}

} // namespace SatEngine

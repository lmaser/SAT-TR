#pragma once
// ============================================================================
// SatDspDiag.h — Lock-free DSP diagnostics for SAT-TR
//
// Captures per-block snapshots of key DSP metrics on the audio thread
// (zero-alloc, no mutex). A timer thread periodically drains the ring buffer
// and writes a human-readable report to Desktop/SAT-TR_DspDiag.txt.
//
// Enable:  #define SAT_DSP_DIAG 1   (set to 0 for zero overhead in release)
// ============================================================================

#include <JuceHeader.h>
#include <atomic>
#include <cmath>
#include <cstdio>

#ifndef SAT_DSP_DIAG
 #define SAT_DSP_DIAG 1
#endif

namespace SatDiag
{

// ── Per-block snapshot ──────────────────────────────────────────────────────
struct BlockSnap
{
    int      implRev        = 24040813;

    // Timing
    double   blockTimeUs    = 0.0;   // wall-clock time for processBlock (µs)
    int      numSamples     = 0;
    float    sampleRate     = 0.0f;
    double   cpuPercent     = 0.0;   // block time / available time × 100

    // Model info
    int      model          = 0;
    int      seriesCount    = 1;
    int      osOrder        = 0;     // 0=none, 1=2×, 2=4×, etc.

    // Parameters
    float    drive          = 0.0f;
    float    girth          = 0.0f;
    float    mod            = 0.0f;
    float    bias           = 0.0f;
    float    react          = 0.0f;

    // Signal levels (peak absolute per block, L channel)
    float    peakIn         = 0.0f;  // input to saturation engine
    float    peakOut        = 0.0f;  // output of saturation engine (post auto-gain + soft-limiter)
    float    peakPreAG      = 0.0f;  // pre-auto-gain peak (raw waveshaper output)
    float    peakFinal      = 0.0f;  // final output (post all processing)
    float    satDeltaPeak   = 0.0f;  // peak |processed loader - loader input|
    float    autoGainVal    = 1.0f;  // precomputed auto-gain multiplier
    float    tapeCorePeak   = 0.0f;  // direct output of processTape() on last series pass
    float    tapeClipPeak   = 0.0f;  // after intermediate safety clip
    float    tapeDcPeak     = 0.0f;  // after final DC blocker
    float    tapeLimPeak    = 0.0f;  // after final safety limiter

    // Routing / context
    int      route          = 0;
    int      diagLoader     = 0;     // 0=A, 1=B, 2=C
    int      enableMask     = 0;     // bitmask A=1 B=2 C=4
    float    globalMix      = 1.0f;
    float    mixA           = 1.0f;
    float    mixB           = 1.0f;
    float    mixC           = 1.0f;

    // ADAA diagnostics (L channel, last series pass)
    float    adaaLastDx     = 0.0f;  // last |dx| in ADAA-2
    float    adaaLastK      = 0.0f;  // last k value fed to ADAA-2
    int      adaaFallbacks  = 0;     // # samples in fallback (|dx| < kTol)
    int      adaaBlends     = 0;     // # samples in blend zone
    int      adaaFull       = 0;     // # samples in full ADAA-2

    // NaN / Inf / denormal counters (across all channels)
    int      nanCount       = 0;
    int      infCount       = 0;
    int      denormalCount  = 0;

    // Click detector: max absolute sample-to-sample delta
    float    maxDelta       = 0.0f;
    int      clickCount     = 0;     // # deltas > clickThreshold

    // Feedback state
    float    fuzzFeedback   = 0.0f;
    float    doomFeedback   = 0.0f;
    float    destroyFeedback = 0.0f;

    // Filter state magnitudes (max across series passes, L channel)
    float    maxFilterState = 0.0f;  // largest one-pole state value
    float    maxDcState     = 0.0f;  // largest DC-blocker state value

    // REACT / sag
    float    sagEnvelope    = 0.0f;
    float    yinFreq        = 0.0f;

    // Timestamp
    juce::int64 timestampMs = 0;
};

// ── Lock-free SPSC ring buffer ──────────────────────────────────────────────
static constexpr int kRingSize = 512;  // ~10 seconds @ 48kHz/512 block

#if SAT_DSP_DIAG

class DiagRing
{
public:
    void push (const BlockSnap& snap) noexcept
    {
        const int w = writePos_.load (std::memory_order_relaxed);
        ring_[w] = snap;
        writePos_.store ((w + 1) & (kRingSize - 1), std::memory_order_release);
    }

    bool pop (BlockSnap& snap) noexcept
    {
        const int r = readPos_.load (std::memory_order_relaxed);
        const int w = writePos_.load (std::memory_order_acquire);
        if (r == w) return false;
        snap = ring_[r];
        readPos_.store ((r + 1) & (kRingSize - 1), std::memory_order_release);
        return true;
    }

private:
    BlockSnap ring_[kRingSize];
    std::atomic<int> writePos_ { 0 };
    std::atomic<int> readPos_  { 0 };
};

// ── Collector: accumulates per-sample stats during a block ─────────────────
struct Collector
{
    float  peakIn       = 0.0f;
    float  peakOut      = 0.0f;
    float  peakPreAG    = 0.0f;   // pre-auto-gain peak (raw waveshaper + filters)
    float  tapeCorePeak = 0.0f;
    float  tapeClipPeak = 0.0f;
    float  tapeDcPeak   = 0.0f;
    float  tapeLimPeak  = 0.0f;
    float  prevSample   = 0.0f;
    float  maxDelta     = 0.0f;
    int    clickCount   = 0;
    int    nanCount     = 0;
    int    infCount     = 0;
    int    denormals    = 0;
    int    adaaFB       = 0;   // fallback count
    int    adaaBlend    = 0;   // blend zone count
    int    adaaFull     = 0;   // full ADAA-2 count
    float  lastDx       = 0.0f;
    float  lastK        = 0.0f;
    bool   started      = false;

    static constexpr float kClickThresh = 0.65f; // per-sample delta threshold (0.35 still triggers on normal harmonics at high drive; avg delta=0.39 at drv=1.0 Triode)

    void reset() noexcept
    {
        peakIn = peakOut = peakPreAG = prevSample = maxDelta = lastDx = lastK = 0.0f;
        tapeCorePeak = tapeClipPeak = tapeDcPeak = tapeLimPeak = 0.0f;
        clickCount = nanCount = infCount = denormals = 0;
        adaaFB = adaaBlend = adaaFull = 0;
        started = false;
    }

    void feedIn (float x) noexcept
    {
        const float a = std::abs (x);
        if (a > peakIn) peakIn = a;
    }

    void feedOut (float x) noexcept
    {
        const float a = std::abs (x);
        if (a > peakOut) peakOut = a;

        if (started)
        {
            const float delta = std::abs (x - prevSample);
            if (delta > maxDelta) maxDelta = delta;
            if (delta > kClickThresh) ++clickCount;
        }
        prevSample = x;
        started = true;

        // NaN/Inf/denormal check
        if (std::isnan (x)) ++nanCount;
        else if (std::isinf (x)) ++infCount;
        else if (a > 0.0f && a < 1.175e-38f) ++denormals;
    }

    // Track pre-auto-gain peak (raw waveshaper + filters output before gain compensation)
    void feedPreAG (float x) noexcept
    {
        const float a = std::abs (x);
        if (a > peakPreAG) peakPreAG = a;
    }

    void feedTapeCore (float x) noexcept
    {
        const float a = std::abs (x);
        if (a > tapeCorePeak) tapeCorePeak = a;
    }

    void feedTapeClip (float x) noexcept
    {
        const float a = std::abs (x);
        if (a > tapeClipPeak) tapeClipPeak = a;
    }

    void feedTapeDc (float x) noexcept
    {
        const float a = std::abs (x);
        if (a > tapeDcPeak) tapeDcPeak = a;
    }

    void feedTapeLim (float x) noexcept
    {
        const float a = std::abs (x);
        if (a > tapeLimPeak) tapeLimPeak = a;
    }

    void feedAdaa (float dx, float k, int path) noexcept
    {
        // path: 0=fallback, 1=blend, 2=full
        lastDx = dx;
        lastK  = k;
        switch (path) {
            case 0: ++adaaFB;    break;
            case 1: ++adaaBlend; break;
            default: ++adaaFull; break;
        }
    }
};

// ── File writer (called from timer thread, NOT audio thread) ───────────────
class DiagWriter
{
public:
    void setEnabled (bool e) { enabled_ = e; }
    bool isEnabled() const { return enabled_; }

    void drain (DiagRing& ring)
    {
        if (! enabled_) return;

        BlockSnap snap;
        int count = 0;
        const int kMaxPerDrain = 64;

        while (ring.pop (snap) && count++ < kMaxPerDrain)
        {
            if (! fileReady_)
                initFile();

            writeSnap (snap);
        }
    }

    void writeHeader()
    {
        initFile();
    }

private:
    void initFile()
    {
        auto desktop = juce::File::getSpecialLocation (juce::File::userDesktopDirectory);
        file_ = desktop.getChildFile ("SAT-TR_DspDiag.txt");

        // Rotate: keep max ~1MB
        if (file_.existsAsFile() && file_.getSize() > 1024 * 1024)
            file_.deleteFile();

        if (! file_.existsAsFile())
        {
            file_.create();
            juce::String hdr;
            hdr << "================================================================\n"
                << "SAT-TR DSP Diagnostics — " << juce::Time::getCurrentTime().toString (true, true) << "\n"
                << "================================================================\n"
                << "Columns:\n"
                << "  time_ms  cpu%  model  sr  blk  os  ser | drive girth mod bias react\n"
                << "  pkIn  pkOut  pkPreAG  pkFin  autoGain | maxDlt clicks | nan inf dnrm\n"
                << "  adaaFB adaaBlend adaaFull lastDx lastK\n"
                << "  fuzzFB doomFB destFB maxFilt sagEnv yinHz\n"
                << "================================================================\n\n";
            file_.appendText (hdr);
        }
        fileReady_ = true;
    }

    void writeSnap (const BlockSnap& s)
    {
        if (! fileReady_) return;

        // Only log "interesting" blocks to avoid flooding:
        // - CPU > 30%
        // - Any NaN/Inf
        // - Click detected
        // - Peak > 2.0 (possible explosion)
        // - Every 200th block (periodic heartbeat)
        heartbeat_++;
        const bool isInteresting =
            s.cpuPercent > 30.0 ||
            s.nanCount > 0 || s.infCount > 0 ||
            s.clickCount > 0 ||
            s.peakOut > 2.0f ||
            s.denormalCount > 10 ||
            s.maxDelta > 0.3f ||
            (heartbeat_ % 200 == 0);

        if (! isInteresting) return;

        char buf[760];
        std::snprintf (buf, sizeof (buf),
            "[%lld] rev=%d cpu=%.1f%%  m=%d ld=%d rt=%d en=%d sr=%.0f blk=%d os=%d ser=%d"
            " | drv=%.3f gir=%.3f mod=%.3f bias=%.3f react=%.3f"
            " | pkI=%.4f pkO=%.4f pkPre=%.4f pkF=%.4f dSat=%.4f ag=%.4f"
            " | tCore=%.4f tClip=%.4f tDc=%.4f tLim=%.4f"
            " | gMix=%.3f mA=%.3f mB=%.3f mC=%.3f"
            " | dlt=%.4f clk=%d"
            " | nan=%d inf=%d dnrm=%d"
            " | aFB=%d aBl=%d aFl=%d dx=%.2e k=%.1f"
            " | fzFB=%.4f dmFB=%.4f dsFB=%.4f filt=%.2e dc=%.2e sag=%.3f yin=%.1f\n",
            (long long) s.timestampMs, s.implRev, s.cpuPercent,
            s.model, s.diagLoader, s.route, s.enableMask, s.sampleRate, s.numSamples, s.osOrder, s.seriesCount,
            s.drive, s.girth, s.mod, s.bias, s.react,
            s.peakIn, s.peakOut, s.peakPreAG, s.peakFinal, s.satDeltaPeak, s.autoGainVal,
            s.tapeCorePeak, s.tapeClipPeak, s.tapeDcPeak, s.tapeLimPeak,
            s.globalMix, s.mixA, s.mixB, s.mixC,
            s.maxDelta, s.clickCount,
            s.nanCount, s.infCount, s.denormalCount,
            s.adaaFallbacks, s.adaaBlends, s.adaaFull, s.adaaLastDx, s.adaaLastK,
            s.fuzzFeedback, s.doomFeedback, s.destroyFeedback,
            s.maxFilterState, s.maxDcState, s.sagEnvelope, s.yinFreq);

        file_.appendText (juce::String (buf));
    }

    juce::File file_;
    bool fileReady_ = false;
    bool enabled_ = true;
    int heartbeat_ = 0;
};

// ── Global singleton (header-only, one per process) ─────────────────────────
inline DiagRing&   getDiagRing()   { static DiagRing   r; return r; }
inline DiagWriter& getDiagWriter() { static DiagWriter w; return w; }

// ── Macros for audio thread ─────────────────────────────────────────────────
#define SAT_DIAG_COLLECTOR          SatDiag::Collector _diagCollector
#define SAT_DIAG_RESET()            _diagCollector.reset()
#define SAT_DIAG_FEED_IN(x)         _diagCollector.feedIn(x)
#define SAT_DIAG_FEED_OUT(x)        _diagCollector.feedOut(x)
#define SAT_DIAG_FEED_ADAA(dx,k,p)  _diagCollector.feedAdaa(dx,k,p)

#define SAT_DIAG_PUSH(snap)         SatDiag::getDiagRing().push(snap)
#define SAT_DIAG_DRAIN()            SatDiag::getDiagWriter().drain(SatDiag::getDiagRing())

#else  // SAT_DSP_DIAG == 0

#define SAT_DIAG_COLLECTOR          ((void)0)
#define SAT_DIAG_RESET()            ((void)0)
#define SAT_DIAG_FEED_IN(x)         ((void)0)
#define SAT_DIAG_FEED_OUT(x)        ((void)0)
#define SAT_DIAG_FEED_ADAA(dx,k,p)  ((void)0)
#define SAT_DIAG_PUSH(snap)         ((void)0)
#define SAT_DIAG_DRAIN()            ((void)0)

#endif // SAT_DSP_DIAG

} // namespace SatDiag

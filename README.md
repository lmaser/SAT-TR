# SAT-TR v1.4

<br/><br/>

SAT-TR is a 3-slot saturation engine with integrated IR convolution, per-loader filtering, and flexible routing topologies.
It combines 10 physically-modeled distortion algorithms with zero-latency partitioned convolution, oversampling up to ×16, and a minimal CRT-inspired interface.

## Concept

SAT-TR models analog saturation circuits at the component level rather than applying generic waveshaping. Each of the 10 algorithms replicates a specific topology — vacuum tube preamps, power amp push-pull stages, transistor fuzz, diode clippers, tape hysteresis, and extreme ring-fuzz — with per-model transfer functions, bias behavior, and frequency-dependent nonlinearities.

Three independent loader slots can each run their own saturation model alongside an IR convolver, filters, expander/gate, and chaos modulation. Loaders are routed through one of four topologies (series, parallel, or hybrid), with Mid/Side bus assignment at the summing stage.

The saturation engine features ADAA (antiderivative anti-aliasing) on all hard-clipping stages, multiband REACT energy tracking for frequency-dependent power-supply sag, analog component tolerance modeling (VARIATION), and a pitch-tracked sub-octave synthesizer for the high-gain models.

## Interface

SAT-TR uses a text-based UI with horizontal bar sliders. All controls are visible at once — no pages, tabs, or hidden menus.

- **Bar sliders**: Click and drag horizontally. Right-click for numeric entry.
- **Toggle buttons**: Click to enable/disable (INV, RAW, EXP, CHAOS D, CHAOS F).
- **Combo boxes**: Click to cycle options (MODE IN, MODE OUT, SUM BUS, SAT TYPE).
- **Collapsible IO section**: Click the toggle bar (triangle) to show/hide per-loader I/O controls. State persists across sessions.
- **Browse button**: Opens a built-in file explorer with drive selector, folder navigation, and scrollable file list. Supports WAV, AIFF, FLAC, MP3, OGG.
- **Filter bar**: Click to open the HP/LP filter configuration prompt.
- **EXP label**: Click to open the expander/gate configuration prompt with scrollable parameters.
- **ALIGN button**: Momentary trigger — cross-correlates loaders and auto-sets delay/invert for phase alignment.
- **Gear icon** (top-right): Opens info popup with version, credits, and Graphics settings.
- **Graphics popup**: Toggle CRT post-processing, switch between default/custom colour palettes.
- **Resize**: Drag the bottom-right corner. Size persists across sessions.

## Parameters

### Saturation

#### SAT TYPE

Saturation model selector. 10 algorithms:

| Model | Algorithm | MOD Control | BIAS Control |
|-------|-----------|-------------|--------------|
| **CLEAN** | Bypass — 1:1 pass-through | — | — |
| **TAPE** | Jiles-Atherton hysteresis + flutter + head bump | Tape speed / flutter depth | Record level |
| **TRIODE** | 12AX7 preamp — Koren asymmetric | Grid conduction knee | Operating point shift |
| **PUSH-PULL** | EL34/6L6 power amp — Class AB | Saturation knee hardness | Class A ↔ cold crossover |
| **CASCADE** | Cascaded triode stages (1–4 fractional) | Number of stages (0→4, 1→1) | Operating point shift |
| **DIODE** | Shockley clipper — Ge/Si blend | Topology (feedback ↔ shunt) | Germanium ↔ Silicon |
| **TUNDRA** | Metal Zone → Modern morph | MT-2 classic ↔ modern tight | Clipping asymmetry |
| **FUZZ** | Transistor fuzz — Ge/Si stages | Clipping symmetry | PNP Germanium ↔ NPN Silicon |
| **DOOM** | Big Muff 3-stage doom fuzz | Feedback amount | Voltage starving |
| **DESTROY** | Ring-fuzz — pitch-tracked ring mod | Fuzz ↔ ring mod blend | Ring freq ratio (÷2 → ×3) |

#### DRIVE (0–100%)

Saturation amount. Per-model drive curves normalize perceived distortion so that mid-range feels consistent across all models. Higher-gain models (Fuzz, Doom, Destroy, Tundra) use steeper exponential curves to spread the usable range.

#### GIRTH (0–100%)

Post-waveshaper wavefolding + transfer function sharpening. Adds harmonic density and edge to the saturated signal.

#### MOD (0–100%)

Model-specific secondary control. See SAT TYPE table above for per-model behavior.

#### BIAS (−100% to +100%)

Operating point / character blend. Per-model behavior ranges from bias shift (tubes) to germanium/silicon blend (fuzz, diodes) to voltage starving (doom).

#### REACT (0–100%)

Multiband energy-dependent parameter modulation. Tracks signal energy in three bands (sub, mid, air) and modulates drive, bias, and headroom to simulate power-supply sag. Also controls sub-octave synthesizer blend for Fuzz, Doom, Destroy, and Tundra models.

#### VARIATION (0–100%)

Analog component tolerance simulation. 70% static per-instance tolerance (hash-seeded — each "unit" has unique character) + 30% very slow thermal drift (sub-Hz incommensurate sine oscillators simulating cathode temperature). No fast modulation — real components don't oscillate at Hz rates.

#### SERIES (1–4)

Number of cascaded saturation passes. Each pass includes inter-stage Miller-cap LPF, coupling-cap DC blocker, and independent ADAA state. Simulates multi-stage amplifier cascading.

#### RAW

Bypasses internal pre-emphasis/de-emphasis EQ and auto-gain compensation. Exposes the raw waveshaper output for external processing or analysis.

### Per-Loader

#### IN (−100 to 0 dB)

Per-loader input gain.

#### OUT (−100 to +24 dB)

Per-loader output gain.

#### TILT (−6 to +6 dB)

Per-loader tilt EQ. First-order symmetric shelf pivoted at 1 kHz. Positive values boost highs and cut lows.

#### HP / LP FILTER

Per-loader high-pass and low-pass filters. Accessible via the filter bar in the I/O section.

- **Frequency**: 20–20 000 Hz.
- **Slope**: 6 dB/oct, 12 dB/oct, or 24 dB/oct.
- **Enable**: Independent toggle for each filter.
- **Position**: F▼T▼ / F▲T▲ / F▲T▼ / F▼T▲ for pre/post saturation and tilt routing.

#### PAN (L–C–R)

Stereo pan for the loader output.

#### MIX (0–100%)

Per-loader dry/wet balance.

#### INV

Phase invert per-loader.

#### DELAY (0–5 ms)

Phase alignment delay per-loader. Used by ALIGN auto-correlation or set manually.

#### FRED (0–100%)

Fredman miking simulation. Models off-axis microphone positioning with ~159 µs inter-channel delay.

#### POS (0–100%)

Microphone distance simulation.

#### RESO (0–200%)

IR resonance control. Scales the resonant character of the loaded impulse response.

### IR Controls

#### START / END (0–10 000 ms)

IR trim window. Sets the start and end points within the loaded impulse response.

#### SIZE (25–400%)

IR time-stretch. Resamples the IR to change its effective length without changing spectral content.

### Expander / Gate

Per-loader noise gate accessible via the EXP label. Parameters:

- **ORDER**: PRE (before saturation) or POST (after saturation).
- **RATIO** (1:1 – 1:10): Expansion ratio.
- **THRESHOLD** (−60 to 0 dB): Gate threshold.
- **ATTACK** (0.01–100 ms): Envelope attack time.
- **RELEASE** (5–2000 ms): Envelope release time.

### Chaos

Per-loader micro-variation engine. Two independent targets:

- **CHAOS D**: Delay modulation — adds organic timing drift.
- **CHAOS F**: Filter modulation — modulates HP/LP cutoff frequencies.

Each target has:
- **AMOUNT** (0–100%): Modulation depth.
- **SPEED** (0.01–100 Hz): Random target rate.

Uses Hermite cubic interpolation between random targets with per-channel quadrature drift LFO.

### Global

#### INPUT (−100 to 0 dB)

Global input gain.

#### OUTPUT (−100 to +24 dB)

Global output gain.

#### ROUTE

Loader topology:
- **A|B|C**: Parallel — all loaders process input independently, summed at output.
- **A>B>C**: Series — output of A feeds B, output of B feeds C.
- **A>B|C**: Hybrid — A feeds B in series, C in parallel.
- **A|B>C**: Hybrid — A in parallel, B feeds C in series.

#### MIX (0–100%)

Global dry/wet balance. Works in INSERT or SEND mode.

#### MIX MODE

- **INSERT**: Single mix knob crossfades between dry and wet.
- **SEND**: Independent DRY LEVEL and WET LEVEL controls.

#### MATCH

Spectral tilt profile applied to the wet signal:
- **None**: No tilt.
- **White**: Flat response.
- **Pink** (−3 dB/oct): Natural roll-off.
- **Brown** (−6 dB/oct): Steep low-end emphasis.
- **Bright** (+3 dB/oct): High-frequency boost.
- **Bright+** (+6 dB/oct): Aggressive brightness.

#### NORM

Peak normalization target for loaded IRs:
- **Off**: No normalization.
- **0 dB / −3 dB / −6 dB / −12 dB / −18 dB**: IR peak normalized to target.

#### OVERSAMPLING

Global oversampling factor: ×1, ×2, ×4, ×8, ×16. Higher factors reduce aliasing but increase CPU. Latency is reported to the DAW for automatic compensation.

#### ALIGN

Momentary trigger. Cross-correlates all enabled loaders against the first and auto-sets per-loader delay and phase invert for optimal coherence.

#### LIMITER

Dual-stage transparent peak limiter:
- **Stage 1 (Leveler)**: 2 ms attack, 10 ms release — catches sustained overs.
- **Stage 2 (Brickwall)**: Instant attack, 100 ms release — catches transient peaks.

Settings:
- **THRESHOLD** (−36 to 0 dB): Limiter ceiling.
- **MODE**: NONE / WET / GLOBAL placement.

#### INV POLARITY

Phase inversion scope: NONE / WET / GLOBAL.

#### INV STEREO

Stereo channel swap scope: NONE / WET / GLOBAL.

### Mode In / Mode Out / Sum Bus

Per-loader routing for Mid/Side processing:

- **MODE IN**: L+R / MID / SIDE — selects which signal component enters the loader.
- **MODE OUT**: L+R / MID / SIDE — selects which signal component exits the loader.
- **SUM BUS**: ST / →M / →S — routes the loader output to the stereo, mid, or side summing bus.

## Technical Details

### DSP Architecture
- **Saturation**: Header-only engine (SaturationEngine.h). All models inline, per-sample processing.
- **ADAA**: 1st-order (Triode, PushPull, Cascade, Tundra) and 2nd-order (Fuzz, Doom, Destroy) antiderivative anti-aliasing on all hard-clipping stages.
- **YIN Pitch Tracker**: Cumulative Mean Normalized Difference autocorrelation with parabolic interpolation. 96-sample hop, 512-lag search (supports down to ~86 Hz). Adaptive smoothing — fast snap on note changes (~2.4 ms), slow smooth on sustain (~24 ms).
- **Sub-Octave**: Pitch-tracked sine oscillator at f/2, shaped by envelope follower (0.3 ms attack, 53 ms release), output LPF tracks detected frequency.
- **REACT**: 3-band energy tracker (sub/mid/air crossovers at 150/3000 Hz). Asymmetric envelope follower per band → drive boost, bias shift, headroom modulation.
- **Smoothing**: Sample-rate-aware one-pole IIR on all parameters (~15 ms time constant, consistent across oversampling rates).
- **Drive Curves**: Per-model power-law exponents normalize perceived distortion (1.5 for gentle models up to 4.0 for extreme models).
- **Auto-Gain**: Per-model output normalization with unity-at-zero protection.
- **Variation**: Deterministic hash-seeded static tolerance + 3 incommensurate sub-Hz sines for thermal drift.
- **Convolution**: Partitioned FFT convolution via FFTConvolver library, zero added latency.
- **Filters**: Transposed Direct Form II biquads. Coefficients updated once per block (or per configurable interval).
- **Tilt EQ**: First-order symmetric shelf at 1 kHz. Coefficient caching with tolerance-based update.
- **Limiter**: Dual-stage (leveler + brickwall), stereo-linked gain reduction.

### Build
- **Framework**: JUCE 7
- **Format**: VST3
- **Compiler**: Visual Studio 2022 (MSVC)
- **Platform**: Windows x64
- **Third-party**: FFTConvolver, FFTW3 (float, single-precision)

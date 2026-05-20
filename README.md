# SAT-TR v1.4

<br/><br/>

SAT-TR is a 3-slot saturation router focused on stacked drive, per-slot filtering, and flexible routing topologies.
It combines 6 saturation algorithms, oversampling up to x16, per-loader dynamics/modulation tools, and a minimal text-first interface with optional graphics styling.

## Concept

SAT-TR does not use one generic waveshaper for every mode. It combines physically-informed stages and fitted black-box models, depending on the algorithm, so each saturation family keeps its own gain staging, voicing, dynamics, and anti-aliasing strategy.

Three independent loader slots can each run their own saturation model alongside per-loader gain staging, tilt, filters, expander/gate, chaos modulation, delay compensation, and Mid/Side bus routing. Loaders are routed through one of four topologies (series, parallel, or hybrid), with optional global wet limiting, normalization, oversampling, and auto-alignment.

The saturation engine uses ADAA-backed nonlinear stages where they matter, per-model dynamics blocks (`SAG`, `COMP`, `PEAK` depending on algorithm), analog-style instability/drift, and multi-pass series processing for denser amplifier-style stacking.

## Interface

SAT-TR uses a text-based UI with horizontal bar sliders. Most core controls stay visible at once, with prompts used for deeper parameter entry and utility configuration.

- **Bar sliders**: Click and drag horizontally. Right-click for numeric entry.
- **Toggle buttons**: Click to enable/disable (`RAW`, `INV`, `EXP`, `CHAOS D`, `CHAOS F`).
- **Combo boxes**: Click to choose routing, mode, algorithm, oversampling, normalization, and limiter placement.
- **Collapsible IO section**: Click the toggle bar (triangle) to show/hide per-loader I/O controls. State persists across sessions.
- **Bottom global section**: Click the bottom toggle bar to switch between loader controls and the global control page. The global page keeps the same left content axis as the loader view.
- **Filter bar**: Click to open the HP/LP filter configuration prompt.
- **EXP button**: Left-click enables/disables the expander. Right-click opens the expander prompt.
- **CHAOS buttons**: Left-click enables/disables the target. Right-click opens the amount/speed prompt for delay or filter chaos.
- **Tooltips**: Toggle tooltips use an opaque palette-background panel with palette text/outline, so `EXP`, `CHAOS D`, and `CHAOS F` match the rest of the TR Series.
- **ALIGN button**: Momentary trigger. Auto-sets per-loader delay compensation and invert state from synthetic probe analysis.
- **Gear icon** (top-right): Opens the info popup with version, credits, and the Graphics prompt.
- **Graphics popup**: Toggles graphic FX and switches between default/custom colour palettes.
- **Resize**: Drag the bottom-right corner. Size persists across sessions.

## Parameters

### Saturation

#### SAT TYPE

Saturation model selector. 6 algorithms:

| Model | Algorithm | Active Control Labels |
|-------|-----------|-----------------------|
| **CLEAN** | 1:1 pass-through | `-` |
| **TAPE** | Tape-style saturation with fitted family morphing | `DRIVE`, `BODY`, `FORM`, `BIAS`, `COMP` |
| **TUBE** | Triode / power-tube inspired stage | `DRIVE`, `BODY`, `TYPE`, `BIAS`, `SAG` |
| **TRANSISTOR** | BJT <-> FET solid-state preamp / fuzz stage | `GAIN`, `BODY`, `TYPE`, `BIAS`, `COMP` |
| **DIODE** | Diode overdrive family (`feedback -> hard -> open`) | `DRIVE`, `COND`, `TOPO`, `SYM`, `COMP` |
| **CLIPPER** | Broadband / pedal clipper family (`classic -> TS -> Klon`) | `DRIVE`, `KNEE`, `VOICE`, `SYM`, `PEAK` |

#### DRIVE / GAIN

Primary amount control. The visible label changes with the selected algorithm:

- **DRIVE**: primary saturation amount
- **GAIN**: transistor input drive / stage push
- In `CLIPPER`, **DRIVE** is implemented internally as clip-threshold drive: higher values lower the effective threshold and produce more clipping.

#### GIRTH / BODY / COND / KNEE

Secondary shape or body control. Its behavior depends on the algorithm:

- **BODY**: low-mid/body/depth emphasis inside the model
- **COND**: conditioning before the diode clipper
- **KNEE**: clip softness / transition sharpness

#### MOD / FORM / TYPE / TOPO / VOICE

Model-specific voicing or topology morph:

- **FORM**: tape family contour / character morph
- **TYPE**: tube or transistor family blend
- **TOPO**: diode topology / openness
- **VOICE**: clipper voicing family

#### BIAS / SYM

Operating-point or asymmetry control, depending on the model:

- **BIAS**: stage bias / operating point
- **SYM**: positive/negative clipping symmetry

#### DYN / COMP / SAG / PEAK

Model-dependent dynamics control:

- **COMP**: dynamics conditioning before or inside the stage
- **SAG**: tube-style reactive compression / supply behavior
- **PEAK**: pre-clip transient shaving in `CLIPPER`
- **DYN**: generic fallback label when no model-specific dynamics name applies

#### SERIES (1-4)

Number of cascaded saturation passes for the active loader. Each pass runs the model again through its own series state for denser stacking.

#### Detail (0-100%)

Per-loader clipped-detail preservation. It derives a high-passed residual from
a parallel hard-clip reference and uses it as a fast sidechain-ring style
reduction path, helping fine high-frequency detail survive heavy saturation.
`0%` is bypass. `50%` reaches the full calibrated preservation engine. From
`50%` to `100%`, preservation stays capped and a broad +18 dB high shelf is
applied inside the sidechain residual path, making the reducer react more to
clipped-air texture without boosting the audio core directly. It is active in
the saturation models after the internal `SERIES` stack and remains inactive in
`CLEAN`.

#### Instability (0-100%)

Analog-style tolerance / drift control. Adds deterministic per-instance spread, slow thermal movement, and smoothed micro-irregularity on gain, bias, shape, and channel asymmetry. Lower settings stay subtle; higher settings increase the instability ceiling for a more clearly unstable analog unit without turning into tempo-style modulation.

#### RAW

Bypasses the model's internal pre/post colour-shaping path where supported, exposing a rawer stage response. `RAW` is not a total safety or dynamics bypass: dedicated model dynamics may remain active when they are part of the mode's behaviour rather than just wrapper voicing. Useful when you want external filtering or want to hear more of the bare nonlinear core.

### Per-Loader

#### IN (-INF to +24 dB)

Per-loader input gain.
The fader floor is -144 dB, displayed as -INF; 0 dB is centered on the control.

#### OUT (-INF to +24 dB)

Per-loader output gain.

#### TILT (-6 to +6 dB)

Per-loader tilt EQ. First-order symmetric shelf pivoted at 1 kHz. Positive values boost highs and cut lows.

#### HP / LP FILTER

Per-loader high-pass and low-pass filters. Accessible via the filter bar in the I/O section.

- **Frequency**: 20-20 000 Hz
- **Slope**: 6 dB/oct, 12 dB/oct, or 24 dB/oct
- **Enable**: independent toggle for each filter
- **Position**: `F post / T post`, `F pre / T pre`, `F pre / T post`, or `F post / T pre` for filter/tilt routing

#### PAN (L-C-R)

Stereo pan for the loader output.

#### MIX (0-100%)

Per-loader dry/wet balance.

#### INV

Per-loader polarity invert.

#### DELAY (0-5 ms)

Per-loader alignment delay. Manual prompt entry supports up to `5.000 ms` with `0.001 ms` precision, while `ALIGN` can still write fine compensation automatically.

### Expander / Gate

Per-loader expander accessible from the `EXP` button. Parameters:

- **ORDER**: `PRE` (before saturation) or `POST` (after saturation)
- **THRESH** (`-60.0` to `0.0 dB`): expander threshold
- **RATIO** (`0.1` to `10.0`, centred at `1.0`): response ratio for material below threshold. `1.0` is neutral, values above `1.0` apply downward expansion, and values below `1.0` invert the action and lift low-level material below threshold
- **KNEE** (`0.0` to `12.0 dB`): softens the transition around threshold; `0.0 dB` keeps the original hard-knee behavior
- **ATK** (`0.00` to `100.00 ms`): envelope attack time
- **REL** (`5.00` to `2000.00 ms`): envelope release time
- **SIDECHAIN GAIN** (`-INF` to `+24.0 dB`): internal detector level trim; it only changes the expander detector signal, not the audio path
- **SIDECHAIN HP/LP** (`20 Hz` to `20 kHz`): optional internal detector filters for frequency-selective expansion. Each band can be enabled independently and set to `6 dB`, `12 dB`, or `24 dB` per octave

### Chaos

Per-loader chaos engine with two independent targets:

- **CHAOS D**: delay-domain drift before saturation
- **CHAOS F**: filter cutoff drift

Each target has:

- **AMOUNT** (`0-100%`): modulation depth
- **SPEED** (`0.01-100 Hz`): random target rate

### Global

#### ROUTE

Loader topology:

- **A>B>C**: full series
- **A|B|C**: full parallel
- **A>B|C**: A into B, with C in parallel
- **A|B>C**: A in parallel with B into C
- **(A|B)>C**: A and B in parallel, then their combined result feeds C
- **A>(B|C)**: A first, then its output splits to B and C in parallel

Notes:
- In **A|B>C**, only the **B** branch feeds **C**. `A` is summed later, after the `B->C` chain.
- In **(A|B)>C**, `A` and `B` are summed first, and that combined signal is what enters `C`.
- In **A>(B|C)**, `A` is a shared pre-stage. Both `B` and `C` receive the output of `A`.

#### MIX (0-100%)

Global dry/wet balance.

#### MIX MODE

- **INSERT**: single wet/dry crossfade
- **SEND**: independent `DRY LEVEL` and `WET LEVEL`

#### OS

Global oversampling factor: `x1`, `x2`, `x4`, `x8`, `x16`. Higher factors reduce aliasing and add host-reported latency.

#### NORM

Wet peak normalization target:

- **Off**
- **0 dB**
- **-3 dB**
- **-6 dB**
- **-12 dB**
- **-18 dB**

#### OUTPUT (-INF to +24 dB)

Global output gain.
The fader floor is -144 dB, displayed as -INF; 0 dB is centered on the control.

#### ALIGN

Momentary trigger. Calculates relative alignment from the active loader responses and writes per-loader delay compensation plus invert state automatically.

#### LIMITER

Dual-stage peak limiter with placement control:

- **THRESH** (`-36.0` to `0.0 dB`): limiter threshold
- **MODE**: `NONE`, `WET`, `GLOBAL`

#### INV POLARITY

Polarity inversion scope: `NONE`, `WET`, `GLOBAL`.

#### INV STEREO

Stereo channel swap scope: `NONE`, `WET`, `GLOBAL`.

### Mode In / Mode Out / Sum Bus

Per-loader Mid/Side routing:

- **MODE IN**
  - `L+R`: standard stereo input
  - `MID`: extracts the Mid component for processing
  - `SIDE`: extracts the Side component for processing
- **MODE OUT**
  - `L+R`: standard stereo output
  - `MID`: outputs a mono Mid signal on both channels
  - `SIDE`: outputs a stereo Side signal as `+S / -S`
- **SUM BUS**
  - `ST`: contributes directly to the stereo path
  - `->M`: contributes to the Mid bus
  - `->S`: contributes to the Side bus

Practical note:
- `MODE IN` decides what component a loader processes.
- `MODE OUT` decides how that processed result is represented when it leaves the loader.
- `SUM BUS` decides how that loader output is injected at a parallel summing point.
- `SUM BUS` matters in routes with an actual split/summing stage: `A|B|C`, `A>B|C`, `A|B>C`, `(A|B)>C`, and `A>(B|C)`.
- In full series `A>B>C`, there is no parallel sum stage, so `SUM BUS` has no practical effect.

## Technical Details

### DSP Architecture

- **Saturation**: header-only engine (`SaturationEngine.h`) with per-sample processing and per-model state.
- **Models**: `CLEAN`, `TAPE`, `TUBE`, `TRANSISTOR`, `DIODE`, `CLIPPER`.
- **ADAA**: used on the main nonlinear stages where needed; exact placement varies by model.
- **Series Processing**: up to 4 internal passes per loader.
- **Dynamics**: model-specific dynamics blocks rather than one universal behavior (`SAG`, `COMP`, `PEAK` depend on algorithm).
- **Tube SAG**: reactive supply/sag behavior with short strike tracking plus longer bloom memory for time-dependent recovery.
- **Detail**: high-passed clipped-residual sidechain path for detail-preserving saturation, with extra sidechain air emphasis above 50%, shared by the saturation models.
- **Instability**: deterministic component spread plus slow drift, with smoothed gain/bias/shape/asymmetry micro-irregularity and a stronger calibrated ceiling at high settings.
- **Oversampling**: global `x1` to `x16`, with latency reported to the host.
- **Filters**: per-loader HP/LP plus tilt filtering with pre/post routing options.
- **Alignment**: synthetic probe analysis used to estimate per-loader compensation delay and invert state.
- **NORM**: wet peak normalization stage with fixed targets.
- **Limiter**: dual-stage user limiter with `WET` or `GLOBAL` placement, plus final safety protection.

### Performance

- Zero-allocation audio thread in the normal processing path.
- Global and per-loader dry buffers are skipped when the corresponding mix path is fully wet and stable, while preserving dry fade-out ramps during transitions.
- Stable per-loader delay blocks avoid redundant per-sample delay setup and keep delay lines continuously fed for click-free re-entry.
- Tube bloom memory uses a 1 ms ring-buffer window with incremental sum updates instead of rescanning the full bloom window during steady processing.

### Build

- **Framework**: JUCE Framework
- **Format**: VST3
- **Compiler**: Visual Studio 2022 (MSVC)
- **Platform**: Windows x64

## Changelog

### v1.4

- Refined model-specific dynamics, including Tube SAG bloom/recovery and COMP behavior in compression-based models.
- Added `DETAIL` clipped-detail preservation for the saturation models.
- Refined `Instability` behavior with deterministic analog spread, slow drift, smoothed gain/bias/shape/asymmetry micro-irregularity, and a stronger calibrated high-range ceiling.
- Added/maintained consistent -INF to +24 dB gain fader behavior with 0 dB centered.
- Optimized global/per-loader dry-wet mix paths and stable delay processing without changing the intended audio behavior.
- Optimized Tube bloom memory updates for lower CPU during SAG-heavy use.

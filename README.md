# SAT-TR v1.4

<br/><br/>

SAT-TR is a 3-slot saturation router focused on stacked drive, per-slot filtering, and flexible routing topologies.
It combines 6 saturation algorithms, oversampling up to x16, per-loader dynamics/modulation tools, and a minimal text-first interface with optional graphics styling.

## Concept

SAT-TR does not use one generic waveshaper for every mode. It combines physically-informed stages and fitted black-box models, depending on the algorithm, so each saturation family keeps its own gain staging, voicing, dynamics, and anti-aliasing strategy.

Three independent loader slots can each run their own saturation model alongside per-loader gain staging, tilt, filters, expander/gate, chaos modulation, delay compensation, and Mid/Side bus routing. Loaders are routed through one of four topologies (series, parallel, or hybrid), with optional global wet limiting, normalization, oversampling, and auto-alignment.

The saturation engine uses ADAA-backed nonlinear stages where they matter, per-model dynamics blocks (`SAG`, `COMP`, `PEAK` depending on algorithm), analog-style variation/drift, and multi-pass series processing for denser amplifier-style stacking.

## Interface

SAT-TR uses a text-based UI with horizontal bar sliders. Most core controls stay visible at once, with prompts used for deeper parameter entry and utility configuration.

- **Bar sliders**: Click and drag horizontally. Right-click for numeric entry.
- **Toggle buttons**: Click to enable/disable (`RAW`, `INV`, `EXP`, `CHAOS D`, `CHAOS F`).
- **Combo boxes**: Click to choose routing, mode, algorithm, oversampling, normalization, and limiter placement.
- **Collapsible IO section**: Click the toggle bar (triangle) to show/hide per-loader I/O controls. State persists across sessions.
- **Filter bar**: Click to open the HP/LP filter configuration prompt.
- **EXP button**: Left-click enables/disables the expander. Right-click opens the expander prompt.
- **CHAOS buttons**: Left-click enables/disables the target. Right-click opens the amount/speed prompt for delay or filter chaos.
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
| **CLIPPER** | Broadband / pedal clipper family (`classic -> TS -> Klon`) | `THR`, `KNEE`, `VOICE`, `SYM`, `PEAK` |

#### DRIVE / GAIN / THR

Primary amount control. The visible label changes with the selected algorithm:

- **DRIVE**: primary saturation amount
- **GAIN**: transistor input drive / stage push
- **THR**: clip-threshold style control in `CLIPPER`

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

#### REACT / COMP / SAG / PEAK

Model-dependent dynamics control:

- **COMP**: dynamics conditioning before or inside the stage
- **SAG**: tube-style reactive compression / supply behavior
- **PEAK**: pre-clip transient shaving in `CLIPPER`

#### VARIATION (0-100%)

Analog-style tolerance / drift control. Adds per-instance spread and slow movement to the model response without turning into obvious modulation.

#### SERIES (1-4)

Number of cascaded saturation passes for the active loader. Each pass runs the model again through its own series state for denser stacking.

#### RAW

Bypasses the model's internal colour-shaping path where supported, exposing a rawer stage response. Useful when you want external filtering or want to hear more of the bare nonlinear core.

### Per-Loader

#### IN (-100 to 0 dB)

Per-loader input gain.

#### OUT (-100 to +24 dB)

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

Per-loader alignment delay. In the current UI this is shown as a readout and is primarily driven by `ALIGN`.

### Expander / Gate

Per-loader expander accessible from the `EXP` button. Parameters:

- **ORDER**: `PRE` (before saturation) or `POST` (after saturation)
- **THRESH** (`-60.0` to `0.0 dB`): gate threshold
- **RATIO** (`1:1` to `1:10`): expansion ratio
- **KNEE** (`0.0` to `12.0 dB`): softens the transition around threshold; `0.0 dB` keeps the original hard-knee behavior
- **ATK** (`0.01` to `100.00 ms`): envelope attack time
- **REL** (`5.00` to `2000.00 ms`): envelope release time

### Chaos

Per-loader micro-variation engine with two independent targets:

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

#### OUTPUT (-100 to +24 dB)

Global output gain.

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

- **MODE IN**: `L+R`, `MID`, `SIDE`
- **MODE OUT**: `L+R`, `MID`, `SIDE`
- **SUM BUS**: `ST`, `->M`, `->S`

## Technical Details

### DSP Architecture

- **Saturation**: header-only engine (`SaturationEngine.h`) with per-sample processing and per-model state.
- **Models**: `CLEAN`, `TAPE`, `TUBE`, `TRANSISTOR`, `DIODE`, `CLIPPER`.
- **ADAA**: used on the main nonlinear stages where needed; exact placement varies by model.
- **Series Processing**: up to 4 internal passes per loader.
- **REACT / Dynamics**: model-specific dynamics blocks rather than one universal behavior (`SAG`, `COMP`, `PEAK` depend on algorithm).
- **Variation**: deterministic spread plus slow drift.
- **Oversampling**: global `x1` to `x16`, with latency reported to the host.
- **Filters**: per-loader HP/LP plus tilt filtering with pre/post routing options.
- **Alignment**: synthetic probe analysis used to estimate per-loader compensation delay and invert state.
- **NORM**: wet peak normalization stage with fixed targets.
- **Limiter**: dual-stage user limiter with `WET` or `GLOBAL` placement, plus final safety protection.

### Build

- **Framework**: JUCE 7
- **Format**: VST3
- **Compiler**: Visual Studio 2022 (MSVC)
- **Platform**: Windows x64

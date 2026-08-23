# SAT-TR WaveShape UI contract

Status: implemented for UI validation on 2026-08-15.

## Effective model

- The visible order is `CLEAN`, `TAPE`, `TUBE`, `TRANSISTOR`, `DIODE`,
  `OVERDRIVE A`, `OVERDRIVE B`, `CLIPPER`, `WAVE SHAPE`, `NAM`.
- `sat_type_a/b/c` remains the unchanged legacy parameter. WAVE SHAPE selection is
  action-backed state and never inserts, removes or renumbers a legacy choice.
- A/B/C retain independent WaveShape enable and curve state.

## Main surface

- WAVE SHAPE replaces the legacy macro deck with `MORPH`, `BIAS`, `SERIES`, `MIX`.
- `DRIVE`, `CHARACTER`, `TYPE`, `RAW`, `DYNAMICS`, `DETAIL` and `INSTABILITY` are
  not presented while the selected loader uses WAVE SHAPE.
- `IN`, `OUT`, `FREQUENCY`, `POSITION`, routing and the global controls remain
  available through their established routes.
- The signature displays the effective morphed transfer as the dominant trace and
  A/B as secondary references. Clicking its body opens the editor only in WAVE SHAPE.
  Edge-meter clicks retain their peak-reset behavior.

## Curve workspace

- The editor uses the family-wide fixed auxiliary size `1040 x 680` and restores the
  exact previous product size on Apply or Cancel.
- A/B tab changes, polarity changes, node edits, tension edits, drawing, reset,
  undo and redo affect a local draft only.
- `Apply` validates and sends one complete state update. `Cancel`, opening, closing
  and tab changes send none.
- BIPOLAR initialization uses `WaveShape::setPolarityMode`; the UI does not construct
  or approximate the negative branch. Returning to UNIPOLAR preserves the original
  source curve exactly.
- Node mode supports drag, double-click add and double-click delete. Segment tension
  is explicit. Draw mode captures ordered points under the backend's persistent node
  limits. Grid cycles through 8, 16 and 32 divisions with optional snapping.

## MACROS

- `waveshape:a|b|c:morph|bias` is available only for loaders currently using
  WAVE SHAPE.
- `loader:a|b|c:drive` is unavailable while that loader uses WAVE SHAPE. Existing
  routes are not deleted; they remain stored and become active again in a legacy model.
- `IN` and unrelated loader destinations remain available.

## Known backend boundary

The ready handoff exposes validation and state commit but no callable error-bounded
freehand simplifier. The UI therefore caps capture at the persistent source limits and
does not implement its own simplification algorithm. If release requires dense gesture
capture followed by error-bounded reduction, the backend must expose that operation;
the UI must consume it rather than duplicate it.

## Required evidence

- Definition probe: composed selector, state classification and mutually exclusive
  SERIES presentation.
- Render journey: WAVE SHAPE main surface, A/B references, fixed editor at 100/150%,
  exact bilateral margins and exact size restoration.
- Interaction journey: legacy value preservation, per-loader destination availability,
  draft isolation, Cancel no-op and Apply commit.
- Final VST3 build, pluginval level 8 and deployed-binary smoke test.

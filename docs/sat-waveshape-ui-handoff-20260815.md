# SAT-TR WaveShape UI handoff

Backend phases 1-3 are complete. Implement UI only; do not duplicate DSP or state codecs.

## Backend contract

- `sat_type_a/b/c` is historical and must not gain a new choice or change indices.
- Effective model is composite. Query `SatBackendBindings::isWaveShapeEnabled(loader)`.
- Select WAVE SHAPE with contextual action `select-waveshape-a|b|c`.
- Return to a legacy model with `select-legacy-model-a|b|c`, then write the selected historical `sat_type_*` value normally.
- Read/write curves through `SatBackendBindings::waveShapeState()` and `setWaveShapeState(...)`.
- Structural state is persistent and non-automatable. Do not create `waveshape_mode_*` parameters.
- Continuous host parameters are `waveshape_morph_a/b/c` and `waveshape_bias_a/b/c`.
- MACROS destinations are `waveshape:a|b|c:morph` and `waveshape:a|b|c:bias`.
- `scopeStatuses()` and `signatureSnapshot()` already report `WAVE SHAPE` and the exact current transfer.
- Internal presets already serialize `sat_waveshape_state`; host presets serialize `WAVESHAPER_STATE`.

## Required UI

1. Add visible `WAVE SHAPE` immediately after `CLIPPER` in the composed selector.
2. In WAVE SHAPE, clicking the main transfer graph opens the curve dialog.
3. Dialog: tabs A/B, BIPOLAR checkbox, Undo, Redo, Reset, Apply, Cancel.
4. Apply sends one validated state update. Cancel sends none. Opening or switching tabs must not mutate state.
5. UNIPOLAR source is at most 64 nodes. BIPOLAR initialization is backend-defined exact `(-x,-y)` mirroring and may contain 127 nodes; do not regenerate it locally.
6. Show the morphed result as dominant trace, with A/B as references.
7. Show contextual knobs MORPH and BIAS. BIAS is the single zero-preserving input operating-point bias; do not expose axis suffixes. Keep IN, OUT, MIX, SERIES and routing.
8. Hide or disable RAW, DETAIL and DYNAMICS for WaveShape V1.
9. No DRIVE, second output-offset bias, spectral morph, frame table, local curve LUT, local oversampling or local DC correction.

## UI acceptance

- A/B/C independently select and retain WAVE SHAPE.
- Switching to a legacy model preserves curves; switching back restores them.
- UNIPOLAR -> BIPOLAR -> UNIPOLAR restores the original unipolar curve bit-for-bit.
- Opening/closing the editor does not dirty the preset or alter audio.
- Preset round-trip restores enabled state, polarity, both A/B domains, Morph and Bias.
- Render probes cover all three loaders, compact/full MACROS, scaling, focus and long labels.

Architecture and interactive reference: `docs/sat-waveshape-architecture-audit-20260815.html`.

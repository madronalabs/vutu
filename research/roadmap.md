# Roadmap: Analyzer Changes from the Three Design Notes

*Status: agreed ordering as of 2026-07-19. Companion to
`variable-hopsize.md`, `ratio-invariant-params.md`, and
`removing-resolution.md` — this doc only sequences the work those papers
specify; details live there.*

## Organizing principle

Of the planned changes, three are **bit-exact refactors** at current
defaults — their correctness is machine-checkable (golden comparison,
selftests) and there is nothing to hear. The rest are **audible** and land
one at a time, each with a specific listening question. Front-loading the
silent plumbing means every listening session afterwards can only be hearing
one thing.

## Order

### 1. noiseWidth → window/noise ratio  *(silent)*

From `ratio-invariant-params.md`. Smallest diff; the associator is
untouched; identity at defaults (dense rule W/3.4 already exists in
autoParams). Also adds the missing `.utu` header entry.
**Verify:** selftests + golden compare. No listening — nothing can change.

### 2. freqDrift → Hz/s rate, with ε floor and 0.25·W cap  *(silent)*

From `ratio-invariant-params.md`. Gate becomes `min(capHz, ε + rate·dt)`;
identity at constant hop, where `rate·dt` reproduces the old Hz gate exactly
(dt = 1/W).
**Verify:** golden compare; chirp selftest (1000 Hz/s must still link).

Steps 1–2 cut the derivation chains (`freqDrift ← 0.5·resolution`, sparse
`noiseWidth = 4·resolution`) that both later tracks depend on.

### 3. ERB masking radius — delete `resolution`  *(audible)*

From `removing-resolution.md`: R(f) = α·ERB(f), α = 1.0 to start. First
audible change, taken before any hop work for three reasons: single-locus
change in `thinPeaks` plus cleanup; biggest perceptual payoff; and it
re-tunes the probe/budget constants — partial counts and floor behavior
should be settled before hop densification, so later listening isn't
confounded.
**Verify:** testNoiseBw, testTwoSine (+ new high-frequency fusion case),
budget-floor comparison.
**Listen (one session, two questions):** low-register dense material —
fidelity gain, no new rustle; bright material — *no audible difference*
despite fewer high-frequency tracks (the falsifier of α = 1).

### 4. Voronoi residue partition, at constant hop  *(silent)*

From `variable-hopsize.md`. Neighbor-midpoint tiles replace the fixed
±hop/2 gate; on a uniform grid this reduces identically to current behavior,
so it lands as a standalone silent refactor. When hop actually varies in
step 5, the residue energy accounting is already proven.
**Verify:** golden compare; testResidueGate (additivity + time-gate
identities).

### 5. Variable grid advance + hop jitter  *(audible)*

From `variable-hopsize.md`. The grid increment itself becomes variable
(jitter as grid variation, not offset-from-fixed-grid); history-drop /
frame-ready bookkeeping updated.
**Listen:** clean sustained tones — the frame-rate signature should dissolve
into a slightly raised but uncorrelated noise floor. A narrow, well-defined
percept; good isolated session.

### 6. Onset-driven hop densification  *(audible)*

From `variable-hopsize.md`. Denser hops at detected attacks. Steps 2
(dt-scaled drift) and 4 (Voronoi partition) are what make this step safe.
**Listen:** attacks — sharper bandwidth-envelope onsets; no regressions on
the decay side of the same notes.

## Optional tail, gated on what we hear

- **Deinterpolation residue deposit** (`variable-hopsize.md` mitigation B):
  only if bandwidth granularity at attacks still sounds quantized after
  step 6.
- **Two-pass guided attack analysis** (`variable-hopsize.md`, out-of-scope
  note): only if attack fidelity demands short-window detail that hop
  density can't provide. Long-window pass owns partial identity; short-window
  pass links backwards in time from the stable side.

## Execution notes for the implementing model

Directives, not suggestions. The design judgment is finished and lives in
the three papers; the job now is disciplined execution.

1. **In silent steps (1, 2, 4), zero behavior change IS the spec.** Never
   widen a tolerance or regenerate a golden to make a silent step pass. A
   small numeric drift is a float-ordering bug to find, not noise. Known
   trap in step 2: the hop is an integer sample count, so dt must be
   computed exact-in-samples (the tracker's `dt()` already is) or `rate·dt`
   will not reproduce the old Hz gate bit-exactly.
2. **Failure here is silent, not loud.** Example: deleting `resolution`
   before the freqDrift re-anchor lands gives a working-looking analysis
   with fragmented tracks (default drift → 0 → every peak spawns a new
   partial; no crash). After every step, diff partial count,
   `maxActivePartials`, and the `achievedAt` floor on a reference set — do
   not rely on crashes or test failures to surface mistakes.
3. **No opportunistic cleanup inside roadmap steps.** The papers
   deliberately preserve the loudest-first sort in `thinPeaks`, the
   one-frame track death, and the `modDriven` gate. Cleanups go in separate
   commits after the step's verification gate passes.
4. **Any constant not derivable from the papers comes from a measurement
   run** (`tune`, `auto-test`, `achievedAt` ground truth), recorded in the
   commit message — never from plausibility. This applies especially to the
   step-3 kept-peak density remeasure and `countP90` retune.
5. **Listening gates are human gates.** Tests passing is not the gate for
   steps 3, 5, 6. Stop at the gate and hand over with a note on what to
   listen for.
6. Sharp edges: the tracker's per-frame override argument becomes a *rate*
   override, not Hz (step 2); the ERB radius is evaluated per pair at the
   mid frequency inside the masking loop, not hoisted per frame (step 3);
   the `.utu` migration reading old `freq_drift` needs the stored
   `windowWidth` and must guard the absent/zero case (step 2).
7. **Keep commits surgical.** The tree carries concurrent app-port work;
   never stage files outside the step's scope.

## Summary table

| # | Change | Paper | Audible? | Gate to proceed |
|---|---|---|---|---|
| 1 | noiseWidth ratio | ratio-invariant-params | no | golden compare |
| 2 | freqDrift rate | ratio-invariant-params | no | golden compare + chirp |
| 3 | ERB radius, resolution deleted | removing-resolution | yes | 2 listening tests pass |
| 4 | Voronoi residue partition | variable-hopsize | no | golden compare + residue gate |
| 5 | variable grid + jitter | variable-hopsize | yes | sustained-tone listening |
| 6 | attack hop densification | variable-hopsize | yes | attack/decay listening |
| — | deinterpolation deposit | variable-hopsize | yes | only if 6 leaves quantization |
| — | two-pass attack analysis | variable-hopsize | yes | only if 6 insufficient |

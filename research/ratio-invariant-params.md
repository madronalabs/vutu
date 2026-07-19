# Ratio-Invariant Parameters: freqDrift and noiseWidth

*Status: proposal / design note, verified against the code as of 2026-07-19.
Companion to `variable-hopsize.md` — the freqDrift change here is a
prerequisite for variable hop. File references use function names rather than
line numbers, since the tree is under active edit.*

## Summary

Two analyzer parameters are currently expressed in absolute Hz but are really
ratios in disguise:

- **`freqDrift`** (tracker linking gate) is secretly a *rate*. It should be
  expressed in **Hz/sec** and applied per link as `rate · dt`, because the
  tolerable frequency motion of a real partial between two observations is
  proportional to the time between them.
- **`noiseWidth`** (residue-region width in the bandwidth associator) is
  secretly a *fraction of the window width* W. The ear-calibrated rule
  `noiseWidth = W / 3.4` already exists in autoParams; the parameter itself
  should carry that ratio, not the derived Hz.

Both changes are **numerically identity-preserving at today's defaults** — at
constant hop = 1/W the rate form reproduces the Hz form exactly, and the
ratio form reproduces W/3.4 exactly — which is the migration safety property:
golden outputs should not move.

## 1. Verification

### 1a. freqDrift is a rate — confirmed, with one refinement

Current facts (audited):

- The gate in `PartialTracker::buildFrame` (`source/analysis/utuTracker.cpp`)
  is a plain absolute-Hz test per link: `|peakFreq − partialEndFreq| < drift`,
  with no dt anywhere.
- The autoParams derivation (`source/analysis/utuAutoParams.cpp`) sets
  `freqDrift = clamp(3·p95, 0.12·W, min(80, 0.25·W))`, where p95 is the 95th
  percentile of |Δf| of strong peaks matched **hop-to-hop, at hop = 1/W**.

That second fact is the verification: the calibrated quantity is *frequency
change per unit time*, sampled at one specific dt. Today's Hz value is
`rate · (1/W)` with the dt frozen in. The moment hop varies (jitter,
densification — see `variable-hopsize.md`), the frozen dt is wrong: dense
attack frames get an effectively loose gate exactly where the spectrum is
messiest, sparse frames an over-tight one. Expressing the parameter as Hz/s
and multiplying by the actual dt per link makes the calibration portable
across any hop sequence. At constant hop it changes nothing.

**The refinement:** the gate is doing three jobs, and only one of them scales
with dt. Decomposed:

1. **True motion** (vibrato, glide, chirp): a rate. Scales with dt. This is
   what the hop-delta p95 mostly measures, and what the chirp selftest pins
   (a 1000 Hz/s sweep must stay linked).
2. **Estimator jitter**: per-measurement frequency noise from the reassigned
   estimator. Does **not** scale with dt. A pure `rate·dt` gate collapses to
   zero at very small dt and would break linking at densified hops — the
   opposite failure from today's. The gate needs a dt-independent floor ε.
3. **Mislink guard**: don't jump to a neighboring partial. An absolute bound
   set by partial spacing / window resolution, i.e. a fraction of W. Does
   **not** scale with dt. This is already present as the `0.25·W` clamp
   (and the 80 Hz `kDriftHi` ceiling).

So the correct per-link gate is:

```
gate(dt) = min( capHz,  epsHz + rateHzPerSec · dt )

capHz  ~ 0.25·W        (resolution-fraction mislink guard, unchanged)
epsHz  ~ small·W       (estimator-jitter floor; a fixed fraction of W is a
                        reasonable stand-in, since estimator variance tracks
                        the window)
rate   = calibrated    (see derivation change below)
```

Today's single Hz number conflates all three; it only works because hop is
pinned to 1/W. Note the existing clamp bounds `[0.12, 0.25]·W` were already
observed to be "self-similar in the window" — the ratio structure is half-way
present.

*(Future option, not required now: the probe could separate ε from rate by
measuring |Δf| at two lags — 1 hop and 2 hops apart. The motion component
doubles, the jitter component doesn't; two equations, two unknowns.)*

### 1b. noiseWidth is a fraction of W — confirmed; already true in one layer

Current facts (audited):

- `noiseWidth` configures the bandwidth associator
  (`source/analysis/utuBandwidth.cpp`): a global grid of overlapping regions
  of that width (centers spaced half a width apart) spanning 0..Nyquist.
  Residue energy deposits into its two straddling regions and is reclaimed as
  bandwidth by kept partials in proportion to amplitude share.
- autoParams **already derives it as a ratio** in the dense (mod-driven)
  regime: `noiseWidth = windowWidth / 3.4` (`kWindowToNoiseRatio`),
  ear-calibrated 2026-07-12 (README: optimum ~50 Hz at W = 170; 310 Hz was a
  rustle source). The same W/3.4 rule is the zero-default in
  `PartialAnalyzer::configure`. The sparse regime instead uses
  `4·resolution` (= 2·W at default resolution) with a 250 Hz floor.
- But the *parameter representation* is absolute Hz everywhere it is touched
  by a human or a tool: the UI dial (10–5000 Hz, log), the controller, the
  `tune` sweep knob in utuCompare, the `auto-test` range assertions.

Why the ratio is the perceptually invariant form: the associator's job is to
classify residue as "inseparable from this partial → becomes its bandwidth"
versus "genuinely distinct." The analyzer's own notion of *inseparable* is
its resolution, which is W. Residue within ~a main lobe of a kept partial is
analysis-indistinguishable from sinusoid sidebands and belongs in the
bandwidth envelope; residue farther out is resolvable structure. So the
collection radius measured **in resolution cells** — i.e. as a fraction of W —
is the quantity with stable meaning. Hold the Hz width fixed while W changes
and the cell-width of the region changes with it: too many cells sweeps
unrelated noise into partials (the recorded rustle failure), too few starves
the noise model. The ear-calibration finding a fixed W/3.4 across settings is
exactly what this argument predicts.

Two honest caveats for the record:

- **The ratio is regime-dependent today.** Dense: W/3.4 ≈ 0.29·W. Sparse:
  2·W (floored at 250 Hz) — nearly 7× wider, deliberately, because with far
  apart partials the clean zone is large and broad collection is safe. A
  ratio-valued parameter doesn't erase the regime gate; it makes both regime
  values dimensionless (≈0.29 and 2.0) with the floor kept as an absolute-Hz
  safety clamp.
- **The ratio is calibrated for the current window family.** "Fraction of W"
  is meaningful because the Kaiser sidelobe/shape is fixed; a different window
  shape would recalibrate the 3.4.

Also noted during audit: the composite quality metric *prefers* wide regions
(README: it lacks a perceptual asymmetry between missing noise and added
rustle), so `tune` cannot search noiseWidth honestly. That strengthens the
case for pinning the ratio structurally rather than leaving an absolute-Hz
knob for a search to wander.

## 2. The ideas, briefly

**freqDrift → `freqDriftRate` (Hz/s) plus internal floors.** The linking gate
becomes `min(0.25·W, ε + rate·dt)` per link, with dt the actual time between
the partial's last breakpoint and the current frame center. The calibrated
number now means "how fast partials in this material actually move," which is
a property of the *signal*, independent of analysis settings — the definition
of a good parameter. It survives hop jitter, hop densification, and any future
change of the nominal hop rule.

**noiseWidth → window/noise ratio.** The parameter becomes the dimensionless
ratio (dense default 3.4, as `W / noiseWidth`); the associator still receives
Hz, computed once at configure. The calibrated number now means "collection
radius in units of what the analyzer can resolve," which is a property of the
*perceptual trade*, independent of window width — it transfers across W
without re-tuning, which is precisely what the 2026-07-12 ear-calibration
found empirically.

Shared theme, worth stating once for future parameters: **a parameter should
be denominated in the units of the thing it actually bounds.** Motion is
Hz/sec; spectral neighborhoood is resolution cells (fractions of W); temporal
neighborhood is local hop (see the residue time gate in
`variable-hopsize.md`). Absolute-Hz forms only ever worked here because hop
and resolution were themselves pinned to W — the "window-primary" invariant
structure made the errors invisible.

## 3. Code changes required

Ordered roughly analyzer-outward. At every step, defaults must reproduce
current behavior exactly (rate = oldHz·W at hop = 1/W; ratio = 3.4).

### freqDrift

1. **`AnalyzerParams`** (`source/analysis/utuAnalyzer.h`): re-declare the
   field as a rate in Hz/s (rename, e.g. `freqDriftRate`, so no stale-unit
   call site survives a compile). Zero-default derivation becomes the rate
   equivalent of today's `0.5·resolution` per hop: `0.5·resolution·W` =
   `0.25·W²` Hz/s.
2. **`PartialTracker`** (`source/analysis/utuTracker.cpp/.h`): configure with
   (rate, epsHz, capHz). `buildFrame` computes dt per candidate link from the
   partial's last stored breakpoint time to the current frame center — the
   tracker already has both (it stores `frameSample` per breakpoint and
   receives the current center), so no new plumbing. Gate:
   `min(capHz, epsHz + rate·dt)`. Keep the dormant per-frame override
   argument (reserved for wider-at-transients); define it as overriding the
   *rate*, not the final Hz, so it composes with dt.
3. **autoParams derivation** (`utuAutoParams.cpp`): the probe measures
   hopDeltas at `hop_ref = 1/W`; convert at the source:
   `rate = 3·p95 · W`, clamps `[0.12·W, min(kDriftHi, 0.25·W)] · W` Hz/s
   (note the rate-form clamps are quadratic in W — correct, since both the
   per-link bound and the reference dt scale with W). Set epsHz and capHz
   from the same fractions of W now used as clamp bounds.
4. **UI** (`source/common/vutuProcessor.cpp` `freq_drift`,
   `vutuView.cpp` dial): units become Hz/s; today's 2–80 Hz range at
   W = 160 maps to 320–12,800 Hz/s, suggesting a log dial ~100–20,000 Hz/s.
   Controller (`vutuController.cpp`) passes through unchanged apart from the
   field rename.
5. **Persistence** (`vutuPartials.h`): the `.utu` header currently stores
   `freq_drift` in Hz. Write a new key (`freq_drift_rate`); on read, accept
   the old key and convert using the stored `windowWidth`
   (`rate = oldHz · W`), which is exact because old files were analyzed at
   hop = 1/W. Keep writing both keys for one transition period if older
   readers matter.
6. **utuCompare**: display strings, the `convert` key parser, the `tune`
   sweep-knob range, and the `auto-test` range assertions
   (`inRange(freqDrift, 2, 80)`) all change to the rate form. The **chirp
   selftest is the behavioral pin**: the 1000 Hz/s sweep must still link, and
   is now directly interpretable against the gate (needs
   `ε + 1000·dt` Hz of gate — trivially satisfied).
7. **Regression**: run the selftest suite and a golden-file comparison at
   constant hop before/after; outputs should be bit-identical, since the gate
   evaluates to the same Hz.

### noiseWidth

1. **`AnalyzerParams`**: replace the Hz field with the dimensionless ratio
   (suggest keeping the existing orientation `kWindowToNoiseRatio = W /
   noiseWidth`, dense default 3.4; rename the field, e.g.
   `windowNoiseRatio`). Zero-default = 3.4, which is what the zero path in
   `PartialAnalyzer::configure` already computes.
2. **`PartialAnalyzer::configure`** (`utuAnalyzer.cpp`): compute
   `noiseWidthHz = W / ratio` once and pass Hz to
   `AssociateBandwidth::configure` — **the associator itself does not
   change**; its region grid is genuinely a Hz structure. (Consequently
   `testResidueGate`, which drives the associator directly in Hz, does not
   change either.)
3. **autoParams** (`utuAutoParams.cpp`): the dense rule becomes a
   passthrough (ratio = 3.4). The sparse rule becomes ratio = 0.5 in the same
   orientation (W/noiseWidth for noiseWidth = 2·W), keeping the 250 Hz
   minimum-width and 2000 Hz maximum-width clamps as absolute safety bounds
   applied to the derived Hz. The `modDriven` gate is unchanged — it now
   selects between two dimensionless values.
4. **UI**: the `noise_width` dial becomes a ratio dial (log, range spanning
   the old behavior — 10–5000 Hz at W = 160 corresponds to ratios 16–0.032
   in W/noiseWidth orientation; a practical range is ~1–16 centered on 3.4).
   Consider displaying the derived Hz alongside, since Hz is what a user sees
   on the spectral display.
5. **Persistence**: `noiseWidth` is **not currently written to the `.utu`
   header at all** (audited gap — the controller stores freqDrift,
   windowWidth, ampFloor, etc., but not noiseWidth). Add the ratio to the
   header while touching this; it costs nothing and completes the record of
   analysis settings.
6. **utuCompare**: display string, `convert` key parser, `tune` knob
   (sweeping the ratio directly), `auto-test` range assertion, and the
   `analyzeSimple` helper's `noiseWidth = 2000` default (becomes a ratio, or
   is dropped in favor of the zero-default). The metric-asymmetry limitation
   (README: composite score prefers wide regions) is unchanged by this work
   and remains the blocker for *searching* the ratio automatically.
7. **README**: update the "noiseWidth ratio rule" note to point at the
   parameter itself, and the parameter tables/usage help in utuCompare.

### Suggested order

noiseWidth first: it is smaller, the associator is untouched, and the ratio
rule already exists — the change is representation only. freqDrift second,
ideally landing together with (or just before) the variable-hop work that
makes its rate form load-bearing; at constant hop it is a pure refactor with
a bit-exact regression check available.

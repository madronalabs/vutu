# Removing `resolution`: a Perceptual Masking Radius Replaces the Parameter

*Status: proposal / design note, verified against the code as of 2026-07-19.
Third in a series with `variable-hopsize.md` and `ratio-invariant-params.md`;
depends on the freqDrift re-anchoring described in the latter. File
references use function names rather than line numbers, since the tree is
under active edit.*

## Summary

`resolution` has exactly one job in the hot path: it is the masking radius in
`thinPeaks`. Everything else it touches is default-derivation plumbing
(freqDrift, freqFloor, the sparse noiseWidth rule) or autoParams bookkeeping.

An important correction to the intuitive framing first: resolution does not
*discard* partials. Masked peaks are moved to the **residue** pool and come
back as noise energy (bandwidth) on the surviving partials. The masking
radius is therefore the analyzer's sinusoid-vs-noise **classification
boundary**, not just a complexity throttle. The throttle job is genuinely
redundant — the budget machinery bisects ampFloor against ground truth and is
the perceptually correct way to bound partial count. The classification job
is real and must go somewhere.

The proposal: **delete the `resolution` parameter, keep the masking, and set
its radius from psychoacoustics rather than from a dial:**

```
R(f) = α · ERB(f),   α ≤ 1  (hard-coded)

ERB(f) = 24.7 · (1 + 4.37·f/1000)  Hz     (Glasberg & Moore)
```

This radius is *perceptually lossless by construction* — it can only fuse
sinusoid pairs the ear itself fuses — and it decouples masking from the
window entirely. The consequences run in both directions and both are wins:
in the low registers the radius is narrower than today's W/2, so masking
stops adding fusion beyond the ear's own; at high frequencies it is *wider*
than W/2, so pairs the window resolves but the ear cannot are folded into
one partial + bandwidth, freeing budget for distinctions listeners can
actually hear. It is not a parameter; it is a property of human hearing, one
hard-coded function. This completes the taxonomy begun in
`ratio-invariant-params.md`: motion is Hz/sec, spectral neighborhood is
fractions of W, temporal neighborhood is local hop — and the fusion radius is
ERBs.

A pleasant audit result: **nothing divides by resolution anywhere in
`source/analysis/`.** The zero-hazards are all in derivation chains, listed
in §6.

## 1. What resolution actually does today (verified)

- **The one hot-path use** — `thinPeaks` (`source/analysis/utuPeaks.cpp`):
  peaks are sorted loudest-first; each kept peak claims ±resolution of
  spectrum; any quieter peak inside a claimed band is rejected — but
  *retained as residue*, which `AssociateBandwidth` then redistributes to
  kept peaks as bandwidth. The header comment states the design intent:
  "resolution = W/2 keeps every pair either cleanly resolved or cleanly
  merged: nothing partially-resolved survives thinning to become masked
  noise."
- **Default-derivation chains** (`PartialAnalyzer::configure`):
  `freqFloor ← resolution`, `freqDrift ← 0.5·resolution`. (noiseWidth's
  default is already W-based, not resolution-based.)
- **autoParams**: the search path always sets resolution = 0.5·W and freezes
  it there (`resFrozen`, the "merge-clean invariant"), so as a *searched*
  quantity it is already not a free parameter. It is free in exactly one
  regime: the noise fallback (nothing coheres), where
  `applyResolution(band / (0.8·budget))` uses it as a track-density target.
  The probe also uses it for internal radii: persistence matching within
  0.5·resolution, coherence linking within 0.1·resolution, and a kept-peak
  density model ("about one track per 1.25·resolution").
- **Interfaces**: UI dial (8–1024 Hz, log, default 40), controller wiring
  including a legacy fallback `windowWidth = 2·resolution`, `.utu` header
  key, and utuCompare's display/key-parser/tune-knob/auto-test surfaces.

Note the irony: the natural question "what if resolution were 0?" cannot
even be asked of the current code — 0 is the "derive W/2" sentinel and the UI
floor is 8 Hz.

## 2. Background: the ear's own frequency resolution

This section is the foundation of the proposal; it is worth recording in
full.

### The ERB scale

The auditory system analyzes sound through a bank of overlapping filters of
finite bandwidth. The modern measurement of those bandwidths is the
**equivalent rectangular bandwidth** scale (Glasberg & Moore, 1990,
refining Moore & Glasberg 1983; successor to Zwicker's critical-band / Bark
scale):

```
ERB(f) = 24.7 · (4.37·f/1000 + 1)  Hz  ≈  24.7 + 0.108·f
```

Worked values:

| f (Hz) | 100 | 300 | 440 | 1000 | 4000 |
|---|---|---|---|---|---|
| ERB (Hz) | ~35 | ~57 | ~72 | ~133 | ~456 |

The minimum, approached at low frequencies, is ≈ 25 Hz. The bandwidth is
roughly constant below ~500 Hz and roughly proportional to f (≈ 11%) above
~1 kHz.

### Fusion: what happens inside one filter

Two stable sinusoids whose spacing Δf is inside one auditory filter are
**never heard as two tones**. They excite the same filter, whose output is a
single carrier at (approximately) the amplitude-weighted centroid frequency,
amplitude-modulated at the difference frequency Δf. The listener perceives
one auditory object; only the character of its modulation changes with Δf.
Segregation into two tone percepts requires the components to produce
resolvable excitation peaks — spacing on the order of an ERB or more (the
transition is gradual, which is what the safety factor α is for).

### The three regimes, and the model's matching representations

The percept of the modulation depends on its rate, and — this is the crux —
the bandwidth-enhanced partial model has a representation that matches each
regime *exactly*:

| Δf | Percept | Faithful representation in our model |
|---|---|---|
| below ~20 Hz | countable, deterministic beating / tremolo | one **merged track** — its amplitude/frequency envelopes carry the beat literally (representable whenever the frame rate exceeds 2·Δf, which the critical hop's frame rate = W covers with orders of magnitude to spare) |
| ~20 Hz → ~ERB(f) | **roughness**: dense, uncountable, noise-like flutter (Plomp & Levelt, 1965 — maximal roughness near 25% of a critical bandwidth, fading toward the band edge) | one **partial + bandwidth** — noise-modulated AM. The ear does not track modulation in this range deterministically either; "two sinusoids beating at 47 Hz" and "one sinusoid with noise-like AM of matching depth and rate" are the same percept. This equivalence is the founding assumption of the bandwidth-enhanced model. |
| above ~ERB(f) | two segregated tones | **two partials** |

The pivotal row is the middle one: within the roughness zone, demoting the
weaker peak of a pair to residue→bandwidth is not an approximation — it is a
change of coordinates between two representations of the same percept. Note
that this energy goes to the *bandwidth* envelope, not the amplitude
envelope, so no frame-rate/Nyquist question arises for it; the noise model
renders the roughness directly.

### Precedent

Original Loris scaled its bandwidth-association regions on the Bark scale
for exactly this class of reason: the perceptually meaningful spectral
neighborhood is an auditory-filter width, not a fixed Hz span. ERB is the
modern refinement of that idea, applied here one stage earlier in the
pipeline (peak thinning rather than residue association).

### Caveats

- **Sub-20 Hz beats are deterministic and audible.** A clean 5 Hz beat must
  land in the survivor's *envelope*, not in bandwidth noise — noise-AM would
  turn tremolo into flutter. Merging (a single consensus crossing in
  `selectPeaks`) does this correctly, and pairs that close essentially
  always merge rather than produce two crossings — a pair spaced a few Hz is
  deep inside one main lobe at any usable window. Masking only ever
  operates on pairs that produced *two* crossings, which puts them well
  above the deterministic-beat zone; their difference tones land in the
  roughness regime, where the bandwidth representation is the faithful one.
- **This is an argument about sinusoidal components, not musical notes.**
  Two complex tones a whole step apart segregate easily even though their
  fundamentals share an ERB — segregation is cued by their upper harmonics,
  which are ERB-separated and which the analyzer keeps as distinct partials.
  The bound only ever fuses component pairs the ear fuses too.

## 3. The bound, and the proposal

Masking at radius R(f) is **perceptually lossless** provided:

1. **R(f) ≤ ERB(f)** — the fused pair was a single auditory object anyway;
2. **the masked energy is conserved** into the survivor's envelope/bandwidth —
   which `thinPeaks` → residue → `associateBandwidth` already guarantees.

Nothing audible is ever gained by resolving finer than the ear's own filter,
and nothing audible is lost by fusing within it. Read as a *lower bound on a
resolution dial*, this says no perceptual purpose is served by any value
below ~25 Hz (minimum ERB) — the current UI floor of 8 Hz is below
perceptual utility. But the frequency-dependent form is strictly better than
any constant: the perceptually free radius at 4 kHz (~456 Hz) is ~18× the
one at 100 Hz (~35 Hz). A constant radius must choose between wasting budget
up high and fusing audible pairs down low; the ERB curve does neither. Hence:

```
R(f) = α · ERB(f)
```

with α ≤ 1 a hard-coded safety constant (start at 1.0, tune by listening —
the fusion/segregation transition is gradual, and α is the margin against
its edge). Deliberately **no W term**: the radius is a property of the
listener, not of the analysis. The window still governs what *appears* as
separate consensus peaks — nothing narrower than the main lobe ever reaches
the masking stage as a pair — so the effective behavior is "the window
limits detection, the ear limits retention," each constraint stated once, in
its own units.

**How this compares to the status quo (W/2), both directions.** With
W = 160, today's radius is 80 Hz; ERB(f) crosses 80 Hz at ~510 Hz.

- **Below ~510 Hz, today's rule is coarser than the ear** — it fuses pairs
  (e.g. 60 Hz apart at 300 Hz center; ERB ≈ 57) that a listener separates.
  The ERB radius stops that. Honesty requires a caveat: at W = 160 the
  newly-preserved spacings (between ERB and 80 Hz) sit in the window's own
  partially-resolved flicker zone, where double crossings are transient — so
  the *full* low-register fidelity gain arrives only together with a finer
  window; what the ERB radius guarantees is that masking is never the
  binding constraint below the ear's resolution, whatever window autoParams
  picks.
- **Above ~510 Hz, today's rule is finer than the ear** — every kept pair
  with spacing between W/2 and ERB(f) is a distinction no listener can make,
  rendered as two tracks where one partial + bandwidth would sound
  identical. At 4 kHz the ear-redundant zone is 80–456 Hz of spacing: wide.
  The ERB radius fuses these, and the budget bisection then converges at a
  lower amplitude floor for the same perceived detail — the budget migrates
  from inaudible high-frequency distinctions to audible low/mid content.
  This is the "spend partials where the ear is looking" property, and it is
  the main practical payoff.

## 4. Do we need spacing-based culling at all?

For **complexity control: no.** The authoritative count mechanism is the
budget verification: `achievedAt` runs the real analyzer and bisects
**ampFloor** until `maxActivePartials` meets the budget, with hiCut as
backup; resolution is not touched in that loop. The floor is also the
perceptually right dial — it sheds the *quietest* partials, whereas a
spacing rule sheds the *closest* regardless of level.

For **classification: yes, and the ERB radius is its principled form.** The
unresolvable flicker zone (transient double crossings from close pairs,
near-peak noise above the floor) must fold into the bandwidth model or the
noise representation starves. An earlier draft of this note proposed
removing masking entirely and accepting that risk; the ERB argument
dissolves the dilemma. Masking stays — so the noise model keeps its food
supply — but its radius is now provably free: it can only fuse what the ear
fuses. The spacing rule stops being a fidelity/complexity trade and becomes
a perceptual identity.

## 5. What still stands — confirming the dials claim

With the parameter gone, the dials that bound instantaneous partial
complexity are confirmed as:

- **ampFloor** — per-peak threshold in `thinPeaks` (below-floor peaks still
  become residue), automated by the budget bisection;
- **loCut** — per-peak low bound in `selectPeaks` (as freqFloor);
- **hiCut** — *caveat 1*: applied as a post-pass (`cutHighs` on finished
  partials in the controller and autoParams), not an in-frame gate; it still
  bounds the output's instantaneous complexity, just not inside the frame
  loop;
- *caveat 2*: two non-frequency gates also remain — the time-offset crop
  (`cropTime`) and the negative-absolute-time drop.

Structural spurious-peak defenses independent of the masking radius:

- the consensus sign-crossing selector emits at most one candidate per
  positive→negative freq-correction crossing — one estimate per consensus
  region;
- window sidelobes are designed below the floor (`sidelobeLevel` defaults to
  −ampFloor), so sidelobe peaks never clear the threshold;
- `cleanOutliers` deletes one-breakpoint blips post-analysis.

The tracker still has no dedup — it trusts thinPeaks' spacing. Under the ERB
radius that trust remains funded (unlike under pure removal), with the
spacing guarantee now frequency-dependent: kept peaks are at least α·ERB
apart.

## 6. Zero-safety inventory

Audited: **no expression in `source/analysis/` divides by resolution, sizes
an allocation from it, or steps a loop by it.** The hazards are all
downstream of the default chains, and all must be re-anchored *before* the
field is deleted:

- `freqDrift ← 0.5·resolution`: deleting resolution without re-anchoring
  makes the default drift 0 → the tracker gate `dist < 0` never passes →
  every peak spawns a new partial every frame (no crash, but degenerate
  tracking). Re-anchor to W: 0.25·W — which `ratio-invariant-params.md`
  already does in rate form (0.25·W² Hz/s). **Land that change first.**
- `freqFloor ← resolution`: re-anchor to 0.5·W to preserve today's default
  (the auto path already sets freqFloor from loCut instead).
- Sparse-regime `noiseWidth = 4·resolution`: at resolution 0 this would send
  noiseWidth toward `AssociateBandwidth::configure`'s
  `_regionRate = 2./regionWidthHz` — the one true division-by-zero in the
  neighborhood — but it is unreachable today (clamped to ≥ 250 Hz) and the
  rule becomes W-based (ratio 0.5) under `ratio-invariant-params.md` anyway.
- The `.utu` reader truncates the header value through a `size_t` in one
  spot (`valueTreeToVutuPartials`, unused local) — harmless, but clean it up
  while in there.

## 7. Pros and cons

**Pros.**

- **Perceptually lossless by construction**: no ear-separable pair is ever
  fused (α ≤ 1), and every fusion performed lands in a representation
  (partial + bandwidth) that is perceptually identical to the pair it
  replaced.
- **Intelligent budget use**: fusion widens with frequency exactly as the
  ear's resolution coarsens. Ear-redundant high-frequency track pairs
  (spacing between W/2 and ERB) collapse into single bandwidth-enhanced
  partials, and the budget bisection converges at a lower floor — tracks
  migrate to where listeners can tell the difference.
- **Masking decouples from the window**: detection limits (the main lobe)
  and retention limits (the ERB) are stated separately, each in its natural
  units. Changing W no longer silently changes what the analyzer considers
  "one sound."
- **Parameter honesty**: a parameter that autoParams pins to W/2 and freezes
  was not a parameter; its replacement is a hearing constant, not a knob.
  The window-primary structure completes: one master knob W, perceptual
  dials floor/loCut/hiCut/budget, and hard-coded psychoacoustic invariants.
- **The classification machinery survives intact** — residue keeps feeding
  the bandwidth model; the noise-starvation risk of pure removal never
  arises.
- Deletes real complexity: the sentinel chain, the `resFrozen` machinery,
  the `applyResolution` W ≥ 2·resolution coupling, the legacy
  `windowWidth = 2·resolution` fallback, a UI dial, a tune knob.

**Cons / risks.**

- **Bright material changes character by design.** Analyses of
  high-frequency-dense sounds will have fewer, noisier-looking partials up
  high (pairs become partial + bandwidth). The perceptual claim is that
  resynthesis sounds the same; the *display* and partial counts will look
  different, and any downstream use that treats partials as symbolic objects
  (e.g. Sumu modulation routing per partial) sees a coarser high end. Worth
  a deliberate listen before committing (§9).
- **Low-register track density rises** where the window is fine enough to
  resolve what the ear resolves: more simultaneous low/mid tracks, absorbed
  by the budget floor. On budget-limited analyses some reclaimed high-end
  budget converts to low-end tracks — the intended migration, but it moves
  where the floor lands; verify on reference material.
- The noise-fallback regime loses its density knob. Replacement exists and
  is arguably cleaner: the fallback currently sets
  resolution = band/(0.8·budget) and then W = 2·resolution; drive W directly
  as W = band/(0.4·budget) — the window itself is the natural track-density
  unit (~one track per lobe).
- Probe calibration shifts: the kept-peak density model ("one track per
  1.25·resolution"), the persistence radius (0.5·resolution), and the
  coherence radius (0.1·resolution) are denominated in resolution. The
  radii convert to W-fractions (0.25·W, 0.05·W at the frozen ratio); the
  density model must be re-measured under the ERB radius (density is now
  frequency-dependent) and the quality-walk targets re-tuned against
  `achievedAt` ground truth.
- Slightly more work per frame in the low registers (more kept peaks →
  tracker and bandwidth association), slightly less up high; worth a timing
  check but likely a wash.
- α is a new hard-coded constant that wants a listening calibration — but it
  has a principled default (1.0) and a bounded meaning (margin against the
  gradual fusion/segregation edge), unlike the dial it replaces.

*Staging option:* if the high-frequency behavior change feels too large for
one step, `min(W/2, α·ERB(f))` is a conservative intermediate — identical to
today above the crossover, ERB-limited below — but note it forfeits the
budget-reclaim pro entirely; it is a migration stage, not the destination.

## 8. Code changes required

Ordering: land `ratio-invariant-params.md` first (it re-anchors freqDrift
and noiseWidth to W, cutting two of the three chains in §6). Then:

1. **`thinPeaks`** (`utuPeaks.cpp/.h`): keep the loudest-first masking loop;
   replace the `freqResolutionHz` argument with the hard-coded radius —

   ```cpp
   inline float erbHz(float f) { return 24.7f * (1.f + 4.37f * f * 0.001f); }
   // masking radius between peaks a and b:
   //   kErbAlpha * erbHz(0.5f * (a.freq + b.freq))
   ```

   evaluated per comparison at the pair's mid frequency; `kErbAlpha` a named
   constant (1.0 to start, tuned by listening). Keep the ampFloor threshold,
   the 10 dB fade, the negative-time drop, and the kept/residue partition
   unchanged.
2. **`AnalyzerParams`** (`utuAnalyzer.h`): delete the field; re-anchor
   `freqFloor` zero-default to `0.5·windowWidth` in
   `PartialAnalyzer::configure`; delete the resolution sentinel line.
3. **`takePartials`**: stop copying resolution into the output struct.
4. **autoParams** (`utuAutoParams.cpp`): delete `applyResolution`,
   `resFrozen`, and the resolution knobs in the quality walk (down-walk
   ×1.25 step, ascent `kKnobRes`); rewrite the noise fallback to drive W
   directly (`W = band/(0.4·budget)`, clamped to the W range); convert probe
   radii to W-fractions (persistence 0.25·W, coherence 0.05·W); probe
   thinning uses the same ERB radius as production; re-measure the kept-peak
   density constant and re-tune `countP90` targets.
5. **UI / controller**: remove the dial (`vutuProcessor.cpp` param def,
   `vutuView.cpp` dial + layout + label); in `vutuController.cpp` remove the
   read/wiring and decide the legacy fallback (`windowWidth = 2·resolution`
   when no width param) — either keep reading a stored resolution value
   solely to serve that fallback for old sessions, or drop it and require
   windowWidth.
6. **Persistence** (`vutuPartials.h`): remove the field and the
   `"resolution"` header key from the writer; keep the reader *tolerant* of
   the key (old files carry it). Fix the `size_t` truncation in passing.
7. **utuCompare**: display string, key parser, tune-knob table, help text,
   `analyzeSimple`'s `resolution = 0.5·W` line, and the auto-test
   `inRange(resolution, 8, 1024)` assertion all go.
8. **README**: update the invariants block — the W/2 line becomes "masking:
   R(f) = α·ERB(f), a hearing property, not a parameter; the window limits
   detection, the ear limits retention" — and the parameter tables.

## 9. Validation

1. **`testNoiseBw`** — standing regression, no longer a decisive canary:
   masking is retained, so white noise should still yield high mean
   bandwidth. Confirms the noise model's food supply survived the radius
   change.
2. **`testTwoSine`** — merged case (0.25·W spacing = 40 Hz near 440 Hz)
   still merges: 40 < ERB(440) ≈ 72, so the assertion passes identically.
   Add a third case pinning the new high-frequency behavior: a pair the
   window resolves but the ear does not — e.g. at W = 160, 240 Hz spacing
   (1.5·W, stable double crossings) centered at 4 kHz, where
   ERB ≈ 456 > 240. The old rule keeps two partials; the new rule must
   yield one simultaneous partial with elevated bandwidth. This is a
   deterministic, non-flaky pin because the window resolves the pair
   reliably and the radius fuses it unconditionally.
3. **Listening comparisons** — the decisive perceptual tests, one per
   direction of the change: (a) low register: dense material below ~500 Hz
   (the gambang/rustle set from the README notes) under the 80 Hz rule vs
   the ERB rule — §3 predicts audible fidelity gain with no new rustle;
   (b) high register: bright/dense material (cymbals, bowed metal) — the
   perceptual-losslessness claim predicts *no audible difference* despite
   substantially fewer high-frequency tracks. Test (b) is the riskier
   claim and the better falsifier of α = 1.
4. **Budget behavior**: compare `achievedAt` floor positions before/after on
   reference material; expect floor *drops* on bright material (high-end
   budget reclaimed) and modest floor rises on low-dense material (the
   intended migration).
5. Full selftest suite + golden comparison; timing check on low-register
   dense material.

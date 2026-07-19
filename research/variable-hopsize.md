# Variable Hop Size in the Vutu Analyzer

*Status: proposal / design note. Concerns the clean-room analysis pipeline in
`source/analysis/` (reassigned, bandwidth-enhanced, breakpoint-partial model).*

## Summary

The analyzer currently advances its frame grid by a constant hop
(`_frameSample += _hopSamples`), with an optional per-frame jitter offset
(`hopJitter`) around that grid. This note proposes making the hop itself
variable — both randomized (jitter as grid variation, not just offset) and
signal-adaptive (denser hops at attacks) — and examines what the math permits.

The short version: hop size never enters the per-frame estimation math. Each
frame's reassigned time, frequency, amplitude, bandwidth, and phase are
functions of the windowed segment alone; the hop sequence is just a sampling
grid laid over continuous ridge functions, and non-uniform sampling of those
ridges is legitimate wherever the *local* frame density satisfies the local
Nyquist condition on envelope modulation. Almost the entire pipeline is
already time-based rather than frame-index-based, so it tolerates a variable
grid as-is. One component does not: the residue/bandwidth association gate,
whose energy accounting is exact only on a uniform grid. Two mitigations for
it are given below.

## Benefits

### 1. Jitter: decorrelating frame-rate artifacts

Envelopes sampled on a uniform grid and reconstructed by linear interpolation
(as the oscillator-bank synth does between breakpoints) carry a periodic
interpolation error: modulation sidebands at the frame rate and its harmonics,
audible on clean sustained tones as a faint frame-rate signature. Randomizing
the sample times converts this coherent error into low-level broadband noise —
the same trade dither makes against quantization distortion, or randomized
sampling makes against aliasing.

The pipeline is unusually well positioned to collect this benefit, because
breakpoint times are real everywhere: the tracker stores the actual (jittered)
frame center plus the reassignment time correction per breakpoint, the phase
fix integrates over actual `dt`, the synth interpolates between actual
breakpoint samples, and the `.utu` format stores the raw time vectors with no
uniform-grid resample. The dithering benefit therefore survives end-to-end.

*Caveat:* if a uniform envelope grid ever appears downstream (e.g. an importer
that resamples partials to a fixed frame rate), the artifact returns at that
grid's rate, regardless of what the analyzer did.

### 2. Densification: temporal resolution at attacks

Dropping the hop near a detected onset samples the amplitude, frequency, and
bandwidth envelopes more finely exactly where they move fastest. Two points
sharpen the picture of what this does and does not buy:

- **It does not change frequency resolution.** Resolution is a property of the
  window, which stays fixed. The familiar time/frequency trade the analyzer
  would face by *shortening the window* at attacks (and the merged-partial
  seam problems that come with switching resolution mid-note) is a separate
  proposal, deliberately out of scope here. Hop-only densification is
  seam-free: same estimator, same resolution, same ridges, just sampled more
  densely.
- **The window still smears the sinusoidal amplitude onset.** A partial's
  amplitude envelope rises over roughly a window length no matter how finely
  it is sampled. Two partial rescues: time reassignment already localizes
  impulsive energy to sub-window precision (breakpoint *times* at an attack
  are better than the window length suggests), and — less obvious — the
  residue model's time granularity is the hop, not the window, so denser hops
  give genuinely finer time structure in the **bandwidth envelopes**. Since
  much of what makes an attack sound like an attack lives in the noise
  component, this is likely the largest audible win.

## Why the math mostly permits it

The per-frame estimates are hop-independent, and there is no COLA constraint
anywhere: resynthesis is a pure oscillator bank, not overlap-add. The
remaining constraint is a sampling condition — local frame rate must exceed
twice the local envelope modulation bandwidth — which is the same
"modulation demand" framing autoParams already uses to choose the window.

A survey of the pipeline finds it already time-based in every load-bearing
place:

| Component | Status under variable hop |
|---|---|
| Breakpoint times | Stored explicitly: actual frame center + reassignment offset. Never reconstructed as `frameIndex * hop`. |
| Phase continuity (`utuPhaseFix`) | Integrates over actual `dt` between breakpoints. |
| Tracker insertion | Time-ordered via `timeAt()`; tolerates out-of-order corrected times. |
| Resynthesis (`utuSynth`) | Linear segments between arbitrary breakpoint samples. |
| Budget contract | `maxActivePartials` computed from time-interval overlap; hop-invariant. |
| `.utu` export | Verbatim per-breakpoint time vectors; no grid, no stored hop. |
| Residue association (`utuBandwidth`) | **Assumes uniform grid. See below.** |

Secondary items that need mechanical (not mathematical) attention:

- The grid advance itself, and the history-drop / frame-ready bookkeeping in
  `utuAnalyzer`, which assume "next center = grid + bounded jitter." A
  suddenly small hop at an attack could want input samples already discarded.
- The tracker's frequency-drift gate is absolute Hz per link, calibrated from
  per-hop |Δf| statistics at the nominal hop. Under variable hop it should
  scale with actual `dt` (a rate gate, Hz/s), else dense attack frames get an
  effectively loose gate exactly where the spectrum is messiest. The plumbing
  exists: `buildFrame` already accepts a per-frame drift override.
- Track death is "unmatched for one frame." At constant hop that is a
  deliberate no-gap-bridging choice; under variable hop it silently becomes
  "gap tolerance = local hop." Probably acceptable, but it should be a
  decision, not an accident.
- `cropTime` defaults to the hop — same "this energy belongs to a neighbor
  frame" role as the residue gate, same fix as below.
- autoParams probe statistics (coherence per frame, per-hop Δf) remain valid
  as *calibration at a reference hop*; only the `probeSimultaneousP90`
  diagnostic shifts meaning under denser production hops.

## The one real problem: residue energy accounting

The bandwidth associator assigns each residue quantum to the frame nearest its
(reassigned) true time, gated by a fixed window of ±hop/2 around each frame
center. On a uniform grid these tiles abut exactly: every quantum is counted
once, and the noise model conserves energy by construction.

Under a variable grid the fixed ±hop/2 tiles no longer partition the timeline:

- where frames are **dense** (attack densification), tiles overlap — residue
  energy is double-counted, inflating bandwidth exactly at attacks;
- where frames are **sparse** (or when jitter stretches a gap), tiles leave
  holes — residue energy is silently dropped.

Both errors land in the bandwidth envelopes, i.e. in the noise component the
densification was meant to improve. This is the one place variable hop changes
the *math* rather than the bookkeeping.

### Mitigation A: Voronoi partition (exact)

Replace the fixed symmetric gate with the Voronoi partition of the actual
center sequence: frame *k*'s tile extends from the midpoint toward its actual
left neighbor to the midpoint toward its actual right neighbor,

```
tile(k) = [ (t[k-1] + t[k]) / 2 ,  (t[k] + t[k+1]) / 2 )
```

The gate becomes asymmetric and per-frame, but the tiles abut exactly for
*any* center sequence — jittered, densified, or both — so single-counting and
energy conservation are restored by construction. On a uniform grid this
reduces identically to the current ±hop/2 gate, which makes it a strict
generalization and easy to verify against the existing selftests.

Cost: frame *k* cannot be finalized until center *k+1* is known — one frame of
lookahead (or deferred finalization) in the streaming loop.

### Mitigation B: deinterpolation (energy-exact, time-smoothed)

Instead of *hard-assigning* each residue quantum to exactly one frame, deposit
it *fractionally* onto the two frames bracketing its reassigned time, with
linear weights:

```
quantum at time t, bracketed by centers t[k] <= t < t[k+1]:

    a = (t[k+1] - t) / (t[k+1] - t[k])      -> frame k receives a * E
    1 - a                                    -> frame k+1 receives (1-a) * E
```

This is linear *de*interpolation — the transpose of the linear interpolation
the synth will later apply when reading the envelope back (the same
scatter-add used to deposit particles onto grids in PIC codes, or grains onto
delay lines). Because the weights sum to one, energy is conserved by
construction for any center spacing; no partition geometry is needed at all.

Properties, versus Voronoi:

- **Smoother bandwidth envelopes.** Hard assignment makes each quantum a
  ±half-tile temporal quantization; fractional deposit removes that
  quantization noise. Analytically: deinterpolation followed by the synth's
  linear interpolation applies a triangular (Bartlett) smoothing of about one
  local hop to the residue energy — a mild low-pass rather than a
  staircase.
- **Slight temporal smearing.** That same triangular kernel spreads a
  quantum's energy over roughly one local hop. Since the local hop is
  *small* precisely where timing matters (densified attacks), the smearing is
  smallest where it would hurt most — the scheme degrades in the right
  direction.
- **Same lookahead cost.** A quantum between centers *k* and *k+1* touches
  both, so frame *k* again finalizes one frame late. Neither mitigation is
  free of the one-frame deferral; it is inherent to knowing where the next
  center landed.
- **Inexact variants are acceptable.** If touching two frames per quantum is
  inconvenient, a cheaper approximation keeps hard assignment but normalizes
  each frame's collected residue by its realized tile coverage (measured
  against actual neighbor midpoints). This restores energy conservation *on
  average* without restructuring the deposit loop — a reasonable interim step,
  since the goal here is to keep the noise model honest, not to make it
  bit-exact.

### Choosing

Voronoi is the minimal, exactly-conservative change and the natural first
implementation: same hard-assignment structure as today, provably identical
output on a uniform grid. Deinterpolation is the better *model* — it matches
the synth's read-side interpolation, conserves energy for free, and trades the
partition's temporal quantization for a small, well-placed smoothing — at the
cost of a two-frame deposit. A sensible path is Voronoi first (correctness,
regression-testable), deinterpolation later if bandwidth-envelope granularity
at attacks proves audible.

## Out of scope, noted

Varying the *window* at attacks (true time/frequency retrade) raises a
different class of problems — merged-partial births at the resolution seam,
estimator-variance mismatch in the linking gate, per-window renormalization of
the sinusoid/noise split. If short-window attack detail is wanted, the robust
structure is a guided second pass over the attack region with the long-window
pass owning partial identity and linking running backwards in time from the
stable side — not in-stream window switching. Separate note if pursued.

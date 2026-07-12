# utu analysis: theory ↔ code map and optimization targets

Reference: Fitz & Fulop, *A Unified Theory of Time-Frequency Reassignment*
(`papers/Time-Freq Reassignment.pdf`). Equation numbers below are that paper's.
North-star goal: a real-time signal → bandwidth-enhanced-partials engine with
reasonable latency and CPU.

## Theory → code

| Paper | Code |
|---|---|
| STFT / moving-window transform, eqs. 3, 5, 12–13 | `ReassignedSpectrum::transform` (window + rotate + zero-pad; rotation converts eq. 5 phases to eq. 3 phases for free) |
| auxiliary windows hT = t·h, hD = dh/dt, Sec. 6.2 / Fig. 6 | `buildReassignmentWindows` (`utuWindow`); hD analytic via I1, so eq. 70's FFT construction is unneeded |
| time reassignment t̂ − t = Re{XTh·X*}/\|X\|², eq. 64 | `timeCorr` kernel in `ReassignedSpectrum::transform` |
| frequency reassignment ω̂ − ω = Im{XDh·X*}/\|X\|², eq. 65 | `freqCorr` kernel (sign flipped by the h(τ−t) window convention; ×N/winlen finishes the rad/sample → bin conversion) |
| consensus regions / fixed points of ω̂(ω), Fig. 3 | `PeakSelector::selectPeaks` (sign-crossing scan of `freqCorr`) |
| pruning by large reassignment, Sec. 7 | cropTime test in `selectPeaks`; amplitude thinning in `thinPeaks` |
| phase at reassigned coordinates, Sec. 8 | `ReassignedSpectrum::phaseAt` (linear interpolation toward the reassigned frequency + travel ω̂·(t̂−t)) |
| phase-correct additive model, Sec. 8 | `utuPhaseFix` (frequency/phase reconciliation), `utuSynth` (integrate frequency, anchor phase only at silence) |
| mixed derivative ∂²φ/∂t∂ω, eqs. 105/110 (Loris "convergence") | intentionally not computed; residue bandwidth association overwrites it |
| bandwidth-enhanced model [18, 24] | `utuBandwidth`, `utuSynth` |

## Optimization / transformation candidates

Cost picture first (44.1 kHz, windowWidth 160 Hz): winlen ≈ 2175, N = 8192,
3 real FFTs per 275-sample hop. With a SIMD FFT this is well under 1% of one
core — **CPU is not the wall; lookahead is**: winlen/2 ≈ 25 ms.

### A. Cross-spectral (finite-difference) reassignment (eqs. 54–57)
**Measured** (utucompare `nelson-test`, sine + cello/flute/clarinet at
44.1 kHz, res 80 / width 160; errors at strong (0..−40 dB) AF peak bins):

- *Hop-spaced cross-frames (the free variant, 1 FFT/hop): dead.* The wrap
  bound |f̂ − f_bin| < sr/(2·hop) equals the main-lobe half-width exactly
  (hop = 1/windowWidth), leaving zero margin, and 6 ms finite differences
  measure a frequency *average*, not an instantaneous value: 0.16–0.33 bins
  RMS at strong peaks on real sounds (vs 2e-4 on an ideal sine).
- *Δt = one sample (2 FFTs/hop, windows at t and t+1): viable for frequency.*
  Wrap-free to Nyquist. Freq agreement with Auger-Flandrin at strong peaks:
  0.008–0.016 bins RMS (≈ 0.04–0.09 Hz); significant (>−60 dB) candidate
  sets from the sign-crossing scan agree to 98.8–99.2%.
- *Adjacent-bin time surface: gate-quality only.* 2.3–3.2 samples RMS at
  strong peaks, large outliers off-peak. Fine for the cropTime gate; not
  fine for breakpoint times or phase (a tc error of a few samples at bin k
  is ~k·2π/N rad of phase error — radians at high bins).

**Recommended shape (A₂ hybrid, ~⅓ fewer FFT ops, partial parameters stay
exact):** two FFTs of h (at t and t+1) for the scan + frequency everywhere +
rough tc for gating; then exact Xt[k] (Goertzel/direct dot products of
x·hT, and Xd[k] if exact frequency is wanted too) at kept peaks only —
tens of peaks × winlen MACs ≈ a quarter of an FFT. Time and phase keep
full Auger-Flandrin precision where they end up in partials.

**A′ (future, 1 FFT):** magnitude-maxima selection (no per-bin corrections
needed for the scan) + exact per-peak DFTs for freq/time/phase. Changes
selection behavior — loses the consensus discrimination of the
reassignment-minima scan — so it needs its own end-to-end evaluation.

### B. Cosine-sum windows: exact hD from one FFT
For any cosine-sum window (Hann … 4-term Blackman-Harris), h′ is a sine sum,
so XDh is an *exact* ±m-bin linear combination of X's bins — exact frequency
reassignment from one FFT. 4-term Blackman-Harris has ≈ −92 dB sidelobes,
matching our Kaiser-at-90 dB requirement. Time reassignment still needs hT
(one extra FFT) or method A's neighbor products. Fallback if A's
finite-difference time estimates disappoint.

### C. Latency shaping (the real-time constraint that matters)
- The pipeline after the spectrum is already incremental (tracker, bandwidth
  association, and forward-only phasefix are all per-frame), so engine
  latency ≈ winlen/2 + hop. No algorithmic blockers.
- Options, in rough order of appeal: multiresolution (short window in high
  bands, long in low; separability per Sec. 5 is a per-band choice), wider
  windowWidth (320 Hz halves both window and hop → ~12 ms), asymmetric
  windows (move the center of gravity earlier; reassignment carries over
  with hT/hD defined about the new reference point).

### D. FFT engine
FFTReal is scalar. pffft (SIMD) is a drop-in behind `RealFFT` for 3–4× on
transform time. Only matters after A/C, or at high rates.

### E. Micro items (profile first)
- Peak scan: SIMD sign-change mask + movemask instead of scalar compares.
- timeCorr lazily at candidates only (~⅓ of kernel work, breaks the
  branchless pass; likely not worth it).
- Windowing multiply loops: currently auto-vectorized; fine.

### F. Already done (for the record)
- Input rotation in the time domain replaces a per-bin phase twist.
- The mixed-derivative (4th) transform is dropped; Loris computed it only to
  overwrite it.
- Two complex FFTs of packed window pairs → three real FFTs, no even/odd
  unpacking.
- atan2 only at accepted peaks (~2 calls each); nothing else needs phase.

### G. Model-quality ideas from the paper (not perf)
- Mixed-derivative consensus (eqs. 105/110) as a peak-reliability test /
  despeckler — free via cross-frame products if A is adopted; could harden
  real-time tracking.
- Second-order derivatives (eqs. 111–112) estimate local time/frequency
  spread per peak: a possible *local* bandwidth estimator, replacing the
  region-based residue association with something that needs no global
  kept/rejected bookkeeping — attractive for a streaming engine.

Suggested order after the A experiment: **C (latency) is the priority — CPU
was never the wall; A₂ is validated and shelved until transform cost
matters (high sample rates, many bands, or small hops); B remains the
fallback if exact single-FFT frequency is ever wanted; then D, G.**

## Reconstruction metrics + tuning findings (2026-07-11)

`utuMetrics` scores render vs source: spectral-shape RMS (mel bands),
**wateriness** (excess 2–30 Hz band-envelope modulation — measures the
artifact directly), transient rise deficit. `utucompare score|tune`;
convert-dir scores every batch. Metric validation: the watery polyphonic
test case scored watery 6.1 dB vs a 2.8–3.9 baseline (matches ears);
the gambang's defect showed up as transient deficit, not wateriness.

Budget-constrained tune results (start = auto params):
- "Careless Synth and Vox" (polyphonic, watery): total −24%
  (watery 6.1→4.3). Lessons: auto achieved only 23/64 actual simultaneous
  partials (probe p90 at busy sites overestimates the global statistic →
  the walk over-raised the floor); windowWidth wanted ~1.3× higher frame
  rate; freqDrift wanted 2× the measured p95 (crossing tracks censor the
  hop-delta statistic on polyphony).
- gambang (inharmonic percussive): total −10%; wanted a longer window and
  *half* the auto drift; transient deficit barely moved (attack smear
  needs a structural fix, not parameters).
- `driftTransientScale` ≈ 1.5–2 helped both sounds by the metric (small).
  `hopJitter` and `onsetSnap` measured no improvement on either — the
  frame-rate-coherence theory is not the dominant artifact here yet;
  retest after tracking improves.

**Ear verdicts (2026-07-12)**: Careless-tuned clearly better than
-converted — the tuned direction is real. gambang-tuned WORSE than
-converted: obvious crunching where mallet decays should ring. The metric
missed it — its blind spot is gating/track-fragmentation crackle in
decays (raised floor cutting tails in and out; drift lowered enough to
fragment ringing tracks). A **decay-continuity** component (dropout/
fragmentation detector on decaying segments) is needed before tune can be
trusted on percussive material.

Cleanup (2026-07-12): the experimental knobs (hopJitter, onsetSnap,
driftTransientScale + the analyzer's transient detector) and the
concluded nelson-test harness are removed — two null results and one ear
veto. The tracker's per-frame drift argument stays for the predictive-
tracking round. The transferable Careless lesson is kept as the
**persistent-peak probe calibration**: budget counts now include only
peaks matched across consecutive probe frames, since one-frame blips
become one-breakpoint partials that cleanOutliers deletes (raw probe p90
57 vs 23 achieved on the dense mix — the walk was over-raising the floor).

Next-round candidates, in order: (1) decay-continuity metric component
(prerequisite for trusting tune on percussive sounds); (2) **predictive
tracking** (per-track frequency extrapolation + small gate, plus
amp/phase continuity costs) — the remaining wateriness and both drift
corrections point straight at track instability; (3) drift estimator
robustness (censoring-aware); (4) transient handling for percussive
attacks (short-window band or onset-locked analysis).

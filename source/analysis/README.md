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

### A. Single-FFT reassignment via cross-spectral surfaces (eqs. 54–57)
Nelson's finite-difference form replaces both auxiliary transforms:
- freq: cross-spectrum between consecutive hops, C = Xₙ·conj(Xₙ₋₁),
  demodulated by the expected per-hop rotation; correction = arg/(hop).
  Valid while the deviation stays under π, i.e. |f̂ − f_bin| < windowWidth/2 —
  exactly the main-lobe half-width, so it holds across each component's
  consensus region.
- time: adjacent-bin cross-spectrum L = X[k+1]·conj(X[k]);
  correction ≈ −arg(L)·N/2π samples. Same transform, neighbor products.

Net: 3 FFTs/hop → 1, and the Xd/Xt arrays disappear. Costs: an arg() per bin
(small-angle Im/Re ratio suffices near peaks, which is where accuracy
matters — Sec. 6.1 notes FD is excellent in high-energy regions), and the
estimates become approximations. Validate by adding both estimators to
utucompare and comparing corrections at kept peaks. **Best cost/benefit; try
first.**

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

Suggested order: **A → C → (B if A's time estimates disappoint) → D → G.**


// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

// utucompare: test and measurement harness for the vutu analysis engine.
//
//   selftest     ground-truth checks on synthetic signals with known answers;
//                these pin the engine's sign conventions, scaling, and model
//                invariants without reference to any other implementation
//   auto-test    automatic analysis-parameter estimation
//   convert-dir  batch auto-analyze + resynthesize, with .utu partials output
//   ab-dir       score the current engine against golden renders of the
//                same sources (regression gate for engine rewrites)
//   score, tune  reconstruction metrics and parameter search

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "sndfile.hh"

#include "utuFFT.h"
#include "utuSpectrum.h"
#include "utuWindow.h"

#include "utuAnalyzer.h"
#include "utuAutoParams.h"
#include "utuBandwidth.h"
#include "utuMetrics.h"
#include "utuPeaks.h"
#include "utuPhaseFix.h"
#include "utuSynth.h"
#include "utuTracker.h"

namespace
{

constexpr double kPi = 3.14159265358979324;
constexpr double kTwoPi = 2. * kPi;

// sample rate for the synthetic @ test signals
constexpr double kSyntheticRate = 44100.;

// ---------------------------------------------------------------------------
// audio file I/O via libsndfile (aiff and wav, correct sample rates)

bool loadAudio(const char* path, std::vector<float>& out, double& sr)
{
  SndfileHandle f(path);
  if (f.error() || (f.frames() <= 0))
  {
    printf("could not read %s\n", path);
    return false;
  }
  sr = f.samplerate();
  const int ch = std::max(1, f.channels());
  std::vector<float> interleaved(size_t(f.frames()) * ch);
  const sf_count_t got = f.readf(interleaved.data(), f.frames());
  out.resize(size_t(std::max<sf_count_t>(0, got)));
  for (size_t i = 0; i < out.size(); ++i)
  {
    float acc = 0.f;
    for (int c = 0; c < ch; ++c) acc += interleaved[i * ch + c];
    out[i] = acc / ch;
  }
  return !out.empty();
}

bool writeAudio(const std::string& path, const float* x, size_t n, double sr, int format)
{
  SndfileHandle f(path, SFM_WRITE, format, 1, int(sr));
  if (f.error()) return false;
  return f.writef(x, sf_count_t(n)) == sf_count_t(n);
}

// ---------------------------------------------------------------------------
// selftest: ground-truth checks on synthetic signals
//
// Every test here has an answer known from first principles (an ideal
// sinusoid's frequency, an impulse's position, an energy-conservation
// identity), so the tests pin down the engine's conventions — reassignment
// signs, magnitude scaling, phase reference — rather than inheriting them
// from a reference implementation. Tolerances marked (cal) were locked by
// measuring the v2 engine and adding roughly 2x headroom; the rest are
// defensible a priori from the math and float precision.

namespace tol
{
// windows
constexpr double kWindowSym = 1e-6;      // symmetry/antisymmetry, rel to max
constexpr double kWindowSum = 1e-4;      // |Σw − 2|
// the Bessel series behind the Kaiser tables truncates at 1e-6 relative, so
// derivative cross-checks bottom out near 1e-4 of max |hD|, not machine eps
// (v2 measured: FD 5.5e-5, eq. 70 7.3e-5)
constexpr double kHdFiniteDiff = 2e-4;   // (cal) analytic hD vs 8th-order FD
constexpr double kHdEq70 = 3e-4;         // (cal) analytic hD vs eq. 70 route
// spectrum
constexpr double kImpulseTimeSamp = 0.5;  // |timeCorr − true offset|, samples
constexpr double kImpulseFreqBins = 0.1;  // |freqCorr| at an impulse, bins
// full pipeline, stationary sine (v2: err 0.0002-0.0003 Hz, amp 0.03 dB,
// dispersion 0.0003 rad; dozens of sub--40 dB junk partials near the floor
// are normal, so assertions are energy-based, never count-based)
constexpr double kSineFreqHz = 0.01;
constexpr double kSineAmpDb = 0.3;       // (cal)
constexpr double kSinePhaseDisp = 0.05;  // (cal) dispersion about the mean offset
constexpr double kSine96FreqHz = 0.02;   // (cal)
constexpr double kEnergyFrac = 0.99;     // (cal) dominant-partial energy share
// chirp (v2: RMS 0.34 Hz)
constexpr double kChirpRmsHz = 1.0;  // (cal)
// crop gate (v2: spread ~0 both sides)
constexpr double kBurstMarginSec = 0.020;  // (cal) 2.5 hops + slack at W=160
// two-sine
constexpr double kTwoSineFreqHz = 0.1;
constexpr double kSignificantDb = -40.;  // partial-energy significance cut
// noise (v2: mean bw 0.169 — kept peaks carry most noise as sinusoids; this
// pins that bandwidth association keeps working at all, not a noise model)
constexpr double kNoiseMeanBw = 0.10;     // (cal)
constexpr double kNoiseRmsDevDb = 3.0;    // (cal) render vs source RMS
constexpr double kResidueGateRel = 1e-5;  // gate identity, rel
// phase fix
constexpr double kPhaseFixRad = 0.01;
constexpr double kMaxFixPct = 0.2;  // engine default, percent
// synth (v2: null -118 dB, amp dev 0.000 dB)
constexpr double kSynthNullDb = -60.;   // (cal)
constexpr double kSynthAmpDb = 0.2;     // (cal)
// roundtrip (v2: spectral 0.88 dB, rustle 0.00, null -55.8 dB. The watery
// metric is asserted nowhere here: it normalizes by the source's envelope
// modulation, which is ~zero for a pure tone, so it explodes on this signal)
constexpr double kRoundtripSpectral = 1.5;  // (cal) dB
constexpr double kRoundtripRustle = 0.5;    // (cal) dB
constexpr double kRoundtripNullDb = -40.;   // (cal)
}  // namespace tol

// The engine's phase reference, measured on the v2 engine during M1
// calibration: breakpoint phase is the phase of the analytic signal, i.e.
// x = A·cos(phase), so a sin input measures stored − 2πft = −π/2 (exactly
// -1.5708 at both 44.1k and 96k), and a rendered partial with stored phase 0
// fits sin(ωt + π/2).
constexpr bool kPhaseOffsetPinned = true;
constexpr double kPhaseOffset = -kPi / 2.;
constexpr double kSynthPhaseRef = kPi / 2.;

std::vector<float> makeSine(double freq, float amp, double seconds, double sr)
{
  // accumulate phase in double: sinf(2π·f·i) loses all precision once the
  // argument grows past ~1e7
  std::vector<float> x(size_t(seconds * sr));
  double phase = 0.;
  const double inc = kTwoPi * freq / sr;
  for (auto& s : x)
  {
    s = amp * float(sin(phase));
    phase += inc;
  }
  return x;
}

// analyze with explicit window-first parameters and the production
// post-processing (cleanOutliers, stats)
std::unique_ptr<ml::VutuPartialsData> analyzeSimple(const std::vector<float>& x, double sr,
                                                    float windowWidth,
                                                    float noiseWidth = 2000.f)
{
  ml::utu::AnalyzerParams p;
  p.sampleRate = float(sr);
  p.windowWidth = windowWidth;
  p.resolution = windowWidth * 0.5f;
  p.noiseWidth = noiseWidth;
  auto partials = ml::utu::analyzeToPartials(x.data(), x.size(), p);
  ml::cleanOutliers(*partials);
  if (!partials->partials.empty()) ml::calcStats(*partials);
  return partials;
}

double partialEnergy(const ml::VutuPartial& p)
{
  double e = 0.;
  for (float a : p.amp) e += double(a) * a;
  return e;
}

size_t dominantPartial(const ml::VutuPartialsData& pd)
{
  size_t best = 0;
  double bestEnergy = -1.;
  for (size_t i = 0; i < pd.partials.size(); ++i)
  {
    const double e = partialEnergy(pd.partials[i]);
    if (e > bestEnergy)
    {
      bestEnergy = e;
      best = i;
    }
  }
  return best;
}

// energy share of the n loudest partials — the robust way to say "this
// signal is one (or two) components": junk partials near the amplitude floor
// are numerous but energetically nothing
double dominantEnergyFraction(const ml::VutuPartialsData& pd, int n)
{
  std::vector<double> e;
  double total = 0.;
  for (const auto& P : pd.partials)
  {
    e.push_back(partialEnergy(P));
    total += e.back();
  }
  std::sort(e.begin(), e.end(), std::greater<double>());
  double top = 0.;
  for (int i = 0; i < n && i < int(e.size()); ++i) top += e[i];
  return total > 0. ? top / total : 0.;
}

// partials within kSignificantDb of the loudest, by energy
std::vector<size_t> significantPartials(const ml::VutuPartialsData& pd)
{
  std::vector<double> e;
  double maxE = 0.;
  for (const auto& P : pd.partials)
  {
    e.push_back(partialEnergy(P));
    maxE = std::max(maxE, e.back());
  }
  const double cut = maxE * pow(10., tol::kSignificantDb / 10.);
  std::vector<size_t> idx;
  for (size_t i = 0; i < e.size(); ++i)
    if (e[i] >= cut) idx.push_back(i);
  return idx;
}

// maximum number of significant partials sounding at once (overlaps shorter
// than minOverlapSec — track handoffs at a beat null — don't count)
size_t maxSimultaneousSignificant(const ml::VutuPartialsData& pd,
                                  const std::vector<size_t>& sig, double minOverlapSec)
{
  size_t worst = 0;
  for (size_t a : sig)
  {
    const auto& A = pd.partials[a];
    size_t overlapping = 1;
    for (size_t b : sig)
    {
      if (a == b) continue;
      const auto& B = pd.partials[b];
      const double lo = std::max(double(A.time.front()), double(B.time.front()));
      const double hi = std::min(double(A.time.back()), double(B.time.back()));
      if (hi - lo > minOverlapSec) ++overlapping;
    }
    worst = std::max(worst, overlapping);
  }
  return worst;
}

// least-squares fit of a·cos + b·sin at freq over [n0, n1); returns fitted
// amplitude and phase (x ≈ amp·sin(ωn + phase)), and the residual RMS
void fitSinusoid(const std::vector<float>& x, size_t n0, size_t n1, double freq, double sr,
                 double& ampOut, double& phaseOut, double& residRmsOut, double& signalRmsOut)
{
  const double w = kTwoPi * freq / sr;
  double sc = 0., ss = 0.;
  for (size_t n = n0; n < n1; ++n)
  {
    sc += x[n] * cos(w * double(n));
    ss += x[n] * sin(w * double(n));
  }
  const double count = double(n1 - n0);
  const double a = 2. * sc / count;  // cos coefficient
  const double b = 2. * ss / count;  // sin coefficient
  double resid = 0., sig = 0.;
  for (size_t n = n0; n < n1; ++n)
  {
    const double fit = a * cos(w * double(n)) + b * sin(w * double(n));
    const double d = x[n] - fit;
    resid += d * d;
    sig += double(x[n]) * x[n];
  }
  ampOut = sqrt(a * a + b * b);
  // x = amp·(sin(ωn)·b/amp + cos(ωn)·a/amp) = amp·sin(ωn + atan2(a, b))
  phaseOut = atan2(a, b);
  residRmsOut = sqrt(resid / count);
  signalRmsOut = sqrt(sig / count);
}

// --- window tests ----------------------------------------------------------

bool testWindowShape()
{
  const auto wins = ml::utu::buildReassignmentWindows(44100., 160., 90.);
  const long L = wins.length;
  bool ok = true;
  if (!(L & 1) || long(wins.w.size()) != L || long(wins.wFreqRamp.size()) != L ||
      long(wins.wTimeRamp.size()) != L)
  {
    printf("  bad lengths: L %ld sizes %zu %zu %zu\n", L, wins.w.size(), wins.wFreqRamp.size(),
           wins.wTimeRamp.size());
    return false;
  }
  double maxW = 0., maxF = 0., maxT = 0.;
  for (long k = 0; k < L; ++k)
  {
    maxW = std::max(maxW, fabs(double(wins.w[k])));
    maxF = std::max(maxF, fabs(double(wins.wFreqRamp[k])));
    maxT = std::max(maxT, fabs(double(wins.wTimeRamp[k])));
  }
  double symW = 0., asymF = 0., asymT = 0., ramp = 0., sum = 0.;
  const double c = 0.5 * (L - 1);
  for (long k = 0; k < L; ++k)
  {
    symW = std::max(symW, fabs(double(wins.w[k]) - wins.w[L - 1 - k]) / maxW);
    asymF = std::max(asymF, fabs(double(wins.wFreqRamp[k]) + wins.wFreqRamp[L - 1 - k]) / maxF);
    asymT = std::max(asymT, fabs(double(wins.wTimeRamp[k]) + wins.wTimeRamp[L - 1 - k]) / maxT);
    ramp = std::max(ramp, fabs(double(wins.wTimeRamp[k]) - wins.w[k] * (k - c)) / maxT);
    sum += wins.w[k];
  }
  printf("  L %ld, sym(w) %.2e, asym(hD) %.2e, asym(hT) %.2e, ramp %.2e, Σw %.6f\n", L, symW,
         asymF, asymT, ramp, sum);
  ok &= symW < tol::kWindowSym;
  ok &= asymF < tol::kWindowSym;
  ok &= asymT < tol::kWindowSym;
  ok &= ramp < tol::kWindowSym;
  ok &= fabs(sum - 2.) < tol::kWindowSum;
  return ok;
}

bool testWindowHd()
{
  // W = 320 keeps the naive-DFT eq. 70 cross-check affordable
  const double shape = ml::utu::kaiser::computeShape(90.);
  const long len = ml::utu::kaiser::computeLength(320. / 44100., shape);
  std::vector<double> h(len), hd(len);
  ml::utu::kaiser::buildWindow(h, shape);
  ml::utu::kaiser::buildTimeDerivativeWindow(hd, shape);
  double maxHd = 0.;
  for (double v : hd) maxHd = std::max(maxHd, fabs(v));

  // (a) analytic derivative vs 8th-order central finite difference, interior
  static const double fd[4] = {4. / 5., -1. / 5., 4. / 105., -1. / 280.};
  double fdErr = 0.;
  for (long k = 4; k < len - 4; ++k)
  {
    double d = 0.;
    for (int m = 1; m <= 4; ++m) d += fd[m - 1] * (h[k + m] - h[k - m]);
    fdErr = std::max(fdErr, fabs(d - hd[k]) / maxHd);
  }

  // (b) the paper's eq. 70 construction: hD = IDFT{ jω_k · DFT(h) }, real part
  long N = 1;
  while (N < 2 * len) N <<= 1;
  std::vector<double> Hre(N, 0.), Him(N, 0.);
  for (long k = 0; k < N; ++k)
  {
    double re = 0., im = 0.;
    for (long n = 0; n < len; ++n)
    {
      const double a = kTwoPi * k * n / N;
      re += h[n] * cos(a);
      im -= h[n] * sin(a);
    }
    Hre[k] = re;
    Him[k] = im;
  }
  // multiply by jω with signed frequency; Nyquist term zeroed for symmetry
  std::vector<double> Gre(N), Gim(N);
  for (long k = 0; k < N; ++k)
  {
    double w = kTwoPi * ((k < N / 2) ? k : (k == N / 2 ? 0 : k - N)) / N;
    Gre[k] = -w * Him[k];
    Gim[k] = w * Hre[k];
  }
  double eqErr = 0.;
  const long lo = len / 10, hi = len - len / 10;
  for (long n = lo; n < hi; ++n)
  {
    double re = 0.;
    for (long k = 0; k < N; ++k)
    {
      const double a = kTwoPi * k * n / N;
      re += Gre[k] * cos(a) - Gim[k] * sin(a);
    }
    eqErr = std::max(eqErr, fabs(re / N - hd[n]) / maxHd);
  }
  printf("  len %ld, FD err %.2e, eq.70 err (interior) %.2e (rel to max |hD|)\n", len, fdErr,
         eqErr);
  return (fdErr < tol::kHdFiniteDiff) && (eqErr < tol::kHdEq70);
}

// --- spectrum tests --------------------------------------------------------

bool testImpulseTime()
{
  const double sr = 44100.;
  auto wins = ml::utu::buildReassignmentWindows(sr, 160., 90.);
  ml::utu::ReassignedSpectrum spec;
  spec.configure(wins);
  const long L = spec.windowLength();
  const long bins = spec.bins();
  std::vector<float> x(3 * L, 0.f);
  const long s0 = 3 * L / 2;
  x[s0] = 1.f;

  const long offsets[] = {-137, -41, 0, 29, 137};  // within ±hop/2 at W=160
  bool ok = true;
  double worstTime = 0., worstFreq = 0.;
  for (long d : offsets)
  {
    spec.transform(x.data(), long(x.size()), s0 + d);
    const auto& f = spec.frame();
    float maxMag = 0.f;
    for (long k = 0; k < bins; ++k) maxMag = std::max(maxMag, f.magSq[k]);
    // an impulse has equal energy in every bin; expected t̂ − t = s0 − center
    const double expected = double(-d);
    for (long k = 16; k < bins - 16; ++k)
    {
      if (f.magSq[k] < 1e-6f * maxMag) continue;
      worstTime = std::max(worstTime, fabs(f.timeCorr[k] - expected));
      worstFreq = std::max(worstFreq, fabs(double(f.freqCorr[k])));
    }
  }
  printf("  worst |timeCorr err| %.4f samples, worst |freqCorr| %.4f bins\n", worstTime,
         worstFreq);
  ok &= worstTime < tol::kImpulseTimeSamp;
  ok &= worstFreq < tol::kImpulseFreqBins;
  return ok;
}

// --- full-pipeline tests ---------------------------------------------------

bool sineChecks(double sr, double f0, float amp, double seconds, double freqTolHz,
                bool checkPhase)
{
  auto x = makeSine(f0, amp, seconds, sr);
  auto pd = analyzeSimple(x, sr, 160.f);
  if (pd->partials.empty())
  {
    printf("  no partials\n");
    return false;
  }
  const double frac = dominantEnergyFraction(*pd, 1);
  bool ok = frac > tol::kEnergyFrac;
  const auto& P = pd->partials[dominantPartial(*pd)];
  const double t0 = 0.15 * seconds, t1 = 0.85 * seconds;
  double maxFreqErr = 0., maxAmpDevDb = 0.;
  std::vector<double> phaseDiffs;
  for (size_t i = 0; i < P.time.size(); ++i)
  {
    const double t = P.time[i];
    if (t < t0 || t > t1) continue;
    maxFreqErr = std::max(maxFreqErr, fabs(double(P.freq[i]) - f0));
    maxAmpDevDb = std::max(maxAmpDevDb, fabs(20. * log10(double(P.amp[i]) / amp)));
    phaseDiffs.push_back(ml::utu::wrapPi(double(P.phase[i]) - kTwoPi * f0 * t));
  }
  if (phaseDiffs.empty())
  {
    printf("  no interior breakpoints\n");
    return false;
  }
  double ss = 0., cc = 0.;
  for (double d : phaseDiffs)
  {
    ss += sin(d);
    cc += cos(d);
  }
  const double meanOff = atan2(ss, cc);
  double disp = 0.;
  for (double d : phaseDiffs) disp = std::max(disp, fabs(ml::utu::wrapPi(d - meanOff)));
  printf(
      "  %zu partials (dominant energy %.5f), freq err %.4f Hz, amp dev %.3f dB,\n"
      "  phase offset %.4f rad (dispersion %.4f) over %zu breakpoints\n",
      pd->partials.size(), frac, maxFreqErr, maxAmpDevDb, meanOff, disp, phaseDiffs.size());
  ok &= maxFreqErr < freqTolHz;
  ok &= maxAmpDevDb < tol::kSineAmpDb;
  if (checkPhase)
  {
    ok &= disp < tol::kSinePhaseDisp;
    if (kPhaseOffsetPinned)
      ok &= fabs(ml::utu::wrapPi(meanOff - kPhaseOffset)) < 0.1;
  }
  return ok;
}

bool testSine() { return sineChecks(44100., 441.3, 0.5f, 3.0, tol::kSineFreqHz, true); }
bool testSine96() { return sineChecks(96000., 441.3, 0.5f, 2.0, tol::kSine96FreqHz, false); }

bool testChirp()
{
  const double sr = 44100.;
  const double fStart = 1000., rate = 1000.;  // Hz, Hz/s
  const double seconds = 2.0;
  std::vector<float> x(size_t(seconds * sr));
  double phase = 0.;
  for (size_t n = 0; n < x.size(); ++n)
  {
    const double t = n / sr;
    x[n] = 0.5f * float(sin(phase));
    phase += kTwoPi * (fStart + rate * t) / sr;
  }
  auto pd = analyzeSimple(x, sr, 160.f);
  if (pd->partials.empty())
  {
    printf("  no partials\n");
    return false;
  }
  const auto& P = pd->partials[dominantPartial(*pd)];
  double sumSq = 0.;
  long n = 0;
  double maxErr = 0.;
  for (size_t i = 0; i < P.time.size(); ++i)
  {
    const double t = P.time[i];
    if (t < 0.3 || t > 1.7) continue;
    // instantaneous frequency at the breakpoint's reassigned time — this
    // couples the frequency and time reassignment signs
    const double fTrue = fStart + rate * t;
    const double e = double(P.freq[i]) - fTrue;
    sumSq += e * e;
    maxErr = std::max(maxErr, fabs(e));
    ++n;
  }
  if (!n)
  {
    printf("  no interior breakpoints\n");
    return false;
  }
  const double rms = sqrt(sumSq / n);
  const double frac = dominantEnergyFraction(*pd, 1);
  printf("  %zu partials (dominant energy %.5f), freq err RMS %.3f Hz, max %.3f Hz over %ld bps\n",
         pd->partials.size(), frac, rms, maxErr, n);
  return (frac > tol::kEnergyFrac) && (rms < tol::kChirpRmsHz);
}

bool testCropBurst()
{
  // a short tone burst must yield breakpoints only near the burst: frames
  // centered farther away see the energy only through large time
  // reassignment, which the crop gate rejects (Sec. 7 pruning). Without the
  // gate, smear would extend to half a window length (~25 ms at W=160).
  const double sr = 44100.;
  const double hopSec = 1. / 160.;
  const double burstStart = 1.0, burstLen = 4. * hopSec;
  std::vector<float> x(size_t(2.0 * sr), 0.f);
  double phase = 0.;
  const double inc = kTwoPi * 800. / sr;
  for (size_t n = size_t(burstStart * sr); n < size_t((burstStart + burstLen) * sr); ++n)
  {
    x[n] = 0.5f * float(sin(phase));
    phase += inc;
  }
  auto pd = analyzeSimple(x, sr, 160.f);
  if (pd->partials.empty())
  {
    printf("  no partials — burst not detected\n");
    return false;
  }
  double minT = 1e9, maxT = -1e9;
  for (const auto& P : pd->partials)
    for (float t : P.time)
    {
      minT = std::min(minT, double(t));
      maxT = std::max(maxT, double(t));
    }
  const double before = burstStart - minT;
  const double after = maxT - (burstStart + burstLen);
  printf("  %zu partials, spread %.1f ms before, %.1f ms after (margin %.1f ms)\n",
         pd->partials.size(), before * 1000., after * 1000., tol::kBurstMarginSec * 1000.);
  return (before < tol::kBurstMarginSec) && (after < tol::kBurstMarginSec);
}

bool testTwoSine()
{
  // unequal amplitudes so the merged pair's beat envelope never touches the
  // amplitude floor (equal amps null completely and fragment any tracker)
  const double sr = 44100.;
  const double W = 160.;
  const double hopSec = 1. / W;
  bool ok = true;

  // resolved: spacing 1.5·W — two simultaneous partials at the true
  // frequencies (separability, Sec. 5)
  {
    auto x = makeSine(440., 0.3f, 2.0, sr);
    auto y = makeSine(440. + 1.5 * W, 0.15f, 2.0, sr);
    for (size_t i = 0; i < x.size(); ++i) x[i] += y[i];
    auto pd = analyzeSimple(x, sr, float(W));
    const auto sig = significantPartials(*pd);
    const size_t simul = maxSimultaneousSignificant(*pd, sig, 2. * hopSec);
    if (sig.size() != 2 || simul != 2)
    {
      printf("  resolved: %zu significant (%zu simultaneous), want 2\n", sig.size(), simul);
      ok = false;
    }
    else
    {
      double f[2];
      for (int j = 0; j < 2; ++j)
      {
        const auto& P = pd->partials[sig[j]];
        double sum = 0.;
        long n = 0;
        for (size_t i = 0; i < P.time.size(); ++i)
        {
          if (P.time[i] < 0.3 || P.time[i] > 1.7) continue;
          sum += P.freq[i];
          ++n;
        }
        f[j] = n ? sum / n : 0.;
      }
      if (f[0] > f[1]) std::swap(f[0], f[1]);
      printf("  resolved: %.3f and %.3f Hz (true 440, %g)\n", f[0], f[1], 440. + 1.5 * W);
      ok &= fabs(f[0] - 440.) < tol::kTwoSineFreqHz;
      ok &= fabs(f[1] - (440. + 1.5 * W)) < tol::kTwoSineFreqHz;
    }
  }

  // merged: spacing 0.25·W — one beating component, never two simultaneous
  // (the merged fate of a sub-resolution pair; the beat is an amplitude
  // envelope on a single track)
  {
    auto x = makeSine(440., 0.3f, 2.0, sr);
    auto y = makeSine(440. + 0.25 * W, 0.15f, 2.0, sr);
    for (size_t i = 0; i < x.size(); ++i) x[i] += y[i];
    auto pd = analyzeSimple(x, sr, float(W));
    const auto sig = significantPartials(*pd);
    const size_t simul = maxSimultaneousSignificant(*pd, sig, 2. * hopSec);
    printf("  merged: %zu partials, %zu significant, %zu simultaneous\n",
           pd->partials.size(), sig.size(), simul);
    ok &= (simul == 1);
  }
  return ok;
}

bool testSilence()
{
  const double sr = 44100.;
  bool ok = true;
  {
    std::vector<float> x(size_t(sr), 0.f);
    auto pd = analyzeSimple(x, sr, 160.f);
    printf("  zeros: %zu partials\n", pd->partials.size());
    ok &= pd->partials.empty();
  }
  {
    std::vector<float> x(size_t(sr), 0.25f);  // DC sits below freqFloor
    auto pd = analyzeSimple(x, sr, 160.f);
    printf("  DC: %zu partials\n", pd->partials.size());
    ok &= pd->partials.empty();
  }
  return ok;
}

bool testNoiseBw()
{
  const double sr = 44100.;
  std::mt19937 gen(11);
  std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
  std::vector<float> x(size_t(2. * sr));
  for (auto& v : x) v = dist(gen);
  auto pd = analyzeSimple(x, sr, 160.f);
  if (pd->partials.empty())
  {
    printf("  no partials\n");
    return false;
  }
  double num = 0., den = 0.;
  for (const auto& P : pd->partials)
    for (size_t i = 0; i < P.amp.size(); ++i)
    {
      const double e = double(P.amp[i]) * P.amp[i];
      num += e * P.bandwidth[i];
      den += e;
    }
  const double meanBw = den > 0. ? num / den : 0.;

  // energy conservation through the whole model: noise in, noise out
  ml::utu::SynthParams sp;
  sp.sampleRate = float(sr);
  sp.fadeTime = 0.001f;
  ml::utu::PartialSynthesizer synth;
  synth.setParams(sp);
  std::vector<float> out;
  synth.render(*pd, out);
  const size_t n = std::min(x.size(), out.size());
  double srcE = 0., outE = 0.;
  for (size_t i = size_t(0.2 * sr); i < n; ++i)
  {
    srcE += double(x[i]) * x[i];
    outE += double(out[i]) * out[i];
  }
  const double rmsDevDb = 10. * log10(std::max(1e-12, outE / std::max(1e-12, srcE)));
  printf("  %zu partials, energy-weighted mean bw %.3f, render RMS dev %+.2f dB\n",
         pd->partials.size(), meanBw, rmsDevDb);
  return (meanBw > tol::kNoiseMeanBw) && (fabs(rmsDevDb) < tol::kNoiseRmsDevDb);
}

bool testResidueGate()
{
  // gate semantics: with the residue time gate at g, association must equal
  // an ungated association run on only the residue peaks within ±g. Also
  // checks the energy-preserving update: bw·amp'² recovers the collected
  // noise energy exactly, and amp only grows.
  const float sr = 44100.f;
  const float regionWidth = 500.f;
  const float hopSec = 1.f / 160.f;
  const float gate = 0.5f * hopSec;

  auto makeFrame = [](std::initializer_list<ml::utu::Peak> residue)
  {
    ml::utu::PeakFrame f;
    ml::utu::Peak kept;
    kept.freq = 3000.f;
    kept.amp = 0.3f;
    f.peaks.push_back(kept);
    for (const auto& r : residue) f.peaks.push_back(r);
    f.numKept = 1;
    return f;
  };
  auto peak = [](float freq, float amp, float off)
  {
    ml::utu::Peak p;
    p.freq = freq;
    p.amp = amp;
    p.timeOffset = off;
    return p;
  };
  const ml::utu::Peak inside[] = {peak(3050.f, 0.10f, 0.f), peak(3120.f, 0.12f, 0.2f * hopSec),
                                  peak(3200.f, 0.08f, -0.45f * hopSec)};
  const ml::utu::Peak outside[] = {peak(3260.f, 0.20f, 0.8f * hopSec),
                                   peak(2950.f, 0.15f, -0.7f * hopSec)};

  auto associate = [&](ml::utu::PeakFrame frame, float gateSec)
  {
    ml::utu::AssociateBandwidth bw;
    bw.configure(regionWidth, sr, gateSec);
    bw.associateBandwidth(frame);
    const auto& k = frame.peaks[0];
    return double(k.bw) * k.amp * k.amp;  // collected noise energy
  };

  const double eAll = associate(makeFrame({inside[0], inside[1], inside[2], outside[0], outside[1]}), 0.f);
  const double eInside = associate(makeFrame({inside[0], inside[1], inside[2]}), 0.f);
  const double eOutside = associate(makeFrame({outside[0], outside[1]}), 0.f);
  const double eGated = associate(makeFrame({inside[0], inside[1], inside[2], outside[0], outside[1]}), gate);

  // amp growth check on one association
  auto frame = makeFrame({inside[0], inside[1], inside[2]});
  ml::utu::AssociateBandwidth bw;
  bw.configure(regionWidth, sr, 0.f);
  bw.associateBandwidth(frame);
  const bool ampGrew = frame.peaks[0].amp >= 0.3f;
  const bool bwInRange = frame.peaks[0].bw >= 0.f && frame.peaks[0].bw <= 1.f;

  const double additivity = fabs(eAll - (eInside + eOutside)) / std::max(1e-12, eAll);
  const double gateIdentity = fabs(eGated - eInside) / std::max(1e-12, eInside);
  printf("  enoise all %.3e = in %.3e + out %.3e (dev %.1e); gated %.3e (dev %.1e)\n", eAll,
         eInside, eOutside, additivity, eGated, gateIdentity);
  printf("  kept amp %.4f (grew %d), bw %.3f\n", frame.peaks[0].amp, int(ampGrew),
         frame.peaks[0].bw);
  return (eInside > 0.) && (additivity < tol::kResidueGateRel) &&
         (gateIdentity < tol::kResidueGateRel) && ampGrew && bwInRange;
}

bool testPhaseFix()
{
  const double sr = 44100.;
  const long hop = 275;
  const double f0 = 440.;
  std::mt19937 gen(3);
  std::uniform_real_distribution<float> jitter(-0.5f, 0.5f);

  ml::utu::BuildingPartial p;
  std::vector<float> origFreq;
  for (int i = 0; i < 40; ++i)
  {
    const double t = double(i) * hop / sr;
    p.frameSample.push_back(int64_t(i) * hop);
    p.timeOffset.push_back(0.f);
    const float f = float(f0) + jitter(gen);
    p.freq.push_back(f);
    origFreq.push_back(f);
    p.amp.push_back(0.3f);
    p.bw.push_back(0.f);
    p.phase.push_back(float(ml::utu::wrapPi(kTwoPi * f0 * t)));
  }
  ml::utu::fixFrequency(p, sr);

  double maxPhaseErr = 0., maxFixPct = 0.;
  for (size_t i = 1; i < p.size(); ++i)
  {
    const double dt = p.dt(i - 1, i, sr);
    const double travel = kPi * (double(p.freq[i - 1]) + p.freq[i]) * dt;
    maxPhaseErr = std::max(
        maxPhaseErr, fabs(ml::utu::wrapPi(double(p.phase[i]) - p.phase[i - 1] - travel)));
  }
  for (size_t i = 0; i < p.size(); ++i)
  {
    maxFixPct = std::max(maxFixPct, 100. * fabs(double(p.freq[i]) / origFreq[i] - 1.));
  }
  printf("  phase-travel err %.5f rad, max freq fix %.4f%% (clamp %.1f%%)\n", maxPhaseErr,
         maxFixPct, tol::kMaxFixPct);
  return (maxPhaseErr < tol::kPhaseFixRad) && (maxFixPct <= tol::kMaxFixPct + 1e-3);
}

bool testSynthNull()
{
  const double sr = 44100.;
  const double f0 = 440.;
  const float A = 0.3f;
  ml::VutuPartialsData pd;
  pd.partials.resize(1);
  auto& P = pd.partials[0];
  for (int i = 0; i <= 100; ++i)
  {
    const double t = i * 0.01;
    P.time.push_back(float(t));
    P.amp.push_back(A);
    P.freq.push_back(float(f0));
    P.bandwidth.push_back(0.f);
    P.phase.push_back(float(ml::utu::wrapPi(kTwoPi * f0 * t)));
  }
  ml::calcStats(pd);

  ml::utu::SynthParams sp;
  sp.sampleRate = float(sr);
  sp.fadeTime = 0.001f;
  ml::utu::PartialSynthesizer synth;
  synth.setParams(sp);
  std::vector<float> out;
  synth.render(pd, out);
  if (out.size() < size_t(sr))
  {
    printf("  render too short: %zu samples\n", out.size());
    return false;
  }
  double amp, phase, residRms, sigRms;
  fitSinusoid(out, size_t(0.1 * sr), size_t(0.9 * sr), f0, sr, amp, phase, residRms, sigRms);
  const double nullDb = 20. * log10(std::max(1e-12, residRms / sigRms));
  const double ampDevDb = fabs(20. * log10(amp / A));
  // fitted phase vs the stored breakpoint phase measures the synth's phase
  // reference relative to sin(2π f t); must agree with the analyzer's
  const double phaseRef = ml::utu::wrapPi(phase);
  printf("  null %.1f dB, amp dev %.3f dB, render phase ref %.4f rad\n", nullDb, ampDevDb,
         phaseRef);
  bool ok = (nullDb < tol::kSynthNullDb) && (ampDevDb < tol::kSynthAmpDb);
  if (kPhaseOffsetPinned) ok &= fabs(ml::utu::wrapPi(phaseRef - kSynthPhaseRef)) < 0.1;
  return ok;
}

bool testRoundtrip()
{
  const double sr = 44100.;
  const double f0 = 440.1;
  auto x = makeSine(f0, 0.5f, 5.0, sr);
  auto pd = analyzeSimple(x, sr, 160.f);
  if (pd->partials.empty())
  {
    printf("  no partials\n");
    return false;
  }
  ml::utu::SynthParams sp;
  sp.sampleRate = float(sr);
  sp.fadeTime = 0.001f;
  ml::utu::PartialSynthesizer synth;
  synth.setParams(sp);
  std::vector<float> out;
  synth.render(*pd, out);
  const size_t n = std::min(x.size(), out.size());
  if (!n)
  {
    printf("  empty render\n");
    return false;
  }
  auto s = ml::utu::scoreReconstruction(x.data(), x.size(), out.data(), out.size(), sr);

  // time-domain null over the steady middle: phase-correct rendering should
  // reproduce the waveform itself, not just its spectrum
  double resid = 0., sig = 0.;
  for (size_t i = size_t(0.5 * sr); i < std::min(n, size_t(4.5 * sr)); ++i)
  {
    const double d = double(out[i]) - x[i];
    resid += d * d;
    sig += double(x[i]) * x[i];
  }
  const double nullDb = 20. * log10(std::max(1e-12, sqrt(resid / std::max(1., sig))));
  // watery is printed but not asserted: see the tolerance table note
  printf("  spectral %.2f dB, watery %.2f dB, rustle %.2f dB, null %.1f dB\n", s.spectralRmsDb,
         s.wateryDb, s.rustleDb, nullDb);
  return (s.spectralRmsDb < tol::kRoundtripSpectral) && (s.rustleDb < tol::kRoundtripRustle) &&
         (nullDb < tol::kRoundtripNullDb);
}

struct SelfTestEntry
{
  const char* name;
  bool (*fn)();
};

int selfTest(const char* filter)
{
  static const SelfTestEntry tests[] = {
      {"window-shape", testWindowShape},
      {"window-hd", testWindowHd},
      {"impulse-time", testImpulseTime},
      {"sine", testSine},
      {"sine96", testSine96},
      {"chirp", testChirp},
      {"crop-burst", testCropBurst},
      {"two-sine", testTwoSine},
      {"silence", testSilence},
      {"noise-bw", testNoiseBw},
      {"residue-gate", testResidueGate},
      {"phasefix", testPhaseFix},
      {"synth-null", testSynthNull},
      {"roundtrip", testRoundtrip},
  };
  int failures = 0, ran = 0;
  for (const auto& t : tests)
  {
    if (filter && !strstr(t.name, filter)) continue;
    printf("[%s]\n", t.name);
    ++ran;
    const bool ok = t.fn();
    printf("  %s\n", ok ? "PASS" : "FAIL");
    failures += !ok;
  }
  if (!ran)
  {
    printf("no tests match '%s'\n", filter ? filter : "");
    return 2;
  }
  printf("selftest: %d/%d passed\n", ran - failures, ran);
  return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// auto-test: automatic analysis-parameter estimation

// synthetic test signals exercising the classifier branches
std::vector<float> makeTestSignal(const std::string& name, double sr)
{
  std::vector<float> x;
  std::mt19937 gen(7);
  std::uniform_real_distribution<float> dist(-1.f, 1.f);

  if (name == "@sine")
  {
    x = makeSine(440.1, 0.5f, 5., sr);
  }
  else if (name == "@harm")
  {
    // 220 Hz, 20 harmonics at 1/k, 5 Hz vibrato of ±10 Hz on the
    // fundamental (scaling with harmonic number), exponential decay
    x.assign(size_t(4 * sr), 0.f);
    std::vector<double> phase(21, 0.);
    for (size_t i = 0; i < x.size(); ++i)
    {
      const double t = i / sr;
      const double f0 = 220. + 10. * sin(2. * kPi * 5. * t);
      const double env = exp(-t / 1.5);
      double v = 0.;
      for (int k = 1; k <= 20; ++k)
      {
        phase[k] += 2. * kPi * k * f0 / sr;
        v += (1. / k) * sin(phase[k]);
      }
      x[i] = float(0.2 * env * v);
    }
  }
  else if (name == "@bell")
  {
    const double freqs[6] = {220., 563., 921., 1372., 1898., 2510.};
    const double amps[6] = {1., .7, .5, .4, .3, .25};
    const double taus[6] = {3., 2.2, 1.6, 1.2, 0.9, 0.7};
    x.assign(size_t(6 * sr), 0.f);
    for (size_t i = 0; i < x.size(); ++i)
    {
      const double t = i / sr;
      double v = 0.;
      for (int k = 0; k < 6; ++k)
      {
        v += amps[k] * exp(-t / taus[k]) * sin(2. * kPi * freqs[k] * t);
      }
      x[i] = float(0.25 * v);
    }
  }
  else if (name == "@noise")
  {
    // lowpassed noise burst: 10 ms attack, 1 s decay
    x.assign(size_t(2 * sr), 0.f);
    float lp = 0.f;
    const float a = float(1. - exp(-2. * kPi * 4000. / sr));
    for (size_t i = 0; i < x.size(); ++i)
    {
      const double t = i / sr;
      const double env = std::min(t / 0.01, 1.) * exp(-t / 1.);
      lp += a * (dist(gen) - lp);
      x[i] = float(0.7 * env * lp);
    }
  }
  return x;
}

void printAutoParams(FILE* f, const ml::utu::AutoAnalyzerParams& r)
{
  fprintf(f, "  resolution   %8.1f Hz      windowWidth %8.1f Hz (frame rate %.1f Hz)\n",
          r.params.resolution, r.params.windowWidth, r.frameRateHz);
  fprintf(f, "  ampFloor     %8.1f dB      freqDrift   %8.1f Hz\n", r.params.ampFloor,
          r.params.freqDrift);
  fprintf(f, "  loCut        %8.1f Hz      hiCut       %8.1f Hz\n", r.params.freqFloor,
          r.hiCut);
  fprintf(f, "  noiseWidth   %8.1f Hz      sidelobe    %8.1f dB\n", r.params.noiseWidth,
          r.params.sidelobeLevel);
  fprintf(f, "  fundamental  %8.1f Hz      confidence  %8.2f\n", r.fundamental,
          r.pitchConfidence);
  fprintf(f, "  spacing dom  %8.1f Hz      min spacing %8.1f Hz\n", r.spacingHz,
          r.minSpacingHz);
  fprintf(f, "  noise floor  %8.1f dB      active dur  %8.2f s\n", r.noiseFloorDb,
          r.activeDuration);
  fprintf(f, "  regime: %s (beat index %.1f dB, fraction %.2f, rate %.1f Hz, merge W %.1f Hz)\n",
          r.dense ? "dense" : "sparse", r.beatIndexDb, r.beatFraction, r.beatRateHz,
          r.mergeWindowHz);
  fprintf(f, "  budget use   %d/%d (p90, %s)\n", r.probedSimultaneousP90, r.budget,
          r.budgetLimited ? "budget-limited" : "ladders exhausted");
}

int autoTest(const char* path)
{
  double sr = kSyntheticRate;
  std::vector<float> samplesF;
  if (std::string(path) == "@short")
  {
    // 0.3 s: exercises the short-file guards (Welch size, window floor)
    samplesF = makeSine(330., 0.5f, 0.3, sr);
  }
  else if (std::string(path) == "@sine96")
  {
    sr = 96000.;
    samplesF = makeSine(440., 0.5f, 3., sr);
  }
  else
  {
    samplesF = makeTestSignal(path, sr);
  }
  if (samplesF.empty())
  {
    if (!loadAudio(path, samplesF, sr)) return 2;
  }
  printf("%s: %zu samples at %g Hz\n", path, samplesF.size(), sr);

  const int budget = 64;
  auto r = ml::utu::computeAnalyzerParams(samplesF.data(), samplesF.size(), float(sr), budget);
  printAutoParams(stdout, r);

  // range invariants
  bool pass = true;
  auto inRange = [&](float v, float lo, float hi, const char* what)
  {
    if (v < lo || v > hi)
    {
      printf("  RANGE VIOLATION: %s = %g not in [%g, %g]\n", what, v, lo, hi);
      pass = false;
    }
  };
  inRange(r.params.resolution, 8, 1024, "resolution");
  inRange(r.params.windowWidth, 16, 768, "windowWidth");
  inRange(r.params.ampFloor, -90, -20, "ampFloor");
  inRange(r.params.freqDrift, 2, 80, "freqDrift");
  inRange(r.params.freqFloor, 20, 2000, "loCut");
  inRange(r.hiCut, 200, 20000, "hiCut");
  inRange(r.params.noiseWidth, 10, 5000, "noiseWidth");

  // the walk must land at or under the target, and must terminate for one
  // of the two legitimate reasons
  if (r.probedSimultaneousP90 > budget - 8)
  {
    printf("  WALK OVERSHOT: p90 %d > target %d\n", r.probedSimultaneousP90, budget - 8);
    pass = false;
  }

  // full analysis at the chosen params: achieved simultaneous partials
  auto partials = ml::utu::analyzeToPartials(samplesF.data(), samplesF.size(), r.params);
  ml::cutHighs(*partials, r.hiCut);
  ml::cleanOutliers(*partials);
  if (partials->partials.size() > 0)
  {
    ml::calcStats(*partials);
    printf("  full analysis: %zu partials, max simultaneous %zu (budget %d)\n",
           partials->partials.size(), partials->stats.maxActivePartials, budget);
    // the budget is enforced on probe statistics; allow tracker overlap slack
    if (partials->stats.maxActivePartials > size_t(budget) * 5 / 4)
    {
      printf("  BUDGET EXCEEDED beyond tolerance\n");
      pass = false;
    }
  }
  else
  {
    printf("  full analysis: no partials\n");
    pass = false;
  }

  printf("auto-test: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// score: reconstruction metrics

void printScore(FILE* f, const ml::utu::ReconstructionScore& s)
{
  fprintf(f,
          "  score: spectral %.2f dB, watery %.2f dB, rustle %.2f dB, transients %.2f dB/ms,"
          " total %.2f\n",
          s.spectralRmsDb, s.wateryDb, s.rustleDb, s.transientDeficit, s.total);
}

int scoreCmd(const char* srcPath, const char* renderPath)
{
  std::vector<float> src, render;
  double srcRate = 0., renderRate = 0.;
  if (!loadAudio(srcPath, src, srcRate)) return 2;
  if (!loadAudio(renderPath, render, renderRate)) return 2;
  if (long(srcRate) != long(renderRate))
  {
    printf("sample rates differ (%g vs %g)\n", srcRate, renderRate);
    return 2;
  }
  auto s = ml::utu::scoreReconstruction(src.data(), src.size(), render.data(), render.size(),
                                        srcRate);
  printScore(stdout, s);
  return 0;
}

// analyze + render + score at the given params: the objective for tuning.
// Parameter sets that exceed the simultaneous-partial budget are infeasible
// — otherwise the descent just buys quality with partials we don't have.
float evalParams(const std::vector<float>& src, double sr,
                 const ml::utu::AnalyzerParams& params, float hiCut, int budget,
                 ml::utu::ReconstructionScore* scoreOut = nullptr,
                 size_t* maxActiveOut = nullptr)
{
  auto partials = ml::utu::analyzeToPartials(src.data(), src.size(), params);
  ml::cutHighs(*partials, hiCut);
  ml::cleanOutliers(*partials);
  if (partials->partials.empty()) return 1e9f;
  ml::calcStats(*partials);
  if (maxActiveOut) *maxActiveOut = partials->stats.maxActivePartials;
  if (int(partials->stats.maxActivePartials) > budget) return 1e9f;

  ml::utu::SynthParams sp;
  sp.sampleRate = float(sr);
  sp.fadeTime = 0.001f;
  ml::utu::PartialSynthesizer synth;
  synth.setParams(sp);
  std::vector<float> out;
  synth.render(*partials, out);
  if (out.empty()) return 1e9f;

  auto s = ml::utu::scoreReconstruction(src.data(), src.size(), out.data(), out.size(), sr);
  if (scoreOut) *scoreOut = s;
  return s.total;
}

// ---------------------------------------------------------------------------
// tune: coordinate descent over analyzer params against the reconstruction
// score, starting from the automatic estimates. The deltas between tuned
// and automatic values, across sounds, teach us better heuristics.

int tuneCmd(const char* path, int budget)
{
  std::vector<float> src;
  double sr = 0.;
  if (!loadAudio(path, src, sr)) return 2;
  // silence cleanOutliers chatter during the many evaluations
  printf("%s: %zu samples at %g Hz, budget %d\n", path, src.size(), sr, budget);

  auto autoParams = ml::utu::computeAnalyzerParams(src.data(), src.size(), float(sr), budget);
  ml::utu::AnalyzerParams p = autoParams.params;
  const float hiCut = autoParams.hiCut;

  ml::utu::ReconstructionScore baseScore;
  size_t baseMaxActive = 0;
  float best = evalParams(src, sr, p, hiCut, budget, &baseScore, &baseMaxActive);
  printf("auto maxActive %zu / budget %d\n", baseMaxActive, budget);
  printf("auto params");
  printScore(stdout, baseScore);

  struct Knob
  {
    const char* name;
    float lo, hi;
    bool multiplicative;
    float step;             // factor or additive amount
    float* (*get)(ml::utu::AnalyzerParams&);
    float sensitivity{0.f};
  };
  auto gWidth = [](ml::utu::AnalyzerParams& a) { return &a.windowWidth; };
  auto gDrift = [](ml::utu::AnalyzerParams& a) { return &a.freqDrift; };
  auto gRes = [](ml::utu::AnalyzerParams& a) { return &a.resolution; };
  auto gFloor = [](ml::utu::AnalyzerParams& a) { return &a.ampFloor; };
  auto gNoise = [](ml::utu::AnalyzerParams& a) { return &a.noiseWidth; };
  Knob knobs[] = {
      {"windowWidth", 16.f, 768.f, true, 1.3f, gWidth},
      {"freqDrift", 2.f, 80.f, true, 2.f, gDrift},
      {"resolution", 8.f, 1024.f, true, 1.3f, gRes},
      {"ampFloor", -90.f, -20.f, false, 9.f, gFloor},
      {"noiseWidth", 10.f, 5000.f, true, 2.f, gNoise},
  };

  const int sweeps = 2;
  for (int sweep = 0; sweep < sweeps; ++sweep)
  {
    for (Knob& k : knobs)
    {
      const float center = *k.get(p);
      float candidates[2];
      if (k.multiplicative)
      {
        candidates[0] = std::min(k.hi, center * k.step);
        candidates[1] = std::max(k.lo, center / k.step);
      }
      else
      {
        candidates[0] = std::min(k.hi, center + k.step);
        candidates[1] = std::max(k.lo, center - k.step);
      }
      float sweepBest = best;
      float bestVal = center;
      for (float cand : candidates)
      {
        if (cand == center) continue;
        ml::utu::AnalyzerParams trial = p;
        *k.get(trial) = cand;
        const float s = evalParams(src, sr, trial, hiCut, budget);
        if (s < sweepBest)
        {
          sweepBest = s;
          bestVal = cand;
        }
      }
      k.sensitivity = std::max(k.sensitivity, best - sweepBest);
      if (bestVal != center)
      {
        *k.get(p) = bestVal;
        best = sweepBest;
        printf("  sweep %d: %s -> %.2f (total %.2f)\n", sweep + 1, k.name, bestVal, best);
      }
      // halve the exploration step per sweep
      k.step = k.multiplicative ? (1.f + (k.step - 1.f) * 0.5f) : (k.step * 0.5f);
    }
  }

  ml::utu::ReconstructionScore tunedScore;
  size_t tunedMaxActive = 0;
  evalParams(src, sr, p, hiCut, budget, &tunedScore, &tunedMaxActive);
  printf("tuned params (deltas from auto), maxActive %zu / budget %d:\n", tunedMaxActive,
         budget);
  for (Knob& k : knobs)
  {
    ml::utu::AnalyzerParams a = autoParams.params;
    printf("  %-20s %8.2f (auto %8.2f)   sensitivity %.3f\n", k.name, *k.get(p), *k.get(a),
           k.sensitivity);
  }
  printScore(stdout, tunedScore);
  printf("total: %.2f -> %.2f (%+.1f%%)\n", baseScore.total, tunedScore.total,
         100.f * (tunedScore.total - baseScore.total) / std::max(1e-6f, baseScore.total));

  // write the tuned render beside the source for listening
  {
    auto partials = ml::utu::analyzeToPartials(src.data(), src.size(), p);
    ml::cutHighs(*partials, hiCut);
    ml::cleanOutliers(*partials);
    ml::calcStats(*partials);
    ml::utu::SynthParams sp;
    sp.sampleRate = float(sr);
    sp.fadeTime = 0.001f;
    ml::utu::PartialSynthesizer synth;
    synth.setParams(sp);
    std::vector<float> out;
    synth.render(*partials, out);
    float peak = 0.f;
    for (float v : out) peak = std::max(peak, fabsf(v));
    if (peak > 1.f)
    {
      for (auto& v : out) v *= 0.999f / peak;
    }
    namespace fs = std::filesystem;
    const fs::path in(path);
    std::string ext = in.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    const int fmt = ((ext == ".wav") ? SF_FORMAT_WAV : SF_FORMAT_AIFF) | SF_FORMAT_PCM_24;
    const fs::path outPath = in.parent_path() / (in.stem().string() + "-tuned" + in.extension().string());
    writeAudio(outPath.string(), out.data(), out.size(), sr, fmt);
    printf("wrote %s\n", outPath.string().c_str());
  }
  return 0;
}

// ---------------------------------------------------------------------------
// convert-dir: batch analyze -> resynthesize every audio file in a directory,
// writing <name>-converted.<ext> and <name>.utu beside each source and
// auto-params.txt with the automatically chosen analysis parameters per file

bool writePartialsFile(const std::string& path, const ml::VutuPartialsData& partials)
{
  auto json = ml::vutuPartialsToJSON(partials);
  auto text = ml::JSONToText(json);
  FILE* f = fopen(path.c_str(), "w");
  if (!f) return false;
  const size_t n = text.lengthInBytes();
  const bool ok = fwrite(text.getText(), 1, n, f) == n;
  fclose(f);
  return ok;
}

int convertDir(const char* dirPath, int budget)
{
  namespace fs = std::filesystem;
  const fs::path dir(dirPath);
  if (!fs::is_directory(dir))
  {
    printf("not a directory: %s\n", dirPath);
    return 2;
  }

  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(dir))
  {
    if (!entry.is_regular_file()) continue;
    std::string ext = entry.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    if ((ext != ".aif") && (ext != ".aiff") && (ext != ".wav")) continue;
    const std::string stem = entry.path().stem().string();
    auto endsWith = [&stem](const char* tag)
    {
      const size_t n = strlen(tag);
      return (stem.size() >= n) && (stem.compare(stem.size() - n, n, tag) == 0);
    };
    if (endsWith("-converted") || endsWith("-tuned"))
    {
      continue;  // derived outputs, not sources
    }
    files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  if (files.empty())
  {
    printf("no audio files (.aif/.aiff/.wav) in %s\n", dirPath);
    return 2;
  }

  const fs::path reportPath = dir / "auto-params.txt";
  FILE* report = fopen(reportPath.string().c_str(), "w");
  if (!report)
  {
    printf("cannot write %s\n", reportPath.string().c_str());
    return 2;
  }
  fprintf(report, "automatic analysis parameters (utucompare convert-dir, budget %d)\n",
          budget);

  int failures = 0;
  for (const fs::path& path : files)
  {
    const std::string name = path.filename().string();
    printf("converting %s...\n", name.c_str());
    fprintf(report, "\n%s\n", name.c_str());

    std::vector<float> x;
    double sr = 0.;
    if (!loadAudio(path.string().c_str(), x, sr))
    {
      fprintf(report, "  ERROR: could not read\n");
      ++failures;
      continue;
    }
    fprintf(report, "  %zu samples at %.0f Hz (%.2f s)\n", x.size(), sr, x.size() / sr);

    auto r = ml::utu::computeAnalyzerParams(x.data(), x.size(), float(sr), budget);
    printAutoParams(report, r);

    auto partials = ml::utu::analyzeToPartials(x.data(), x.size(), r.params);
    ml::cutHighs(*partials, r.hiCut);
    ml::cleanOutliers(*partials);
    if (partials->partials.empty())
    {
      fprintf(report, "  ERROR: analysis produced no partials\n");
      ++failures;
      continue;
    }
    ml::calcStats(*partials);
    fprintf(report, "  analysis: %zu partials, max simultaneous %zu\n",
            partials->partials.size(), partials->stats.maxActivePartials);

    // partials beside the render, for structural comparison across engines
    partials->sourceFile = ml::TextFragment(name.c_str());
    partials->sourceDuration = float(x.size() / sr);
    partials->hiCut = r.hiCut;
    partials->fundamental = r.fundamental;
    const fs::path utuPath = path.parent_path() / (path.stem().string() + ".utu");
    if (writePartialsFile(utuPath.string(), *partials))
    {
      fprintf(report, "  wrote %s\n", utuPath.filename().string().c_str());
    }
    else
    {
      fprintf(report, "  ERROR: could not write %s\n", utuPath.filename().string().c_str());
    }

    ml::utu::SynthParams sp;
    sp.sampleRate = float(sr);
    sp.fadeTime = 0.001f;
    ml::utu::PartialSynthesizer synth;
    synth.setParams(sp);
    std::vector<float> out;
    synth.render(*partials, out);

    // preserve level; scale down only to prevent clipping
    float peak = 0.f;
    for (float v : out) peak = std::max(peak, fabsf(v));
    if (peak > 1.f)
    {
      const float scale = 0.999f / peak;
      for (auto& v : out) v *= scale;
      fprintf(report, "  render: peak %.3f, scaled by %.3f to prevent clipping\n", peak,
              scale);
    }
    else
    {
      fprintf(report, "  render: peak %.3f\n", peak);
    }

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    const int fmt = ((ext == ".wav") ? SF_FORMAT_WAV : SF_FORMAT_AIFF) | SF_FORMAT_PCM_24;
    const fs::path outPath =
        path.parent_path() / (path.stem().string() + "-converted" + path.extension().string());
    if (!writeAudio(outPath.string(), out.data(), out.size(), sr, fmt))
    {
      fprintf(report, "  ERROR: could not write %s\n", outPath.filename().string().c_str());
      ++failures;
      continue;
    }
    fprintf(report, "  wrote %s\n", outPath.filename().string().c_str());

    // every batch is a measurement run
    const auto s =
        ml::utu::scoreReconstruction(x.data(), x.size(), out.data(), out.size(), sr);
    printScore(report, s);
  }
  fclose(report);
  printf("wrote %s\n", reportPath.string().c_str());
  printf("convert-dir: %d/%zu files converted%s\n", int(files.size()) - failures, files.size(),
         failures ? " (WITH ERRORS)" : "");
  return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// ab-dir: regression gate. Analyze + render every source in srcdir with the
// current engine and score against the source; compare with the golden
// renders (goldendir/<stem>-converted.<ext>) and golden partials
// (goldendir/<stem>.utu) produced by a previous engine via convert-dir.
// Acceptance: each score component within +0.5 dB of golden (better is
// always fine), partial count within ±10% when golden partials exist, and
// no file regressing more than 1.0 in total score.

constexpr float kAbComponentSlack = 0.5f;
constexpr float kAbTotalSlack = 1.0f;
constexpr float kAbCountSlack = 0.10f;

ml::VutuPartialsData* loadPartialsFile(const std::string& path)
{
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return nullptr;
  std::string text;
  fseek(f, 0, SEEK_END);
  text.resize(size_t(ftell(f)));
  fseek(f, 0, SEEK_SET);
  const size_t got = fread(text.data(), 1, text.size(), f);
  fclose(f);
  if (got != text.size()) return nullptr;
  auto json = ml::textToJSON(ml::TextFragment(text.c_str()));
  return ml::jsonToVutuPartials(json);
}

int abDir(const char* srcDirPath, const char* goldenDirPath, int budget)
{
  namespace fs = std::filesystem;
  const fs::path srcDir(srcDirPath), goldenDir(goldenDirPath);
  if (!fs::is_directory(srcDir) || !fs::is_directory(goldenDir))
  {
    printf("need two directories\n");
    return 2;
  }

  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(srcDir))
  {
    if (!entry.is_regular_file()) continue;
    std::string ext = entry.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    if ((ext != ".aif") && (ext != ".aiff") && (ext != ".wav")) continue;
    const std::string stem = entry.path().stem().string();
    auto endsWith = [&stem](const char* tag)
    {
      const size_t n = strlen(tag);
      return (stem.size() >= n) && (stem.compare(stem.size() - n, n, tag) == 0);
    };
    if (endsWith("-converted") || endsWith("-tuned")) continue;
    files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  if (files.empty())
  {
    printf("no audio files in %s\n", srcDirPath);
    return 2;
  }

  int failures = 0, skipped = 0;
  for (const fs::path& path : files)
  {
    const std::string stem = path.stem().string();
    const fs::path goldenRender = goldenDir / (stem + "-converted" + path.extension().string());
    if (!fs::exists(goldenRender))
    {
      printf("%s: SKIP (no golden %s)\n", stem.c_str(),
             goldenRender.filename().string().c_str());
      ++skipped;
      continue;
    }

    std::vector<float> src, golden;
    double sr = 0., gsr = 0.;
    if (!loadAudio(path.string().c_str(), src, sr) ||
        !loadAudio(goldenRender.string().c_str(), golden, gsr) || long(sr) != long(gsr))
    {
      printf("%s: SKIP (read error)\n", stem.c_str());
      ++skipped;
      continue;
    }

    const auto goldScore =
        ml::utu::scoreReconstruction(src.data(), src.size(), golden.data(), golden.size(), sr);

    // current engine
    auto r = ml::utu::computeAnalyzerParams(src.data(), src.size(), float(sr), budget);
    auto partials = ml::utu::analyzeToPartials(src.data(), src.size(), r.params);
    ml::cutHighs(*partials, r.hiCut);
    ml::cleanOutliers(*partials);
    if (partials->partials.empty())
    {
      printf("%s: FAIL (no partials from current engine)\n", stem.c_str());
      ++failures;
      continue;
    }
    ml::calcStats(*partials);
    ml::utu::SynthParams sp;
    sp.sampleRate = float(sr);
    sp.fadeTime = 0.001f;
    ml::utu::PartialSynthesizer synth;
    synth.setParams(sp);
    std::vector<float> out;
    synth.render(*partials, out);
    float peak = 0.f;
    for (float v : out) peak = std::max(peak, fabsf(v));
    if (peak > 1.f)
      for (auto& v : out) v *= 0.999f / peak;
    const auto curScore =
        ml::utu::scoreReconstruction(src.data(), src.size(), out.data(), out.size(), sr);

    bool fileOk = true;
    auto cmp = [&](const char* what, float cur, float gold)
    {
      const float d = cur - gold;
      const bool bad = d > kAbComponentSlack;
      if (bad) fileOk = false;
      printf("    %-10s %7.2f vs %7.2f (%+.2f)%s\n", what, cur, gold, d, bad ? "  REGRESSED" : "");
    };
    printf("%s:\n", stem.c_str());
    cmp("spectral", curScore.spectralRmsDb, goldScore.spectralRmsDb);
    cmp("watery", curScore.wateryDb, goldScore.wateryDb);
    cmp("rustle", curScore.rustleDb, goldScore.rustleDb);
    cmp("transients", curScore.transientDeficit, goldScore.transientDeficit);
    if (curScore.total - goldScore.total > kAbTotalSlack)
    {
      printf("    total      %7.2f vs %7.2f  REGRESSED > %.1f\n", curScore.total,
             goldScore.total, kAbTotalSlack);
      fileOk = false;
    }
    else
    {
      printf("    total      %7.2f vs %7.2f\n", curScore.total, goldScore.total);
    }

    const fs::path goldenUtu = goldenDir / (stem + ".utu");
    if (fs::exists(goldenUtu))
    {
      std::unique_ptr<ml::VutuPartialsData> gp(loadPartialsFile(goldenUtu.string()));
      if (gp && !gp->partials.empty())
      {
        const double ratio =
            double(partials->partials.size()) / double(gp->partials.size()) - 1.;
        const bool bad = fabs(ratio) > kAbCountSlack;
        if (bad) fileOk = false;
        printf("    partials   %zu vs %zu (%+.1f%%)%s\n", partials->partials.size(),
               gp->partials.size(), 100. * ratio, bad ? "  DRIFTED" : "");
      }
    }
    if (!fileOk) ++failures;
  }
  printf("ab-dir: %zu files, %d failed, %d skipped\n", files.size(), failures, skipped);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    printf(
        "usage: utucompare <command>\n"
        "  selftest [filter]      ground-truth checks on synthetic signals\n"
        "  auto-test <aiff|@sine|@harm|@bell|@noise|@short|@sine96>  automatic parameters\n"
        "  convert-dir <dir> [budget]      auto-analyze + resynthesize every audio file\n"
        "  ab-dir <dir> <goldendir> [budget]  score current engine vs golden renders\n"
        "  score <src> <render>            reconstruction metrics\n"
        "  tune <file> [budget]            coordinate-descent parameter search\n");
    return 2;
  }
  const std::string cmd(argv[1]);
  if (cmd == "selftest") return selfTest(argc > 2 ? argv[2] : nullptr);
  if (cmd == "auto-test" && argc > 2) return autoTest(argv[2]);
  if (cmd == "convert-dir" && argc > 2)
    return convertDir(argv[2], argc > 3 ? atoi(argv[3]) : 64);
  if (cmd == "ab-dir" && argc > 3)
    return abDir(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 64);
  if (cmd == "score" && argc > 3) return scoreCmd(argv[2], argv[3]);
  if (cmd == "tune" && argc > 2) return tuneCmd(argv[2], argc > 3 ? atoi(argv[3]) : 64);
  printf("unknown command '%s'\n", cmd.c_str());
  return 2;
}


// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#include "utuAutoParams.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "utuFFT.h"
#include "utuPeaks.h"
#include "utuSpectrum.h"
#include "utuWindow.h"

namespace ml::utu
{

namespace
{

constexpr double kPi = 3.14159265358979324;

// UI parameter ranges (vutuProcessor readParameterDescriptions)
constexpr float kResolutionLo = 8.f, kResolutionHi = 1024.f;
constexpr float kWidthLo = 16.f, kWidthHi = 768.f;
constexpr float kAmpFloorLo = -90.f, kAmpFloorHi = -20.f;
constexpr float kDriftLo = 2.f, kDriftHi = 80.f;
constexpr float kLoCutLo = 20.f, kLoCutHi = 2000.f;
constexpr float kHiCutLo = 200.f, kHiCutHi = 20000.f;
constexpr float kNoiseWidthLo = 10.f, kNoiseWidthHi = 5000.f;
constexpr float kFundamentalLo = 22.f, kFundamentalHi = 2200.f;

float clampf(float x, float lo, float hi) { return std::min(hi, std::max(lo, x)); }

long prevPow2(long n)
{
  long p = 1;
  while (2 * p <= n) p *= 2;
  return p;
}

// p in [0,1]; modifies its copy of v
float percentile(std::vector<float> v, float p)
{
  if (v.empty()) return 0.f;
  const size_t idx = size_t(p * (v.size() - 1) + 0.5f);
  std::nth_element(v.begin(), v.begin() + idx, v.end());
  return v[idx];
}

// autocorrelation of x via FFT; result r[lag] for lag in [0, x.size())
std::vector<float> autocorrelate(const std::vector<float>& x)
{
  const long n = long(x.size());
  long fftN = 8;
  while (fftN < 2 * n) fftN *= 2;
  RealFFT fft(fftN);
  std::vector<float> in(fftN, 0.f);
  std::copy(x.begin(), x.end(), in.begin());
  const long bins = fft.bins();
  std::vector<float> re(bins), im(bins);
  fft.forward(in.data(), re.data(), im.data());

  // power spectrum -> inverse transform by naive symmetry: autocorr is the
  // inverse FFT of |X|²; with a real even spectrum this is a cosine sum.
  // For the modest sizes used here (<= 32k), evaluate directly per lag over
  // the first n lags. O(n·bins) but only run twice per call.
  std::vector<float> r(n, 0.f);
  std::vector<double> power(bins);
  for (long k = 0; k < bins; ++k)
  {
    power[k] = double(re[k]) * re[k] + double(im[k]) * im[k];
  }
  for (long lag = 0; lag < n; ++lag)
  {
    double acc = 0.;
    for (long k = 0; k < bins; ++k)
    {
      acc += power[k] * cos(2. * kPi * k * lag / fftN);
    }
    r[lag] = float(acc);
  }
  return r;
}

// parabolic interpolation of a peak at integer index i of y
double interpPeak(const std::vector<float>& y, long i)
{
  if (i <= 0 || i + 1 >= long(y.size())) return double(i);
  const double a = y[i - 1], b = y[i], c = y[i + 1];
  const double d = a - 2 * b + c;
  if (d >= 0.) return double(i);
  return i + 0.5 * (a - c) / d;
}

struct Ctx
{
  const float* x;
  size_t n;
  double sr;
  int budget;

  // stage 0
  std::vector<float> envDb;  // 2 ms block RMS in dB
  double envRate{0};         // blocks per second
  size_t activeBegin{0}, activeEnd{0};  // samples
  double activeDuration{0};
  float peak{0};
  double r95{0};             // dominant envelope rate region, Hz
  bool transients{false};
  std::vector<size_t> probeSites;  // sample positions

  // stage 1
  long welchN{8192};
  double binHz{0};
  std::vector<float> avgDb;    // averaged amplitude spectrum, dB
  std::vector<float> floorDb;  // sliding-percentile noise floor
  float peakDb{-160.f};
  float loCut{20.f}, hiCut{20000.f}, ampFloorV1{-90.f};
  size_t stationaryCenter{0};  // sample index of strongest steady segment

  // stage 2
  struct SigPeak { double freq, db; };
  std::vector<SigPeak> sigPeaks;
  float spacingDom{0}, spacingConf{0}, spacingMin{0};
  float f0{0}, pitchConf{0};
  bool pitched{false}, unpitched{false};
};

// ---------------------------------------------------------------------------
// stage 0: time-domain statistics

bool stage0(Ctx& c)
{
  // remove DC via running measurement (the buffer itself is not modified;
  // the mean is subtracted where samples are consumed)
  double mean = 0.;
  for (size_t i = 0; i < c.n; ++i) mean += c.x[i];
  mean /= std::max(size_t(1), c.n);

  const size_t blockSize = std::max(size_t(1), size_t(0.002 * c.sr + 0.5));
  c.envRate = c.sr / blockSize;
  const size_t nBlocks = c.n / blockSize;
  if (nBlocks < 4) return false;

  std::vector<float> env(nBlocks);
  c.peak = 0.f;
  for (size_t b = 0; b < nBlocks; ++b)
  {
    double acc = 0.;
    const float* p = c.x + b * blockSize;
    for (size_t i = 0; i < blockSize; ++i)
    {
      const double v = p[i] - mean;
      acc += v * v;
      c.peak = std::max(c.peak, float(fabs(v)));
    }
    env[b] = float(sqrt(acc / blockSize));
  }
  if (c.peak < 3.16e-4f)  // < -70 dBFS: effectively silence
  {
    return false;
  }

  // activity mask: blocks within 60 dB of the loudest block
  const float envPeak = *std::max_element(env.begin(), env.end());
  const float activeThresh = envPeak * 1e-3f;
  size_t first = 0, last = nBlocks - 1;
  while (first < nBlocks && env[first] < activeThresh) ++first;
  while (last > first && env[last] < activeThresh) --last;
  c.activeBegin = first * blockSize;
  c.activeEnd = std::min(c.n, (last + 1) * blockSize);
  c.activeDuration = (c.activeEnd - c.activeBegin) / c.sr;
  if (c.activeDuration < 0.05) return false;

  c.envDb.resize(nBlocks);
  for (size_t b = 0; b < nBlocks; ++b)
  {
    c.envDb[b] = 20.f * log10f(std::max(env[b], envPeak * 1e-6f));
  }

  // transient statistic: 95th percentile of block-to-block rise in dB/ms.
  // fast rises need frame rates that a modulation-rate statistic alone
  // would not demand
  std::vector<float> rises;
  rises.reserve(last - first);
  for (size_t b = first + 1; b <= last; ++b)
  {
    rises.push_back(std::max(0.f, c.envDb[b] - c.envDb[b - 1]) / 2.f);  // dB per ms
  }
  c.transients = percentile(rises, 0.95f) > 1.f;

  // envelope modulation rate: r95 = 95th-percentile-energy rate of the
  // active envelope's spectrum in 2..100 Hz. The frame rate must sample the
  // envelopes partials actually have; modulation faster than ~100 Hz reads
  // as noise and belongs to bandwidth enhancement instead
  {
    std::vector<float> e(env.begin() + first, env.begin() + last + 1);
    const float eMean = std::accumulate(e.begin(), e.end(), 0.f) / e.size();
    for (auto& v : e) v -= eMean;
    long fftN = 8;
    while (fftN < long(e.size())) fftN *= 2;
    fftN = std::min(fftN, 65536L);
    e.resize(fftN, 0.f);
    RealFFT fft(fftN);
    std::vector<float> re(fft.bins()), im(fft.bins());
    fft.forward(e.data(), re.data(), im.data());
    const double envBinHz = c.envRate / fftN;
    const long k2 = std::max(1L, long(2. / envBinHz));
    const long k100 = std::min(long(fft.bins() - 1), long(100. / envBinHz));
    double total = 0.;
    std::vector<double> cum(k100 + 1, 0.);
    for (long k = k2; k <= k100; ++k)
    {
      total += double(re[k]) * re[k] + double(im[k]) * im[k];
      cum[k] = total;
    }
    c.r95 = 0.;
    if (total > 0.)
    {
      for (long k = k2; k <= k100; ++k)
      {
        if (cum[k] >= 0.95 * total)
        {
          c.r95 = k * envBinHz;
          break;
        }
      }
    }
  }

  // probe sites: 4 loudest well-separated blocks, 2 sharpest rises, 2 time
  // quartiles — so budget and drift are measured where the sound is busy,
  // at onsets, and at representative points
  {
    std::vector<size_t> order(last - first + 1);
    std::iota(order.begin(), order.end(), first);
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return env[a] > env[b]; });
    const size_t minSep = std::max(size_t(1), size_t(0.25 * c.envRate));  // 250 ms
    auto addSite = [&](size_t block)
    {
      const size_t s = block * blockSize;
      for (size_t existing : c.probeSites)
      {
        if (size_t(labs(long(existing) - long(s))) < minSep * blockSize) return;
      }
      c.probeSites.push_back(s);
    };
    for (size_t i = 0; i < order.size() && c.probeSites.size() < 4; ++i) addSite(order[i]);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
              { return (a > first ? c.envDb[a] - c.envDb[a - 1] : 0) >
                       (b > first ? c.envDb[b] - c.envDb[b - 1] : 0); });
    for (size_t i = 0; i < order.size() && c.probeSites.size() < 6; ++i) addSite(order[i]);
    addSite(first + (last - first) / 4);
    addSite(first + 3 * (last - first) / 4);
  }
  return true;
}

// ---------------------------------------------------------------------------
// stage 1: Welch-averaged spectrum, floor, extents, first-cut amp floor

void stage1(Ctx& c)
{
  const size_t active = c.activeEnd - c.activeBegin;
  // ~5.5 Hz bins resolve partial spacings down to the resolution range's
  // low end; short files get what they can support
  long N = prevPow2(long(c.sr / 5.5) * 2 - 1);
  while ((N > 2048) && (size_t(N) > active / 4)) N /= 2;
  N = std::max(2048L, N);
  c.welchN = N;
  c.binHz = c.sr / N;

  // 90 dB Kaiser, amplitude-calibrated: dB values here are commensurable
  // with the analyzer's ampFloor
  const double shape = kaiser::computeShape(90.);
  std::vector<double> winD(N);
  kaiser::buildWindow(winD, shape);
  const double winsum = std::accumulate(winD.begin(), winD.end(), 0.);
  std::vector<float> win(N);
  for (long i = 0; i < N; ++i) win[i] = float(winD[i] * 2. / winsum);

  RealFFT fft(N);
  const long bins = fft.bins();
  std::vector<double> avgPower(bins, 0.);
  std::vector<float> seg(N), re(bins), im(bins);

  long frames = 0;
  double bestFramePower = -1.;
  double prevFramePower = -1.;
  const size_t hop = N / 2;
  const size_t end = (active > size_t(N)) ? (c.activeEnd - N) : c.activeBegin;
  for (size_t s = c.activeBegin; s <= end; s += hop)
  {
    double framePower = 0.;
    for (long i = 0; i < N; ++i)
    {
      const float v = (s + i < c.n) ? c.x[s + i] : 0.f;
      seg[i] = v * win[i];
    }
    fft.forward(seg.data(), re.data(), im.data());
    for (long k = 0; k < bins; ++k)
    {
      const double p = double(re[k]) * re[k] + double(im[k]) * im[k];
      avgPower[k] += p;
      framePower += p;
    }
    ++frames;

    // strongest quasi-stationary frame: high power, similar to its
    // predecessor — a good home for the pitch autocorrelation
    if ((prevFramePower > 0.) &&
        (fabs(10. * log10(framePower / prevFramePower)) < 3.) &&
        (framePower > bestFramePower))
    {
      bestFramePower = framePower;
      c.stationaryCenter = s + N / 2;
    }
    prevFramePower = framePower;
  }
  if (frames == 0) frames = 1;
  if (bestFramePower < 0.) c.stationaryCenter = c.activeBegin + active / 2;

  c.avgDb.resize(bins);
  for (long k = 0; k < bins; ++k)
  {
    c.avgDb[k] = float(10. * log10(std::max(avgPower[k] / frames, 1e-16)));
  }

  // sliding-percentile noise floor: 20th percentile of dB values over a
  // ±250 Hz window. Percentile-across-a-local-window rather than global, so
  // dense partials cannot inflate the floor between them
  const long halfWin = std::max(4L, long(250. / c.binHz));
  const long kMin = std::max(1L, long(20. / c.binHz));
  c.floorDb.assign(bins, -160.f);
  for (long k = kMin; k < bins; ++k)
  {
    const long lo = std::max(kMin, k - halfWin);
    const long hi = std::min(bins - 1, k + halfWin);
    std::vector<float> w(c.avgDb.begin() + lo, c.avgDb.begin() + hi + 1);
    c.floorDb[k] = percentile(std::move(w), 0.2f);
  }

  c.peakDb = -160.f;
  for (long k = kMin; k < bins; ++k) c.peakDb = std::max(c.peakDb, c.avgDb[k]);

  // spectral extents from above-floor energy only, so broadband hiss and
  // rumble floors cannot drag the cut points around; only structure counts
  std::vector<double> ePlus(bins, 0.);
  double eTotal = 0.;
  for (long k = kMin; k < bins; ++k)
  {
    const double p = std::pow(10., c.avgDb[k] / 10.) - std::pow(10., c.floorDb[k] / 10.);
    ePlus[k] = std::max(0., p);
    eTotal += ePlus[k];
  }
  double cum = 0.;
  long kLo = kMin, kHi = bins - 1;
  for (long k = kMin; k < bins; ++k)
  {
    cum += ePlus[k];
    if (cum <= 0.005 * eTotal) kLo = k;
    if (cum <= 0.995 * eTotal) kHi = k;
  }
  // protect low-energy but above-floor brilliance that a pure energy
  // percentile misses (cymbal sheen)
  long kHiFloor = kHi;
  for (long k = bins - 1; k > kMin; --k)
  {
    if (c.avgDb[k] >= c.floorDb[k] + 15.f)
    {
      kHiFloor = k;
      break;
    }
  }
  kHi = std::max(kHi, kHiFloor);
  c.loCut = clampf(float(kLo * c.binHz), kLoCutLo, kLoCutHi);
  c.hiCut = clampf(float(kHi * c.binHz) * 1.05f + 50.f, kHiCutLo,
                   std::min(kHiCutHi, float(0.45 * c.sr)));

  // first-cut amplitude floor: 10 dB above the median in-band noise floor
  // (≈3× the Rayleigh mode of magnitude noise — +6 dB still crosses
  // constantly and seeds spurious tracks), but no more than 72 dB below the
  // strongest partial; the analyzer fades kept peaks over the 10 dB above
  // this floor
  {
    const long lo = std::max(kMin, long(c.loCut / c.binHz));
    const long hi = std::min(bins - 1, long(c.hiCut / c.binHz));
    std::vector<float> f(c.floorDb.begin() + lo, c.floorDb.begin() + hi + 1);
    const float medianFloor = percentile(std::move(f), 0.5f);
    c.ampFloorV1 =
        clampf(std::max(medianFloor + 10.f, c.peakDb - 72.f), kAmpFloorLo, kAmpFloorHi);
  }
}

// ---------------------------------------------------------------------------
// stage 2: partial spacing (spectral ACF), minimum spacing, fundamental

void stage2(Ctx& c)
{
  const long bins = long(c.avgDb.size());
  const long kMin = std::max(1L, long(20. / c.binHz));

  // significant peaks: local maxima well above the floor and within 50 dB
  // of the strongest — the population the analyzer will actually track.
  // Each must also have >= 6 dB of topographic prominence (rise above the
  // deepest saddle separating it from any higher peak): vibrato smears
  // harmonics into wide plateaus in the time-averaged spectrum, and plateau
  // ripple would otherwise read as many closely-spaced false peaks
  const long kLo = std::max(kMin, long(c.loCut / c.binHz));
  const long kHi = std::min(bins - 2, long(c.hiCut / c.binHz));
  std::vector<long> maxima;
  for (long k = std::max(kLo, 2L); k <= kHi; ++k)
  {
    if ((c.avgDb[k] > c.avgDb[k - 1]) && (c.avgDb[k] >= c.avgDb[k + 1]) &&
        (c.avgDb[k] >= c.floorDb[k] + 15.f) && (c.avgDb[k] >= c.peakDb - 50.f))
    {
      maxima.push_back(k);
    }
  }
  for (long k : maxima)
  {
    // walk to the nearest higher ground on each side, tracking the saddle
    auto saddleTo = [&](int dir)
    {
      float saddle = c.avgDb[k];
      for (long j = k + dir; (j >= kLo) && (j <= kHi); j += dir)
      {
        saddle = std::min(saddle, c.avgDb[j]);
        if (c.avgDb[j] > c.avgDb[k]) return saddle;
      }
      return std::min(saddle, c.floorDb[k]);  // ran off the band: full drop
    };
    const float prominence = c.avgDb[k] - std::max(saddleTo(-1), saddleTo(+1));
    if (prominence >= 6.f)
    {
      c.sigPeaks.push_back({interpPeak(c.avgDb, k) * c.binHz, double(c.avgDb[k])});
    }
  }

  // dominant spacing: autocorrelation of the above-floor spectrum across
  // frequency. Finds the common partial spacing even with a missing
  // fundamental, and for regularly-spaced inharmonic sounds
  {
    std::vector<float> s(bins - kMin, 0.f);
    for (long k = kMin; k < bins; ++k)
    {
      s[k - kMin] = std::max(0.f, c.avgDb[k] - c.floorDb[k]);
    }
    const float mean = std::accumulate(s.begin(), s.end(), 0.f) / s.size();
    for (auto& v : s) v -= mean;
    auto r = autocorrelate(s);
    const long lagLo = std::max(2L, long(10. / c.binHz));
    const long lagHi = std::min(long(r.size()) - 2, long(1300. / c.binHz));
    long best = -1;
    float bestVal = 0.f;
    // a lag qualifies as a peak of the ACF, not just a large value
    for (long lag = lagLo; lag <= lagHi; ++lag)
    {
      if ((r[lag] > r[lag - 1]) && (r[lag] >= r[lag + 1]) && (r[lag] > bestVal))
      {
        bestVal = r[lag];
        best = lag;
      }
    }
    if ((best > 0) && (r[0] > 0.f))
    {
      c.spacingDom = float(interpPeak(r, best) * c.binHz);
      c.spacingConf = clampf(bestVal / r[0], 0.f, 1.f);
    }
    // a spacing statistic is only meaningful with several partials; with
    // one or two the ACF just reports the shape of a single spectral lobe
    if (c.sigPeaks.size() < 3)
    {
      c.spacingConf = 0.f;
    }
  }

  // minimum significant spacing drives resolution: the analyzer masks
  // ±resolution around each kept peak every frame, so resolution must sit
  // below the closest spacing we intend to preserve
  if (c.sigPeaks.size() >= 3)
  {
    std::vector<float> spacings;
    for (size_t i = 1; i < c.sigPeaks.size(); ++i)
    {
      spacings.push_back(float(c.sigPeaks[i].freq - c.sigPeaks[i - 1].freq));
    }
    c.spacingMin = percentile(std::move(spacings), 0.1f);
  }

  // fundamental: normalized time-domain autocorrelation of the strongest
  // quasi-stationary 0.4 s
  {
    const size_t segLen = std::min(size_t(0.4 * c.sr), c.activeEnd - c.activeBegin);
    size_t s0 = c.stationaryCenter > segLen / 2 ? c.stationaryCenter - segLen / 2 : 0;
    s0 = std::min(s0, c.n - segLen);
    std::vector<float> seg(c.x + s0, c.x + s0 + segLen);
    const float mean = std::accumulate(seg.begin(), seg.end(), 0.f) / seg.size();
    for (auto& v : seg) v -= mean;
    auto r = autocorrelate(seg);
    const long lagLo = std::max(2L, long(c.sr / kFundamentalHi));
    const long lagHi = std::min(long(r.size()) - 2, long(c.sr / kFundamentalLo));
    long best = -1;
    float bestVal = 0.f;
    for (long lag = lagLo; lag <= lagHi; ++lag)
    {
      if ((r[lag] > r[lag - 1]) && (r[lag] >= r[lag + 1]) && (r[lag] > bestVal))
      {
        bestVal = r[lag];
        best = lag;
      }
    }
    if ((best > 0) && (r[0] > 0.f))
    {
      c.f0 = float(c.sr / interpPeak(r, best));
      c.pitchConf = clampf(bestVal / r[0], 0.f, 1.f);
    }
  }

  // classify. pitch is confident when the autocorrelation peak is strong
  // and, when a spacing estimate exists, the two agree (spacing an integer
  // multiple or half of f0 within 5%) — disagreement usually means a chord
  bool agree = true;
  if ((c.spacingConf >= 0.4f) && (c.f0 > 0.f))
  {
    const double ratio = c.spacingDom / c.f0;
    const double nearest = (ratio < 0.75) ? 0.5 : std::max(1.0, floor(ratio + 0.5));
    agree = fabs(ratio - nearest) / nearest < 0.05;
  }
  c.pitched = (c.pitchConf >= 0.6f) && (agree || (c.sigPeaks.size() <= 2));

  // resolve the spacing that drives resolution
  if (c.spacingMin <= 0.f)
  {
    if (c.pitched)
      c.spacingMin = c.f0;
    else if (c.spacingConf >= 0.4f)
      c.spacingMin = c.spacingDom;
  }
  c.unpitched = (c.spacingMin <= 0.f);

  // admit the lowest real partial: the energy percentile can park loCut
  // just under the strongest peak; when pitch is confident, open the floor
  // to 0.75 of the lowest partial that actually exists (not blindly to
  // 0.75·f0 — a missing fundamental would waste the low range)
  if (c.pitched && !c.sigPeaks.empty())
  {
    const float lowestReal = float(c.sigPeaks.front().freq);
    c.loCut = clampf(std::min(c.loCut, 0.75f * lowestReal), kLoCutLo, kLoCutHi);
  }
}

// ---------------------------------------------------------------------------
// stage 3: resolution and window width with anti-aliasing guard

// derive the window width for a given resolution (called again whenever the
// quality ladder changes resolution)
float deriveWindowWidth(const Ctx& c, float resolution)
{
  // absolute frame-rate cap, independent of resolution: linear
  // interpolation of a sampled envelope at modulation rate r has peak error
  // ≈ (π·r/F)²/2, ≤14% at F = 6r. Modulation faster than the cap belongs to
  // bandwidth enhancement; chasing it with frame rate turns estimator
  // jitter into synthesized FM noise
  const float fMax = c.transients ? 500.f : std::max(250.f, float(6. * c.r95));

  // at least ~3 window lengths within the sound (window length ≈ 8/W s)
  const float wFloor = std::max(kWidthLo, float(24. / std::max(0.05, c.activeDuration)));

  float w = std::min({2.f * resolution, fMax, kWidthHi});
  w = std::max(w, wFloor);

  // anti-beating guard. The frame rate F samples every partial's envelopes;
  // interference between components at spacing d rides on them at d Hz.
  // Resolved spacings (d > W/2) leave sidelobe-level residue that aliases
  // to near-DC wobble when d lands near a multiple of F — keep it at least
  // F/8 away. Unresolved spacings merge into one beating track; the beat is
  // representable when d < 0.3·W but between 0.3·W and 0.5·W it is
  // critically sampled and warbles — keep spacings out of that band.
  std::vector<float> strongSpacings;
  if (c.spacingDom > 0.f)
  {
    strongSpacings.push_back(c.spacingDom);
    strongSpacings.push_back(2.f * c.spacingDom);
  }
  for (size_t i = 1; i < std::min(c.sigPeaks.size(), size_t(6)); ++i)
  {
    strongSpacings.push_back(float(c.sigPeaks[i].freq - c.sigPeaks[i - 1].freq));
  }
  auto violates = [&](float W)
  {
    const long hopSamples = std::max(1L, long(c.sr / W));
    const float F = float(c.sr / hopSamples);  // actual frame rate
    for (float d : strongSpacings)
    {
      const float ratio = d / W;
      if ((ratio > 0.3f) && (ratio < 0.5f)) return true;
      if (d > 0.5f * W)
      {
        const float k = floorf(d / F + 0.5f);
        if ((k >= 1.f) && (fabsf(d - k * F) < F / 8.f)) return true;
      }
    }
    return false;
  };
  if (violates(w))
  {
    // nudge around the working width (±25%, nearest first). The band is
    // relative to w rather than to resolution because the frame-rate cap
    // can bind below 2·resolution; downward moves are always safe against
    // the cap, upward moves must respect it
    const float lo = std::max({wFloor, kWidthLo, 0.75f * w, 1.05f * resolution});
    const float hi = std::min({kWidthHi, fMax, 1.25f * w});
    bool fixed = false;
    for (float step = 0.02f * w; !fixed && (step <= 0.25f * w); step += 0.02f * w)
    {
      for (float cand : {w - step, w + step})
      {
        if ((cand >= lo) && (cand <= hi) && !violates(cand))
        {
          w = cand;
          fixed = true;
          break;
        }
      }
    }
  }

  return clampf(w, kWidthLo, kWidthHi);
}

void applyResolution(const Ctx& c, AnalyzerParams& p, float resolution)
{
  p.resolution = clampf(resolution, kResolutionLo, kResolutionHi);
  p.windowWidth = deriveWindowWidth(c, p.resolution);
}

void stage3(Ctx& c, AnalyzerParams& p)
{
  float res;
  if (c.unpitched)
  {
    // spread the partial budget across the occupied band: kept-peak density
    // after ±resolution masking is about one track per 1.25·resolution
    const float band = std::max(200.f, c.hiCut - c.loCut);
    res = band / (0.8f * c.budget);
  }
  else
  {
    // 0.8: the masking radius must sit below the smallest real spacing,
    // with headroom for inharmonic stretch and vibrato excursion
    res = 0.8f * c.spacingMin;
  }
  applyResolution(c, p, res);
  p.freqFloor = c.loCut;
  p.ampFloor = c.ampFloorV1;
  p.sidelobeLevel = 90.f;  // never coupled to ampFloor: budget-driven floor
                           // raises must not degrade sidelobe rejection
}

// ---------------------------------------------------------------------------
// stages 4+5: probe pass with the real analyzer front end, then a quality
// walk that spends the partial budget

struct ProbePeakData
{
  float freq, amp;
};

struct ProbeData
{
  std::vector<std::vector<ProbePeakData>> frames;  // kept peaks per frame, freq-ascending
  std::vector<float> hopDeltas;                    // |Δf| of strong peaks matched hop-to-hop
};

// One probe pass at the given resolution/window, taken at the most
// permissive floor and cuts. ampFloor/loCut/hiCut don't change the spectrum
// or the masking structure — masking only ever comes from louder peaks,
// which survive any floor — so the kept set at any real settings is a pure
// filter of this one, and the whole floor/cut ladder can be evaluated
// without further DSP. The probe floor sits below the analyzer range so
// thinPeaks' 10 dB fade zone cannot distort stored amplitudes above -90 dB.
//
// Only *persistent* peaks — those matched to the previous frame — are
// stored for budget counting: one-frame blips (dense/transient material is
// full of them) become one-breakpoint partials that cleanOutliers deletes,
// so counting them makes the probe overestimate the achieved simultaneous
// count and the quality walk over-raises the amplitude floor. (Measured on
// a dense mix: raw probe p90 57 vs 23 partials actually achieved.)
ProbeData probeFrames(const Ctx& c, const AnalyzerParams& p)
{
  ProbeData d;
  ReassignedSpectrum spectrum;
  spectrum.configure(buildReassignmentWindows(c.sr, p.windowWidth, p.sidelobeLevel));
  PeakSelector selector;
  const long hopSamples = std::max(1L, long(c.sr / p.windowWidth));
  selector.configure(float(c.sr), float(hopSamples / c.sr));  // cropTime = hopTime

  const float permissiveFloor = -100.f;
  const float strongAmp = powf(10.f, (c.ampFloorV1 + 20.f) / 20.f);
  const int framesPerSite = 5;

  PeakFrame frame;
  std::vector<ProbePeakData> prev;
  for (size_t site : c.probeSites)
  {
    prev.clear();
    for (int j = 0; j < framesPerSite; ++j)
    {
      const long center = long(site) + j * hopSamples;
      if ((center < 0) || (size_t(center) >= c.n)) break;
      spectrum.transform(c.x, long(c.n), center);
      selector.selectPeaks(spectrum, kLoCutLo, frame);
      thinPeaks(frame, p.resolution, permissiveFloor, center / c.sr);

      std::vector<ProbePeakData> kept;
      kept.reserve(frame.numKept);
      for (size_t i = 0; i < frame.numKept; ++i)
      {
        kept.push_back({frame.peaks[i].freq, frame.peaks[i].amp});
      }
      std::sort(kept.begin(), kept.end(), [](const ProbePeakData& a, const ProbePeakData& b)
                { return a.freq < b.freq; });

      // match against the previous frame: persistent peaks enter the
      // budget counts; strong matched pairs drive freqDrift
      if (j > 0)
      {
        std::vector<ProbePeakData> persistent;
        persistent.reserve(kept.size());
        size_t a = 0, b = 0;
        while (a < prev.size() && b < kept.size())
        {
          const float df = kept[b].freq - prev[a].freq;
          if (fabsf(df) < 0.5f * p.resolution)
          {
            if ((prev[a].amp > strongAmp) && (kept[b].amp > strongAmp))
            {
              d.hopDeltas.push_back(fabsf(df));
            }
            persistent.push_back(kept[b]);
            ++a;
            ++b;
          }
          else if (df > 0)
            ++a;
          else
            ++b;
        }
        d.frames.push_back(std::move(persistent));
      }
      prev = std::move(kept);
    }
  }
  return d;
}

// p90 of per-frame kept-peak counts under the given floor and band — the
// budget currency, computed by filtering the stored probe peaks
int countP90(const ProbeData& d, float floorDb, float lo, float hi)
{
  if (d.frames.empty()) return 0;
  const float minAmp = powf(10.f, floorDb / 20.f);
  std::vector<float> counts;
  counts.reserve(d.frames.size());
  for (const auto& f : d.frames)
  {
    int ct = 0;
    for (const auto& pk : f)
    {
      if ((pk.amp >= minAmp) && (pk.freq >= lo) && (pk.freq <= hi)) ++ct;
    }
    counts.push_back(float(ct));
  }
  return int(percentile(std::move(counts), 0.9f));
}

// The quality walk. Each budget-coupled parameter has a ladder of steps in
// perceptual units (1/3 octave for the cuts, 6 dB for the floor, ×0.8 for
// unpitched resolution), starting from the conservative stage 1-3
// estimates. Under budget: ascend round-robin, most audible benefit first
// (hiCut — dullness is the common failure, and steps through empty spectrum
// are free — then ampFloor, loCut, resolution); a step that would exceed
// the budget is reverted and its knob frozen while the others continue.
// Over budget at the start: the same ladders walk down, floor first
// (rejected quiet peaks become bandwidth, which is energy-preserving).
void walkQuality(Ctx& c, AnalyzerParams& p, float& hiCut, AutoAnalyzerParams& out)
{
  const int target = std::max(8, c.budget - 8);  // tracker spawn/decay headroom
  const float kThirdOctave = 1.259921f;          // 2^(1/3)
  const float hiCutMax = std::min(kHiCutHi, float(0.45 * c.sr));

  ProbeData probe = probeFrames(c, p);
  int cnt = countP90(probe, p.ampFloor, p.freqFloor, hiCut);
  bool budgetLimited = false;

  if (cnt > target)
  {
    budgetLimited = true;
    // floor rises are capped: if whole regions lose all kept peaks their
    // residue is dropped and decay tails truncate audibly
    const float floorCap = std::min(kAmpFloorHi, c.ampFloorV1 + 15.f);
    bool changed = true;
    while ((cnt > target) && changed)
    {
      changed = false;
      if (p.ampFloor + 6.f <= floorCap)
      {
        p.ampFloor += 6.f;
        changed = true;
      }
      else if (hiCut / kThirdOctave >= std::max(kHiCutLo, 2000.f))
      {
        hiCut /= kThirdOctave;
        changed = true;
      }
      else if (c.unpitched && (p.resolution * 1.25f <= kResolutionHi))
      {
        applyResolution(c, p, p.resolution * 1.25f);
        probe = probeFrames(c, p);
        changed = true;
      }
      cnt = countP90(probe, p.ampFloor, p.freqFloor, hiCut);
    }
  }

  // ascend — also after a down-walk: shedding partials with one knob can
  // leave headroom that other knobs can still spend (a knob the down-walk
  // moved simply re-freezes on its first ascent attempt)
  {
    // knob order: hiCut up, ampFloor down, loCut down, resolution down
    // (unpitched only — finer-than-spacing resolution on pitched material
    // recreates the unstable-frequency pathology)
    enum
    {
      kKnobHiCut,
      kKnobFloor,
      kKnobLoCut,
      kKnobRes,
      kNumKnobs
    };
    bool frozen[kNumKnobs] = {false, false, false, !c.unpitched};
    const float wFloor = std::max(kWidthLo, float(24. / std::max(0.05, c.activeDuration)));
    const float resMin = std::max(kResolutionLo, 0.5f * wFloor);

    while (!(frozen[kKnobHiCut] && frozen[kKnobFloor] && frozen[kKnobLoCut] && frozen[kKnobRes]))
    {
      for (int knob = 0; knob < kNumKnobs; ++knob)
      {
        if (frozen[knob]) continue;

        AnalyzerParams trial = p;
        float trialHiCut = hiCut;
        bool reprobed = false;
        ProbeData trialProbe;
        switch (knob)
        {
          case kKnobHiCut:
            if (hiCut >= hiCutMax)
            {
              frozen[knob] = true;
              continue;
            }
            trialHiCut = std::min(hiCutMax, hiCut * kThirdOctave);
            break;
          case kKnobFloor:
            if (p.ampFloor <= kAmpFloorLo)
            {
              frozen[knob] = true;
              continue;
            }
            trial.ampFloor = std::max(kAmpFloorLo, p.ampFloor - 6.f);
            break;
          case kKnobLoCut:
            if (p.freqFloor <= kLoCutLo)
            {
              frozen[knob] = true;
              continue;
            }
            trial.freqFloor = std::max(kLoCutLo, p.freqFloor / kThirdOctave);
            break;
          case kKnobRes:
            if (p.resolution <= resMin)
            {
              frozen[knob] = true;
              continue;
            }
            applyResolution(c, trial, std::max(resMin, p.resolution * 0.8f));
            trialProbe = probeFrames(c, trial);
            reprobed = true;
            break;
        }

        const ProbeData& eval = reprobed ? trialProbe : probe;
        const int trialCnt = countP90(eval, trial.ampFloor, trial.freqFloor, trialHiCut);
        if (trialCnt <= target)
        {
          p = trial;
          hiCut = trialHiCut;
          if (reprobed) probe = std::move(trialProbe);
          cnt = trialCnt;
        }
        else
        {
          frozen[knob] = true;  // this knob is out of budget; others continue
          budgetLimited = true;
        }
      }
    }
  }

  // freqDrift: pass the movement real tracks exhibit at the final hop (p95
  // with headroom), but stay under half the minimum kept spacing so a track
  // cannot capture its neighbor
  if (!probe.hopDeltas.empty())
  {
    const float p95 = percentile(probe.hopDeltas, 0.95f);
    p.freqDrift = clampf(1.5f * p95, kDriftLo, std::min(kDriftHi, 0.5f * p.resolution));
  }
  else
  {
    p.freqDrift = clampf(0.2f * p.resolution, kDriftLo, std::min(kDriftHi, 0.5f * p.resolution));
  }

  out.probedSimultaneousP90 = cnt;
  out.budgetLimited = budgetLimited;
}


}  // namespace

AutoAnalyzerParams computeAnalyzerParams(const float* samples, size_t n, float sampleRate,
                                         int maxSimultaneousPartials)
{
  AutoAnalyzerParams out;
  out.params.sampleRate = sampleRate;
  out.params.sidelobeLevel = 90.f;

  Ctx c;
  c.x = samples;
  c.n = n;
  c.sr = sampleRate;
  c.budget = std::max(8, maxSimultaneousPartials);

  if (!samples || (n < 16) || !stage0(c))
  {
    // silence or too short to measure: defaults
    out.params.resolution = 40.f;
    out.params.windowWidth = 80.f;
    out.params.ampFloor = -60.f;
    out.params.freqDrift = 20.f;
    out.params.freqFloor = 20.f;
    return out;
  }

  stage1(c);
  stage2(c);
  stage3(c, out.params);
  out.hiCut = c.hiCut;
  out.budget = c.budget;
  walkQuality(c, out.params, out.hiCut, out);

  // noise regions: at least ~3 kept-peak spacings wide so residue always
  // lands in a region that still holds a kept peak to carry it; at most
  // 2 kHz (largest auditory-filter scale — beyond that, noise attaches to
  // perceptually unrelated partials). Small is the safe direction for bass:
  // the associator discards residue below about one region width
  out.params.bwRegionWidth =
      clampf(4.f * out.params.resolution, std::max(kNoiseWidthLo, 250.f), 2000.f);

  if (c.pitched)
  {
    out.fundamental = clampf(c.f0, kFundamentalLo, kFundamentalHi);
    out.pitchConfidence = c.pitchConf;
  }
  else if (c.spacingConf >= 0.4f)
  {
    out.fundamental = clampf(c.spacingDom, kFundamentalLo, kFundamentalHi);
    out.pitchConfidence = 0.5f * c.spacingConf;
  }

  out.noiseFloorDb = c.ampFloorV1 - 10.f;
  out.spacingHz = c.spacingDom;
  out.minSpacingHz = c.spacingMin;
  const long hopSamples = std::max(1L, long(c.sr / out.params.windowWidth));
  out.frameRateHz = float(c.sr / hopSamples);
  out.activeDuration = float(c.activeDuration);
  return out;
}

}  // namespace ml::utu


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

  // stage 1b
  float beatIndexDb{-120.f};  // absolute beat-band modulation index, dB
  float beatFraction{0};      // beat-band share of all sub-beat-top modulation
  float beatRate90{0};        // Hz, p90 of the beat-band modulation energy

  // stage 2
  struct SigPeak { double freq, db; };
  std::vector<SigPeak> sigPeaks;
  float spacingDom{0}, spacingConf{0}, spacingMin{0};
  float f0{0}, pitchConf{0};
  bool pitched{false}, unpitched{false};

  // window decision (chooseWindow)
  bool modDriven{false};  // audibly-loud fast modulation: window chosen first
  float wDemand{0};       // Hz, beatRate90/0.3 — pre-clamp window demand
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
// stage 1b: per-band beat spectrum. Beating between components the analysis
// window fails to resolve cleanly is what the bandwidth mechanism
// misclassifies as noise ("rustle"). The beats are directly visible in the
// source's band envelopes — evidence no time-averaged spacing statistic can
// provide, since sub-resolution component pairs merge into single peaks of
// the averaged spectrum. Band envelopes at ~170 frames/s expose modulation
// to ~85 Hz. The statistic counts 15-80 Hz modulation only in *tonal*
// bands (bands whose spectrum stands well above the noise floor): fast
// envelope modulation of tonal content is beating that the window must
// merge or resolve, while modulation of noise bands is just noise — which
// bandwidth enhancement models correctly. Dense polyphony produces a
// thicket of beat lines, so no per-line peakiness test is applied.

void stage1b(Ctx& c)
{
  const size_t active = c.activeEnd - c.activeBegin;
  if (active < size_t(0.25 * c.sr)) return;

  // Band envelopes from a time-domain filter bank rather than an STFT: an
  // STFT-based envelope lowpasses AM at roughly 1/frameLength (a 23 ms
  // frame ceilinged the old statistic near 43 Hz — right where dense
  // material's measured beat rates were piling up). Bandpassed, rectified,
  // 300 Hz-followed envelopes decimated to ~700 Hz measure modulation
  // cleanly to ~250 Hz. Offline estimation code: scalar doubles for
  // clarity, cost is 8 biquads over the file.
  constexpr int kBands = 8;
  // bands start at 300 Hz: a real input excites the resonator's
  // negative-frequency image too, putting ripple at 2f on |z|; with f >=
  // 300 the ripple sits at >= 600 Hz where the narrow resonator and the
  // cascaded 200 Hz envelope smoothing together bury it (~-80 dB energy)
  // before decimation can alias it into the beat band. Beats of sub-300 Hz
  // component pairs go unmeasured — a documented blind spot
  const double fLo = 300., fHi = 0.4 * c.sr;
  const long decim = std::max(1L, long(c.sr / 700.));
  const double envRate = c.sr / decim;

  // 4th-order Butterworth lowpass at 270 Hz (two biquad sections): the
  // decimation filter for the envelopes. The envelope of a real narrowband
  // signal unavoidably ripples at 2f (negative-frequency image); with
  // measurement bands starting at 300 Hz the ripple lives at >= 600 Hz —
  // squarely in this filter's stopband — while beats to 250 Hz pass
  struct BiquadLP
  {
    double b0, b1, b2, a1, a2;
    double z1{0}, z2{0};
    double process(double x)
    {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };
  auto makeLP = [&](double fc, double q)
  {
    BiquadLP f;
    const double w0 = 2. * kPi * fc / c.sr;
    const double alpha = sin(w0) / (2. * q);
    const double cw = cos(w0);
    const double a0 = 1. + alpha;
    f.b0 = (1. - cw) / 2. / a0;
    f.b1 = (1. - cw) / a0;
    f.b2 = f.b0;
    f.a1 = -2. * cw / a0;
    f.a2 = (1. - alpha) / a0;
    return f;
  };

  struct Band
  {
    double ar, ai;  // complex pole
    double g;       // input gain
    double zr{0}, zi{0};
    BiquadLP lp1, lp2;      // 4th-order Butterworth decimation filter
    double env{0};
    double bandLo, bandHi;  // Hz, for the tonality gate
  };
  Band bands[kBands];
  for (int b = 0; b < kBands; ++b)
  {
    bands[b].bandLo = fLo * std::pow(fHi / fLo, double(b) / kBands);
    bands[b].bandHi = fLo * std::pow(fHi / fLo, double(b + 1) / kBands);
    const double center = sqrt(bands[b].bandLo * bands[b].bandHi);
    // the resonator only needs to capture beats (<= ~250 Hz pair spacings),
    // not span its whole nominal band; narrow poles also shrink the
    // negative-frequency image
    const double bw = std::min(bands[b].bandHi - bands[b].bandLo, 600.);
    const double r = exp(-kPi * bw / c.sr);
    const double w0 = 2. * kPi * center / c.sr;
    bands[b].ar = r * cos(w0);
    bands[b].ai = r * sin(w0);
    bands[b].g = 1. - r;
    bands[b].lp1 = makeLP(270., 0.5412);  // Butterworth Q pair
    bands[b].lp2 = makeLP(270., 1.3066);
  }

  std::vector<std::vector<float>> env(kBands);
  for (int b = 0; b < kBands; ++b) env[b].reserve(active / decim + 1);
  long phase = 0;
  // skip the filter settling transient: its step would otherwise leak
  // across the whole modulation spectrum
  const size_t settle = c.activeBegin + size_t(0.1 * c.sr);
  for (size_t i = c.activeBegin; i < c.activeEnd; ++i)
  {
    const double x = c.x[i];
    for (int b = 0; b < kBands; ++b)
    {
      const double zr = bands[b].ar * bands[b].zr - bands[b].ai * bands[b].zi + bands[b].g * x;
      const double zi = bands[b].ar * bands[b].zi + bands[b].ai * bands[b].zr;
      bands[b].zr = zr;
      bands[b].zi = zi;
      bands[b].env = bands[b].lp2.process(bands[b].lp1.process(sqrt(zr * zr + zi * zi)));
    }
    if (++phase == decim)
    {
      phase = 0;
      if (i >= settle)
      {
        for (int b = 0; b < kBands; ++b) env[b].push_back(float(bands[b].env));
      }
    }
  }
  const size_t frames = env[0].size();
  if (frames < 64) return;

  // Beats faster than about half the dominant component spacing are the
  // interference of *resolved* neighbors sharing a measurement band — the
  // primary-resolution rule already handles those pairs, and merging them
  // would mask real partials. Only slower beats demand merging.
  double beatHiHz = 120.;  // conservative when no spacing is known
  if (c.pitched && (c.f0 > 0.f))
  {
    beatHiHz = 0.6 * c.f0;
  }
  if (c.spacingConf >= 0.4f)
  {
    beatHiHz = std::max(beatHiHz, 0.6 * c.spacingDom);
  }
  beatHiHz = std::min(beatHiHz, 250.);

  long modN = 64;
  while (modN < long(frames)) modN *= 2;
  const double modBinHz = envRate / modN;
  const long kBeatLo = std::max(2L, long(15. / modBinHz));
  const long kBeatHi = std::min(modN / 2 - 1, long(beatHiHz / modBinHz));
  const long kTotLo = std::max(1L, long(2. / modBinHz));
  if (kBeatHi <= kBeatLo) return;

  RealFFT modFft(modN);
  std::vector<float> me(modN), mre(modFft.bins()), mim(modFft.bins());
  std::vector<double> beatHist(kBeatHi + 1, 0.);
  double beatE = 0., modE = 0., weightSum = 0.;

  // bands carrying no meaningful signal energy have float-noise envelopes
  // with huge relative modulation; exclude them (-50 dB of the strongest).
  // Bands whose spectrum does not stand well above the noise floor are
  // noise, not beating tones; exclude them too
  double bandMeans[kBands];
  double bestBandMean = 0.;
  bool tonal[kBands];
  for (int b = 0; b < kBands; ++b)
  {
    double mean = 0.;
    for (float v : env[b]) mean += v;
    bandMeans[b] = mean / frames;
    bestBandMean = std::max(bestBandMean, bandMeans[b]);

    tonal[b] = false;
    if (!c.avgDb.empty())
    {
      const long kLo = std::max(1L, long(bands[b].bandLo / c.binHz));
      const long kHi = std::min(long(c.avgDb.size()) - 1, long(bands[b].bandHi / c.binHz));
      // same significance bar as the stage-2 peak population; a decaying
      // strike's modes are diluted by the time average, so this must not
      // be stricter than the peak finder's own threshold
      for (long k = kLo; k <= kHi; ++k)
      {
        if (c.avgDb[k] >= c.floorDb[k] + 15.f)
        {
          tonal[b] = true;
          break;
        }
      }
    }
  }

  for (int b = 0; b < kBands; ++b)
  {
    const double mean = bandMeans[b];
    // envelopes are amplitude-like: -50 dB energy gate on mean²
    if ((mean * mean <= bestBandMean * bestBandMean * 1e-5) || !tonal[b]) continue;
    std::fill(me.begin(), me.end(), 0.f);
    // Hann window: without it, slow envelope drift (decays!) leaks a
    // broadband floor across the modulation spectrum that swamps the
    // beat band
    for (size_t i = 0; i < frames; ++i)
    {
      const float w = 0.5f - 0.5f * cosf(2.f * float(kPi) * i / (frames - 1));
      me[i] = w * float(env[b][i] / mean - 1.);
    }
    modFft.forward(me.data(), mre.data(), mim.data());

    std::vector<double> p(kBeatHi + 2, 0.);
    double bandModTotal = 0.;
    for (long k = kTotLo; k <= kBeatHi; ++k)
    {
      p[k] = double(mre[k]) * mre[k] + double(mim[k]) * mim[k];
      bandModTotal += p[k];
    }
    // weight bands by their share of signal energy (amplitude envelope
    // squared)
    const double w = mean * mean;
    weightSum += w;
    modE += w * bandModTotal;
    for (long k = kBeatLo; k <= kBeatHi; ++k)
    {
      beatE += w * p[k];
      beatHist[k] += w * p[k];
    }
  }

  // Two complementary statistics, each guarding against a different
  // impostor. beatIndexDb is the *absolute* beat-band modulation index: how
  // much AM, in dB, the tonal band envelopes carry at beat rates. Real
  // beating measures -10..+20 dB; the resonator's residual
  // negative-frequency image ripple is ~-33 dB, so an absolute threshold
  // rejects it even on sounds with no other modulation (where a fraction's
  // denominator collapses and promotes the artifact to 100%). The 3/8 Hann
  // power gain is a constant offset absorbed by the threshold.
  // beatFraction is the beat band's share of all modulation from 2 Hz up:
  // sounds ruled by slow modulation (vibrato, tremolo, breath) have real
  // but subordinate beat-rate energy and analyze best in the sparse regime.
  if (weightSum > 0.)
  {
    c.beatIndexDb = float(10. * log10(std::max(1e-12, beatE / weightSum / frames)));
    if (modE > 0.) c.beatFraction = float(beatE / modE);
    double cum = 0.;
    for (long k = kBeatLo; k <= kBeatHi; ++k)
    {
      cum += beatHist[k];
      if (cum >= 0.9 * beatE)
      {
        c.beatRate90 = float(k * modBinHz);
        break;
      }
    }
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
// stage 3: the window decision.
//
// A component pair at spacing d has three fates under a window of main-lobe
// width W: merged (d < 0.3·W, its beat is tracked as an amplitude envelope
// — correct), resolved (d > 0.5·W, two partials — correct), or the
// partially-resolved bad zone in between, where unstable candidates get
// masked or rejected frame by frame and their energy is misclassified as
// noise.
//
// The window floor comes from temporal stability, not frequency spacing:
// when the tonal band envelopes carry audibly-loud fast modulation
// (beating decays, chords, vocal roughness), the analysis must merge that
// modulation cluster and track it as amplitude envelopes — W >= rate/0.3 —
// with the frame rate (= W) keeping up. Spacing statistics cannot see this
// need: sub-resolution pairs merge in the averaged spectrum, and a share-
// of-modulation gate hides a vocal's real fast roughness behind its louder
// slow vibrato (the old beatFraction gate discarded exactly the sounds
// that needed the wide window — huygens measured index +12.9 dB, fraction
// 0.03). Only when no such modulation is measured does spacing set the
// window.
//
// Ear calibration (2026-07-18, huygens / gambang2 / cello / 1977):
//   kModIndexGateDb  +5   between cello -4.5 dB (spacing window correct by
//                         ear) and huygens +12.9 dB (needs the mod window)
//   kModRateGateHz   30   rejects the DC skirt of decay envelopes leaking
//                         into the bottom modulation bins (@bell pins at
//                         15.2 Hz, the band edge)
//   kWModLo         150   below, complex sounds smear toward a primitive-
//                         PSOLA time-stretch quality (1977 sweep)
//   kWModHi         300   above, partials never find stability and the
//                         render goes watery (1977's auto 649 was clearly
//                         too high); every ear optimum so far is 150-305

constexpr float kModIndexGateDb = 5.f;
constexpr float kModRateGateHz = 30.f;
constexpr float kWModLo = 150.f;
constexpr float kWModHi = 300.f;

// at least ~3 window lengths within the sound (window length ≈ 8/W s)
float windowFloor(const Ctx& c)
{
  return std::max(kWidthLo, float(24. / std::max(0.05, c.activeDuration)));
}

// resolution moves (quality walk, unpitched material only) keep W = 2·res
void applyResolution(const Ctx& c, AnalyzerParams& p, float resolution)
{
  p.resolution = clampf(resolution, kResolutionLo, kResolutionHi);
  p.windowWidth = clampf(std::max(2.f * p.resolution, windowFloor(c)), kWidthLo, kWidthHi);
}

void chooseWindow(Ctx& c, AnalyzerParams& p)
{
  c.modDriven = (c.beatIndexDb > kModIndexGateDb) && (c.beatRate90 > kModRateGateHz);
  if (c.modDriven)
  {
    // temporal stability drives the window; resolution follows as half the
    // main lobe so everything the window resolves is kept — nothing
    // partially-resolved survives to be masked into noise. Budget pressure
    // is handled by the quality walk's amplitude floor
    c.wDemand = c.beatRate90 / 0.3f;
    const float w = clampf(std::max(clampf(c.wDemand, kWModLo, kWModHi), windowFloor(c)),
                           kWidthLo, kWidthHi);
    p.windowWidth = w;
    p.resolution = clampf(0.5f * w, kResolutionLo, kResolutionHi);
  }
  else if (c.unpitched)
  {
    // spread the partial budget across the occupied band: kept-peak density
    // after ±resolution masking is about one track per 1.25·resolution
    const float band = std::max(200.f, c.hiCut - c.loCut);
    applyResolution(c, p, band / (0.8f * c.budget));
  }
  else
  {
    // 0.8: the masking radius must sit below the smallest real spacing,
    // with headroom for inharmonic stretch and vibrato excursion
    applyResolution(c, p, 0.8f * c.spacingMin);
  }
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
      else if (c.unpitched && !c.modDriven && (p.resolution * 1.25f <= kResolutionHi))
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
    // recreates the unstable-frequency pathology, and mod-driven material
    // must keep resolution = W/2, the merge-clean invariant)
    enum
    {
      kKnobHiCut,
      kKnobFloor,
      kKnobLoCut,
      kKnobRes,
      kNumKnobs
    };
    bool frozen[kNumKnobs] = {false, false, false, !c.unpitched || c.modDriven};
    const float resMin = std::max(kResolutionLo, 0.5f * windowFloor(c));

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
            // finer steps than the down-walk's 6 dB: the floor is the knob
            // that restores quiet high harmonics, and a coarse last step
            // from within a few dB of -90 gets refused wholesale
            trial.ampFloor = std::max(kAmpFloorLo, p.ampFloor - 3.f);
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
        // the ascent accepts up to the full budget, not the down-walk's
        // target: probe p90 is taken at the deliberately-busiest sites and
        // overestimates the achieved global count (measured: probe 55 vs
        // achieved 29 on gambang2), so stopping 8 under the budget starves
        // real quality — cello left 18 partials unspent and lost its highs
        if (trialCnt <= c.budget)
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

  // freqDrift: pass the movement real tracks exhibit at the final hop, but
  // stay under half the resolution so a track cannot capture its neighbor.
  // 3x the measured p95: crossing and interfering tracks censor the
  // hop-delta statistic, so the raw estimate reads low — both tune runs
  // that improved by ear landed at ~2x the old 1.5x rule (gambang2
  // 2.3 -> 4.6; the same lesson was recorded on the Careless polyphony)
  if (!probe.hopDeltas.empty())
  {
    const float p95 = percentile(probe.hopDeltas, 0.95f);
    p.freqDrift = clampf(3.f * p95, 3.f, std::min(kDriftHi, 0.5f * p.resolution));
  }
  else
  {
    p.freqDrift = clampf(0.2f * p.resolution, 3.f, std::min(kDriftHi, 0.5f * p.resolution));
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
  stage1b(c);  // after stage 2: the beat band is capped by the spacing
  chooseWindow(c, out.params);
  out.hiCut = c.hiCut;
  out.budget = c.budget;
  walkQuality(c, out.params, out.hiCut, out);

  // noise regions. Mod-driven: the reassigned spectrum is clean of
  // candidates out to about half the main lobe around each kept partial
  // (the consensus region admits no sign-crossings); junk candidates —
  // sidelobe artifacts, unresolved-pair interference — live beyond ~W/2.
  // The residue-collection radius must stay inside that clean zone, which
  // fixes the window/noiseWidth *ratio* (ear-calibrated: W 170 -> R 50).
  // This also restores self-similarity: every other frequency scale in the
  // analyzer tracks W. Spacing-driven: little junk exists and a small
  // region would drop genuine inter-harmonic noise (regions without a kept
  // peak lose their residue), so regions stay wide: ~3 kept-peak spacings
  // up to the 2 kHz auditory-filter scale.
  constexpr float kWindowToNoiseRatio = 3.4f;
  if (c.modDriven)
  {
    out.params.noiseWidth =
        clampf(out.params.windowWidth / kWindowToNoiseRatio, kNoiseWidthLo, 2000.f);
  }
  else
  {
    out.params.noiseWidth =
        clampf(4.f * out.params.resolution, std::max(kNoiseWidthLo, 250.f), 2000.f);
  }

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
  out.modDriven = c.modDriven;
  out.beatIndexDb = c.beatIndexDb;
  out.beatFraction = c.beatFraction;
  out.beatRateHz = c.beatRate90;
  out.wDemandHz = c.wDemand;
  const long hopSamples = std::max(1L, long(c.sr / out.params.windowWidth));
  out.frameRateHz = float(c.sr / hopSamples);
  out.activeDuration = float(c.activeDuration);
  return out;
}

}  // namespace ml::utu

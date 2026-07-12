
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#include "utuMetrics.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include "utuFFT.h"
#include "utuWindow.h"

namespace ml::utu
{

namespace
{

constexpr double kPi = 3.14159265358979324;

// analysis geometry: 4096-point frames every 512 samples gives band
// envelopes at ~86 Hz (44.1k), enough to observe modulation up to 30 Hz
constexpr long kFrameN = 4096;
constexpr long kHop = 512;
constexpr int kNumBands = 40;

// composite weights: wateriness is the artifact under attack, so it gets
// the largest weight; calibrate against ear rankings as data accumulates
constexpr float kWeightSpectral = 1.0f;
constexpr float kWeightWatery = 2.0f;
constexpr float kWeightTransient = 0.5f;

// band energies per frame: rows = bands, columns = frames
struct BandEnvelopes
{
  std::vector<std::vector<double>> bands;  // [band][frame], linear energy
  std::vector<double> frameEnergy;         // total per frame
};

BandEnvelopes bandEnvelopes(const float* x, size_t n, double sr,
                            const std::vector<long>& bandEdges)
{
  BandEnvelopes env;
  env.bands.assign(kNumBands, {});

  // Kaiser 90 dB analysis window, as elsewhere in the analyzer
  const double shape = kaiser::computeShape(90.);
  std::vector<double> winD(kFrameN);
  kaiser::buildWindow(winD, shape);
  std::vector<float> win(kFrameN);
  for (long i = 0; i < kFrameN; ++i) win[i] = float(winD[i]);

  RealFFT fft(kFrameN);
  const long bins = fft.bins();
  std::vector<float> seg(kFrameN), re(bins), im(bins);

  for (size_t s = 0; s + kFrameN <= n; s += kHop)
  {
    for (long i = 0; i < kFrameN; ++i) seg[i] = x[s + i] * win[i];
    fft.forward(seg.data(), re.data(), im.data());

    double total = 0.;
    for (int b = 0; b < kNumBands; ++b)
    {
      double e = 0.;
      for (long k = bandEdges[b]; k < bandEdges[b + 1]; ++k)
      {
        e += double(re[k]) * re[k] + double(im[k]) * im[k];
      }
      env.bands[b].push_back(e);
      total += e;
    }
    env.frameEnergy.push_back(total);
  }
  return env;
}

// modulation index of one band envelope in the 2-30 Hz range, in dB
// relative to the squared mean (so it is level-independent)
double modulationDb(const std::vector<double>& bandEnv, size_t f0, size_t f1, double frameRate)
{
  const size_t n = f1 - f0;
  if (n < 16) return -120.;
  std::vector<float> e(n);
  double mean = 0.;
  for (size_t i = 0; i < n; ++i) mean += bandEnv[f0 + i];
  mean /= n;
  if (mean <= 0.) return -120.;
  for (size_t i = 0; i < n; ++i) e[i] = float(bandEnv[f0 + i] / mean - 1.);

  long fftN = 16;
  while (fftN < long(n)) fftN *= 2;
  e.resize(fftN, 0.f);
  RealFFT fft(fftN);
  std::vector<float> re(fft.bins()), im(fft.bins());
  fft.forward(e.data(), re.data(), im.data());

  const double binHz = frameRate / fftN;
  const long k2 = std::max(1L, long(2. / binHz));
  const long k30 = std::min(long(fft.bins() - 1), long(30. / binHz));
  double energy = 0.;
  for (long k = k2; k <= k30; ++k)
  {
    energy += double(re[k]) * re[k] + double(im[k]) * im[k];
  }
  // normalize by envelope length so the statistic is duration-independent
  return 10. * log10(std::max(1e-12, energy / n));
}

// 2 ms RMS envelope in dB
std::vector<float> rmsEnvelopeDb(const float* x, size_t n, double sr, size_t& blockSizeOut)
{
  const size_t blockSize = std::max(size_t(1), size_t(0.002 * sr + 0.5));
  blockSizeOut = blockSize;
  const size_t nBlocks = n / blockSize;
  std::vector<float> env(nBlocks, -160.f);
  for (size_t b = 0; b < nBlocks; ++b)
  {
    double acc = 0.;
    const float* p = x + b * blockSize;
    for (size_t i = 0; i < blockSize; ++i) acc += double(p[i]) * p[i];
    env[b] = 10.f * log10f(float(std::max(1e-16, acc / blockSize)));
  }
  return env;
}

}  // namespace

ReconstructionScore scoreReconstruction(const float* src, size_t nSrc, const float* render,
                                        size_t nRender, double sr)
{
  ReconstructionScore score;
  const size_t n = std::min(nSrc, nRender);
  if (n < size_t(2 * kFrameN)) return score;

  // log-spaced band edges, 60 Hz .. 0.45·sr
  std::vector<long> bandEdges(kNumBands + 1);
  const double fLo = 60., fHi = 0.45 * sr;
  const double binHz = sr / kFrameN;
  for (int b = 0; b <= kNumBands; ++b)
  {
    const double f = fLo * pow(fHi / fLo, double(b) / kNumBands);
    bandEdges[b] = std::max(1L, long(f / binHz));
  }
  for (int b = 0; b < kNumBands; ++b)
  {
    bandEdges[b + 1] = std::max(bandEdges[b + 1], bandEdges[b] + 1);
  }

  const BandEnvelopes se = bandEnvelopes(src, n, sr, bandEdges);
  const BandEnvelopes re = bandEnvelopes(render, n, sr, bandEdges);
  const size_t frames = std::min(se.frameEnergy.size(), re.frameEnergy.size());
  if (frames < 16) return score;
  const double frameRate = sr / kHop;

  // active frames: source within 60 dB of its loudest frame
  const double maxE = *std::max_element(se.frameEnergy.begin(), se.frameEnergy.end());
  std::vector<size_t> active;
  for (size_t f = 0; f < frames; ++f)
  {
    if (se.frameEnergy[f] > maxE * 1e-6) active.push_back(f);
  }
  if (active.size() < 16) return score;

  // ---- spectral-shape fidelity: mean-offset-removed band dB differences.
  // A global gain error is nearly inaudible; spectral tilt and holes are
  // not, so the mean difference is removed before the RMS
  {
    std::vector<float> diffs;
    diffs.reserve(active.size() * kNumBands);
    double meanDiff = 0.;
    for (size_t f : active)
    {
      for (int b = 0; b < kNumBands; ++b)
      {
        const double sDb = 10. * log10(std::max(1e-14, se.bands[b][f]));
        const double rDb = 10. * log10(std::max(1e-14, re.bands[b][f]));
        diffs.push_back(float(rDb - sDb));
        meanDiff += rDb - sDb;
      }
    }
    meanDiff /= diffs.size();
    double sumSq = 0.;
    for (float d : diffs)
    {
      const double c = d - meanDiff;
      sumSq += c * c;
    }
    score.spectralRmsDb = float(sqrt(sumSq / diffs.size()));
  }

  // ---- wateriness: excess low-rate modulation, per band, over the active
  // span. Only bands carrying meaningful source energy participate (a band
  // 50 dB below the strongest is inaudible detail)
  {
    const size_t f0 = active.front(), f1 = active.back() + 1;
    std::vector<double> bandMean(kNumBands, 0.);
    double bestBand = 0.;
    for (int b = 0; b < kNumBands; ++b)
    {
      for (size_t f = f0; f < f1; ++f) bandMean[b] += se.bands[b][f];
      bandMean[b] /= double(f1 - f0);
      bestBand = std::max(bestBand, bandMean[b]);
    }
    double sum = 0.;
    int count = 0;
    for (int b = 0; b < kNumBands; ++b)
    {
      if (bandMean[b] < bestBand * 1e-5) continue;  // -50 dB
      const double mSrc = modulationDb(se.bands[b], f0, f1, frameRate);
      const double mRen = modulationDb(re.bands[b], f0, f1, frameRate);
      sum += std::max(0., mRen - mSrc);
      ++count;
    }
    score.wateryDb = count ? float(sum / count) : 0.f;
  }

  // ---- transient sharpness: rise-slope deficit at source onsets
  {
    size_t blockSize = 0;
    const auto sEnv = rmsEnvelopeDb(src, n, sr, blockSize);
    const auto rEnv = rmsEnvelopeDb(render, n, sr, blockSize);
    const size_t nb = std::min(sEnv.size(), rEnv.size());
    const float msPerBlock = float(1000. * blockSize / sr);
    const size_t minSep = size_t(0.05 * sr / blockSize);  // 50 ms between onsets

    double deficit = 0.;
    int onsets = 0;
    size_t lastOnset = 0;
    for (size_t b = 1; b < nb; ++b)
    {
      const float sRise = (sEnv[b] - sEnv[b - 1]) / msPerBlock;  // dB per ms
      if ((sRise > 2.f) && ((b - lastOnset) > minSep))
      {
        // best render rise within ±2 blocks, tolerating small time shifts
        float rRise = -1e9f;
        for (long d = -2; d <= 2; ++d)
        {
          const long rb = long(b) + d;
          if ((rb >= 1) && (rb < long(nb)))
          {
            rRise = std::max(rRise, (rEnv[rb] - rEnv[rb - 1]) / msPerBlock);
          }
        }
        deficit += std::max(0.f, sRise - rRise);
        ++onsets;
        lastOnset = b;
      }
    }
    score.transientDeficit = onsets ? float(deficit / onsets) : 0.f;
  }

  score.total = kWeightSpectral * score.spectralRmsDb + kWeightWatery * score.wateryDb +
                kWeightTransient * score.transientDeficit;
  return score;
}

}  // namespace ml::utu

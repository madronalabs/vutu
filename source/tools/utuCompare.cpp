
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

// utucompare: transition-only test and comparison harness for the vutu
// analysis rewrite. Links the old Loris library as the reference engine.
// Subcommands are added milestone by milestone.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
#include "utuSynth.h"

// old Loris, reference engine
#include "Analyzer.h"
#include "AssociateBandwidth.h"
#include "KaiserWindow.h"
#include "Partial.h"
#include "PartialList.h"
#include "ReassignedSpectrum.h"
#include "SpectralPeakSelector.h"
#include "Synthesizer.h"

namespace
{

constexpr double kPi = 3.14159265358979324;

// sample rate for the synthetic @ test signals
constexpr double kSyntheticRate = 44100.;

// ---------------------------------------------------------------------------
// audio file I/O via libsndfile (aiff and wav, correct sample rates —
// the old Loris AiffFile reader misparses the 80-bit rate on this build)

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
// fft-test: RealFFT vs naive double-precision DFT

double dftCompare(ml::utu::RealFFT& fft, const std::vector<float>& x)
{
  const long n = fft.length();
  const long nBins = fft.bins();
  std::vector<float> re(nBins), im(nBins);
  fft.forward(x.data(), re.data(), im.data());

  // reference DFT in double
  double maxMag = 0.;
  std::vector<double> refRe(nBins, 0.), refIm(nBins, 0.);
  for (long k = 0; k < nBins; ++k)
  {
    for (long i = 0; i < n; ++i)
    {
      const double w = 2. * kPi * k * i / n;
      refRe[k] += x[i] * cos(w);
      refIm[k] -= x[i] * sin(w);
    }
    maxMag = std::max(maxMag, sqrt(refRe[k] * refRe[k] + refIm[k] * refIm[k]));
  }

  double maxErr = 0.;
  for (long k = 0; k < nBins; ++k)
  {
    const double dr = re[k] - refRe[k];
    const double di = im[k] - refIm[k];
    maxErr = std::max(maxErr, sqrt(dr * dr + di * di));
  }
  return maxErr / maxMag;
}

int fftTest()
{
  std::mt19937 gen(42);
  std::uniform_real_distribution<float> dist(-1.f, 1.f);
  const double tol = 1e-5;
  int failures = 0;

  for (long n : {512L, 1024L, 4096L, 16384L})
  {
    ml::utu::RealFFT fft(n);

    // impulse, off-bin sine, and noise
    std::vector<float> impulse(n, 0.f);
    impulse[3] = 1.f;
    std::vector<float> sine(n);
    for (long i = 0; i < n; ++i) sine[i] = 0.5f * sinf(2.f * float(kPi) * 17.37f * i / n);
    std::vector<float> noise(n);
    for (auto& v : noise) v = dist(gen);

    for (auto* sig : {&impulse, &sine, &noise})
    {
      double err = dftCompare(fft, *sig);
      const char* name = (sig == &impulse) ? "impulse" : (sig == &sine) ? "sine" : "noise";
      bool ok = err < tol;
      if (!ok) ++failures;
      printf("  N=%5ld %-7s rel err %.3g %s\n", n, name, err, ok ? "OK" : "FAIL");
    }
  }
  printf("fft-test: %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// window-test: kaiser port + scaled reassignment windows vs Loris

int windowTest()
{
  const double tolShape = 1e-12;
  const double tolWin = 1e-12;
  const float tolScaled = 1e-6f;
  int failures = 0;

  for (double sr : {44100., 48000., 96000.})
  {
    for (double widthHz : {50., 100., 184.5, 400., 1000., 3200.})
    {
      for (double sidelobeDb : {60., 80., 90., 95.})
      {
        // shape and length must match Loris exactly
        const double shape = ml::utu::kaiser::computeShape(sidelobeDb);
        const double lorisShape = Loris::KaiserWindow::computeShape(sidelobeDb);
        if (fabs(shape - lorisShape) > tolShape * std::max(1., fabs(lorisShape)))
        {
          printf("  shape mismatch: %g vs %g (sidelobe %g)\n", shape, lorisShape, sidelobeDb);
          ++failures;
        }

        long len = ml::utu::kaiser::computeLength(widthHz / sr, shape);
        const long lorisLen = long(Loris::KaiserWindow::computeLength(widthHz / sr, lorisShape));
        if (len != lorisLen)
        {
          printf("  length mismatch: %ld vs %ld (sr %g width %g)\n", len, lorisLen, sr, widthHz);
          ++failures;
        }
        if (!(len % 2)) ++len;

        // raw windows must match Loris to double precision
        std::vector<double> win(len), winDeriv(len);
        ml::utu::kaiser::buildWindow(win, shape);
        ml::utu::kaiser::buildTimeDerivativeWindow(winDeriv, shape);
        std::vector<double> lorisWin(len), lorisDeriv(len);
        Loris::KaiserWindow::buildWindow(lorisWin, lorisShape);
        Loris::KaiserWindow::buildTimeDerivativeWindow(lorisDeriv, lorisShape);
        double maxErr = 0.;
        for (long k = 0; k < len; ++k)
        {
          maxErr = std::max(maxErr, fabs(win[k] - lorisWin[k]));
          maxErr = std::max(maxErr, fabs(winDeriv[k] - lorisDeriv[k]));
        }
        if (maxErr > tolWin)
        {
          printf("  window mismatch: max err %.3g (sr %g width %g sidelobe %g)\n", maxErr, sr,
                 widthHz, sidelobeDb);
          ++failures;
        }

        // scaled float windows vs the same scaling done in double on the
        // Loris windows (the Loris scaling itself lives inside
        // ReassignedSpectrum; replicate it here per ReassignedSpectrum.C:644)
        auto rw = ml::utu::buildReassignmentWindows(sr, widthHz, sidelobeDb);
        if (rw.length != len)
        {
          printf("  reassignment window length mismatch: %ld vs %ld\n", rw.length, len);
          ++failures;
          continue;
        }
        double winsum = 0.;
        for (auto v : lorisWin) winsum += v;
        const double magScale = 2. / winsum;
        const double fancyScale = len / (winsum * kPi);
        const double center = 0.5 * (len - 1);
        float maxScaledErr = 0.f;
        for (long k = 0; k < len; ++k)
        {
          maxScaledErr =
              std::max(maxScaledErr, fabsf(rw.w[k] - float(magScale * lorisWin[k])));
          maxScaledErr = std::max(maxScaledErr,
                                  fabsf(rw.wFreqRamp[k] - float(fancyScale * lorisDeriv[k])));
          maxScaledErr = std::max(
              maxScaledErr,
              fabsf(rw.wTimeRamp[k] - float(magScale * lorisWin[k] * (k - center))));
        }
        if (maxScaledErr > tolScaled)
        {
          printf("  scaled window mismatch: max err %.3g (sr %g width %g sidelobe %g)\n",
                 maxScaledErr, sr, widthHz, sidelobeDb);
          ++failures;
        }
      }
    }
  }
  printf("window-test: %s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// spectrum-test: reassigned spectrum vs Loris on real audio (M2 + M3)

int spectrumTest(const char* path)
{
  std::vector<float> samplesF;
  double sr = 0.;
  if (!loadAudio(path, samplesF, sr)) return 2;
  std::vector<double> samplesD(samplesF.begin(), samplesF.end());
  const long nSamples = long(samplesD.size());
  printf("%s: %ld samples at %g Hz\n", path, nSamples, sr);

  // vutu-typical analysis setup: resolution 80 Hz, window width 160 Hz
  const double widthHz = 160.;
  const double sidelobeDb = 90.;

  // Loris reference spectrum, built exactly as Analyzer::analyze does
  const double shape = Loris::KaiserWindow::computeShape(sidelobeDb);
  long winlen = long(Loris::KaiserWindow::computeLength(widthHz / sr, shape));
  if (!(winlen % 2)) ++winlen;
  std::vector<double> win(winlen), winDeriv(winlen);
  Loris::KaiserWindow::buildWindow(win, shape);
  Loris::KaiserWindow::buildTimeDerivativeWindow(winDeriv, shape);
  Loris::ReassignedSpectrum lorisSpectrum(win, winDeriv);

  ml::utu::ReassignedSpectrum spectrum;
  spectrum.configure(ml::utu::buildReassignmentWindows(sr, widthHz, sidelobeDb));
  if (spectrum.windowLength() != winlen)
  {
    printf("window length mismatch: %ld vs %ld\n", spectrum.windowLength(), winlen);
    return 1;
  }
  const long n = spectrum.fftSize();
  const long half = winlen / 2;
  printf("winlen %ld, fft size %ld\n", winlen, n);

  const long hopSamples = long((1. / widthHz) * sr);
  const double* bufBegin = samplesD.data();
  const double* bufEnd = bufBegin + nSamples;

  // corrections are ratios with |Xh|² denominators, so their float error
  // grows as bins approach the spectral floor; bucket the comparison by
  // level below the frame peak
  struct Bucket
  {
    const char* name;
    double loDb;  // bucket holds bins in (loDb, hiDb] below peak
    double hiDb;
    double maxMagRelErr{0}, maxFreqCorrErr{0}, maxTimeCorrErr{0}, maxPhaseErr{0};
    long bins{0};
  };
  Bucket buckets[3] = {{"  0..-40dB", -40., 0., 0, 0, 0, 0, 0},
                       {"-40..-60dB", -60., -40., 0, 0, 0, 0, 0},
                       {"-60..-90dB", -90., -60., 0, 0, 0, 0, 0}};
  long frames = 0, binsCompared = 0;
  for (long center = 0; center < nSamples; center += hopSamples, ++frames)
  {
    const double* winMiddle = bufBegin + center;
    const double* sampsBegin = std::max(winMiddle - half, bufBegin);
    const double* sampsEnd = std::min(winMiddle + half + 1, bufEnd);
    lorisSpectrum.transform(sampsBegin, winMiddle, sampsEnd);
    spectrum.transform(samplesF.data(), nSamples, center);

    double framePeak = 0.;
    for (long k = 0; k <= n / 2; ++k)
    {
      framePeak = std::max(framePeak, lorisSpectrum.reassignedMagnitude(k));
    }
    if (framePeak < 1e-7) continue;  // silent frame
    const double magFloor = framePeak * 3.16e-5;  // -90 dB

    for (long k = 1; k < n / 2; ++k)
    {
      const double lorisMag = lorisSpectrum.reassignedMagnitude(k);
      if (lorisMag < magFloor) continue;
      ++binsCompared;
      const double db = 20. * log10(lorisMag / framePeak);
      Bucket& b = (db > -40.) ? buckets[0] : (db > -60.) ? buckets[1] : buckets[2];
      ++b.bins;

      b.maxMagRelErr =
          std::max(b.maxMagRelErr, fabs(spectrum.magnitudeAt(k) - lorisMag) / lorisMag);
      const double lorisFreqCorr = lorisSpectrum.reassignedFrequency(k) - double(k);
      const double lorisTimeCorr = lorisSpectrum.reassignedTime(k);
      b.maxFreqCorrErr =
          std::max(b.maxFreqCorrErr, fabs(spectrum.frame().freqCorr[k] - lorisFreqCorr));
      b.maxTimeCorrErr =
          std::max(b.maxTimeCorrErr, fabs(spectrum.frame().timeCorr[k] - lorisTimeCorr));

      // compare phases only where the correction terms are moderate, as at
      // real peaks; phase wraps make direct comparison meaningless when the
      // time correction is large
      if (fabs(lorisTimeCorr) < hopSamples && fabs(lorisFreqCorr) < 2.)
      {
        double dp = spectrum.phaseAt(k) - lorisSpectrum.reassignedPhase(k);
        dp = fabs(remainder(dp, 2. * kPi));
        b.maxPhaseErr = std::max(b.maxPhaseErr, dp);
      }
    }
  }

  printf("%ld frames, %ld bins >-90dB compared\n", frames, binsCompared);
  printf("%-11s %10s %10s %10s %10s %9s\n", "bucket", "magRel", "freqCorr", "timeCorr",
         "phase", "bins");
  for (const Bucket& b : buckets)
  {
    printf("%-11s %10.3g %10.3g %10.3g %10.3g %9ld\n", b.name, b.maxMagRelErr,
           b.maxFreqCorrErr, b.maxTimeCorrErr, b.maxPhaseErr, b.bins);
  }

  // gate on the well-conditioned bins; the analyzer only forms breakpoints
  // at spectral peaks, and floor-bin corrections only affect which
  // candidates get amplitude-rejected
  const Bucket& top = buckets[0];
  const bool pass = (top.maxMagRelErr < 1e-4) && (top.maxFreqCorrErr < 1e-3) &&
                    (top.maxTimeCorrErr < 1e-2) && (top.maxPhaseErr < 1e-2);
  printf("spectrum-test: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// peaks-test: peak selection + thinning vs Loris (M4)

// literal copy of Loris Analyzer::thinPeaks (private there), on Loris peaks
Loris::Peaks::iterator lorisThinPeaks(Loris::Peaks& peaks, double frameTime, double ampFloordB,
                                      double freqResolution)
{
  const double fadeRangedB = 10.0;
  const double threshold = std::pow(10., 0.05 * ampFloordB);
  const double beginFade = std::pow(10., 0.05 * (ampFloordB + fadeRangedB));

  std::sort(peaks.begin(), peaks.end(), Loris::SpectralPeak::sort_greater_amplitude);

  peaks.erase(std::remove_if(peaks.begin(), peaks.end(),
                             [frameTime](const Loris::SpectralPeak& v)
                             { return 0 > (v.time() + frameTime); }),
              peaks.end());

  auto it = peaks.begin();
  auto beginRejected = it;
  while (it != peaks.end())
  {
    Loris::SpectralPeak& pk = *it;
    const double lower = pk.frequency() - freqResolution;
    const double upper = pk.frequency() + freqResolution;
    const bool masked =
        beginRejected != std::find_if(peaks.begin(), beginRejected,
                                      [lower, upper](const Loris::SpectralPeak& v) {
                                        return (v.frequency() > lower) &&
                                               (v.frequency() < upper);
                                      });
    if (pk.amplitude() > threshold && !masked)
    {
      if (pk.amplitude() < beginFade)
      {
        double alpha = (beginFade - pk.amplitude()) / (beginFade - threshold);
        pk.setAmplitude(pk.amplitude() * (1. - alpha));
      }
      if (it != beginRejected)
      {
        std::swap(*it, *beginRejected);
      }
      ++beginRejected;
    }
    ++it;
  }
  return beginRejected;
}

// with withBandwidth, both engines also run residue bandwidth association
// (M5) and matched-peak bw/adjusted-amp errors are gated
int peaksTest(const char* path, bool withBandwidth)
{
  std::vector<float> samplesF;
  double sr = 0.;
  if (!loadAudio(path, samplesF, sr)) return 2;
  std::vector<double> samplesD(samplesF.begin(), samplesF.end());
  const long nSamples = long(samplesD.size());

  const double resolutionHz = 80.;
  const double widthHz = 160.;
  const double sidelobeDb = 90.;
  const double ampFloorDb = -90.;
  const double freqFloorHz = resolutionHz;  // Loris default: freqFloor = resolution

  const double shape = Loris::KaiserWindow::computeShape(sidelobeDb);
  long winlen = long(Loris::KaiserWindow::computeLength(widthHz / sr, shape));
  if (!(winlen % 2)) ++winlen;
  std::vector<double> win(winlen), winDeriv(winlen);
  Loris::KaiserWindow::buildWindow(win, shape);
  Loris::KaiserWindow::buildTimeDerivativeWindow(winDeriv, shape);
  Loris::ReassignedSpectrum lorisSpectrum(win, winDeriv);

  const double hopTime = 1. / widthHz;
  const double cropTime = hopTime;
  const double bwRegionWidthHz = 2000.;
  Loris::SpectralPeakSelector lorisSelector(sr, cropTime);
  Loris::AssociateBandwidth lorisBw(bwRegionWidthHz, sr);

  ml::utu::ReassignedSpectrum spectrum;
  spectrum.configure(ml::utu::buildReassignmentWindows(sr, widthHz, sidelobeDb));
  ml::utu::PeakSelector selector;
  selector.configure(float(sr), float(cropTime));
  ml::utu::AssociateBandwidth bwAssociator;
  bwAssociator.configure(float(bwRegionWidthHz), float(sr));

  const long hopSamples = long(hopTime * sr);
  const long half = winlen / 2;
  const double* bufBegin = samplesD.data();
  const double* bufEnd = bufBegin + nSamples;

  long frames = 0;
  long lorisKeptTotal = 0, keptTotal = 0, matchedTotal = 0;
  long lorisRejTotal = 0, rejTotal = 0;
  double maxFreqErr = 0., maxAmpRelErr = 0., maxPhaseErr = 0., maxTimeErr = 0., maxBwErr = 0.;
  // a single borderline peak landing on the other side of the kept/rejected
  // partition shifts the residue energy of its whole region, so bw agreement
  // is only meaningful on frames where both engines partition identically
  long partitionDiffFrames = 0;
  double maxBwErrSamePartition = 0.;
  ml::utu::PeakFrame frame;

  for (long center = 0; center < nSamples; center += hopSamples, ++frames)
  {
    const double frameTime = double(center) / sr;

    const double* winMiddle = bufBegin + center;
    lorisSpectrum.transform(std::max(winMiddle - half, bufBegin), winMiddle,
                            std::min(winMiddle + half + 1, bufEnd));
    Loris::Peaks lorisPeaks = lorisSelector.selectPeaks(lorisSpectrum, freqFloorHz);
    auto lorisRejected = lorisThinPeaks(lorisPeaks, frameTime, ampFloorDb, resolutionHz);
    const long lorisKept = long(lorisRejected - lorisPeaks.begin());

    spectrum.transform(samplesF.data(), nSamples, center);
    selector.selectPeaks(spectrum, float(freqFloorHz), frame);
    ml::utu::thinPeaks(frame, float(resolutionHz), float(ampFloorDb), frameTime);

    if (withBandwidth)
    {
      lorisBw.associateBandwidth(lorisPeaks.begin(), lorisRejected, lorisPeaks.end());
      bwAssociator.associateBandwidth(frame);
    }

    lorisKeptTotal += lorisKept;
    keptTotal += long(frame.numKept);
    lorisRejTotal += long(lorisPeaks.size()) - lorisKept;
    rejTotal += long(frame.peaks.size() - frame.numKept);

    // match kept sets by frequency, greedy two-pointer over freq-sorted lists
    std::vector<const Loris::SpectralPeak*> lk;
    for (long i = 0; i < lorisKept; ++i) lk.push_back(&lorisPeaks[i]);
    std::sort(lk.begin(), lk.end(),
              [](auto* a, auto* b) { return a->frequency() < b->frequency(); });
    std::vector<const ml::utu::Peak*> mk;
    for (size_t i = 0; i < frame.numKept; ++i) mk.push_back(&frame.peaks[i]);
    std::sort(mk.begin(), mk.end(), [](auto* a, auto* b) { return a->freq < b->freq; });

    const double matchTolHz = 0.5;
    size_t a = 0, b = 0;
    long matchedThisFrame = 0;
    double frameBwErr = 0.;
    while (a < lk.size() && b < mk.size())
    {
      const double fa = lk[a]->frequency();
      const double fb = mk[b]->freq;
      if (fabs(fa - fb) < matchTolHz)
      {
        ++matchedThisFrame;
        maxFreqErr = std::max(maxFreqErr, fabs(fa - fb));
        maxAmpRelErr = std::max(
            maxAmpRelErr, fabs(mk[b]->amp - lk[a]->amplitude()) / lk[a]->amplitude());
        maxTimeErr = std::max(maxTimeErr, fabs(mk[b]->timeOffset - lk[a]->time()));
        double dp = remainder(mk[b]->phase - lk[a]->createBreakpoint().phase(), 2. * kPi);
        maxPhaseErr = std::max(maxPhaseErr, fabs(dp));
        frameBwErr = std::max(frameBwErr, fabs(mk[b]->bw - lk[a]->bandwidth()));
        ++a;
        ++b;
      }
      else if (fa < fb)
        ++a;
      else
        ++b;
    }
    matchedTotal += matchedThisFrame;
    maxBwErr = std::max(maxBwErr, frameBwErr);
    const bool samePartition = (long(frame.numKept) == lorisKept) &&
                               (frame.peaks.size() == lorisPeaks.size()) &&
                               (matchedThisFrame == lorisKept);
    if (samePartition)
    {
      maxBwErrSamePartition = std::max(maxBwErrSamePartition, frameBwErr);
    }
    else
    {
      ++partitionDiffFrames;
    }
  }

  const double keptDrift =
      fabs(double(keptTotal - lorisKeptTotal)) / std::max(1L, lorisKeptTotal);
  const double unmatched = 1. - double(matchedTotal) / std::max(1L, lorisKeptTotal);
  printf("%ld frames\n", frames);
  printf("  kept peaks:     %ld loris, %ld new (drift %.3g%%)\n", lorisKeptTotal, keptTotal,
         100. * keptDrift);
  printf("  rejected peaks: %ld loris, %ld new\n", lorisRejTotal, rejTotal);
  printf("  matched: %ld (unmatched %.3g%%)\n", matchedTotal, 100. * unmatched);
  printf("  max matched err: freq %.3g Hz, amp rel %.3g, time %.3g s, phase %.3g rad\n",
         maxFreqErr, maxAmpRelErr, maxTimeErr, maxPhaseErr);
  if (withBandwidth)
  {
    printf("  max matched bw err: %.3g overall; %.3g on the %ld/%ld frames with identical"
           " partitions\n",
           maxBwErr, maxBwErrSamePartition, frames - partitionDiffFrames, frames);
  }

  bool pass = (keptDrift < 0.02) && (unmatched < 0.02);
  if (withBandwidth)
  {
    // the meaningful agreement check is bw on identically-partitioned
    // frames; the fraction of frames with any partition difference grew
    // when file rates were read correctly (44.1k frames hold many more
    // near-floor borderline candidates than the old 11 kHz misread), so
    // that gate is loose
    pass = pass && (maxBwErrSamePartition < 0.02) &&
           (double(partitionDiffFrames) / frames < 0.15);
  }
  printf("%s: %s\n", withBandwidth ? "bandwidth-test" : "peaks-test", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// analyze-test: full analysis pipeline vs Loris::Analyzer (M6 + M7)

struct TrackInfo
{
  double t0{0}, t1{0};  // start/end time in seconds
  double f0{0};         // frequency at start
  double energy{0};     // sum of amp²·dt over segments
  long nbp{0};
  int index{-1};
  bool matched{false};
};

// linear interpolation into a VutuPartial at time t (within its range)
void utuPartialAt(const ml::VutuPartial& p, double t, double& freq, double& amp)
{
  size_t i1 = 0, i2 = 1;
  for (size_t i = 1; i < p.time.size(); ++i)
  {
    if (t < p.time[i])
    {
      i1 = i - 1;
      i2 = i;
      break;
    }
    i1 = i - 1;
    i2 = i;
  }
  const double t1 = p.time[i1], t2 = p.time[i2];
  const double frac = (t2 > t1) ? (t - t1) / (t2 - t1) : 0.;
  freq = p.freq[i1] + frac * (p.freq[i2] - p.freq[i1]);
  amp = p.amp[i1] + frac * (p.amp[i2] - p.amp[i1]);
}

int analyzeTest(const char* path)
{
  std::vector<float> samplesF;
  double sr = 0.;
  if (!loadAudio(path, samplesF, sr)) return 2;
  std::vector<double> samplesD(samplesF.begin(), samplesF.end());
  const long nSamples = long(samplesD.size());
  printf("%s: %ld samples at %g Hz\n", path, nSamples, sr);

  const double resolutionHz = 80.;
  const double widthHz = 160.;

  // Loris reference: full analyzer, all defaults from (res, width)
  Loris::Analyzer lorisAnalyzer(resolutionHz, widthHz);
  Loris::PartialList lorisPartials =
      lorisAnalyzer.analyze(samplesD.data(), samplesD.data() + nSamples, sr);

  ml::utu::AnalyzerParams params;
  params.sampleRate = float(sr);
  params.resolution = float(resolutionHz);
  params.windowWidth = float(widthHz);
  auto result = ml::utu::analyzeToPartials(samplesF.data(), nSamples, params);

  // collect track info from both engines
  std::vector<TrackInfo> lt, mt;
  {
    int idx = 0;
    for (const Loris::Partial& p : lorisPartials)
    {
      TrackInfo t;
      t.t0 = p.startTime();
      t.t1 = p.endTime();
      t.f0 = p.first().frequency();
      t.nbp = long(p.numBreakpoints());
      auto it = p.begin();
      double prevT = it.time(), prevA = it.breakpoint().amplitude();
      for (++it; it != p.end(); ++it)
      {
        const double dt = it.time() - prevT;
        const double a = it.breakpoint().amplitude();
        t.energy += 0.5 * (a * a + prevA * prevA) * dt;
        prevT = it.time();
        prevA = a;
      }
      t.index = idx++;
      lt.push_back(t);
    }
    idx = 0;
    for (const ml::VutuPartial& p : result->partials)
    {
      TrackInfo t;
      t.t0 = p.time.front();
      t.t1 = p.time.back();
      t.f0 = p.freq.front();
      t.nbp = long(p.time.size());
      for (size_t i = 1; i < p.time.size(); ++i)
      {
        const double dt = double(p.time[i]) - p.time[i - 1];
        t.energy += 0.5 * (double(p.amp[i]) * p.amp[i] + double(p.amp[i - 1]) * p.amp[i - 1]) * dt;
      }
      t.index = idx++;
      mt.push_back(t);
    }
  }

  // greedy matching: tracks born from the same peak stream have nearly
  // identical start time and start frequency
  auto byStart = [](const TrackInfo& a, const TrackInfo& b)
  { return (a.t0 != b.t0) ? (a.t0 < b.t0) : (a.f0 < b.f0); };
  std::sort(lt.begin(), lt.end(), byStart);
  std::sort(mt.begin(), mt.end(), byStart);

  const double hopTime = 1. / widthHz;
  const double startTimeTol = 1.5 * hopTime;
  const double startFreqTol = std::max(1., resolutionHz / 4.);

  double lorisEnergyTotal = 0., matchedEnergy = 0.;
  long matched = 0;
  double freqCentsSumSq = 0., ampRelSumSq = 0.;
  long comparePoints = 0;
  size_t searchBegin = 0;
  for (auto& L : lt)
  {
    lorisEnergyTotal += L.energy;
    while ((searchBegin < mt.size()) && (mt[searchBegin].t0 < L.t0 - startTimeTol))
    {
      ++searchBegin;
    }
    int best = -1;
    double bestDf = startFreqTol;
    for (size_t j = searchBegin; (j < mt.size()) && (mt[j].t0 <= L.t0 + startTimeTol); ++j)
    {
      if (mt[j].matched) continue;
      const double df = fabs(mt[j].f0 - L.f0);
      if (df < bestDf)
      {
        bestDf = df;
        best = int(j);
      }
    }
    if (best < 0) continue;

    TrackInfo& M = mt[best];
    M.matched = true;
    L.matched = true;
    ++matched;
    matchedEnergy += L.energy;

    // sample freq/amp over the overlap
    const double o0 = std::max(L.t0, M.t0), o1 = std::min(L.t1, M.t1);
    if (o1 <= o0) continue;
    const Loris::Partial* lp = nullptr;
    {  // recover the loris partial by index
      long k = 0;
      for (const Loris::Partial& p : lorisPartials)
      {
        if (k++ == L.index)
        {
          lp = &p;
          break;
        }
      }
    }
    const ml::VutuPartial& mp = result->partials[M.index];
    const int kPoints = 9;
    for (int k = 1; k < kPoints - 1; ++k)
    {
      const double t = o0 + (o1 - o0) * k / (kPoints - 1);
      double mf, ma;
      utuPartialAt(mp, t, mf, ma);
      const double lf = lp->frequencyAt(t);
      const double la = lp->amplitudeAt(t);
      if ((lf > 0.) && (mf > 0.))
      {
        const double cents = 1200. * log2(mf / lf);
        freqCentsSumSq += cents * cents;
        ++comparePoints;
        if (la > 1e-7)
        {
          const double rel = (ma - la) / la;
          ampRelSumSq += rel * rel;
        }
      }
    }
  }

  const double countDrift =
      fabs(double(long(mt.size()) - long(lt.size()))) / std::max(size_t(1), lt.size());
  const double energyFrac = matchedEnergy / std::max(1e-12, lorisEnergyTotal);
  const double freqRmsCents = sqrt(freqCentsSumSq / std::max(1L, comparePoints));
  const double ampRms = sqrt(ampRelSumSq / std::max(1L, comparePoints));

  printf("  partials: %zu loris, %zu new (drift %.3g%%)\n", lt.size(), mt.size(),
         100. * countDrift);
  printf("  matched tracks: %ld (%.4g%% of loris energy)\n", matched, 100. * energyFrac);
  printf("  freq RMS %.3g cents, amp env RMS rel %.3g (%ld points)\n", freqRmsCents, ampRms,
         comparePoints);

  const bool pass = (countDrift < 0.05) && (energyFrac > 0.95) && (freqRmsCents < 10.);
  printf("analyze-test: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// synth-test: bandwidth-enhanced rendering vs Loris::Synthesizer (M8)

// vutu partials -> Loris PartialList, as vutu's old _sumuToLorisPartials
Loris::PartialList vutuToLorisPartials(const ml::VutuPartialsData& d)
{
  Loris::PartialList list;
  for (const ml::VutuPartial& sp : d.partials)
  {
    Loris::Partial lp;
    for (size_t i = 0; i < sp.time.size(); ++i)
    {
      Loris::Breakpoint b(sp.freq[i], sp.amp[i], sp.bandwidth[i], sp.phase[i]);
      lp.insert(sp.time[i], b);
    }
    list.push_back(lp);
  }
  return list;
}

double rmsOf(const float* x, long n)
{
  double s = 0.;
  for (long i = 0; i < n; ++i) s += double(x[i]) * x[i];
  return sqrt(s / std::max(1L, n));
}
double rmsOfD(const double* x, long n)
{
  double s = 0.;
  for (long i = 0; i < n; ++i) s += x[i] * x[i];
  return sqrt(s / std::max(1L, n));
}

// one constant partial, breakpoints every 10 ms
ml::VutuPartialsData makeTestPartial(double freq, double amp, double bw, double dur)
{
  ml::VutuPartialsData d;
  ml::VutuPartial p;
  for (double t = 0.; t <= dur; t += 0.01)
  {
    p.time.push_back(float(t));
    p.freq.push_back(float(freq));
    p.amp.push_back(float(amp));
    p.bandwidth.push_back(float(bw));
    p.phase.push_back(0.f);
  }
  d.partials.push_back(p);
  return d;
}

void renderBoth(const ml::VutuPartialsData& d, double sr, double fade,
                std::vector<double>& lorisOut, std::vector<float>& newOut)
{
  Loris::PartialList lp = vutuToLorisPartials(d);
  lorisOut.clear();
  Loris::Synthesizer lorisSynth(sr, lorisOut, fade);
  for (const Loris::Partial& p : lp) lorisSynth.synthesize(p);

  ml::utu::SynthParams sp;
  sp.sampleRate = float(sr);
  sp.fadeTime = float(fade);
  ml::utu::PartialSynthesizer synth;
  synth.setParams(sp);
  synth.render(d, newOut);
}

int synthTest(const char* path)
{
  const double sr = 48000.;
  const double fade = 0.001;
  std::vector<double> lorisOut;
  std::vector<float> newOut;

  // pure sine (bw 0): engines should nearly null
  {
    auto d = makeTestPartial(440., 0.5, 0., 2.);
    renderBoth(d, sr, fade, lorisOut, newOut);
    const long n = std::min(lorisOut.size(), newOut.size());
    double maxDiff = 0.;
    for (long i = 0; i < n; ++i)
    {
      maxDiff = std::max(maxDiff, fabs(lorisOut[i] - newOut[i]));
    }
    printf("sine bw=0:  loris RMS %.4f, new RMS %.4f, max sample diff %.3g\n",
           rmsOfD(lorisOut.data(), n), rmsOf(newOut.data(), n), maxDiff);
    if (maxDiff > 1e-3)
    {
      printf("synth-test: FAIL (sine mismatch)\n");
      return 1;
    }
  }

  // full-bandwidth noise: RMS should match after modulator calibration
  double noiseRatio;
  {
    auto d = makeTestPartial(440., 0.5, 1., 2.);
    renderBoth(d, sr, fade, lorisOut, newOut);
    const long n = std::min(lorisOut.size(), newOut.size());
    const long skip = n / 4;  // measure the steady middle
    const double lr = rmsOfD(lorisOut.data() + skip, n / 2);
    const double nr = rmsOf(newOut.data() + skip, n / 2);
    noiseRatio = lr / std::max(1e-12, nr);
    printf("noise bw=1: loris RMS %.4f, new RMS %.4f (gain ratio %.4f)\n", lr, nr, noiseRatio);
  }

  // half bandwidth sanity
  {
    auto d = makeTestPartial(440., 0.5, 0.5, 2.);
    renderBoth(d, sr, fade, lorisOut, newOut);
    const long n = std::min(lorisOut.size(), newOut.size());
    const long skip = n / 4;
    printf("mixed bw=.5: loris RMS %.4f, new RMS %.4f\n", rmsOfD(lorisOut.data() + skip, n / 2),
           rmsOf(newOut.data() + skip, n / 2));
  }

  // full render A/B from a real sound, if given: analyze with the new
  // engine, render the same partials with both synths, compare coarse RMS
  // envelopes and write aiffs for listening
  if (path)
  {
    std::vector<float> samplesF;
    double fsr = 0.;
    if (!loadAudio(path, samplesF, fsr)) return 2;

    ml::utu::AnalyzerParams params;
    params.sampleRate = float(fsr);
    params.resolution = 80.f;
    params.windowWidth = 160.f;
    auto partials = ml::utu::analyzeToPartials(samplesF.data(), samplesF.size(), params);

    // (a) deterministic part: render with bandwidth zeroed; the engines
    // should track each other tightly
    {
      ml::VutuPartialsData det = *partials;
      for (auto& p : det.partials)
      {
        std::fill(p.bandwidth.begin(), p.bandwidth.end(), 0.f);
      }
      renderBoth(det, fsr, fade, lorisOut, newOut);
      const long n = long(std::min(lorisOut.size(), newOut.size()));
      const long win = long(0.05 * fsr);
      double peak = 0.;
      for (long i = 0; i < n; ++i) peak = std::max(peak, fabs(lorisOut[i]));
      double maxEnvRel = 0., sumRel = 0.;
      long envPoints = 0;
      for (long i = 0; i + win <= n; i += win)
      {
        const double lr = rmsOfD(lorisOut.data() + i, win);
        const double nr = rmsOf(newOut.data() + i, win);
        if (lr > 1e-3 * peak)
        {
          const double rel = fabs(nr - lr) / lr;
          maxEnvRel = std::max(maxEnvRel, rel);
          sumRel += rel;
          ++envPoints;
        }
      }
      printf("%s bw=0: envelope rel diff mean %.3g max %.3g (%ld windows)\n", path,
             sumRel / std::max(1L, envPoints), maxEnvRel, envPoints);
      if ((maxEnvRel > 0.1) || (sumRel / std::max(1L, envPoints) > 0.01))
      {
        printf("synth-test: FAIL (deterministic render mismatch)\n");
        return 1;
      }
    }

    // (b) full render: different noise realizations, so gate loosely on
    // global RMS and mean envelope difference; judge finally by listening
    renderBoth(*partials, fsr, fade, lorisOut, newOut);
    const long n = long(std::min(lorisOut.size(), newOut.size()));
    const long win = long(0.05 * fsr);
    double peak = 0.;
    for (long i = 0; i < n; ++i) peak = std::max(peak, fabs(lorisOut[i]));
    double maxEnvRel = 0., sumRel = 0.;
    long envPoints = 0;
    for (long i = 0; i + win <= n; i += win)
    {
      const double lr = rmsOfD(lorisOut.data() + i, win);
      const double nr = rmsOf(newOut.data() + i, win);
      if (lr > 1e-3 * peak)
      {
        const double rel = fabs(nr - lr) / lr;
        maxEnvRel = std::max(maxEnvRel, rel);
        sumRel += rel;
        ++envPoints;
      }
    }
    const double globalRel =
        fabs(rmsOf(newOut.data(), n) - rmsOfD(lorisOut.data(), n)) / rmsOfD(lorisOut.data(), n);
    printf("%s full: global RMS rel diff %.3g, envelope rel diff mean %.3g max %.3g\n", path,
           globalRel, sumRel / std::max(1L, envPoints), maxEnvRel);

    // write renders for listening
    std::vector<float> lorisOutF(lorisOut.begin(), lorisOut.end());
    const int fmt = SF_FORMAT_AIFF | SF_FORMAT_PCM_24;
    writeAudio("utucompare-loris-render.aiff", lorisOutF.data(), lorisOutF.size(), fsr, fmt);
    writeAudio("utucompare-new-render.aiff", newOut.data(), newOut.size(), fsr, fmt);
    printf("wrote utucompare-loris-render.aiff, utucompare-new-render.aiff\n");

    const bool pass = (globalRel < 0.05) && (sumRel / std::max(1L, envPoints)) < 0.15;
    printf("synth-test: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
  }

  printf("synth-test: PASS (calibration only)\n");
  return 0;
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
    // accumulate phase in double: sinf(2π·f·i) loses all precision once the
    // argument grows past ~1e7
    x.resize(size_t(5 * sr));
    double phase = 0.;
    const double inc = 2. * kPi * 440.1 / sr;
    for (size_t i = 0; i < x.size(); ++i)
    {
      x[i] = 0.5f * float(sin(phase));
      phase += inc;
    }
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
  fprintf(f, "  noiseWidth   %8.1f Hz      sidelobe    %8.1f dB\n", r.params.bwRegionWidth,
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
  auto fillSine = [](std::vector<float>& v, double freq, double rate)
  {
    double phase = 0.;
    const double inc = 2. * kPi * freq / rate;
    for (auto& s : v)
    {
      s = 0.5f * float(sin(phase));
      phase += inc;
    }
  };
  if (std::string(path) == "@short")
  {
    // 0.3 s: exercises the short-file guards (Welch size, window floor)
    samplesF.resize(size_t(0.3 * sr));
    fillSine(samplesF, 330., sr);
  }
  else if (std::string(path) == "@sine96")
  {
    sr = 96000.;
    samplesF.resize(size_t(3 * sr));
    fillSine(samplesF, 440., sr);
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
  inRange(r.params.bwRegionWidth, 10, 5000, "noiseWidth");

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
  auto gNoise = [](ml::utu::AnalyzerParams& a) { return &a.bwRegionWidth; };
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
// writing <name>-converted.<ext> beside each source and auto-params.txt with
// the automatically chosen analysis parameters per file

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

}  // namespace

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    printf(
        "usage: utucompare <command>\n"
        "  fft-test               RealFFT vs naive DFT\n"
        "  window-test            Kaiser windows vs Loris\n"
        "  spectrum-test <aiff>   reassigned spectrum vs Loris\n"
        "  peaks-test <aiff>      peak selection + thinning vs Loris\n"
        "  bandwidth-test <aiff>  peaks + residue bandwidth association vs Loris\n"
        "  analyze-test <aiff>    full analysis + phase fix vs Loris::Analyzer\n"
        "  synth-test [aiff]      bandwidth-enhanced synthesis vs Loris::Synthesizer\n"
        "  auto-test <aiff|@sine|@harm|@bell|@noise>  automatic analysis parameters\n"
        "  convert-dir <dir> [budget]   auto-analyze + resynthesize every audio file\n"
        "  score <src> <render>         reconstruction metrics\n"
        "  tune <file> [budget]         coordinate-descent parameter search vs the score\n");
    return 2;
  }
  const std::string cmd(argv[1]);
  if (cmd == "auto-test" && argc > 2) return autoTest(argv[2]);
  if (cmd == "convert-dir" && argc > 2)
    return convertDir(argv[2], argc > 3 ? atoi(argv[3]) : 64);
  if (cmd == "score" && argc > 3) return scoreCmd(argv[2], argv[3]);
  if (cmd == "tune" && argc > 2) return tuneCmd(argv[2], argc > 3 ? atoi(argv[3]) : 64);
  if (cmd == "fft-test") return fftTest();
  if (cmd == "window-test") return windowTest();
  if (cmd == "spectrum-test" && argc > 2) return spectrumTest(argv[2]);
  if (cmd == "peaks-test" && argc > 2) return peaksTest(argv[2], false);
  if (cmd == "bandwidth-test" && argc > 2) return peaksTest(argv[2], true);
  if (cmd == "analyze-test" && argc > 2) return analyzeTest(argv[2]);
  if (cmd == "synth-test") return synthTest(argc > 2 ? argv[2] : nullptr);
  printf("unknown command '%s'\n", cmd.c_str());
  return 2;
}

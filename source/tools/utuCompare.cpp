
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

// utucompare: transition-only test and comparison harness for the vutu
// analysis rewrite. Links the old Loris library as the reference engine.
// Subcommands are added milestone by milestone.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "utuFFT.h"
#include "utuSpectrum.h"
#include "utuWindow.h"

#include "utuAnalyzer.h"
#include "utuBandwidth.h"
#include "utuPeaks.h"
#include "utuSynth.h"

// old Loris, reference engine
#include "AiffFile.h"
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

// The installed libloris's AiffFile misparses the 80-bit extended sample
// rate on this machine (it reports 11332 Hz for every file), so the rate is
// hardcoded for the test sounds rather than read from the file.
constexpr double kFileSampleRate = 44100.;

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
  Loris::AiffFile file(path);
  const double sr = kFileSampleRate;
  std::vector<double>& samplesD = file.samples();
  const long nSamples = long(samplesD.size());
  std::vector<float> samplesF(samplesD.begin(), samplesD.end());
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
  Loris::AiffFile file(path);
  const double sr = kFileSampleRate;
  std::vector<double>& samplesD = file.samples();
  const long nSamples = long(samplesD.size());
  std::vector<float> samplesF(samplesD.begin(), samplesD.end());

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
    pass = pass && (maxBwErrSamePartition < 0.02) &&
           (double(partitionDiffFrames) / frames < 0.02);
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
  Loris::AiffFile file(path);
  const double sr = kFileSampleRate;
  std::vector<double>& samplesD = file.samples();
  const long nSamples = long(samplesD.size());
  std::vector<float> samplesF(samplesD.begin(), samplesD.end());
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
    Loris::AiffFile file(path);
    const double fsr = kFileSampleRate;
    std::vector<double>& samplesD = file.samples();
    std::vector<float> samplesF(samplesD.begin(), samplesD.end());

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
    std::vector<double> newOutD(newOut.begin(), newOut.end());
    Loris::AiffFile fL(lorisOut.data(), lorisOut.size(), fsr);
    fL.write("utucompare-loris-render.aiff");
    Loris::AiffFile fN(newOutD.data(), newOutD.size(), fsr);
    fN.write("utucompare-new-render.aiff");
    printf("wrote utucompare-loris-render.aiff, utucompare-new-render.aiff\n");

    const bool pass = (globalRel < 0.05) && (sumRel / std::max(1L, envPoints)) < 0.15;
    printf("synth-test: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
  }

  printf("synth-test: PASS (calibration only)\n");
  return 0;
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
        "  synth-test [aiff]      bandwidth-enhanced synthesis vs Loris::Synthesizer\n");
    return 2;
  }
  const std::string cmd(argv[1]);
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


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

#include "utuPeaks.h"

// old Loris, reference engine
#include "AiffFile.h"
#include "KaiserWindow.h"
#include "ReassignedSpectrum.h"
#include "SpectralPeakSelector.h"

namespace
{

constexpr double kPi = 3.14159265358979324;

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
  const double sr = file.sampleRate();
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

int peaksTest(const char* path)
{
  Loris::AiffFile file(path);
  const double sr = file.sampleRate();
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
  Loris::SpectralPeakSelector lorisSelector(sr, cropTime);

  ml::utu::ReassignedSpectrum spectrum;
  spectrum.configure(ml::utu::buildReassignmentWindows(sr, widthHz, sidelobeDb));
  ml::utu::PeakSelector selector;
  selector.configure(float(sr), float(cropTime));

  const long hopSamples = long(hopTime * sr);
  const long half = winlen / 2;
  const double* bufBegin = samplesD.data();
  const double* bufEnd = bufBegin + nSamples;

  long frames = 0;
  long lorisKeptTotal = 0, keptTotal = 0, matchedTotal = 0;
  long lorisRejTotal = 0, rejTotal = 0;
  double maxFreqErr = 0., maxAmpRelErr = 0., maxPhaseErr = 0., maxTimeErr = 0.;
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
    while (a < lk.size() && b < mk.size())
    {
      const double fa = lk[a]->frequency();
      const double fb = mk[b]->freq;
      if (fabs(fa - fb) < matchTolHz)
      {
        ++matchedTotal;
        maxFreqErr = std::max(maxFreqErr, fabs(fa - fb));
        maxAmpRelErr = std::max(
            maxAmpRelErr, fabs(mk[b]->amp - lk[a]->amplitude()) / lk[a]->amplitude());
        maxTimeErr = std::max(maxTimeErr, fabs(mk[b]->timeOffset - lk[a]->time()));
        double dp = remainder(mk[b]->phase - lk[a]->createBreakpoint().phase(), 2. * kPi);
        maxPhaseErr = std::max(maxPhaseErr, fabs(dp));
        ++a;
        ++b;
      }
      else if (fa < fb)
        ++a;
      else
        ++b;
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

  const bool pass = (keptDrift < 0.02) && (unmatched < 0.02);
  printf("peaks-test: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
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
        "  peaks-test <aiff>      peak selection + thinning vs Loris\n");
    return 2;
  }
  const std::string cmd(argv[1]);
  if (cmd == "fft-test") return fftTest();
  if (cmd == "window-test") return windowTest();
  if (cmd == "spectrum-test" && argc > 2) return spectrumTest(argv[2]);
  if (cmd == "peaks-test" && argc > 2) return peaksTest(argv[2]);
  printf("unknown command '%s'\n", cmd.c_str());
  return 2;
}

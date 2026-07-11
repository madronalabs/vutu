
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
#include "utuWindow.h"

// old Loris, reference engine
#include "KaiserWindow.h"

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

}  // namespace

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    printf(
        "usage: utucompare <command>\n"
        "  fft-test      RealFFT vs naive DFT\n"
        "  window-test   Kaiser windows vs Loris\n");
    return 2;
  }
  const std::string cmd(argv[1]);
  if (cmd == "fft-test") return fftTest();
  if (cmd == "window-test") return windowTest();
  printf("unknown command '%s'\n", cmd.c_str());
  return 2;
}

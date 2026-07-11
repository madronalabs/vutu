
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <vector>

namespace ml::utu
{

// Kaiser window math ported from Loris KaiserWindow. All computation is
// double precision and runs once at configure time; only the final window
// tables are stored as float.
namespace kaiser
{

// shape parameter from sidelobe attenuation in dB (>= 0)
double computeShape(double attenDb);

// window length in samples; width is the main lobe width as a fraction of
// the sample rate
long computeLength(double widthOverSR, double shape);

void buildWindow(std::vector<double>& win, double shape);
void buildTimeDerivativeWindow(std::vector<double>& win, double shape);

}  // namespace kaiser

// The three scaled analysis windows used by the reassigned spectrum, matching
// Loris ReassignedSpectrum::buildReassignmentWindows: the main window scaled
// so reported magnitudes are correct, the time-derivative window scaled so
// frequency corrections come out in fractional bins, and the time-ramped
// window giving time corrections in samples.
struct ReassignmentWindows
{
  long length{0};  // always odd
  std::vector<float> w;          // (2/winsum)·kaiser[k]
  std::vector<float> wFreqRamp;  // (length/(winsum·π))·kaiserDeriv[k]
  std::vector<float> wTimeRamp;  // (2/winsum)·kaiser[k]·(k − (length−1)/2)
};

ReassignmentWindows buildReassignmentWindows(double sampleRate, double windowWidthHz,
                                             double sidelobeDb);

}  // namespace ml::utu

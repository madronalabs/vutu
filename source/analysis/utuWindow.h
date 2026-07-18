
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include <vector>

namespace ml::utu
{

// Kaiser window math from the published Kaiser & Schafer 1980 formulas. All
// computation is double precision and runs once at configure time; only the
// final window tables are stored as float.
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

// The three analysis windows of Auger-Flandrin reassignment (Fitz & Fulop,
// "A Unified Theory of Time-Frequency Reassignment", Sec. 6.2): the window h
// itself, its time derivative hD = dh/dt used for frequency reassignment
// (eq. 65), and the time-ramped window hT = t·h used for time reassignment
// (eq. 64). Scaling:
//
//   w         = (2/winsum)·h            so |X| at a resolved peak reads
//                                       directly as sinusoid amplitude
//                                       (2 = analytic-signal factor)
//   wFreqRamp = (length/(winsum·π))·hD  relative to w this is hD·length/2π,
//                                       half of the rad/sample -> fractional
//                                       bin conversion; the spectrum kernel
//                                       supplies the other half, N/length
//   wTimeRamp = w·(k − center)          ramp in samples, so the time
//                                       correction comes out in samples with
//                                       no further scaling
//
// hD is analytic (d/dx I0 = I1), so the FFT-based construction of eq. 70 is
// unnecessary.
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

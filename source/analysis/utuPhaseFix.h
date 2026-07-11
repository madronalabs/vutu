
// vutu
// Copyright (c) 2026 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include "utuTracker.h"

namespace ml::utu
{

// wrap an unwrapped phase to [-pi, pi] (O'Donnell)
double wrapPi(double x);

// Port of Loris phasefix fixFrequency/matchPhaseFwd: walk each partial
// adjusting breakpoint frequencies (by at most maxFixPct percent, correcting
// half the error per step) so that rendered phase travel matches the
// analyzed phases, then recompute the stored phases to be consistent.
// Breakpoint time differences come from the exact sample-domain times.
void fixFrequency(BuildingPartial& partial, double sampleRate, double maxFixPct = 0.2);
void fixFrequency(std::vector<BuildingPartial>& partials, double sampleRate,
                  double maxFixPct = 0.2);

}  // namespace ml::utu

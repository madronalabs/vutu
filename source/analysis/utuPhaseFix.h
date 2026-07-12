
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
//
// This is the reconciliation step of phase-correct additive modeling (Fitz &
// Fulop Sec. 8): breakpoint frequencies and phases are estimated
// independently per frame by reassignment, but a rendering oscillator
// integrates frequency, accumulating travel π(f0+f1)·dt per linear segment.
// Small per-frame estimation errors would otherwise beat against the stored
// phases; nudging frequency, the derivative of phase, is the gentlest way to
// make the two agree.
void fixFrequency(BuildingPartial& partial, double sampleRate, double maxFixPct = 0.2);
void fixFrequency(std::vector<BuildingPartial>& partials, double sampleRate,
                  double maxFixPct = 0.2);

}  // namespace ml::utu

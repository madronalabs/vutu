// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once


#include "mldsp.h"
#include "madronalib.h"
#include "MLParameters.h"

#include <array>

using namespace ml;

// In the current madronalib, an Interval is no longer directly storable in a
// Value and its members are x1/x2. These helpers convert between an Interval and
// the 2-element float-array Value we use to carry the analysis interval param.
inline ml::Interval valueToInterval(const ml::Value& v)
{
  auto a = v.getFloatArray< 2 >();
  return ml::Interval{a[0], a[1]};
}

inline ml::Value intervalToValue(const ml::Interval& iv)
{
  return ml::Value(std::array< float, 2 >{iv.x1, iv.x2});
}



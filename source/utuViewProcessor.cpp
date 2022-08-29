// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#include "UtuViewProcessor.h"
#include "utuViewController.h"
#include "utuViewParameters.h"

#include <cmath>
#include <cstdlib>
#include <math.h>
#include <iostream>

using namespace ml;

constexpr float kSizeLo = 0, kSizeHi = 40;
constexpr float kToneLo = 250, kToneHi = 4000;
constexpr float kDecayLo = 0.8, kDecayHi = 20;
constexpr float kLevelLo = 0.5f, kLevelHi = 2.f;

void readParameterDescriptions(ParameterDescriptionList& params)
{
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "resolution" },
    { "range", { 20, 2000 } },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "window_width" },
    { "range", {40, 4000} },
    { "log", true },
    { "units", "Hz" }
  } ) );
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "amp_floor" },
    { "range", {-100, -30} },
    { "plaindefault", -90 },
    { "units", "dB" }
  } ) );
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "freq_drift" },
    { "range", {3, 300} },
    { "log", true },
    { "units", "Hz" }
  } ) );
  


}


// declare the processVector function that will run our DSP in vectors of size kFloatsPerDSPVector
// with the nullptr constructor argument above, RtAudioProcessor
void UtuViewProcessor::processVector(MainInputs inputs, MainOutputs outputs, void *stateDataUnused)
{
  // get params from the SignalProcessor.
  float f1 = getParam("freq1");
  float f2 = getParam("freq2");
  float gain = getParam("gain");
  
  // Running the sine generators makes DSPVectors as output.
  // The input parameter is omega: the frequency in Hz divided by the sample rate.
  // The output sines are multiplied by the gain.
  outputs[0] = s1(f1/kSampleRate)*gain;
  outputs[1] = s2(f2/kSampleRate)*gain;
}

// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#include "utuViewProcessor.h"
#include "utuViewController.h"
#include "utuViewParameters.h"

#include <cmath>
#include <cstdlib>
#include <math.h>
#include <iostream>

using namespace ml;
using namespace utu;

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
  
  params.push_back( ml::make_unique< ParameterDescription >(WithValues{
    { "name", "master_volume" },
    { "range", {-60, 6} },
    { "log", false },
    { "plaindefault", -15 },
    { "units", "dB" }
  } ) );
  


}


// declare the processVector function that will run our DSP in vectors of size kFloatsPerDSPVector
// with the nullptr constructor argument above, RtAudioProcessor
void UtuViewProcessor::processVector(MainInputs inputs, MainOutputs outputs, void *stateDataUnused)
{
  // get params from the SignalProcessor.
  float f1 = 220.0;//getParam("freq1");
  float f2 = 330.0;//getParam("freq2");
  float gain = getParam("master_volume");
  float amp = dBToAmp(gain);
    
 // std::cout << "gain: " << gain << "\n";
  
  // Running the sine generators makes DSPVectors as output.
  // The input parameter is omega: the frequency in Hz divided by the sample rate.
  // The output sines are multiplied by the gain.
  outputs[0] = s1(f1/kSampleRate)*amp;
  outputs[1] = s2(f2/kSampleRate)*amp;
}

void UtuViewProcessor::onMessage(Message msg)
{
  std::cout << "UtuViewProcessor: " << msg.address << " -> " << msg.value << "\n";
  
  switch(hash(head(msg.address)))
  {
    case(hash("set_param")):
    {
      setParam(tail(msg.address), msg.value.getFloatValue());
      break;
    }
    case(hash("set_prop")):
    {
      break;
    }
    case(hash("do")):
    {
      switch(hash(second(msg.address)))
      {
        case(hash("set_audio_data")):
        {
          // get pointer from message
          Sample* pSample = *reinterpret_cast<Sample**>(msg.value.getBlobValue());
 
        }
      }
      break;
    }
    default:
    {
      std::cout << " UtuViewProcessor: uncaught message " << msg << "! \n";
      break;
    }
  }
}

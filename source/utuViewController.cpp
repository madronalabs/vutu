// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details


// use clipboard code from VST3 SDK
//#include "public.sdk/source/common/systemclipboard.h"

#include "utuViewController.h"
#include "utuViewView.h"
#include "utuView.h"

// param definitions for this plugin
#include "utuViewParameters.h"

#include <cmath>
#include <iostream>
#include <chrono>

#include "MLSerialization.h"

#include "mlvg.h"

// includes in mlvg - TODO wrap
#include "miniz.h"
#include "nfd.h"

#include <loris/Analyzer.h>
#include <loris/Channelizer.h>
#include <loris/Distiller.h>
#include <loris/FrequencyReference.h>
#include <loris/PartialList.h>
#include <loris/SdifFile.h>
#include <loris/Synthesizer.h>

//#include "AudioFile.h"


using namespace ml;
using namespace utu;


//-----------------------------------------------------------------------------
// UtuViewController implementation

UtuViewController::UtuViewController(TextFragment appName, const ParameterDescriptionList& pdl)
  : AppController(appName, pdl)
{
  _debugTimer.start([=]() { _debug(); }, milliseconds(1000));
}

UtuViewController::~UtuViewController()
{
  // don't stop the master Timers-- there may be other plugin instances using it!
  // std::cout << "UtuViewController: BYE!\n";

}

void UtuViewController::_debug()
{
//  std::cout << "UtuViewController: " << getMessagesAvailable() << " messages in queue. \n";
//  std::cout << "UtuViewController @ " << std::hex << (this) << std::dec << " : \n";
//  std::cout << "        timers @ " << std::hex << (&_timers.get()) << std::dec << "\n";
}

void UtuViewController::_printToConsole(TextFragment t)
{
  sendMessageToActor(_viewName, {"info/set_prop/text", t});
}

void UtuViewController::_loadFileFromDialog()
{
  nfdchar_t *outPath;
  std::string sourcePath;
  nfdfilteritem_t filterItem[2] = { { "WAV audio", "wav" }, { "AIFF audio", "aiff,aif,aifc" } };
  nfdresult_t result = NFD_OpenDialog(&outPath, filterItem, 2, NULL);
  if (result == NFD_OKAY)
  {
    puts("Success!");
    puts(outPath);
    
    sourcePath = outPath;
    NFD_FreePath(outPath);
  }
  else if (result == NFD_CANCEL)
  {
    puts("User pressed cancel.");
  }
  else
  {
    printf("Error: %s\n", NFD_GetError());
  }
  
  Path filePath(sourcePath.c_str());
  File f(filePath);
  if(f)
  {
    std::cout << "file to load: " << filePath << "\n";
    std::cout << "init sr: " << _sample.sampleRate << "\n";

    // load the file

    SF_INFO fileInfo;
    
    auto file = sf_open(sourcePath.c_str(), SFM_READ, &fileInfo);
    
    std::cout << "        format: " << fileInfo.format << "\n";
    std::cout << "        frames: " << fileInfo.frames << "\n";
    std::cout << "        samplerate: " << fileInfo.samplerate << "\n";
    std::cout << "        channels: " << fileInfo.channels << "\n";
    
    constexpr size_t kMaxSeconds = 32;
    size_t fileSizeInFrames = fileInfo.frames;
    size_t kMaxFrames = kMaxSeconds*fileInfo.samplerate;

    size_t framesToRead = std::min(fileSizeInFrames, kMaxFrames);

    size_t kMaxSamples = kMaxFrames*fileInfo.channels;
    size_t samplesToRead = framesToRead*fileInfo.channels;
    
    _printToConsole(TextFragment("loading ", pathToText(filePath), "..."));
    
    _sample.data.resize(samplesToRead);
    float* pData = _sample.data.data();
    _sample.sampleRate = fileInfo.samplerate;
    
    sf_count_t framesRead = sf_readf_float(file, pData, static_cast<sf_count_t>(framesToRead));
    
    TextFragment readStatus;
    if(framesRead != framesToRead)
    {
      readStatus = "file read failed!";
    }
    else
    {
      TextFragment truncatedMsg = (framesToRead == kMaxFrames) ? "(truncated)" : "";
      readStatus = (TextFragment(textUtils::naturalNumberToText(framesRead), " frames read ", truncatedMsg ));
    }
    
    _printToConsole(readStatus);
    sf_close(file);
        
    // deinterleave to extract first channel if needed
    if(fileInfo.channels > 1)
    {
      for(int i=0; i < framesRead; ++i)
      {
        pData[i] = pData[i*fileInfo.channels];
      }
      _sample.data.resize(fileInfo.frames);
    }
    
    // if we have good audio data, send to View and Processor
    Sample* pSample = &_sample;
    Value samplePtrValue(&pSample, sizeof(void *));
    sendMessageToActor(_processorName, {"do/set_audio_data", samplePtrValue});
    sendMessageToActor(_viewName, {"do/set_audio_data", samplePtrValue});
  }
}



void UtuViewController::analyze()
{
  // TEST
  auto res = params["resolution"].getFloatValue();
  auto width = params["window_width"].getFloatValue();
  Loris::Analyzer a(res, width);
  
}



void UtuViewController::onMessage(Message m)
{
  if(!m.address) return;
  
 // std::cout << "UtuViewController::onMessage:" << m.address << " " << m.value << " \n ";
  
  bool messageHandled{false};
  
  Path addr = m.address;
  switch(hash(head(addr)))
  {
    case(hash("set_param")):
    {
      Path whatParam = tail(addr);
      switch(hash(head(whatParam)))
      {
      }
      break;
    }
    case(hash("set_prop")):
    {
      Path whatProp = tail(addr);
      switch(hash(head(whatProp)))
      {
      }
      break;
    }
    case(hash("do")):
    {
      Path whatAction = tail(addr);
      switch(hash(head(whatAction)))
      {
        case(hash("open")):
        {
          std::cout << "let's open a file!\n";
          _loadFileFromDialog();
          
          
          messageHandled = true;
          break;
        }
        default:
        {
          break;
        }
        
      }
      break;
    }
    default:
    {
      break;
    }
  }
  
  if(!messageHandled)
  {
    AppController::onMessage(m);
  }
}


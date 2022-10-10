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

// MLTEST #include "AudioFile.h"


//#define JUCE_APP_CONFIG_HEADER "external/juce_core/JuceCoreConfig.h"
///#include "external/juce_core/juce_core.h"

using namespace ml;


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
  std::cout << "UtuViewController: " << getMessagesAvailable() << " messages in queue. \n";
//  std::cout << "UtuViewController @ " << std::hex << (this) << std::dec << " : \n";
//  std::cout << "        timers @ " << std::hex << (&_timers.get()) << std::dec << "\n";
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
  
  //AudioFile f = AudioFile::forRead(sourcePath);
  
}

void UtuViewController::onMessage(Message m)
{
  if(!m.address) return;
  
  std::cout << "UtuViewController::onMessage:" << m.address << " " << m.value << " \n ";
  
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
          messageHandled = false;//true;
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


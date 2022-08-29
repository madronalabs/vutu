
#include <stdlib.h>
#include <stdio.h>

#include "SDL.h"
#include "SDL_syswm.h"
#include "MLAppController.h"
#include "mldsp.h"
#include "MLRtAudioProcessor.h"

#include "utuView.h"
#include "utuViewView.h"
#include "utuViewProcessor.h"

#include "MLSDLUtils.h"

int main(int argc, char *argv[])
{
  bool doneFlag{false};
  
  
  // read parameter descriptions into a list
  ParameterDescriptionList pdl;
  readParameterDescriptions(pdl);
  
  // make controller and get instance number
  AppController appController(getAppName(), pdl);
  auto instanceNum = appController.getInstanceNum();
  
  // make view
  UtuViewView appView(getAppName(), instanceNum);
  
  // set initial size.
  appView.setSizeInGridUnits({32, 16});
  appView.setGridSizeDefault(30);
  
  
  // make widgets and setup parameters
  appView.startup(pdl);
  
  SDL_Window *window = initSDLWindow(appView);
  if(window)
  {
    // watch for window resize events during drag
    ResizingEventWatcherData watcherData{window, &appView};
    SDL_AddEventWatch( resizingEventWatcher, &watcherData );
    
    // make Processor and register Actor
    UtuViewProcessor appProcessor(kInputChannels, kOutputChannels, kSampleRate, pdl);
    TextFragment processorName(getAppName(), "processor", ml::textUtils::naturalNumberToText(instanceNum));
    registerActor(Path(processorName), &appProcessor);
    
    // attach app view to window and resize
    ParentWindowInfo windowInfo = getParentWindowInfo(window);
    appView.doAttached(windowInfo.windowPtr, windowInfo.flags);
    SdlAppResize(&watcherData);
    
    // start Actor and audio processing
    appProcessor.start();
    appProcessor.startAudio();
    
    // just vibe
    while(!doneFlag)
    {
      SDLAppLoop(window, &doneFlag);
    }
    
    // stop audio and Actor and quit
    appProcessor.stopAudio();
    appProcessor.stop();
    SDL_Quit();
  }
  
  
  std::cout << "bye.\n";
  
  return 0;
}



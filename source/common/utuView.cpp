
#include <stdlib.h>
#include <stdio.h>

#include "SDL.h"
#include "SDL_syswm.h"
#include "MLAppController.h"
#include "madronalib.h"
#include "mldsp.h"
#include "MLRtAudioProcessor.h"

#include "utuView.h"
#include "utuViewView.h"
#include "utuViewProcessor.h"
#include "utuViewController.h"



#include <utu/utu.h>

#include "MLSDLUtils.h"
#include "nfd.h"

using namespace ml;

void utuViewAppLoop(SDL_Window* window, bool* done, Path controllerName)
{
  SDL_Event e;
  while (SDL_PollEvent(&e))
  {
    int w{0};
    int h{0};
    
    if (e.type == SDL_QUIT)
    {
      *done = true;
      return;
    }
    
    if (e.type == SDL_KEYDOWN)
    {
      SDL_Keycode sym = e.key.keysym.sym;
      uint16_t mod = e.key.keysym.mod;
      
      std::cout << "sym: " << sym << ", mod: " << mod << "\n";
      
      switch(sym)
      {
        case SDLK_ESCAPE:
        {
          *done = true;
          break;
        }
        case SDLK_o:
        {
          // TODO flash button. Use kMsgFromController flag
          sendMessageToActor(controllerName, Message{"do/open"});
          break;
        }
      }
    }
  }
}

int main(int argc, char *argv[])
{
  bool doneFlag{false};
  
  // read parameter descriptions into a list
  ParameterDescriptionList pdl;
  readParameterDescriptions(pdl);
  
  // make controller and get instance number
  UtuViewController appController(getAppName(), pdl);
  auto instanceNum = appController.getInstanceNum();
  
  // make view
  UtuViewView appView(getAppName(), instanceNum);
  
  // set initial size.
  appView.setSizeInGridUnits({32, 16});
  appView.setGridSizeDefault(30);
  
 
  SDL_Window *window = initSDLWindow(appView);
  if(window)
  {
    // init NFD after SDL.
    NFD_Init();
    
    // watch for window resize events during drag
    ResizingEventWatcherData watcherData{window, &appView};
    SDL_AddEventWatch( resizingEventWatcher, &watcherData );
    
    // make Processor
    UtuViewProcessor appProcessor(getAppName(), instanceNum, kInputChannels, kOutputChannels, kSampleRate, pdl);

    // attach app view to window and resize
    ParentWindowInfo windowInfo = getParentWindowInfo(window);
    appView.createPlatformView(windowInfo.windowPtr, windowInfo.flags);
    appView.makeWidgets(pdl);
    appView.startTimersAndActor();
    SdlAppResize(&watcherData);
    
    appController.sendAllParamsToView();
    appController.sendAllParamsToProcessor();

    // start Actor and audio processing
    appProcessor.start();
    appProcessor.startAudio();
    
    SharedResourcePointer< ActorRegistry > registry;
    registry->dump();
    
    // run
    while(!doneFlag)
    {
      utuViewAppLoop(window, &doneFlag, appController.getInstanceName());
    }
    
    // stop audio and Actor and quit
    appView.stopTimersAndActor();
    appProcessor.stopAudio();
    appProcessor.stop();
        
    NFD_Quit();
    SDL_Quit();
  }
  
  
  std::cout << "bye.\n";
  
  return 0;
}




#include <stdlib.h>
#include <stdio.h>

#include "SDL.h"
#include "SDL_syswm.h"
#include "MLAppController.h"
#include "madronalib.h"
#include "mldsp.h"
#include "MLRtAudioProcessor.h"

#include "vutu.h"
#include "vutuView.h"
#include "vutuProcessor.h"
#include "vutuController.h"

#include "MLSDLUtils.h"

using namespace ml;

void vutuAppLoop(SDL_Window* window, bool* done, Path controllerName)
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
      
      // std::cout << "sym: " << sym << ", mod: " << mod << "\n";
      
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
  VutuController appController(getAppName(), pdl);
  auto instanceNum = appController.getInstanceNum();
  
  // make view
  VutuView appView(getAppName(), instanceNum);


  // TODO get persistent window rect if available


  // if there is no persistent rect, use default
  // we have a few utilities in PlatformView that apps can use to make their own default strategies.
  Vec2 c = PlatformView::getPrimaryMonitorCenter();
  float devScale = PlatformView::getDeviceScaleAtPoint(c);


  // set initial size. This is not a fixed-ratio app, meaning the window sizes
  // freely and the grid unit size remains constant. 
  appView.setSizeInGridUnits(kDefaultGridUnits);
  appView.setGridSizeDefault(kDefaultGridUnitSize * devScale);

  // get default rect 
  Vec2 defaultSize = kDefaultGridUnits * kDefaultGridUnitSize * devScale;
  Rect boundsRect(0, 0, defaultSize.x(), defaultSize.y());
  Rect defaultRect = alignCenterToPoint(boundsRect, c);
 
  SDL_Window *window = initSDLWindow(appView, defaultRect, "vutu");
  if(window)
  {
    // watch for window resize events during drag
    ResizingEventWatcherData watcherData{window, &appView};
    SDL_AddEventWatch( resizingEventWatcher, &watcherData );
    
    // make Processor
    VutuProcessor appProcessor(getAppName(), instanceNum, kInputChannels, kOutputChannels, kSampleRate, pdl);

    // attach app view to window and resize
    ParentWindowInfo windowInfo = getParentWindowInfo(window);
    appView.makeWidgets(pdl);
    appView.createPlatformView(windowInfo.windowPtr, windowInfo.flags);
    appView.startTimersAndActor();
    SdlAppResize(&watcherData);
        
    appController.broadcastParams();
    appController.setButtonEnableStates();
    
    // start Actor and audio processing
    appProcessor.start();
    appProcessor.startAudio();
    
    SharedResourcePointer< ActorRegistry > registry;
    registry->dump();
    
    // run
    while(!doneFlag)
    {
      vutuAppLoop(window, &doneFlag, appController.getInstanceName());
    }
    
    // stop audio and Actor and quit
    appView.stopTimersAndActor();
    appProcessor.stopAudio();
    appProcessor.stop();

    SDL_Quit();
  }
  
  
  std::cout << "bye.\n";
  
  return 0;
}



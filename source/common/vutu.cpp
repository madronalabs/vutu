
#include <stdlib.h>
#include <stdio.h>

#include "SDL.h"
#include "SDL_syswm.h"
#include "MZAppController.h"
#include "madronalib.h"
#include "mldsp.h"
#include "MLAudioContext.h"
#include "MLAudioTask.h"

#include "vutu.h"
#include "vutuView.h"
#include "vutuProcessor.h"
#include "vutuController.h"

#include "MZSDLUtils.h"

using namespace ml;

void vutuAppLoop(SDL_Window* window, bool* done, Path controllerName)
{
  SDL_Event e;
  while (SDL_PollEvent(&e))
  {
    if (e.type == SDL_QUIT)
    {
      *done = true;
      return;
    }

    if (e.type == SDL_KEYDOWN)
    {
      SDL_Keycode sym = e.key.keysym.sym;

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
  PlatformView::initPlatform();

  // read parameter descriptions into a list
  ParameterDescriptionList pdl;
  readParameterDescriptions(pdl);

  // get default window rect in system coordinates, centered on primary monitor
  Vec2 defaultSize = kDefaultGridUnits * kDefaultGridUnitSize;
  Rect systemBoundsRect(0, 0, defaultSize.x(), defaultSize.y());
  Vec2 screenCenter = PlatformView::getPrimaryMonitorCenter();
  Rect systemDefaultRect = alignCenterToPoint(systemBoundsRect, screenCenter);

  // make controller and get instance number
  VutuController appController(getAppName(), pdl);
  auto instanceNum = appController.getInstanceNum();

  // Declaration order matters for shutdown: platformView must outlive appView so
  // that widget backing-layer textures can be torn down while the Metal/NVG
  // context still exists. Held as unique_ptrs because PlatformView needs an
  // AppView* in its ctor, so we have to construct appView first and platformView
  // second — but we still want platformView to *destruct* last.
  std::unique_ptr<PlatformView> platformView;
  std::unique_ptr<VutuView> appView;

  appView = std::make_unique<VutuView>(getAppName(), instanceNum);
  appView->setGridSizeDefault(kDefaultGridUnitSize);

  // make SDL window
  int windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
  SDL_Window* window = ml::newSDLWindow(systemDefaultRect, "vutu", windowFlags);
  if(!window)
  {
    std::cout << "newSDLWindow failed!\n";
    return 1;
  }

  // make PlatformView (drawing apparatus connecting AppView to Window)
  ParentWindowInfo windowInfo = ml::getParentWindowInfo(window);
  platformView = std::make_unique<PlatformView>("vutu", windowInfo.windowPtr,
                                                appView.get(), nullptr,
                                                windowInfo.flags, 60,
                                                windowInfo.sdlWindow);
  appView->initializeResources(platformView->getNativeDrawContext());

  // build widgets, attach view, watch for resize, start view actor.
  appView->makeWidgets(pdl);
  platformView->attachViewToParent();
  ResizingEventWatcherData watcherData{ window, platformView.get() };
  SDL_AddEventWatch(resizingEventWatcher, &watcherData);
  appView->startTimersAndActor();

  // make Processor
  VutuProcessor appProcessor(getAppName(), instanceNum, pdl);
  appProcessor.start();

  appController.broadcastParams();
  appController.setButtonEnableStates();

  // make AudioContext + AudioTask and start audio
  AudioContext ctx(kInputChannels, kOutputChannels);
  AudioTask vutuTask(&ctx, processVutu, &appProcessor);
  vutuTask.startAudio();

  SharedResourcePointer< ActorRegistry > registry;
  registry->dump();

  // run
  while (!doneFlag)
  {
    vutuAppLoop(window, &doneFlag, appController.getInstanceName());
  }

  // stop audio and Actor and quit
  vutuTask.stopAudio();
  appView->stopTimersAndActor();
  appProcessor.stop();

  SDL_DestroyWindow(window);
  SDL_Quit();

  std::cout << "bye.\n";

  return 0;
}

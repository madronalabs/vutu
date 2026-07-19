
#include <stdlib.h>
#include <stdio.h>

#include "mldsp.h"
#include "madronalib.h"
#include "manzanita.h"
#include "MZProcessorUtils.h"

#include "MLAudioContext.h"
#include "MLAudioTask.h"

#include "vutu.h"
#include "vutuView.h"
#include "vutuProcessor.h"
#include "vutuController.h"

#include "SDL.h"
#include "SDL_syswm.h"
#include "MZSDLUtils.h"

using namespace ml;

int main(int argc, char *argv[])
{
  PlatformView::initPlatform();

  // timers for everyone.
  SharedResourcePointer< ml::Timers > timers;
  timers->start(true);

  // keep the Actor registry alive for the lifetime of the app.
  SharedResourcePointer< ActorRegistry > registry;

  // read parameter descriptions into a list
  ParameterDescriptionList pdl;
  readParameterDescriptions(pdl);

  // single standalone instance.
  const size_t instanceNum = 0;

  // make the Controller (owns params + orchestration) and Processor (audio).
  // Both build their parameter stores and register as Actors in their ctors.
  VutuController appController(getAppName(), instanceNum, pdl);
  appController.start();

  VutuProcessor appProcessor(getAppName(), instanceNum, pdl);
  appProcessor.start();

  // get default window rect in system coords, centered on the primary monitor.
  Vec2 defaultSize = kDefaultGridUnits * kDefaultGridUnitSize;
  Rect systemBoundsRect(0, 0, defaultSize.x(), defaultSize.y());
  Vec2 screenCenter = PlatformView::getPrimaryMonitorCenter();
  Rect systemDefaultRect = alignCenterToPoint(systemBoundsRect, screenCenter);

  // make the View and initialize drawing resources.
  VutuView appView;
  if (!appView.createWindow(systemDefaultRect)) return 1;
  appView.attachPlatformViewToParent();
  appView.initializePlatformResources();

  // wire View <-> Controller messaging.
  appView.setProcessorMessageHandler([&](ml::Message m){ appController.enqueueMessage(m); });
  appController.setViewMessageHandler([&](ml::Message m){ appView.enqueueMessage(m); });

  // make widgets and connect them to the Controller's parameters.
  appView.makeWidgets();
  appView.connectWidgetsToParameters(appController.getParameterStore());
  appView.prepareToDraw();
  appView.startTimers();

  // push initial parameter values and button enable states to the View / Processor.
  appController.broadcastParams();
  appController.setButtonEnableStates();

  // start audio processing.
  AudioContext ctx(kInputChannels, kOutputChannels);
  AudioTask vutuTask(&ctx, processVutu, &appProcessor);
  vutuTask.startAudio();

  // run the app until the window is closed.
  appView.runAppLoop();

  // stop audio, actors, timers.
  vutuTask.stopAudio();
  timers->stop();
  appController.stop();
  appProcessor.stop();
  appView.stop();

  std::cout << "bye.\n";
  return 0;
}

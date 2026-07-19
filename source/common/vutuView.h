// Vutu
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once

#include "madronalib.h"
#include "manzanita.h"

#include "SDL.h"
#include "SDL_syswm.h"
#include "MZSDLUtils.h"

#include "vutuParameters.h"

// constrain window if true
constexpr bool kFixedRatioSize {false};
const ml::Vec2 kDefaultGridUnits{ 32, 16 };
const int kDefaultGridUnitSize(36);

const ml::Rect kDefaultPopupSize{0, 0, 3.5, 3.5};
const ml::Rect kDefaultPopupStartRect{0, 0, 1, 1};


class VutuView final :
  public ml::AppView
{
public:

  VutuView();
  ~VutuView() override;

  // AppView interface
  void initializeResources(NativeDrawContext* nvg) override;
  void clearResources() override;
  void layoutView(DrawContext dc) override;
  void makeWidgets() override;
  void prepareToDraw() override;
  void onResize(Vec2 newSize) override;
  void requestResize(Vec2 newSize) override;

  // handle messages from the controller and from Widgets.
  void onMessage(Message m) override;

  // windowing
  bool createWindow(ml::Rect defaultSize);
  void runAppLoop();
  void stop();
  void attachPlatformViewToParent();
  void initializePlatformResources();

private:

  SDL_Window* window{ nullptr };
  ResizingEventWatcherData watcherData;
  std::unique_ptr< PlatformView > platformView;

};

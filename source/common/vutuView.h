// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once

#include "MLAppView.h"
#include "MLPlatformView.h"
#include "MLWidget.h"
#include "MLView.h"

#include "vutuParameters.h"

// constrain window if true
constexpr bool kFixedRatioSize {false};

const ml::Rect kDefaultPopupSize{0, 0, 3.5, 3.5};
const ml::Rect kDefaultPopupStartRect{0, 0, 1, 1};


class VutuView final :
  public ml::AppView
{
public:

  VutuView(TextFragment appName, size_t instanceNum);
  ~VutuView();

  // AppView interface
  void initializeResources(NativeDrawContext* nvg) override;
  void layoutView(DrawContext dc) override;
  void onGUIEvent(const GUIEvent& event) override {};
  void onResize(Vec2 newSize) override {};
  
  // Actor interface
  void onMessage(Message m) override;
  
  // VutuView interface
  void makeWidgets(const ParameterDescriptionList& pdl);

  
};

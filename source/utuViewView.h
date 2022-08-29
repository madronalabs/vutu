// VST3 example code for madronalib
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#pragma once

#include "MLAppView.h"
#include "MLPlatformView.h"
#include "MLWidget.h"
#include "MLView.h"

#include "utuViewParameters.h"

// constrain window if true
constexpr bool kFixedRatioSize {false};

const ml::Rect kDefaultPopupSize{0, 0, 3.5, 3.5};
const ml::Rect kDefaultPopupStartRect{0, 0, 1, 1};


class UtuViewView :
  public ml::AppView
{
public:

  UtuViewView(TextFragment appName, size_t instanceNum);

  virtual ~UtuViewView ();

  // Actor interface
  void onMessage(Message m) override;

  // AppView interface
  void initializeResources(NativeDrawContext* nvg) override;
  void makeWidgets() override;
  void layoutView() override;
  void onGUIEvent(const GUIEvent& event) override {};
  void onResize(Vec2 newSize) override {};

private:
  
  Path _controllerName;

  void _initializeParams();

  Vec2 _constrainSize(Vec2 size);

  void _sendParameterToWidgets(const Message& m);

  bool _dismissPopupOnClick(GUIEvent e);

  int getElapsedTime();
  
  
  time_point< system_clock > _previousFrameTime;

  void* _platformHandle{ nullptr };
  
  std::unique_ptr< PlatformView > _platformView;
  
  int _animCounter{0};
  NVGcolor _popupIndicatorColor;

  
  // Queue of input events from mouse, keyboard, etc.
  Queue< GUIEvent > _inputQueue{ 1024 };


  // param tree
  ParameterTreeNormalized _params;

  Tree< std::vector< Widget* > > _widgetsByParameter;
  Tree< std::vector< Widget* > > _modalWidgetsByParameter;
  Tree< std::vector< Widget* > > _widgetsByProperty;
  Tree< std::vector< Widget* > > _widgetsByCollection;
  Tree< std::vector< Widget* > > _widgetsBySignal;

  Tree< Value > _popupPropertiesBuffer;
  
  // used to store param descriptions for Widgets in Popup to point to
  Tree< std::unique_ptr< ParameterDescription > > _popupParams;
  std::vector< Path > _popupParamNames;
  

  ml::Rect _borderRect;
  
  NativeDrawContext* _drawContext{};
  
  Timer _ioTimer;
  Timer _doubleClickTimer;
  Timer _animationTimer;
  
  Vec2 _defaultSystemSize{};
  Vec2 _dragPrev{};
  Vec2 resizerDragStartPosition{};
  Vec2 viewSizeAtDragStart{};

  void* _parent{nullptr};
  float _editorScale{ 1.0f };

  Timer _debugTimer;
  void debug();
  
};


// ml-gui: GUI library for madronalib apps
// Copyright (c) 2019 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

#pragma once

#include "MLDSPSample.h"
#include "mlvg.h"
#include "vutuPartials.h"

using namespace ml;

class SampleDisplay : public Widget
{
  static constexpr float kBracketWidth = 1.f/4.f;
  
  bool paintSample(ml::DrawContext dc);

  Rect getControlRect(int cIdx);
  void updateParamValue(MessageList& r, Interval val, uint32_t flags);

  bool _initialized{ false };
  bool sampleDirty_{ false };

  std::unique_ptr< Layer > _backingLayer;
  const ml::Sample * _pSample{nullptr};
  ml::DrawContext _prevDC{nullptr};
  
  float _playbackTime{0};
  float sampleDuration_{0};
  Interval analysisInterval_{0, 1};
  
  enum controlIdx
  {
    leftControl = 0,
    rightControl,
    none
  };
  float _dragX1{0};
  int currentDragControl_{none};
  
public:
  SampleDisplay(WithValues p) : Widget(p) {}

  // Widget implementation
  void resize(ml::DrawContext d) override;
  MessageList animate(int elapsedTimeInMs, ml::DrawContext dc) override;
  void draw(ml::DrawContext d) override;
  MessageList processGUIEvent(const GUICoordinates& gc, GUIEvent e) override;
  void receiveNamedRawPointer(Path name, void* ptr) override;
  

};


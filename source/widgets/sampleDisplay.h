
// ml-gui: GUI library for madronalib apps
// Copyright (c) 2019 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

#pragma once

#include "MLDSPSignal.h"
#include "mlvg.h"
#include "vutuPartials.h"

using namespace ml;

class SampleDisplay : public Widget
{
  void paintSample(ml::DrawContext dc);

  bool _initialized{ false };
  bool _partialsDirty{ false };

  std::unique_ptr< Layer > _backingLayer;
  const ml::Signal * _pSample{nullptr};
  ml::DrawContext _prevDC{nullptr};
  
  float _playbackTime{0};
  
public:
  SampleDisplay(WithValues p) : Widget(p) {}

  // Widget implementation
  void resize(ml::DrawContext d) override;
  MessageList animate(int elapsedTimeInMs, ml::DrawContext dc) override;
  void draw(ml::DrawContext d) override;
  void receiveNamedRawPointer(Path name, void* ptr) override;
};


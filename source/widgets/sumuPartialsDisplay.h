
// ml-gui: GUI library for madronalib apps
// Copyright (c) 2019 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

#pragma once

#include "mlvg.h"
#include "sumuPartials.h"

using namespace ml;

class SumuPartialsDisplay : public Widget
{
  void paintPartials(ml::DrawContext dc);

  bool _initialized{ false };
  bool _partialsDirty{ true };

  
  std::unique_ptr< Layer > _backingLayer;
  
  const SumuPartialsData * _pPartials{nullptr};
  

  ml::DrawContext _prevDC{nullptr};
  
public:
  
  virtual void handleMessage(Message msg, Message* r) override
  {
    std::cout << "partialsDisplay got message: " <<  msg << "\n";
    Widget::handleMessage(msg, r);
  }
  
  SumuPartialsDisplay(WithValues p) : Widget(p) {}

  // Widget implementation
  void resize(ml::DrawContext d) override;
  MessageList animate(int elapsedTimeInMs, ml::DrawContext dc) override;
  void draw(ml::DrawContext d) override;
  void receiveNamedRawPointer(Path name, void* ptr) override;


};


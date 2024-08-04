
// ml-gui: GUI library for madronalib apps
// Copyright (c) 2019 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

#pragma once

#include "mlvg.h"
#include "vutuPartials.h"

using namespace ml;

class VutuPartialsDisplay : public Widget
{
  void paintPartials(ml::DrawContext dc);

  bool _initialized{ false };
  bool _partialsDirty{ true };

  
  std::unique_ptr< DrawableImage > _backingLayer;
  
  const VutuPartialsData * _pPartials{nullptr};
  

  ml::DrawContext _prevDC{nullptr};
  float prevFundamental{0};
  Timer redrawTimer_;
  
public:
  
  virtual void handleMessage(Message msg, MessageList* r) override
  {
    //std::cout << "partialsDisplay got message: " <<  msg << "\n";
    Widget::handleMessage(msg, r);
  }
  
  VutuPartialsDisplay(WithValues p) : Widget(p) {}

  // Widget implementation
  void resize(ml::DrawContext d) override;
  MessageList animate(int elapsedTimeInMs, ml::DrawContext dc) override;
  void draw(ml::DrawContext d) override;
  void receiveNamedRawPointer(Path name, void* ptr) override;
  
  void redrawPartials();
  
  // overriding this because we will probably need multiple parameters.
  bool knowsParam(Path paramName) override
  {
    // user param in "param" property
    if(Path(getTextProperty("param")) == paramName)
    {
      return true;
    }
    if(paramName == "fundamental")
    {
      return true;
    }
    

    return false;
  }


};



// ml-gui: GUI library for madronalib apps
// Copyright (c) 2019 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

#pragma once

#include "mlvg.h"
#include "sumuPartials.h"

using namespace ml;

class SumuPartialsDisplay : public Widget
{
    
  /*
  struct SumuVector
  {
    union _Data
    {
      SIMDVectorFloat _align[kPartialsPerVoice * kFloatsPerDSPVector];   // unused except to force alignment
      std::array<float, kPartialsPerVoice * kFloatsPerDSPVector> mArrayData;  // for constexpr ctor
      float asFloat[kPartialsPerVoice * kFloatsPerDSPVector];
      
      _Data() {}
      constexpr _Data(std::array<float, kPartialsPerVoice * kFloatsPerDSPVector> a) : mArrayData(a) {}
    };
  };
*/
  
  

  void paintPartials(ml::DrawContext dc);

  bool _initialized{ false };
  bool _partialsDirty{ true };

  
  std::unique_ptr< Layer > _backingLayer;
  
  const SumuPartialsData * _pPartials{nullptr};
  

  ml::DrawContext _prevDC{nullptr};
public:
  
  SumuPartialsDisplay(WithValues p) : Widget(p) {}

  // Widget implementation
  void resize(ml::DrawContext d) override;
  MessageList animate(int elapsedTimeInMs, ml::DrawContext dc) override;
  void draw(ml::DrawContext d) override;
  void receiveNamedRawPointer(Path name, void* ptr) override;


};


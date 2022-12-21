// mlvg test application
// Copyright (C) 2019-2022 Madrona Labs LLC
// This software is provided 'as-is', without any express or implied warranty.
// See LICENSE.txt for details.

#pragma once

namespace sumu
{

struct Sample
{
  std::vector< float > data;
  size_t sampleRate{0};
  
  void normalize()
  {
    // get ratio
    float xMax{0.f};
    for(int i=0; i<data.size(); ++i)
    {
      float x = data[i];
      xMax = std::max(xMax, x);
    }
    
    // multiply
    float ratio = 1.0f / xMax;
    for(int i=0; i<data.size(); ++i)
    {
      data[i] *= ratio;
    }
  }
  
  void clear()
  {
    data.clear();
  }

  
};




};

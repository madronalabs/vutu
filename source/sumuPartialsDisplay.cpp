
// mlvg: GUI library for madronalib apps
// (c) 2020, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#include "sumuPartialsDisplay.h"

using namespace ml;

void SumuPartialsDisplay::resize(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);

  // constant background layer size, for now.
  int w = 564;
  int h = 170;
  int bw = 0;
  int bh = 0;
  
  // create new backing layer if needed
  if(_backingLayer.get())
  {
    bw = _backingLayer->width;
    bh = _backingLayer->height;
  }
  
  // if we do add a variable background layer size, this will repaint it when it changes.
  if((w != bw) || (h != bh))
  {
    std::cout << "SumuPartialsDisplay::resize: " << w << " x " << h << "\n";
    _backingLayer = ml::make_unique< Layer >(nvg, w, h);
    _partialsDirty = true;
  }
}


MessageList SumuPartialsDisplay::animate(int elapsedTimeInMs, ml::DrawContext dc)
{
  if(_partialsDirty)
  {
    paintPartials(dc);
    _partialsDirty = false;
    _dirty = true;
  }
  
  return MessageList{};
}


void SumuPartialsDisplay::receiveNamedRawPointer(Path name, void* ptr)
{
  switch(hash(head(name)))
  {
    case(hash("partials")):
      _pPartials = static_cast< const SumuPartialsData* > (ptr);
      _partialsDirty = true;
      break;
    default:
      break;
  }
}


// Repaint the backing layer with an image of the partials.
// Since we draw to a backing layer here, this must be called only
// from animate(), not from draw().
//
void SumuPartialsDisplay::paintPartials(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);
  const int gridSize = dc.coords.gridSizeInPixels;

  if(!_pPartials)
  {
    /*
    Rect bounds = getLocalBounds(dc, *this);
    nvgBeginPath(nvg);
    nvgRect(nvg, bounds);
    nvgFillColor(nvg, rgba(0, 0, 0, 1));
    nvgFill(nvg);
     */
    return;
  }
  
  if(!_backingLayer) return;

  int w = _backingLayer->width;
  int h = _backingLayer->height;
   
  // begin rendering to backing layer
  drawToLayer(_backingLayer.get());
  nvgBeginFrame(nvg, w, h, 1.0f);
  
  // draw opaque black bg
  {
    nvgBeginPath(nvg);
    nvgRect(nvg, ml::Rect{0.f, 0.f, float(_backingLayer->width), float(_backingLayer->height)});
    nvgFillColor(nvg, rgba(0, 0, 0, 1));
    nvgFill(nvg);
  }

  size_t nPartials = _pPartials->stats.nPartials;
  std::cout << "painting " << nPartials << " partials... \n";

  Interval xRange{0.f, w - 1.f};
  Interval yRange{h - 1.f, 0.f};

  constexpr float kMinLineLength{2.f};
  auto xToTime = projections::linear({0.f, w - 1.f}, _pPartials->stats.timeRange);
  auto timeToX = projections::linear(_pPartials->stats.timeRange, {0.f, w - 1.f});
  auto freqRange = _pPartials->stats.freqRange;
  auto freqToY = projections::intervalMap(freqRange, yRange, projections::log(freqRange));
  
  // drawn amplidutes range from -60dB to max in partials
  auto ampRange = _pPartials->stats.ampRange;
  ampRange.mX1 = dBToAmp(-60);
  
  Interval thicknessRange{0, h/16.f};
  auto ampToThickness = projections::intervalMap(ampRange, thicknessRange, projections::exp(thicknessRange));
  // projections::printTable(ampToThickness, "ampToThickness", ampRange, 5);
  auto bandwidthToUnity = projections::linear(_pPartials->stats.bandwidthRange, {0, 1});

  auto noiseColor = rgba(1, 1, 1, 1);
  auto sineColor = getColor(dc, "partials");
  
  // time the partials bit
  auto roughStart = high_resolution_clock::now();
  size_t totalFramesRead{0};
  size_t totalFramesDrawn{0};

  for(int p=0; p<nPartials; ++p)
  {
    const auto& partial = _pPartials->partials[p];
    size_t framesInPartial = partial.time.size();
    
    nvgStrokeColor(nvg, sineColor);// TODO make avg color for stroke?
    nvgBeginPath(nvg);
    
    float x1 = 0.;
    
    for(int i = 0; i < framesInPartial; ++i)
    {
      auto frame = getPartialFrameByIndex(*_pPartials, p, i);

      float x = timeToX(_pPartials->partials[p].time[i]);

      totalFramesRead++;
      
      //if(frame.amp > 0.f)
      if((i == 0) || (x > x1 + kMinLineLength))
      {
        x1 = x;
        totalFramesDrawn++;
        
        float thickness = ampToThickness(frame.amp);
        float y = freqToY(frame.freq);
        
        float colorOpacity = 0.5f;
        float maxOpacity = 0.75f;
        float pathOpacity = 0.75f;
        
        // adding noise fades opacity up to 1
        float bw = bandwidthToUnity(frame.bandwidth);
        pathOpacity = colorOpacity + bw*(maxOpacity - colorOpacity);
        
        /*
        // nanovg workaround
        if(thickness < 1.f)
        {
          pathOpacity *= thickness;
          thickness = 1.f;
        }*/
        
        auto colorWithNoise = lerp(sineColor, noiseColor, bw);
        auto partialColor = multiplyAlpha(colorWithNoise, pathOpacity);// rgba(1, 1, 1, opacity);
        
        float y1 = clamp(y - thickness/2.f, 0.f, float(h));
        float y2 = clamp(y + thickness/2.f, 0.f, float(h));
        
        if(i == 0)
        {
          nvgMoveTo(nvg, x, y);
        }
        else
        {
          nvgLineTo(nvg, x, y);
        }
      }
    }
    
    
    //nvgStrokeColor(nvg, sineColor);
    nvgStroke(nvg);
  }

  
  auto roughEnd = high_resolution_clock::now();
  auto roughMillisTotal = duration_cast<milliseconds>(roughEnd - roughStart).count();
  std::cout << " frames read: " << totalFramesRead << " frames drawn: " << totalFramesDrawn << ", painting time rough millis: " << roughMillisTotal << "\n";
  

  
/*
  for(int x=0; x<w; ++x)
  {
    float time = xToTime(x);
    float dTime = xToTime(x + 1) - time;
    
    for(int p=0; p<nPartials; ++p)
    {
      auto frame = getPartialFrameDownsampled(*_pPartials, p, time, dTime);
      //    auto frame = getPartialFrame(*_pPartials, p, time);
      
      if(frame.amp > 0.f)
      {
        float thickness = ampToThickness(frame.amp);
        float y = freqToY(frame.freq);
        
        float colorOpacity = 0.5f;
        float maxOpacity = 0.75f;
        float pathOpacity = 0.75f;
        
        // adding noise fades opacity up to 1
        float bw = bandwidthToUnity(frame.bandwidth);
        pathOpacity = colorOpacity + bw*(maxOpacity - colorOpacity);
        
        // nanovg workaround
        if(thickness < 1.f)
        {
          pathOpacity *= thickness;
          thickness = 1.f;
        }
        
        auto colorWithNoise = lerp(sineColor, noiseColor, bw);
        auto partialColor = multiplyAlpha(colorWithNoise, pathOpacity);// rgba(1, 1, 1, opacity);
        
        float y1 = clamp(y - thickness/2.f, 0.f, float(h));
        float y2 = clamp(y + thickness/2.f, 0.f, float(h));
        
        nvgBeginPath(nvg);
        nvgMoveTo(nvg, x, y1);
        nvgLineTo(nvg, x, y2);
        
        nvgStrokeColor(nvg, partialColor);//
        //nvgStrokeColor(nvg, sineColor);
        nvgStroke(nvg);
      }
    }
  }
  */
  
  // end backing layer update
  nvgEndFrame(nvg);
}
 

void SumuPartialsDisplay::draw(ml::DrawContext dc)
{

  NativeDrawContext* nvg = getNativeContext(dc);
  Rect bounds = getLocalBounds(dc, *this);
  int w = bounds.width();
  int h = bounds.height();
  const int gridSize = dc.coords.gridSizeInPixels;
  
  
  if(!_pPartials)
  {
    auto color = getColorPropertyWithDefault("color", getColor(dc, "mark"));
    nvgBeginPath(nvg);
    nvgRect(nvg, 0, 0, w, h);
    nvgFillColor(nvg, color);
    nvgFill(nvg);
    return;
  }
  if(!_backingLayer) return;
 
  
  Rect pBounds = getPixelBounds(dc, *this);
  int pw = pBounds.width();
  int ph = pBounds.height();
  
  int bw = _backingLayer->width;
  int bh = _backingLayer->height;
  
  int margin = gridSize/64.f;
  
  // blit backing layer to main layer
  auto nativeImage = getNativeImageHandle(*_backingLayer);
  
  // paint background color
  auto color = getColorPropertyWithDefault("color", getColor(dc, "panel_bg"));
  nvgBeginPath(nvg);
  nvgRect(nvg, 0, 0, w, h);
  nvgFillColor(nvg, color);
  nvgFill(nvg);
  
  
  // make an image pattern. The entire source image maps to the specified rect of the destination.
  NVGpaint img = nvgImagePattern(nvg, margin, margin, w - margin*2, h - margin*2, 0, nativeImage, 1.0f);

  nvgSave(nvg);
  nvgGlobalCompositeOperation(nvg, NVG_LIGHTER);
  
  // paint image lighten over bg
  nvgBeginPath(nvg);
  nvgRect(nvg, margin, margin, w - margin*2, h - margin*2);
  nvgFillPaint(nvg, img);
  nvgFill(nvg);
  
  nvgRestore(nvg);
 
}

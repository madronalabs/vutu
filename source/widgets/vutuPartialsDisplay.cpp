

#include "vutuPartialsDisplay.h"

using namespace ml;

void VutuPartialsDisplay::resize(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);

  // constant background layer size, for now.
  int w = 2000;
  int h = 800;
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
    std::cout << "VutuPartialsDisplay::resize: " << w << " x " << h << "\n";
    _backingLayer = ml::make_unique< Layer >(nvg, w, h);
    _partialsDirty = true;
  }
}


MessageList VutuPartialsDisplay::animate(int elapsedTimeInMs, ml::DrawContext dc)
{
  if(_partialsDirty)
  {
    paintPartials(dc);
    _partialsDirty = false;
    _dirty = true;
  }
  
  return MessageList{};
}


void VutuPartialsDisplay::receiveNamedRawPointer(Path name, void* ptr)
{
  switch(hash(head(name)))
  {
    case(hash("partials")):
      _pPartials = static_cast< const VutuPartialsData* > (ptr);
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
void VutuPartialsDisplay::paintPartials(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);
  const int gridSize = dc.coords.gridSizeInPixels;

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
  
  bool partialsOK = _pPartials && _pPartials->stats.nPartials;
  if(partialsOK)
  {
    size_t nPartials = _pPartials->stats.nPartials;
    std::cout << "painting " << nPartials << " partials... \n";
    
    Interval xRange{0.f, w - 1.f};
    Interval yRange{h - 1.f, 0.f};
    
    
    float intervalStart = getFloatProperty("interval_start");
    float intervalEnd = getFloatProperty("interval_end");
    Interval timeInterval{intervalStart, intervalEnd};

    
    constexpr float kMinLineLength{2.f};
    auto xToTime = projections::linear({0.f, w - 1.f}, timeInterval);
    auto timeToX = projections::linear(timeInterval, {0.f, w - 1.f});
//    auto freqRange = _pPartials->stats.freqRange;
    auto freqRange = Interval{0, _pPartials->stats.freqRange.mX2};
    //auto freqToY = projections::intervalMap(freqRange, yRange, projections::unity);//exp(freqRange));
    
    auto freqToY = projections::linear(freqRange, yRange);
    
    // drawn amplidutes range from -60dB to max in partials
    auto ampRange = _pPartials->stats.ampRange;
    ampRange.mX1 = dBToAmp(-90);
    
    Interval thicknessRange{h/512.f, h/32.f};
    //auto ampToThickness = projections::intervalMap(ampRange, thicknessRange, projections::unity);
    auto ampToThickness = projections::intervalMap(ampRange, thicknessRange, projections::exp(thicknessRange));
    
    // projections::printTable(ampToThickness, "ampToThickness", ampRange, 5);
    auto bandwidthToUnity = projections::linear(_pPartials->stats.bandwidthRange, {0, 1});
    
    auto noiseColor = rgba(1, 1, 1, 1);
    auto sineColor = getColor(dc, "partials");
    
    // time the partials bit
    auto roughStart = high_resolution_clock::now();
    size_t totalFramesDrawn{0};
    
    // draw frame amplitudes
    for(int p=0; p<nPartials; ++p)
    {
      const auto& partial = _pPartials->partials[p];
      size_t framesInPartial = partial.time.size();
      
      //    nvgStrokeColor(nvg, sineColor);// TODO make avg color for stroke?
      nvgStrokeWidth(nvg, 3); // TEMP
      nvgBeginPath(nvg);
      
      float x1 = 0.;
      
      for(int i = 0; i < framesInPartial; ++i)
      {
        auto frame = getPartialFrameByIndex(*_pPartials, p, i);
        
        float x = timeToX(_pPartials->partials[p].time[i]);
        

        if((i == 0) || (x > x1 + kMinLineLength))
        {
          x1 = x;
          totalFramesDrawn++;
          
          float thickness = ampToThickness(frame.amp);
          float y = freqToY(frame.freq);
          
          float colorOpacity = 0.5f;
          float maxOpacity = 1.0f;
          
          
          // adding noise fades opacity up to 1
          float bw = bandwidthToUnity(frame.bandwidth);
          
          // std::cout << "frame " << i << " bw " << bw << "\n";
          
          float pathOpacity = 0.5f;//colorOpacity + bw*(maxOpacity - colorOpacity);
          
          
          if(thickness < 1.f)
          {
            thickness = 1.f;
          }
          
          auto colorWithNoise = lerp(sineColor, noiseColor, bw);
          auto partialColor = multiplyAlpha(colorWithNoise, pathOpacity);
          
          float y1 = clamp(y - thickness/2.f, 0.f, float(h));
          float y2 = clamp(y + thickness/2.f, 0.f, float(h));

          nvgStrokeColor(nvg, partialColor);
          nvgBeginPath(nvg);
          
          nvgMoveTo(nvg, x, y1);
          nvgLineTo(nvg, x, y2);
          nvgStroke(nvg);
        }
      }
    }
    
    float maxAvgAmp = 0.f;
    float minAvgAmp = 100000.f;
        
    // draw spines
    auto spineColor(rgba(0, 1, 1, 1));
    nvgStrokeWidth(nvg, 2); // TEMP
    nvgStrokeColor(nvg, multiplyAlpha(spineColor, 0.5));
    nvgBeginPath(nvg);
    for(int p=0; p<nPartials; ++p)
    {
      const auto& partial = _pPartials->partials[p];
      size_t framesInPartial = partial.time.size();
      
      for(int i = 0; i < framesInPartial; ++i)
      {
        auto frame = getPartialFrameByIndex(*_pPartials, p, i);
        float x = timeToX(_pPartials->partials[p].time[i]);
        float y = freqToY(frame.freq);
        
        if(i == 0)
        {
          //nvgBeginPath(nvg);
          nvgMoveTo(nvg, x, y);
        }
        else if(i < framesInPartial - 1)
        {
          nvgLineTo(nvg, x, y);
        }
        else
        {
          //nvgStroke(nvg);
        }
      }

    }
    nvgStroke(nvg);
    
    std::cout << "\n min avg ampL: " << minAvgAmp << " max avg amp: " << maxAvgAmp << "\n";

    auto roughEnd = high_resolution_clock::now();
    auto roughMillisTotal = duration_cast<milliseconds>(roughEnd - roughStart).count();
   std::cout << "partials painting time rough millis: " << roughMillisTotal << "\n";
    
  
  }
  
  // end backing layer update
  nvgEndFrame(nvg);
}
 

void VutuPartialsDisplay::draw(ml::DrawContext dc)
{
  NativeDrawContext* nvg = getNativeContext(dc);
  Rect bounds = getLocalBounds(dc, *this);
  int w = bounds.width();
  int h = bounds.height();
  const int gridSizeInPixels = dc.coords.gridSizeInPixels;
  float strokeWidthMul = getFloatPropertyWithDefault("stroke_width", getFloat(dc, "common_stroke_width"));
  int strokeWidth = gridSizeInPixels*strokeWidthMul;

  int margin = gridSizeInPixels/8;
  Rect marginBounds = shrink(bounds, margin);
  
  auto bgColor = getColorPropertyWithDefault("color", getColor(dc, "panel_bg"));
  auto markColor = getColor(dc, "partials");

  // background
  {
    nvgBeginPath(nvg);
    nvgRect(nvg, marginBounds);
    nvgFillColor(nvg, bgColor);
    nvgFill(nvg);
  }
  
  if(!_backingLayer) return;
 
  bool partialsOK = _pPartials && _pPartials->stats.nPartials;
  if(partialsOK)
  {
    Rect pBounds = getPixelBounds(dc, *this);
    int pw = pBounds.width();
    int ph = pBounds.height();
    
    int bw = _backingLayer->width;
    int bh = _backingLayer->height;
    
    // blit backing layer to main layer
    auto nativeImage = getNativeImageHandle(*_backingLayer);
    
    // paint background color
    nvgBeginPath(nvg);
    nvgRect(nvg, 0, 0, w, h);
    nvgFillColor(nvg, bgColor);
    nvgFill(nvg);
    
    float intervalStart = getFloatProperty("interval_start");
    float intervalEnd = getFloatProperty("interval_end");
    Interval timeInterval{intervalStart, intervalEnd};

    
    // make an image pattern. The entire source image maps to the specified rect of the destination.
    NVGpaint img = nvgImagePattern(nvg, margin, margin, w - margin*2, h - margin*2, 0, nativeImage, 1.0f);
    
    // paint image lighten over bg
    nvgSave(nvg);
    nvgGlobalCompositeOperation(nvg, NVG_LIGHTER);
    nvgBeginPath(nvg);
    nvgRect(nvg, margin, margin, w - margin*2, h - margin*2);
    nvgFillPaint(nvg, img);
    nvgFill(nvg);
    nvgRestore(nvg);
    
    // draw max active time triangle
    if(_pPartials->stats.maxActiveTime > 0.f)
    {
      Interval xRange{marginBounds.left(), marginBounds.right()};
      auto xToProgress = projections::linear({0.f, 1.f}, xRange);
      auto timeToX = projections::linear(timeInterval, {0.f, w - 1.f});
      
      float px = timeToX(_pPartials->stats.maxActiveTime);
      
      float triSize = gridSizeInPixels/8;

      nvgBeginPath(nvg);
      nvgFillColor(nvg, markColor);

      nvgMoveTo(nvg, px, marginBounds.top());
      nvgLineTo(nvg, px + triSize, marginBounds.top());
      nvgLineTo(nvg, px, marginBounds.top() + triSize*2 );
      nvgLineTo(nvg, px - triSize, marginBounds.top());
      nvgLineTo(nvg, px, marginBounds.top());

      nvgFill(nvg);
    }
  }
  
  // TEST
  //std::cout << "PartialsDisplay start: " << getFloatProperty("interval_start") << " -- " <<  getFloatProperty("interval_end") << "\n";
  
  // draw border
  nvgBeginPath(nvg);
  nvgRoundedRect(nvg, marginBounds, strokeWidth*2);
  nvgStrokeWidth(nvg, strokeWidth);
  nvgStrokeColor(nvg, markColor);
  nvgStroke(nvg);
 
}

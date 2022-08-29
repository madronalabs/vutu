
// sumu
// Copyright (c) 2022 Madrona Labs LLC. http://www.madronalabs.com



#include "sumuPartials.h"

namespace ml
{

Interval getVectorRange(const std::vector< float >& vec)
{
  float fMin = *std::min_element(vec.begin(), vec.end());
  float fMax = *std::max_element(vec.begin(), vec.end());
  return Interval{fMin, fMax};
}

Interval getParamRangeInPartials(const SumuPartialsData& partialData, Symbol param)
{
  Interval r{std::numeric_limits<float>::max(), std::numeric_limits<float>::min()};
  
  for(int i=0; i<partialData.partials.size(); ++i)
  {
    const SumuPartial& partial = partialData.partials[i];
    Interval paramRange{0, 0};
    switch(hash(param))
    {
      case(hash("time")):
        paramRange = getVectorRange(partial.time);
        break;
      case(hash("amp")):
        paramRange = getVectorRange(partial.amp);
        break;
      case(hash("freq")):
        paramRange = getVectorRange(partial.freq);
        break;
      case(hash("bandwidth")):
        paramRange = getVectorRange(partial.bandwidth);
        break;
      default:
        break;
    }
    if(paramRange.mX1 < r.mX1)
    {
      r.mX1 = paramRange.mX1;
    }
    if(paramRange.mX2 > r.mX2)
    {
      r.mX2 = paramRange.mX2;
    }
  }
  return r;
}

// Get stats for partials data to aid synthesis and drawing.
// TODO check that time is monotonically increasing
//
PartialsStats getPartialsStats(const SumuPartialsData& partialData)
{
  PartialsStats r;
  r.timeRange = getParamRangeInPartials(partialData, "time");
  r.ampRange = getParamRangeInPartials(partialData, "amp");
  r.bandwidthRange = getParamRangeInPartials(partialData, "bandwidth");
  r.freqRange = getParamRangeInPartials(partialData, "freq");
  r.nPartials = partialData.partials.size();
  
  // get max frames by looking at size of time data in all partials
  size_t maxFrames = 0;
  for(int i=0; i<r.nPartials; ++i)
  {
    const SumuPartial& partial = partialData.partials[i];
    size_t nFrames = partial.time.size();
    if(nFrames > maxFrames)
    {
      maxFrames = nFrames;
    }
  }
    
  // get min, max time for each partial
  r.partialTimeRanges.clear();
  for(int i=0; i<r.nPartials; ++i)
  {
    const SumuPartial& partial = partialData.partials[i];
    if(partial.time.size() > 0)
    {
      const auto& timeVec = partial.time;
      Interval ptr = getVectorRange(timeVec);
      r.partialTimeRanges.push_back(ptr);
    }
    else
    {
      r.partialTimeRanges.push_back(Interval{0, 0});
    }
  }
  
  r.maxFrames = maxFrames;
  return r;
}

// get an interpolated frame of data from the partial index p of the SumuPartialsData at time t.
// note that the SumuPartialsData stats must be filled in first!
//
PartialFrame getPartialFrame(const SumuPartialsData& partialData, size_t partialIndex, float t)
{
  PartialFrame f;
  size_t nPartials = partialData.stats.nPartials;
  
  if(within(partialIndex, 0UL, nPartials))
  {
    auto partial = partialData.partials[partialIndex];
    auto partialTimeRange = partialData.stats.partialTimeRanges[partialIndex];
    if(within(t, partialTimeRange))
    {
      // get indexes of time samples before and after t
      // if this is slow, we can make an index into the time vector
      size_t i1{0};
      size_t i2{1};
      for(int i=1; i<partial.time.size(); ++i)
      {
        if(t < partial.time[i])
        {
          i1 = i - 1;
          i2 = i;
          break;
        }
      }
      
      // interpolate time to get fractional index corresponding to t
      float t1 = partial.time[i1];
      float t2 = partial.time[i2];
      float timeFrac = (t - t1) / (t2 - t1);
      
      // interpolate data
      f.amp = lerp(partial.amp[i1], partial.amp[i2], timeFrac);
      f.freq = lerp(partial.freq[i1], partial.freq[i2], timeFrac);
      f.bandwidth = lerp(partial.bandwidth[i1], partial.bandwidth[i2], timeFrac);
      
      // use phase directly
      // TODO compute from freq
      f.phase = partial.phase[i1];
    }
    else
    {
      // TODO generate interpolated frames just before and after timeRange
      
    }
  }
  
  return f;
}

// inefficient!
PartialFrame  getPartialFrameDownsampled(const SumuPartialsData& partialData, size_t partialIndex, float time, float dTime)
{
//  PartialFrame f;
//  if(dTime <= 1.0f)
  
  int sampleRadius{2};
  size_t totalSamples{0};
  
  PartialFrame sum;
  
  if(partialIndex == 5)
  {
//    std::cout << "t = " << time << ", dt = " << dTime << "\n";
  }
  
  for(int offset = -sampleRadius; offset <= sampleRadius; ++offset)
  {
    float x = time + dTime*offset;
    PartialFrame sample = getPartialFrame(partialData, partialIndex, x);
    
    // get sum of frames
    sum.amp += sample.amp;
    sum.freq += sample.freq;
    sum.bandwidth += sample.bandwidth;
    
    // phase ?
    totalSamples++;
    
    if(partialIndex == 5)
    {
  //    std::cout << "+" << sample.amp << " ";
    }
  }
  
//  sum.amp /= totalSamples;
  sum.freq /= totalSamples;
  sum.bandwidth /= totalSamples;

  if(partialIndex == 5)
  {

  
//  std::cout << "/" << totalSamples << " = " << sum.amp << "\n";
  }
  
  return sum;
}


// get an interpolated frame of data from the partial index p of the SumuPartialsData at time t.
// note that the SumuPartialsData stats must be filled in first!
//
PartialFrame getPartialFrameNearest(const SumuPartialsData& partialData, size_t partialIndex, float t)
{
  PartialFrame f;
  size_t nPartials = partialData.stats.nPartials;
  
  if(within(partialIndex, 0UL, nPartials))
  {
    auto partial = partialData.partials[partialIndex];
    auto partialTimeRange = partialData.stats.partialTimeRanges[partialIndex];
    
    //   std::cout << partialIndex << " range:" << partialTimeRange << "\n";
    
    if(within(t, partialTimeRange))
    {
      // get indexes of time samples before and after t
      // if this is slow, we can make an index into the time vector
      size_t i1{0};
      size_t i2{1};
      for(int i=1; i<partial.time.size(); ++i)
      {
        if(t < partial.time[i])
        {
          i1 = i - 1;
          i2 = i;
          break;
        }
      }
      
      // get the nearest index to time t
      float d1 = t - partial.time[i1];
      float d2 = partial.time[i2] - t;
      size_t nearestIndex = (d1 < d2) ? i1 : i2;
      
      // get data
      f.amp = partial.amp[nearestIndex];
      f.freq = partial.freq[nearestIndex];
      f.bandwidth = partial.bandwidth[nearestIndex];
      f.phase = partial.phase[nearestIndex];
    }
  }
  
  
  return f;
}


// get an interpolated frame of data from the partial index p of the SumuPartialsData at time t.
// note that the SumuPartialsData stats must be filled in first!
//
PartialFrame getPartialFrameByIndex(const SumuPartialsData& partialData, size_t partialIndex, size_t frameIndex)
{
  PartialFrame f;
  size_t nPartials = partialData.stats.nPartials;
  
  if(within(partialIndex, 0UL, nPartials))
  {
    auto partial = partialData.partials[partialIndex];
    size_t partialFrames = partial.time.size();
    
    //   std::cout << partialIndex << " range:" << partialTimeRange << "\n";
    
    if(within(frameIndex, 0UL, partialFrames))
    {
      // get data
      f.amp = partial.amp[frameIndex];
      f.freq = partial.freq[frameIndex];
      f.bandwidth = partial.bandwidth[frameIndex];
      f.phase = partial.phase[frameIndex];
    }
  }
  return f;
}

}

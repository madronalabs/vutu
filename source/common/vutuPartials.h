
// vutu
// Copyright (c) 2022 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include "mldsp.h"

#include "madronalib.h"

#include "MLFiles.h"

// stats about partials

namespace ml
{

static constexpr float kVutuPartialsFileVersion{ 1.0 };
static constexpr char kVutuPartialsFileType[] = "VutuPartials";
static constexpr char kVutuPartials2FileType[] = "VutuPartials2";

// these values are calculated after reading in the partials data.
struct PartialsStats
{
  // get range of values for each parameter (except phase)
  Interval timeRange; // min and max time in all partials
  Interval ampRange;
  Interval bandwidthRange;
  Interval freqRange;
  size_t nPartials;
  size_t maxActivePartials;
  float maxActiveTime;
  
  std::vector< Interval > partialTimeRanges; // time range for each partial
};

// a single partial is a trajectory of these five values over time.
struct VutuPartial
{
  std::vector< float > time;
  std::vector< float > amp;
  std::vector< float > freq;
  std::vector< float > bandwidth;
  std::vector< float > phase;
};

// this structure holds the individual partials and related data.
// all this data except stats is stored to a partials file.
struct VutuPartialsData
{
  PartialsStats stats;
  std::vector< VutuPartial > partials;
  int version;
  Symbol type;
  
  // source and analysis parameters
  TextFragment sourceFile;
  float sourceDuration{0}; // whole duration of source, will be longer than stats.timeRange
  float resolution{0};
  float windowWidth{0};
  float ampFloor{0};
  float freqDrift{0};
  float loCut{0};
  float hiCut{0};
  float fundamental{0};
};

struct PartialFrame
{
  float amp{0};
  float freq{0};
  float bandwidth{0};
  float phase{0};
};


Interval getParamRangeInPartials(const VutuPartialsData& partialData, Symbol param);

// return an interpolated frame of a single Partial at the specified time.
PartialFrame getPartialFrame(const VutuPartialsData& partialData, size_t partialIndex, float time);

// return a frame of a single Partial at the nearest sample to the specified time.
PartialFrame getPartialFrameNearest(const VutuPartialsData& partialData, size_t partialIndex, float time);

// return a frame of a single Partial by its frame index.
PartialFrame getPartialFrameByIndex(const VutuPartialsData& partialData, size_t partialIndex, size_t frameIndex);

inline std::ostream& operator<< (std::ostream& out, const PartialFrame& r)
{
  out << "[";
  out << r.amp;
  out << ", ";
  out << r.freq;
  out << ", ";
  out << r.bandwidth;
  out << ", ";
  out << r.phase;
  out << "]";
  return out;
}


inline Interval getVectorExtrema(const std::vector< float >& vec)
{
  float fMin{0}, fMax{0};
  if(vec.size() > 0)
  {
    fMin = *std::min_element(vec.begin(), vec.end());
    fMax = *std::max_element(vec.begin(), vec.end());
  }
  return Interval{fMin, fMax};
}

inline Interval getParamRangeInPartials(const VutuPartialsData& partialData, Symbol param)
{
  Interval r{std::numeric_limits<float>::max(), std::numeric_limits<float>::min()};
  
  for(int i=0; i<partialData.partials.size(); ++i)
  {
    const VutuPartial& partial = partialData.partials[i];
    Interval paramRange{0, 0};
    switch(hash(param))
    {
      case(hash("time")):
        paramRange = getVectorExtrema(partial.time);
        break;
      case(hash("amp")):
        paramRange = getVectorExtrema(partial.amp);
        break;
      case(hash("freq")):
        paramRange = getVectorExtrema(partial.freq);
        break;
      case(hash("bandwidth")):
        paramRange = getVectorExtrema(partial.bandwidth);
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

inline void cutHighs(VutuPartialsData& p, float fCut)
{
  for(int i=0; i < p.partials.size(); ++i)
  {
    const VutuPartial& partial = p.partials[i];
    
    // if any instantaneous frequency of partial is > f, remove the partial
    float fMax = *std::max_element(partial.freq.begin(), partial.freq.end());
    if(fMax > fCut)
    {
      p.partials[i] = VutuPartial();
    }
  }
  
  // do cleanOutliers() afterwards to remove empty Partials
}

inline void cleanOutliers(VutuPartialsData& p)
{
  int before, after;
  
  auto discardPartial = [](const VutuPartial& p){
    return p.time.size() <= 1;
  };
  
  before = p.partials.size();
  p.partials.erase(std::remove_if(p.partials.begin(), p.partials.end(), discardPartial), p.partials.end());
  after = p.partials.size();
  std::cout << "cleanOutliers: before: " << before << ", after: " << after << "\n";
}

// Get stats for partials data to aid synthesis and drawing.
// TODO check that time is monotonically increasing
//
inline void calcStats(VutuPartialsData& p)
{
  p.stats.timeRange = getParamRangeInPartials(p, "time");
  p.stats.ampRange = getParamRangeInPartials(p, "amp");
  p.stats.bandwidthRange = getParamRangeInPartials(p, "bandwidth");
  p.stats.freqRange = getParamRangeInPartials(p, "freq");
  p.stats.nPartials = p.partials.size();
  
  std::cout << "calcStats: " <<   p.stats.nPartials << " partials. \n";
  std::cout << "    timeRange: " <<   p.stats.timeRange << "\n";
  // store min, max times for each partial
  p.stats.partialTimeRanges.clear();
  for(int i=0; i<p.stats.nPartials; ++i)
  {
    const VutuPartial& partial = p.partials[i];
    if(partial.time.size() > 0)
    {
      const auto& timeVec = partial.time;
      Interval ptr = getVectorExtrema(timeVec);
      p.stats.partialTimeRanges.push_back(ptr);
    }
    else
    {
      p.stats.partialTimeRanges.push_back(Interval{0, 0});
    }
  }
  
  // calc max simultaneous partials:
  //
  // push all start and end times

  std::vector< std::pair< float, bool > > startAndEndTimes;
  for(const auto& startAndEnd : p.stats.partialTimeRanges)
  {
    startAndEndTimes.push_back(std::pair< float, bool >{startAndEnd.mX1, 0});
    startAndEndTimes.push_back(std::pair< float, bool >{startAndEnd.mX2, 1});
  }
  // sort them
  std::sort(startAndEndTimes.begin(), startAndEndTimes.end(), [](std::pair< float, bool > a, std::pair< float, bool > b){
    return a.first < b.first;
  });
  // walk the sorted list keeping track of max simultaneously active partials
  int activePartials{0};
  int maxActive{0};
  float maxActiveTime{0.f};
  for(auto& p : startAndEndTimes)
  {
    if(p.second)
    {
      activePartials--;
    }
    else
    {
      activePartials++;
      maxActive = std::max(activePartials, maxActive);
      if(maxActive == activePartials)
      {
        maxActiveTime = p.first;
      }
    }
    
    // std::cout << "time: " << p.time << (p.isEnd ? "-" : "+") << ", n = " << activePartials << "\n";
  }
  
  assert(activePartials == 0);
  
  p.stats.maxActivePartials = maxActive;
  p.stats.maxActiveTime = maxActiveTime;
  
  std::cout << "\n\n max active partials: " << p.stats.maxActivePartials <<  " at time: " << p.stats.maxActiveTime << "\n";
}

// get an interpolated frame of data from the partial index p of the VutuPartialsData at time t.
// note that the VutuPartialsData stats must be filled in first!
//
inline PartialFrame getPartialFrame(const VutuPartialsData& partialData, size_t partialIndex, float t)
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

// get an interpolated frame of data from the partial index p of the VutuPartialsData at time t.
// note that the VutuPartialsData stats must be filled in first!
//
inline PartialFrame getPartialFrameNearest(const VutuPartialsData& partialData, size_t partialIndex, float t)
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


// get an interpolated frame of data from the partial index p of the VutuPartialsData at time t.
// note that the VutuPartialsData stats must be filled in first!
//
inline PartialFrame getPartialFrameByIndex(const VutuPartialsData& partialData, size_t partialIndex, size_t frameIndex)
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


// return a JSON object representing the partials. The caller is responsible for freeing the object.
//
inline JSONHolder vutuPartialsToJSON(const VutuPartialsData& partialsData)
{
  JSONHolder root;
  
  cJSON_AddNumberToObject(root.data(), "version", kVutuPartialsFileVersion);
  cJSON_AddStringToObject(root.data(), "type", kVutuPartialsFileType);
  cJSON_AddStringToObject(root.data(), "source", partialsData.sourceFile.getText());
  
  // add analysis parameters
  cJSON_AddNumberToObject(root.data(), "resolution", partialsData.resolution);
  cJSON_AddNumberToObject(root.data(), "window_width", partialsData.windowWidth);
  cJSON_AddNumberToObject(root.data(), "amp_floor", partialsData.ampFloor);
  cJSON_AddNumberToObject(root.data(), "freq_drift", partialsData.freqDrift);
  cJSON_AddNumberToObject(root.data(), "lo_cut", partialsData.loCut);
  cJSON_AddNumberToObject(root.data(), "hi_cut", partialsData.hiCut);
  cJSON_AddNumberToObject(root.data(), "fundamental", partialsData.fundamental);

  const size_t nPartials = partialsData.partials.size();
  
  std::cout << "exporting " << nPartials << " partials \n";
  
  for(int i=0; i<nPartials; ++i)
  {
    // TODO make const-aware version of cJSON?
    VutuPartial& sp = const_cast<VutuPartial&>(partialsData.partials[i]);
    
    size_t partialLength = sp.time.size();
    // todo check partial arrays match in size
    
    TextFragment partialIndexText ("p", textUtils::naturalNumberToText(i));
    
    auto pNewJSONPartial = cJSON_CreateObject();
    cJSON_AddItemToObject(pNewJSONPartial, "time", cJSON_CreateFloatArray(sp.time.data(), partialLength));
    cJSON_AddItemToObject(pNewJSONPartial, "amp", cJSON_CreateFloatArray(sp.amp.data(), partialLength));
    cJSON_AddItemToObject(pNewJSONPartial, "freq", cJSON_CreateFloatArray(sp.freq.data(), partialLength));
    cJSON_AddItemToObject(pNewJSONPartial, "bw", cJSON_CreateFloatArray(sp.bandwidth.data(), partialLength));
    cJSON_AddItemToObject(pNewJSONPartial, "phase", cJSON_CreateFloatArray(sp.phase.data(), partialLength));
    cJSON_AddItemToObject(root.data(), partialIndexText.getText(), pNewJSONPartial);
  }
  return root;
}


// return a binary blob representing the partials. The caller is responsible for freeing the object.
//
inline std::vector<uint8_t> vutuPartialsToBinary(const VutuPartialsData& partialsData)
{
  Tree<Value> tree;
  tree["version"] = kVutuPartialsFileVersion;
  tree["type"] = kVutuPartials2FileType;
  tree["source"] = partialsData.sourceFile.getText();
  tree["resolution"] = partialsData.resolution;
  tree["window_width"] = partialsData.windowWidth;
  tree["amp_floor"] = partialsData.ampFloor;
  tree["freq_drift"] = partialsData.freqDrift;
  tree["lo_cut"] = partialsData.loCut;
  tree["hi_cut"] = partialsData.hiCut;
  tree["fundamental"] = partialsData.fundamental;

  const size_t nPartials = partialsData.partials.size();
  tree["n_partials"] = nPartials;

  std::cout << "exporting " << nPartials << " partials as binary\n";

  
  for(int i=0; i<nPartials; ++i)
  {
    VutuPartial& sp = const_cast<VutuPartial&>(partialsData.partials[i]);
    
    size_t partialLength = sp.time.size();
    
    TextFragment partialIndexText ("p", textUtils::naturalNumberToText(i));
    size_t arrayBytes = partialLength*sizeof(float);
    
    Value timeBlob(sp.time.data(), arrayBytes);
    Path timePath(Symbol(partialIndexText), "time");
    tree[timePath] = timeBlob;
    
    Value ampBlob(sp.amp.data(), arrayBytes);
    Path ampPath(Symbol(partialIndexText), "amp");
    tree[ampPath] = ampBlob;
    
    Value freqBlob(sp.freq.data(), arrayBytes);
    Path freqPath(Symbol(partialIndexText), "freq");
    tree[freqPath] = freqBlob;
    
    Value bwBlob(sp.bandwidth.data(), arrayBytes);
    Path bwPath(Symbol(partialIndexText), "bw");
    tree[bwPath] = bwBlob;
    
    Value phaseBlob(sp.phase.data(), arrayBytes);
    Path phasePath(Symbol(partialIndexText), "phase");
    tree[phasePath] = phaseBlob;
  }

  return valueTreeToBinary(tree);
}

inline std::vector< float > getPartialDataFromTree(const Tree<Value>& tree, int partialIdx, Path pname)
{
  TextFragment partialIndexText ("p", textUtils::naturalNumberToText(partialIdx));
  Path dataPath(Symbol(partialIndexText), pname);
  Value dataBlob = tree[dataPath];
  char* blobDataPtr = static_cast<char*>(dataBlob.getBlobValue());
  unsigned blobSize = dataBlob.getBlobSize();
  
  const float* pVectorData{reinterpret_cast<const float*>(blobDataPtr)};
  unsigned sizeInFloats = blobSize/sizeof(float);
  return std::vector<float>(pVectorData, pVectorData + sizeInFloats);
}

// parse the binary data and return a new VutuPartialsData object.
//
inline VutuPartialsData* binaryToVutuPartials(const std::vector<unsigned char>& binaryData)
{
  VutuPartialsData* partials = new VutuPartialsData;
  const uint8_t* pData{binaryData.data()};

  Tree<Value> tree = binaryToValueTree(binaryData);
  if(tree.getNode("n_partials"))
  {
    size_t nPartials = tree["n_partials"].getUnsignedLongValue();
    size_t res = tree["resolution"].getFloatValue();
    
    if(nPartials > 0)
    {
      partials->version = tree["version"].getIntValue();
      partials->sourceDuration = tree["source_duration"].getFloatValue();
      partials->resolution = tree["resolution"].getFloatValue();
      partials->windowWidth = tree["window_width"].getFloatValue();
      partials->ampFloor = tree["amp_floor"].getFloatValue();
      partials->freqDrift = tree["freq_drift"].getFloatValue();
      partials->loCut = tree["lo_cut"].getFloatValue();
      partials->hiCut = tree["hi_cut"].getFloatValue();
      partials->fundamental = tree["fundamental"].getFloatValue();

      partials->partials.resize(nPartials);
      std::cout << "reading " << nPartials << " partials from binary\n";
      for(int i=0; i<nPartials; ++i)
      {
        partials->partials[i].time = getPartialDataFromTree(tree, i, "time");
        partials->partials[i].amp = getPartialDataFromTree(tree, i, "amp");
        partials->partials[i].freq = getPartialDataFromTree(tree, i, "freq");
        partials->partials[i].bandwidth = getPartialDataFromTree(tree, i, "bw");
        partials->partials[i].phase = getPartialDataFromTree(tree, i, "phase");
      }
    }
  }

  calcStats(*partials);
  return partials;
}

// parse the JSON and return a new VutuPartialsData object.
//
inline VutuPartialsData* jsonToVutuPartials(JSONHolder& jsonData)
{
  VutuPartialsData* pVutuPartials = new VutuPartialsData;
  
  cJSON* rootObj = jsonData.data();
  assert(rootObj->type == cJSON_Object);
  
  cJSON* obj = rootObj->child;
  while (obj)
  {
    TextFragment objStr(obj->string);
    switch(obj->type)
    {
      case cJSON_Number:
      {
        auto stringHash = hash(objStr);
        switch(stringHash)
        {
          case(hash("version")):
            pVutuPartials->version = obj->valueint;
            break;
          case(hash("source_duration")):
            pVutuPartials->sourceDuration = obj->valuedouble;
            break;
          case(hash("resolution")):
            pVutuPartials->resolution = obj->valuedouble;
            break;
          case(hash("window_width")):
            pVutuPartials->windowWidth = obj->valuedouble;
            break;
          case(hash("amp_floor")):
            pVutuPartials->ampFloor = obj->valuedouble;
            break;
          case(hash("freq_drift")):
            pVutuPartials->freqDrift = obj->valuedouble;
            break;
          case(hash("lo_cut")):
            pVutuPartials->loCut = obj->valuedouble;
            break;
          case(hash("hi_cut")):
            pVutuPartials->hiCut = obj->valuedouble;
            break;
          case(hash("fundamental")):
            pVutuPartials->fundamental = obj->valuedouble;
            break;
        }
        
        // TEMP
        std::cout << " loaded JSON " << objStr << " : " << obj->valuedouble << "\n";

        break;
      }
      case cJSON_String:
      {
        auto stringHash = hash(objStr);
        switch(stringHash)
        {
          case(hash("type")):
            pVutuPartials->type = Symbol(obj->valuestring);
            break;
          case(hash("source")):
            pVutuPartials->sourceFile = TextFragment(obj->valuestring);
            break;
        }
        break;
      }
      case cJSON_Object:
      {
        if(objStr.beginsWith("p")) // get partial
        {
          //std::cout << "partial:" << pStr << "\n";
          
          pVutuPartials->partials.emplace_back(VutuPartial());
          VutuPartial* pNewPartial = &pVutuPartials->partials.back();
          
          cJSON* jsonArrays = obj->child;
          
          auto pushAllArrayItems = [&](std::vector< float >& destVec){
            for(cJSON* arrayItem = jsonArrays->child; arrayItem; arrayItem = arrayItem->next)
            {
              destVec.push_back(float(arrayItem->valuedouble));
            };
          };
          
          // parse arrays within partial. Arrays can appear in any order.
          int nArrays{0};
          while(jsonArrays)
          {
            assert(jsonArrays->type == cJSON_Array);
            Symbol dataType(jsonArrays->string);
            switch(hash(dataType))
            {
              case(hash("time")):
              {
                pushAllArrayItems(pNewPartial->time);
                break;
              }
              case(hash("amp")):
              {
                pushAllArrayItems(pNewPartial->amp);
                break;
              }
              case(hash("freq")):
              {
                pushAllArrayItems(pNewPartial->freq);
                break;
              }
              case(hash("bw")):
              {
                pushAllArrayItems(pNewPartial->bandwidth);
                break;
              }
              case(hash("phase")):
              {
                pushAllArrayItems(pNewPartial->phase);
                break;
              }
              default:
              {
                // TODO
                std::cout << "warning: unknown data in partial!\n";
                break;
              }
            }
            
            jsonArrays = jsonArrays->next;
            nArrays++;
            
          }
        }
        break;
      }
    }
    obj = obj->next;
  }
  calcStats(*pVutuPartials);
  return pVutuPartials;
}


// load Vutu partials from the file. If successful, creates a new VutuPartialsData object that the caller must own.
//
inline VutuPartialsData* loadVutuPartialsFromFile(const File& fileToLoad)
{
  VutuPartialsData* newPartials{nullptr};
  
  // TODO verify successful load
  
  Path filePath = fileToLoad.getFullPath();
  Symbol extension = getExtensionFromPath(filePath);
  
  if(extension == "utu")
  {
    TextFragment partialsText;
    if(fileToLoad.loadAsText(partialsText))
    {
      auto partialsJSON = textToJSON(partialsText);
      newPartials = jsonToVutuPartials(partialsJSON);
    }
  }
  if(extension == "ut2")
  {
    BinaryVector binaryData;
    if(fileToLoad.load(binaryData))
    {
      newPartials = binaryToVutuPartials(binaryData);
    }
  }
  
  // if we didn't save a source duration, fake one from partials data
  if(newPartials->sourceDuration == 0.0f)
  {
    std::cout << "No duration found! using partials range " << newPartials->stats.timeRange << "\n";
    newPartials->sourceDuration = newPartials->stats.timeRange.mX2;
  }
  
  return newPartials;
}



}

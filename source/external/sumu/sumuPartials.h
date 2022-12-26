
// vutu
// Copyright (c) 2022 Madrona Labs LLC. http://www.madronalabs.com

#pragma once

#include "mldsp.h"

#include "madronalib.h"

// stats about partials

namespace ml
{

static constexpr size_t kPartialsPerVoice{ 64 };
static constexpr float kSumuPartialsFileVersion{ 1.0 };
static constexpr char kSumuPartialsFileType[] = "sumuPartials";

struct PartialsStats
{
  // get range of values for each parameter (except phase)
  float maxTimeInSeconds; // duration of the whole analyzed sample
  Interval timeRange; // min and max time in all partials
  Interval ampRange;
  Interval bandwidthRange;
  Interval freqRange;
  size_t nPartials;
  size_t maxFrames;   // max number of frames in any partial
  std::vector< Interval > partialTimeRanges; // time range for each partial
};

struct SumuPartial
{
  std::vector< float > time;
  std::vector< float > amp;
  std::vector< float > freq;
  std::vector< float > bandwidth;
  std::vector< float > phase;
};

struct SumuPartialsData
{
  PartialsStats stats;
  std::vector< SumuPartial > partials;
  int version;
  Symbol type;
  
  void calcStats();
};

struct PartialFrame
{
  float amp{0};
  float freq{0};
  float bandwidth{0};
  float phase{0};
};

Interval getVectorRange(const std::vector< float >& vec);
Interval getParamRangeInPartials(const SumuPartialsData& partialData, Symbol param);

// return an interpolated frame of a single Partial at the specified time.
PartialFrame getPartialFrame(const SumuPartialsData& partialData, size_t partialIndex, float time);

// return an interpolated frame of a single Partial at the specified time.
PartialFrame getPartialFrameDownsampled(const SumuPartialsData& partialData, size_t partialIndex, float time, float dTime);

// return a frame of a single Partial at the nearest sample to the specified time.
PartialFrame getPartialFrameNearest(const SumuPartialsData& partialData, size_t partialIndex, float time);

// return a frame of a single Partial by its frame index.
PartialFrame getPartialFrameByIndex(const SumuPartialsData& partialData, size_t partialIndex, size_t frameIndex);

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


// return a JSON object representing the partials. The caller is responsible for freeing the object.
//
inline JSONHolder sumuPartialsToJSON(const SumuPartialsData& partialsData)
{
  JSONHolder root;
  
  cJSON_AddNumberToObject(root.data(), "version", kSumuPartialsFileVersion);
  cJSON_AddStringToObject(root.data(), "type", kSumuPartialsFileType);
  
  const size_t nPartials = partialsData.partials.size();
  
  std::cout << "exporting " << nPartials << " partials \n";
  
  for(int i=0; i<nPartials; ++i)
  {
    // TODO make const-aware version of cJSON?
    SumuPartial& sp = const_cast<SumuPartial&>(partialsData.partials[i]);
    
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


// parse the JSON and return a new SumuPartialsData object.
//
inline SumuPartialsData* jsonToSumuPartials(JSONHolder& jsonData)
{
  SumuPartialsData* pSumuPartials = new SumuPartialsData;
  
  cJSON* rootObj = jsonData.data();
  assert(rootObj->type == cJSON_Object);
  
  cJSON* obj = rootObj->child;
  while (obj)
  {
    switch(obj->type)
    {
      case cJSON_Number:
      {
        if(TextFragment(obj->string) == "version")
        {
          pSumuPartials->version = obj->valueint;
          std::cout << "version: " << pSumuPartials->version << "\n";
        }
        break;
      }
      case cJSON_String:
      {
        if(TextFragment(obj->string) == "type")
        {
          pSumuPartials->type = Symbol(obj->valuestring);
          std::cout << "type: " << pSumuPartials->type << "\n";
        }
        break;
      }
      case cJSON_Object:
      {
        TextFragment pStr(obj->string);
        if(pStr.beginsWith("p"))
        {
          
          std::cout << "partial:" << pStr << "\n";
          
          pSumuPartials->partials.emplace_back(SumuPartial());
          SumuPartial* pNewPartial = &pSumuPartials->partials.back();
          
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
  
  pSumuPartials->calcStats();
  return pSumuPartials;
}



}

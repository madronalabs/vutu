// mlvg test application
// Copyright (C) 2019-2022 Madrona Labs LLC
// This software is provided 'as-is', without any express or implied warranty.
// See LICENSE.txt for details.

#define stringAppName "Vutu"
#define stringAppNameLC "vutu"
#define stringMakerName "Madrona Labs"

#define MAJOR_VERSION_STR "0"
#define MAJOR_VERSION_INT 0

#define SUB_VERSION_STR "1"
#define SUB_VERSION_INT 1

#define RELEASE_NUMBER_STR "0"
#define RELEASE_NUMBER_INT 0

#define BETA_NUMBER_STR "0"
#define BETA_NUMBER_INT 0

#define BUILD_NUMBER_INT 0

#define SHORT_VERSION_STR "0.1.0"
#define VERSION_STR  SHORT_VERSION_STR

#define FULL_VERSION_STR "0.1.0"

#if defined(__x86_64__) || defined(_M_X64)
#define ML_ARCH "x86_64"
#elif defined(i386) || defined(__i386__) || defined(__i386) || defined(_M_IX86)
#define ML_ARCH "x86_32";
#elif defined(__arm__) || defined(__aarch64__)
#define ML_ARCH "Arm"
#else
#define ML_ARCH "unknown"
warning ML_ARCH is undefined!
#endif


inline const char* getAppVersion()
{
  return (FULL_VERSION_STR);
}

inline const char* getAppName()
{
  return (stringAppName);
}

inline const char* getMakerName()
{
  return (stringMakerName);
}

inline const char* getAppArchitecture()
{
  return (ML_ARCH);
}



// vutu (c) 2022, Madrona Labs LLC, all rights reserved
// see LICENSE.txt for details

#define MAJOR_VERSION_STR "0"
#define MAJOR_VERSION_INT 0

#define SUB_VERSION_STR "9"
#define SUB_VERSION_INT 9

#define RELEASE_NUMBER_STR "3"
#define RELEASE_NUMBER_INT 3

#define BETA_NUMBER_STR "0"
#define BETA_NUMBER_INT 0

#define BUILD_NUMBER_INT 0

#define SHORT_VERSION_STR "0.9.3"
#define VERSION_STR  SHORT_VERSION_STR

#define FULL_VERSION_STR "0.9.3"

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
  return (stringPluginName);
}

inline const char* getMakerName()
{
  return (stringCompanyName);
}

inline const char* getAppArchitecture()
{
  return (ML_ARCH);
}

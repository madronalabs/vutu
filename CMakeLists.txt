# root CMakeLists.txt for utu-view.
# to build:
#
# Mac OS:
# mkdir build
# cd build
# cmake -GXcode ..
#
# Windows:
# cmake -G"Visual Studio 14 2015 Win64" ..

cmake_minimum_required (VERSION 3.16)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11" CACHE STRING "Minimum OS X deployment version")

# cmake_policy(SET CMP0114 NEW)

IF(APPLE)
 # uncomment this to build a universal binary for both platforms.
 # SET(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for Mac OS X" FORCE)
ENDIF(APPLE)

project(utu-view)

#--------------------------------------------------------------------
# Compiler flags
#--------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

#--------------------------------------------------------------------
# find madronalib
#--------------------------------------------------------------------

if(APPLE)
  include(GNUInstallDirs)
  set (MADRONALIB_INCLUDE_DIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}/madronalib")
  set (MADRONALIB_LIBRARY_DIR "${CMAKE_INSTALL_FULL_LIBDIR}")
elseif(WIN32)
  set (MADRONALIB_INCLUDE_DIR "C:/Program Files/madronalib/include")
  set (MADRONALIB_LIBRARY_DIR "C:/Program Files/madronalib/lib")
else()
endif()

# add -debug suffix to link debug madronalib for debug builds
set(madronalib_NAME madrona$<$<CONFIG:Debug>:-debug>)

message("madronalib headers should be in: " ${MADRONALIB_INCLUDE_DIR} )
message("madronalib library should be at: " ${MADRONALIB_LIBRARY_DIR}/${madronalib_NAME} )

message("cmake lib dir: " ${CMAKE_INSTALL_FULL_LIBDIR} )

#--------------------------------------------------------------------
# find loris
#--------------------------------------------------------------------

if(APPLE)
    set (LORIS_INCLUDE_DIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}/loris")
    set (LORIS_LIBRARY_DIR "${CMAKE_INSTALL_FULL_LIBDIR}")
elseif(WIN32)
    set (LORIS_INCLUDE_DIR "C:/Program Files/loris/include")
    set (LORIS_LIBRARY_DIR "C:/Program Files/loris/lib")
else()
endif()

# add -debug suffix to link debug loris for debug builds
set(loris_NAME loris$<$<CONFIG:Debug>:-debug>)

message("loris headers should be in: " ${LORIS_INCLUDE_DIR} )
message("loris library should be at: " ${LORIS_LIBRARY_DIR}/${loris_NAME} )

#--------------------------------------------------------------------
# find mlvg 
#--------------------------------------------------------------------

if(APPLE)
   include(GNUInstallDirs)
   set (MLVG_INCLUDE_DIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}/mlvg")
   set (MLVG_LIBRARY_DIR "${CMAKE_INSTALL_FULL_LIBDIR}")
elseif(WIN32)
   set (MLVG_INCLUDE_DIR "C:/Program Files/mlvg/include")
   set (MLVG_LIBRARY_DIR "C:/Program Files/mlvg/lib")
else()
endif()

# add -debug suffix to link debug library for debug builds
set(mlvg_NAME mlvg$<$<CONFIG:Debug>:-debug>)

message("mlvg headers should be in: " ${MLVG_INCLUDE_DIR} )
message("mlvg library should be at: " ${MLVG_LIBRARY_DIR}/${mlvg_NAME} )

#--------------------------------------------------------------------
# find mlvg sources - including our juce-core subset and nfd
#--------------------------------------------------------------------

set (MLVG_SOURCE_DIR "../mlvg/source")

#--------------------------------------------------------------------
# compile binary resources
#--------------------------------------------------------------------

# Creates C resources file from files in given directory
function(create_resources dir outputdir)

  # Collect input files
  file(GLOB bins ${dir}/*)

  # Create empty main include file
  set(includefile "${outputdir}/resources.c")
  file(WRITE "${includefile}" "")

  # Iterate through input files
  foreach(bin ${bins})
  
      # Get short filename
      string(REGEX MATCH "([^/]+)$" filename ${bin})

      # Replace filename spaces & extension separator for C compatibility
      string(REGEX REPLACE "\\.| |-" "_" filename ${filename})

      # Read hex data from file
      file(READ ${bin} filedata HEX)

      # Convert hex data for C compatibility
      string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," filedata ${filedata})

      # Create empty output file
      set(outputfile "${outputdir}/${filename}.c")
      file(WRITE "${outputfile}" "namespace resources \n{\n")

      # Append data to output file
      file(APPEND "${outputfile}" "const unsigned char ${filename}[] = {${filedata}};\nconst unsigned ${filename}_size = sizeof(${filename});\n")
      file(APPEND "${outputfile}" "\n}")

      # Append filename to main include file
      file(APPEND "${includefile}" "#include \"${filename}.c\"\n")

  endforeach()

endfunction()

create_resources (resource build/resources)

#--------------------------------------------------------------------
# gather nanovg and nanosvg sources
#--------------------------------------------------------------------

if(APPLE)
  set(NANOVG_SOURCES
      ${MLVG_SOURCE_DIR}/external/nanovg/src/nanovg.c
      ${MLVG_SOURCE_DIR}/external/nanovg/src/nanovg.h
      ${MLVG_SOURCE_DIR}/external/nanosvg/src/nanosvg.h
      ${MLVG_SOURCE_DIR}/external/MetalNanoVG/src/nanovg_mtl.h
      ${MLVG_SOURCE_DIR}/external/MetalNanoVG/src/nanovg_mtl.m
  )
  set(NANOVG_INCLUDE_DIRS
      ${MLVG_SOURCE_DIR}/external/nanovg/src
      ${MLVG_SOURCE_DIR}/external/nanosvg/src
      ${MLVG_SOURCE_DIR}/external/MetalNanoVG/src
  )
elseif(WIN32)
  set(NANOVG_SOURCES
      ${MLVG_SOURCE_DIR}/external/nanovg/src/nanovg.c
      ${MLVG_SOURCE_DIR}/external/nanovg/src/nanovg.h
      ${MLVG_SOURCE_DIR}/external/nanosvg/src/nanosvg.h
      ${MLVG_SOURCE_DIR}/external/nanovg/src/nanovg_gl.h
      ${MLVG_SOURCE_DIR}/external/nanovg/src/nanovg_gl_utils.h
      ${MLVG_SOURCE_DIR}/external/glad/src/glad.c
      ${MLVG_SOURCE_DIR}/external/glad/include/glad/glad.h
      ${MLVG_SOURCE_DIR}/external/glad/include/KHR/khrplatform.h
  )
  set(NANOVG_INCLUDE_DIRS
      ${MLVG_SOURCE_DIR}/external/nanovg/src
      ${MLVG_SOURCE_DIR}/external/nanosvg/src
      ${MLVG_SOURCE_DIR}/external/glad/include
      ${MLVG_SOURCE_DIR}/external/glad/include/glad
  )
  if (MSVC)
      set_source_files_properties(source/external/glad/src/glad.c
      PROPERTIES COMPILE_FLAGS /wd4055)
  endif()
endif()

#--------------------------------------------------------------------
# gather juce-core sources
#--------------------------------------------------------------------

if(APPLE)
  set(juce_core_sources
      ${MLVG_SOURCE_DIR}/external/juce_core/juce_core.mm
      ${MLVG_SOURCE_DIR}/external/juce_core/JuceCoreConfig.h
  )
  set(juce_core_include_dirs
  ${MLVG_SOURCE_DIR}/external/juce_core
  ${MLVG_SOURCE_DIR}
  )
else()
  set(juce_core_sources
      ${MLVG_SOURCE_DIR}/external/juce_core/juce_core.cpp
      ${MLVG_SOURCE_DIR}/external/juce_core/JuceCoreConfig.h
  )
  set(juce_core_include_dirs
  ${MLVG_SOURCE_DIR}/external/juce_core
  ${MLVG_SOURCE_DIR}
  )
endif()


 #--------------------------------------------------------------------
 # make juce-core library
 #--------------------------------------------------------------------
 
 set(target juce-core)
 
 add_library(${target} STATIC ${juce_core_sources})
 
 target_include_directories(${target} PRIVATE ${juce_core_include_dirs})
 
 # set juce-core configuration for this target
 target_compile_definitions(${target} PRIVATE JUCE_APP_CONFIG_HEADER="external/juce_core/JuceCoreConfig.h")
 target_compile_definitions(${target} PUBLIC "$<$<CONFIG:DEBUG>:DEBUG>")
 target_compile_definitions(${target} PUBLIC "$<$<CONFIG:RELEASE>:NDEBUG>")
 
 if(APPLE)
     # ARC must be off for juce
     target_compile_options(${target} PRIVATE "-fno-objc-arc")
 
     # build only active arch for Debug builds
     set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH[variant=Debug] "YES")
 
     # set debug info format
     set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")
 endif()
 
 #--------------------------------------------------------------------
 # gather nfd sources
 #--------------------------------------------------------------------
 
 set(nfd_sources
   ${MLVG_SOURCE_DIR}/external/nativefiledialog-extended/src/include/nfd.h
   ${MLVG_SOURCE_DIR}/external/nativefiledialog-extended/src/include/nfd.hpp)
 
 if(WIN32)
     set(nfd_sources_native
         ${MLVG_SOURCE_DIR}/external/nativefiledialog-extended/src/nfd_win.cpp
     )
 elseif(APPLE)
     set(nfd_sources_native
         ${MLVG_SOURCE_DIR}/external/nativefiledialog-extended/src/nfd_cocoa.m
     )
 elseif(UNIX AND NOT APPLE)
     set(nfd_sources_native
         ${MLVG_SOURCE_DIR}/external/nativefiledialog-extended/src/nfd_gtk.cpp
     )
 endif()
 
 list(APPEND nfd_sources ${nfd_sources_native} )
 
 #--------------------------------------------------------------------
 # make nfd library
 #--------------------------------------------------------------------
 
 set(target nfd)
 
 # Define the library
 add_library(${target} STATIC ${nfd_sources})
 
 # Allow includes from include/
 target_include_directories(${target} PUBLIC ${MLVG_SOURCE_DIR}/external/nativefiledialog-extended/src/include/)
 
 # find and link libraries 
 if(WIN32)
 
 elseif(APPLE)
     find_library(APPKIT_LIBRARY AppKit)
     target_link_libraries(${target} PRIVATE ${APPKIT_LIBRARY})
 elseif(UNIX AND NOT APPLE)
     find_package(PkgConfig REQUIRED)
     pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
     message("Using GTK version: ${GTK3_VERSION}")
     target_include_directories(${target} PRIVATE ${GTK3_INCLUDE_DIRS})
     target_link_libraries(${target} PRIVATE ${GTK3_LIBRARIES})
 endif()
 
 # compiler settings
 if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
   string(REPLACE "/EHsc" "/EHs-c-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
   string(REPLACE "/GR" "/GR-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
   set_property(TARGET ${target} APPEND_STRING PROPERTY STATIC_LIBRARY_OPTIONS /NODEFAULTLIB)
 endif()
 
 if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
   target_compile_options(${target} PRIVATE -nostdlib -fno-exceptions -fno-rtti)
 endif()
 
 if(APPLE)
     # build only active arch for Debug builds
     set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH[variant=Debug] "YES")
 
     # set debug info format
     set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")
 endif()
 
 #--------------------------------------------------------------------
 # libsndfile
 #--------------------------------------------------------------------
 
 if(MSVC)
   set(ENABLE_STATIC_RUNTIME OFF CACHE INTERNAL "")
 endif()
 set(ENABLE_EXTERNAL_LIBS OFF CACHE INTERNAL "")
 set(ENABLE_CPACK OFF CACHE INTERNAL "")
 set(ENABLE_PACKAGE_CONFIG OFF CACHE INTERNAL "")
 set(BUILD_TESTING OFF CACHE INTERNAL "")
 set(BUILD_PROGRAMS OFF CACHE INTERNAL "")
 set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
 add_subdirectory(source/external/libsndfile)
 
 
#--------------------------------------------------------------------
# set source groups for some source above
#--------------------------------------------------------------------

# source_group(mlvg-native REGULAR_EXPRESSION "${MLVG_SOURCE_DIR}/vg/native.*")
source_group(nanovg REGULAR_EXPRESSION "${MLVG_SOURCE_DIR}/external/nanovg/src.*|${MLVG_SOURCE_DIR}/external/MetalNanoVG/src.*")
source_group(nanosvg REGULAR_EXPRESSION "${MLVG_SOURCE_DIR}/external/nanosvg/src.*")

#--------------------------------------------------------------------
# make mlvg / SDL2 application target 
#--------------------------------------------------------------------

set(target utu-view)

create_resources (source/resources build/resources/utu-view)
 
file(GLOB COMMON_SOURCES "${CMAKE_SOURCE_DIR}/source/common/*.cpp")
file(GLOB COMMON_INCLUDES "${CMAKE_SOURCE_DIR}/source/common/*.h")

file(GLOB SUMU_SOURCES "${CMAKE_SOURCE_DIR}/source/external/sumu/*.cpp")
file(GLOB SUMU_INCLUDES "${CMAKE_SOURCE_DIR}/source/external/sumu/*.h")

file(GLOB UTU_SOURCES "${CMAKE_SOURCE_DIR}/source/external/utu/*.cpp")
file(GLOB UTU_INCLUDES "${CMAKE_SOURCE_DIR}/source/external/utu/*.h")

file(GLOB WIDGETS_SOURCES "${CMAKE_SOURCE_DIR}/source/widgets/*.cpp")
file(GLOB WIDGETS_INCLUDES "${CMAKE_SOURCE_DIR}/source/widgets/*.h")

# add utu source
list(APPEND app_sources ${COMMON_SOURCES} ${SUMU_SOURCES} ${UTU_SOURCES} ${WIDGETS_SOURCES} )
list(APPEND app_sources ${COMMON_INCLUDES} ${SUMU_INCLUDES} ${UTU_INCLUDES} ${WIDGETS_INCLUDES} )

add_executable(${target} ${app_sources})

# add include dirs
target_include_directories(${target} PRIVATE  "${CMAKE_SOURCE_DIR}/source" )
target_include_directories(${target} PRIVATE  "${CMAKE_SOURCE_DIR}/source/common" )
target_include_directories(${target} PRIVATE  "${CMAKE_SOURCE_DIR}/source/external/sumu" )
target_include_directories(${target} PRIVATE  "${CMAKE_SOURCE_DIR}/source/external/utu" )
target_include_directories(${target} PRIVATE  "${CMAKE_SOURCE_DIR}/source/widgets" )

# add SDL2
find_package(SDL2 REQUIRED)
target_include_directories(${target} PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(${target} PRIVATE ${SDL2_LIBRARIES})

# add madronalib library
target_include_directories(${target} PRIVATE ${MADRONALIB_INCLUDE_DIR})
if(APPLE)
    target_link_libraries(${target} PRIVATE "${MADRONALIB_LIBRARY_DIR}/lib${madronalib_NAME}.a")   
elseif(WIN32)
    target_link_libraries(${target} PRIVATE "${MADRONALIB_LIBRARY_DIR}/${madronalib_NAME}.lib")
endif()

# add loris library
target_include_directories(${target} PRIVATE ${LORIS_INCLUDE_DIR})
if(APPLE)
    target_link_libraries(${target} PRIVATE "${LORIS_LIBRARY_DIR}/lib${loris_NAME}.a")   
elseif(WIN32)
    target_link_libraries(${target} PRIVATE "${LORIS_LIBRARY_DIR}/${loris_NAME}.lib")
endif()

# add mlvg library
target_include_directories(${target} PRIVATE ${MLVG_INCLUDE_DIR})
if(APPLE)
    target_link_libraries(${target} PRIVATE "${MADRONALIB_LIBRARY_DIR}/lib${mlvg_NAME}.a")   
elseif(WIN32)
    target_link_libraries(${target} PRIVATE "${MADRONALIB_LIBRARY_DIR}/${mlvg_NAME}.lib")
endif()

# add other external libraries
target_link_libraries(${target} PRIVATE mlvg nfd juce-core SndFile::sndfile)

if(APPLE)
    # ARC must be off for juce
    target_compile_options(${target} PRIVATE "-fno-objc-arc")
    
    # build only active arch for Debug builds
    set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH[variant=Debug] "YES")
    
    # set debug info format
    set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")
endif()
 
# set compiler options
if(APPLE)
    target_compile_options(${target} PRIVATE "-Werror=return-type")
elseif(WIN32)
    target_compile_options(${target} PRIVATE "/wd4068") # silence unknown pragma warning
    target_compile_options(${target} PRIVATE "/EHa") # standard C++ stack unwinding
endif()

target_compile_definitions(${target} PUBLIC "$<$<CONFIG:DEBUG>:DEBUG>")
target_compile_definitions(${target} PUBLIC "$<$<CONFIG:RELEASE>:NDEBUG>")

# add UI frameworks- note that these appear under 
# "other linker flags" in XCode and not in its file browser
if(APPLE)
    target_link_libraries(${target} PRIVATE "-framework Cocoa")
    target_link_libraries(${target} PRIVATE "-framework Metal")
    target_link_libraries(${target} PRIVATE "-framework MetalKit")
    target_link_libraries(${target} PRIVATE "-framework CoreAudio")
     target_link_libraries(${target} PRIVATE "${MADRONALIB_LIBRARY_DIR}/lib${madronalib_NAME}.a")   
elseif(WIN32)
    find_package(OpenGL REQUIRED)
    target_link_libraries(${target} PRIVATE ${OPENGL_gl_LIBRARY})
    target_link_libraries(${target} PRIVATE "${MADRONALIB_LIBRARY_DIR}/${madronalib_NAME}.lib")
endif()

if(APPLE)
    # build only active arch for Debug builds
    set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH[variant=Debug] "YES")

    # set debug info format
    set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")
endif() 



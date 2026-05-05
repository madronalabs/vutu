# root CMakeLists.txt for vutu.
# to build:
#
# Mac OS:
# mkdir build
# cd build
# cmake -GXcode ..
#
# Windows:
# cmake -G"Visual Studio 14 2015 Win64" ..

cmake_minimum_required (VERSION 3.5)

# CMake 4 dropped policy compatibility for projects requiring < 3.5;
# libsndfile (vendored under source/external) still requests an older minimum.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")

IF(APPLE)
  SET(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for Mac OS X" FORCE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.14" CACHE STRING "Minimum OS X deployment version")
ENDIF(APPLE)

project(Vutu)

#--------------------------------------------------------------------
# Compiler flags
#--------------------------------------------------------------------

 set(CMAKE_CXX_STANDARD 17)
 set(CMAKE_CXX_STANDARD_REQUIRED True)
 
 if(APPLE)
   # For now, explicitly disable C++17 alignment feature
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-aligned-new")
   # Loris 1.8 still uses std::auto_ptr / std::unary_function / std::bind2nd
   # which were removed in C++17. Re-enable them via libc++ feature flags.
   add_definitions(-D_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR=1
                   -D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION=1
                   -D_LIBCPP_ENABLE_CXX17_REMOVED_BINDERS=1)
 elseif(WIN32)
   # no unknown pragma warning
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4068")
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Zc:alignedNew-")
 endif()

if(MSVC)
    # arcane thing about setting runtime library flags
    cmake_policy(SET CMP0091 NEW)

    add_compile_options(
        $<$<CONFIG:>:/MT>
        $<$<CONFIG:Debug>:/MTd>
        $<$<CONFIG:Release>:/MT>
    )
endif()


#--------------------------------------------------------------------
# find madronalib + manzanita (GUI library, formerly mlvg)
#--------------------------------------------------------------------

find_package(madronalib REQUIRED)
find_package(manzanita REQUIRED)

#--------------------------------------------------------------------
# find loris library
#--------------------------------------------------------------------

if(APPLE)
    include(GNUInstallDirs)
    set (LORIS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../loris/src")
    set (LORIS_LIBRARY_DIR "${CMAKE_INSTALL_FULL_LIBDIR}")
elseif(WIN32)
    set (LORIS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../loris/src")
    set (LORIS_LIBRARY_DIR "C:/Program Files/loris/lib")
else()
endif()

# loris (autotools) only ships a single libloris.a regardless of build type.
set(loris_NAME loris)

message("loris headers should be in: " ${LORIS_INCLUDE_DIR} )
message("loris library should be at: " ${LORIS_LIBRARY_DIR}/${loris_NAME} )

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
# make libsndfile from source
#--------------------------------------------------------------------

if(MSVC)
 set(ENABLE_STATIC_RUNTIME OFF CACHE INTERNAL "")
endif()
set(ENABLE_EXTERNAL_LIBS OFF CACHE INTERNAL "")
set(ENABLE_MPEG OFF CACHE INTERNAL "")
set(ENABLE_CPACK OFF CACHE INTERNAL "")
set(ENABLE_PACKAGE_CONFIG OFF CACHE INTERNAL "")
set(BUILD_TESTING OFF CACHE INTERNAL "")
set(BUILD_PROGRAMS OFF CACHE INTERNAL "")
set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
add_subdirectory(source/external/libsndfile)


#--------------------------------------------------------------------
# make libresample from source
#--------------------------------------------------------------------

file(GLOB LIBRESAMPLE_SOURCES "${CMAKE_SOURCE_DIR}/source/external/libresample/src/*.c")
file(GLOB LIBRESAMPLE_INCLUDES "${CMAKE_SOURCE_DIR}/source/external/libresample/src/*.h")

set(target libresample)

# Define the library
add_library(${target} STATIC ${LIBRESAMPLE_SOURCES})

target_include_directories(${target} PUBLIC ${LIBRESAMPLE_INCLUDES})

#--------------------------------------------------------------------
# make mlvg /SDL2 application target 
#--------------------------------------------------------------------

set(target Vutu)

# convert any small binary resources we need to .c files
create_resources (source/resources build/resources/vutu)

# gather sources
file(GLOB COMMON_SOURCES "${CMAKE_SOURCE_DIR}/source/common/*.cpp")
file(GLOB COMMON_INCLUDES "${CMAKE_SOURCE_DIR}/source/common/*.h")

file(GLOB WIDGETS_SOURCES "${CMAKE_SOURCE_DIR}/source/widgets/*.cpp")
file(GLOB WIDGETS_INCLUDES "${CMAKE_SOURCE_DIR}/source/widgets/*.h")

list(APPEND app_sources ${COMMON_SOURCES} ${WIDGETS_SOURCES} )
list(APPEND app_sources ${COMMON_INCLUDES} ${WIDGETS_INCLUDES} )

if(APPLE)
elseif(WIN32)
    list(APPEND app_sources "${CMAKE_SOURCE_DIR}/source/windows/hidpi.manifest" )
endif()

# get icon file
if(APPLE)
  # NOTE: Don't include the path in MACOSX_BUNDLE_ICON_FILE -- this is
  # the property added to Info.plist
  set(MACOSX_BUNDLE_ICON_FILE vutu.icns)
  
  # And this part tells CMake where to find and install the file itself
  set(vutu_ICON ${CMAKE_CURRENT_SOURCE_DIR}/source/mac/vutu.icns)
  set_source_files_properties(${vutu_ICON} PROPERTIES
         MACOSX_PACKAGE_LOCATION "Resources")
endif()

add_executable(${target} WIN32 ${vutu_ICON} ${app_sources})

target_compile_definitions(${target} PRIVATE ML_INCLUDE_SDL=1)

# find SDL headers and libraries
if(APPLE)
    # to use the SDL2 framework in /Library/Frameworks, do this:
    find_package(SDL2 REQUIRED COMPONENTS SDL2)
elseif(WIN32)
        # it seems like there's no standard place to put the SDL dev tools on Windows.
        # we put them in an SDL dir alongside the main project directory.
        set(SDL2_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../SDL/include")

        # Support 64 bit builds
        set(SDL2_LIBRARIES_DIR "${CMAKE_CURRENT_LIST_DIR}/../SDL/VisualC/x64/Release")
endif()
message("SDL2 headers should be in: " ${SDL2_INCLUDE_DIRS} )
message("SDL2 libraries should be in: " ${SDL2_LIBRARIES_DIR} )
target_include_directories(${target} PRIVATE ${SDL2_INCLUDE_DIRS})
if(WIN32)
    target_link_libraries(${target} PRIVATE "${SDL2_LIBRARIES_DIR}/SDL2.lib")
    target_link_libraries(${target} PRIVATE "${SDL2_LIBRARIES_DIR}/SDL2main.lib")
endif()

# add include dirs
target_include_directories(${target} PRIVATE  "${CMAKE_SOURCE_DIR}/source" )
target_include_directories(${target} PRIVATE  "${CMAKE_SOURCE_DIR}/source/common" )
target_include_directories(${target} PRIVATE  "${CMAKE_SOURCE_DIR}/source/external/sumu" )
target_include_directories(${target} PRIVATE  "${CMAKE_SOURCE_DIR}/source/external/utu" )
target_include_directories(${target} PRIVATE  "${CMAKE_SOURCE_DIR}/source/external/libresample/include" )
target_include_directories(${target} PRIVATE  "${CMAKE_SOURCE_DIR}/source/widgets" )


# add madronalib + manzanita via imported targets
target_link_libraries(${target} PRIVATE madronalib::madronalib manzanita::manzanita)

# add loris library
target_include_directories(${target} PRIVATE ${LORIS_INCLUDE_DIR})
if(APPLE)
    target_link_libraries(${target} PRIVATE "${LORIS_LIBRARY_DIR}/lib${loris_NAME}.a")
elseif(WIN32)
    target_link_libraries(${target} PRIVATE "${LORIS_LIBRARY_DIR}/${loris_NAME}.lib")
endif()

# add other external libraries
target_link_libraries(${target} PRIVATE SndFile::sndfile libresample)




if(APPLE)
    # ARC must be off for juce
    target_compile_options(${target} PRIVATE "-fno-objc-arc")
      
    # set debug info format
    set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT "dwarf-with-dsym")
    
    # make macOS bundle
    set_target_properties(${target} PROPERTIES MACOSX_BUNDLE TRUE)
    
    # build only active arch for Debug builds
    set_target_properties(${target} PROPERTIES XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH[variant=Debug] "YES")
endif()
 
# set compiler options
if(APPLE)
    # missing return value should be an error
    target_compile_options(${target} PRIVATE "-Werror=return-type")
    # suppress block comments warnings
    target_compile_options(${target} PRIVATE "-Wno-comment")
elseif(WIN32)
    target_compile_options(${target} PRIVATE "/wd4068") # silence unknown pragma warning
    target_compile_options(${target} PRIVATE "/EHa") # standard C++ stack unwinding
endif()


# add the package info
if(APPLE)
    set_target_properties( 
        ${target} PROPERTIES
        # these settings allow the bundle version to be updated from the buildApp script.
        # the stale version from when the XCode project was made will still appear in the
        # project's "Custom macOS Application Target Properties."
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.madronalabs.vutu"
        XCODE_ATTRIBUTE_INFOPLIST_FILE ${CMAKE_CURRENT_LIST_DIR}/source/mac/Info.plist
        XCODE_ATTRIBUTE_INFOPLIST_PREPROCESS NO
        )
elseif(APPLE)
    target_sources(${target} PRIVATE resource/vutu.rc)
endif(APPLE)
        
message (STATUS "Info.plist source: ${CMAKE_CURRENT_LIST_DIR}/source/mac/Info.plist")

target_compile_definitions(${target} PUBLIC "$<$<CONFIG:DEBUG>:DEBUG>")
target_compile_definitions(${target} PUBLIC "$<$<CONFIG:RELEASE>:NDEBUG>")

# add UI frameworks- note that these appear under 
# "other linker flags" in XCode and not in its file browser
if(APPLE)
    target_link_libraries(${target} PRIVATE "-framework Cocoa" "-framework Metal" "-framework MetalKit"
      "-framework CoreAudio" "-framework AppKit" "-framework SDL2" )
     
     # add SDL2 framework location to @runpath 
     set_target_properties( ${target} PROPERTIES
        XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@loader_path/../Frameworks")

elseif(WIN32)
        find_package(OpenGL REQUIRED)
        target_link_libraries(${target} PRIVATE ${OPENGL_gl_LIBRARY})
        target_link_libraries(${target} PRIVATE "Shcore.lib")
        target_link_libraries(${target} PRIVATE "winmm.lib")
        target_link_libraries(${target} PRIVATE "setupapi.lib")
        target_link_libraries(${target} PRIVATE "version.lib")
        target_link_libraries(${target} PRIVATE "Imm32.lib")
        target_link_libraries(${target} PRIVATE debug "msvcrtd.lib" optimized "msvcrt.lib")
endif()

# for Mac OS copy SDL2.framework into bundle and codesign it
if(APPLE)

  set(APP_BUNDLE ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/Vutu.app)
  
  # source location of the SDL2 framework. Get the latest dmg from
  # https://github.com/libsdl-org/SDL/releases and copy the .framework
  # to this location.
  SET(FRAMEWORK "/Library/Frameworks/SDL2.framework")

  add_custom_command(
     TARGET ${target}
     POST_BUILD
     COMMAND ditto
         "${FRAMEWORK}"
         "${APP_BUNDLE}/Contents/Frameworks/SDL2.framework"
  )

  # Ad-hoc codesign the embedded framework. We previously passed
  # --entitlements pointing at the generated .xcent file, but its path
  # has moved between Xcode versions:
  #   Monterey / Xcode 14.0:   ${BIN_DIR}/build/${target}.build/$<CONFIG>/${target}.build/${target}.app.xcent
  #   Ventura  / Xcode 14.2:   ${BIN_DIR}/${target}.build/$<CONFIG>/${target}.build/${target}.app.xcent
  #   Xcode 26 (current):      unknown — neither of the above resolves, breaking the post-build phase.
  # TODO: locate the .xcent path Xcode 26 uses and re-introduce --entitlements.
  # For now we ad-hoc sign without entitlements; this is fine for dev because the
  # SDL2 framework has no entitlement requirements of its own.
  add_custom_command(TARGET ${target}
      POST_BUILD
      COMMAND codesign --force --sign "-" "${APP_BUNDLE}/Contents/Frameworks/SDL2.framework/Versions/A"
  )
endif()


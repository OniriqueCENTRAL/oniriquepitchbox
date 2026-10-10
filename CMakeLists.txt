cmake_minimum_required(VERSION 3.22)

project(OniriquePitchBox VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    # Universal binary, and the oldest macOS the JUCE 9 runtime supports.
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies.
#
# Both are expected next to this file: JUCE/ and ARA_SDK/. Point the cache
# variables elsewhere if you keep them in a shared location.
set(PITCHBOX_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to JUCE")
set(PITCHBOX_ARA_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ARA_SDK" CACHE PATH "Path to the ARA SDK")

if(NOT EXISTS "${PITCHBOX_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found at ${PITCHBOX_JUCE_DIR}.\n"
        "Run:  git clone --depth 1 https://github.com/juce-framework/JUCE.git")
endif()

if(NOT EXISTS "${PITCHBOX_ARA_SDK_DIR}/ARA_API")
    message(FATAL_ERROR
        "ARA SDK not found at ${PITCHBOX_ARA_SDK_DIR}.\n"
        "Run:  git clone --depth 1 https://github.com/Celemony/ARA_SDK.git")
endif()

add_subdirectory("${PITCHBOX_JUCE_DIR}" JUCE EXCLUDE_FROM_ALL)

# Must come before any target that depends on it.
juce_set_ara_sdk_path("${PITCHBOX_ARA_SDK_DIR}")

# ---------------------------------------------------------------------------
# The DSP and model layer, built on its own so the offline tests can link it
# without dragging in JUCE. Everything that decides how the plugin sounds lives
# here, which is what makes it testable from a plain command line.
add_library(pitchbox_core STATIC
    Source/dsp/Fft.cpp
    Source/dsp/YinPitchTracker.cpp
    Source/dsp/PitchMarks.cpp
    Source/dsp/Psola.cpp
    Source/dsp/Scale.cpp
    Source/model/Notes.cpp
    Source/model/Analysis.cpp
    Source/model/EditState.cpp
    Source/model/RegionRenderer.cpp)

target_include_directories(pitchbox_core PUBLIC Source)
set_target_properties(pitchbox_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # MSVC's <cmath> only declares M_PI when asked. PUBLIC so the tests get it too.
    target_compile_definitions(pitchbox_core PUBLIC _USE_MATH_DEFINES)
    target_compile_options(pitchbox_core PRIVATE /W4)
else()
    target_compile_options(pitchbox_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# The plugin.
set(PITCHBOX_FORMATS VST3)

if(APPLE)
    list(APPEND PITCHBOX_FORMATS AU)
endif()

# The system VST3 folder on Windows is under Program Files, which a normal build
# cannot write to, so installing after the build is opt-in there.
if(WIN32)
    option(PITCHBOX_COPY_AFTER_BUILD "Install the plugin after building" OFF)
else()
    option(PITCHBOX_COPY_AFTER_BUILD "Install the plugin after building" ON)
endif()

juce_add_plugin(OniriquePitchBox
    COMPANY_NAME "Onirique"
    BUNDLE_ID "com.onirique.pitchbox"
    PLUGIN_MANUFACTURER_CODE Onrq
    PLUGIN_CODE Ptbx
    FORMATS ${PITCHBOX_FORMATS}
    PRODUCT_NAME "PitchBox"
    IS_ARA_EFFECT TRUE
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${PITCHBOX_COPY_AFTER_BUILD})

target_sources(OniriquePitchBox PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ara/PitchBoxARA.cpp
    Source/ui/NoteGrid.cpp
    Source/ui/Theme.cpp)

target_compile_definitions(OniriquePitchBox PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(OniriquePitchBox
    PRIVATE
        pitchbox_core
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Offline tests. These run the whole audio path without a host: they are how the
# renderer is held to producing the same samples block by block as it does in
# one offline pass, which is the property an ARA plugin lives or dies by.
enable_testing()

foreach(test_name yin psola notes realtime renderer)
    add_executable(test_${test_name} tests/test_${test_name}.cpp)
    target_link_libraries(test_${test_name} PRIVATE pitchbox_core)
    target_include_directories(test_${test_name} PRIVATE tests)
    add_test(NAME ${test_name} COMMAND test_${test_name})
endforeach()

cmake_minimum_required(VERSION 3.22)

project(ZeroEQ VERSION 0.8.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target")

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# --------------------------------------------------------------------------------------
# Build identity for crash reports. A compiled binary cannot read its own git revision at
# runtime, and a report carrying only a version number cannot tell a released build from
# one three commits past the tag. `-dirty` means uncommitted changes were built.
# --------------------------------------------------------------------------------------
find_package(Git QUIET)
set(ZEROEQ_GIT_REV "unknown")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE ZEROEQ_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE ZEROEQ_GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT ZEROEQ_GIT_REV)
        set(ZEROEQ_GIT_REV "unknown")
    elseif(ZEROEQ_GIT_DIRTY)
        set(ZEROEQ_GIT_REV "${ZEROEQ_GIT_REV}-dirty")
    endif()
endif()

juce_add_plugin(ZeroEQ
    COMPANY_NAME "Allan Sargeant"
    BUNDLE_ID "com.allansargeant.zeroeq"
    PLUGIN_MANUFACTURER_CODE Alsg
    PLUGIN_CODE Zeq1
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "Zero EQ"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    VST3_CATEGORIES "Fx" "EQ"
    AU_MAIN_TYPE "kAudioUnitType_Effect"
)

juce_generate_juce_header(ZeroEQ)

target_sources(ZeroEQ
    PRIVATE
        Source/Diag/Diag.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/PresetManager.cpp
        Source/DSP/EQBand.cpp
        Source/DSP/EQEngine.cpp
        Source/DSP/DynamicEQDetector.cpp
        Source/DSP/HarmonicShaper.cpp
        Source/DSP/LevelMeter.cpp
        Source/DSP/Compressor.cpp
        Source/DSP/SpectrumAnalyzer.cpp
        Source/GUI/EQCurveComponent.cpp
        Source/GUI/BandControlPanel.cpp
        Source/GUI/CompressorPanel.cpp
        Source/GUI/IOPanel.cpp
        Source/GUI/PresetBar.cpp
        Source/GUI/ZeroEQLookAndFeel.cpp
)

target_compile_definitions(ZeroEQ
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        DIAG_GIT_REV="${ZEROEQ_GIT_REV}"
)

target_link_libraries(ZeroEQ
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# --------------------------------------------------------------------------------------
# zqfilm — renders the editor to a PNG sequence for the project video.
#
# Not a screen recording: see the comment at the top of tools/zqfilm.cpp. The plugin has
# no control surface to drive from outside, and photographing a window for 42 seconds
# needs a quiet machine and still produces blank frames; this runs the real processor and
# the real editor offscreen and is reproducible.
# --------------------------------------------------------------------------------------
juce_add_console_app(zqfilm PRODUCT_NAME "zqfilm")
juce_generate_juce_header(zqfilm)
target_sources(zqfilm PRIVATE tools/zqfilm.cpp)
target_compile_definitions(zqfilm PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # Needed for runDispatchLoopUntil, which is what lets the editor's timers repaint
    # against each frame's new state before it is snapshotted.
    JUCE_MODAL_LOOPS_PERMITTED=1
    DIAG_GIT_REV="${ZEROEQ_GIT_REV}")
target_include_directories(zqfilm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Linking the plugin target rather than re-listing its sources: that is what carries the
# JucePlugin_* defines the processor header needs, which a console app does not get on its own.
target_link_libraries(zqfilm PRIVATE
    ZeroEQ
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

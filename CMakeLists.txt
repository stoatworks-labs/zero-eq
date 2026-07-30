cmake_minimum_required(VERSION 3.22)

project(ZeroEQ VERSION 0.7.1)

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

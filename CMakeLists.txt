cmake_minimum_required(VERSION 3.22)

# Apple Silicon (arm64) build by default; override with -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" for a universal binary.
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS architectures to build")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(WavetableMorpher VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(WTM_BUILD_TESTS "Build the command-line self-test" OFF)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(WavetableMorpher
    PRODUCT_NAME "Wavetable Morpher"
    COMPANY_NAME "dgussmann"
    BUNDLE_ID com.dgussmann.wavetablemorpher
    PLUGIN_MANUFACTURER_CODE Dgsm
    PLUGIN_CODE Wtmr
    FORMATS VST3 AU Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice)

set(WTM_CORE_SOURCES
    Source/WaveSources.cpp
    Source/WavetableEngine.cpp
    Source/WavetableExporter.cpp
    Source/VitalBridge.cpp)

target_sources(WavetableMorpher PRIVATE
    ${WTM_CORE_SOURCES}
    Source/PreviewSynth.cpp
    Source/Views.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(WavetableMorpher PUBLIC
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(WavetableMorpher
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(WTM_BUILD_TESTS)
    juce_add_console_app(WavetableMorpherTests PRODUCT_NAME "WavetableMorpherTests")
    target_sources(WavetableMorpherTests PRIVATE Tests/SelfTest.cpp ${WTM_CORE_SOURCES})
    target_compile_definitions(WavetableMorpherTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(WavetableMorpherTests PRIVATE juce::juce_audio_formats
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()

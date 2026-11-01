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

# zstd: Serum 2 compresses its patch data with it
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG v1.5.6
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR build/cmake)
FetchContent_MakeAvailable(zstd)
set_target_properties(libzstd_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(WTM_CORE_SOURCES
    Source/WaveSources.cpp
    Source/WavetableEngine.cpp
    Source/WavetableExporter.cpp
    Source/VitalBridge.cpp
    Source/SerumBridge.cpp
    Source/PluginState.cpp)

# Two versions from the same code:
#   Wavetable Morpher       - with the built-in preview synth
#   Wavetable Morpher Link  - no built-in synth; always plays through Serum 2 / Vital (loads Serum 2 automatically)
function(wtm_add_variant target product plugin_code bundle_id builtin)
    juce_add_plugin(${target}
        PRODUCT_NAME "${product}"
        COMPANY_NAME "dgussmann"
        BUNDLE_ID ${bundle_id}
        PLUGIN_MANUFACTURER_CODE Dgsm
        PLUGIN_CODE ${plugin_code}
        FORMATS VST3 AU Standalone
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_CATEGORIES Instrument Synth
        AU_MAIN_TYPE kAudioUnitType_MusicDevice)

    target_sources(${target} PRIVATE
        ${WTM_CORE_SOURCES}
        Source/PreviewSynth.cpp
        Source/Views.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)

    target_compile_definitions(${target} PUBLIC
        WTM_BUILTIN_SYNTH=${builtin}
        JUCE_PLUGINHOST_VST3=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

    target_include_directories(${target} PRIVATE ${zstd_SOURCE_DIR}/lib)
    target_link_libraries(${target}
        PRIVATE
            juce::juce_audio_utils
            juce::juce_cryptography
            libzstd_static
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endfunction()

wtm_add_variant(WavetableMorpher     "Wavetable Morpher"      Wtmr com.dgussmann.wavetablemorpher     1)
wtm_add_variant(WavetableMorpherLink "Wavetable Morpher Link" Wtml com.dgussmann.wavetablemorpherlink 0)

if(WTM_BUILD_TESTS)
    juce_add_console_app(WavetableMorpherTests PRODUCT_NAME "WavetableMorpherTests")
    target_sources(WavetableMorpherTests PRIVATE Tests/SelfTest.cpp ${WTM_CORE_SOURCES})
    target_compile_definitions(WavetableMorpherTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_include_directories(WavetableMorpherTests PRIVATE ${zstd_SOURCE_DIR}/lib)
    target_link_libraries(WavetableMorpherTests PRIVATE juce::juce_audio_formats juce::juce_cryptography libzstd_static
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()

cmake_minimum_required(VERSION 3.22)
project(MicFilter VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE is fetched automatically on first configure (needs git + internet).
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        7.0.12
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# One processor, two products:
#  - MicFilter_VST3       : loads in OBS / DAW / any VST3 host
#  - MicFilter_Standalone : the dedicated client.
#    Its top "Options" button opens JUCE's audio device selector
#    (input device, output device, sample rate, buffer size).
juce_add_plugin(MicFilter
    COMPANY_NAME            "Takumi"
    PLUGIN_MANUFACTURER_CODE Takm
    PLUGIN_CODE             McFt
    FORMATS                 VST3 Standalone
    PRODUCT_NAME            "MicFilter"
    PLUGIN_CATEGORY         "Fx"
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD   TRUE   # installs VST3 to the system VST3 folder on Windows
    VST3_CATEGORIES           "Fx|Tools"
)

target_sources(MicFilter PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/dsp/DspChain.h
)

target_compile_definitions(MicFilter PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(MicFilter PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

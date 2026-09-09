cmake_minimum_required(VERSION 3.22)

project(ClipOnizer VERSION 1.0.2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE: Підключаємо локальну папку замість викачування з інтернету
# ---------------------------------------------------------------------------
add_subdirectory("F:/SDK/JUCE" JUCE)

juce_add_plugin(ClipOnizer
    COMPANY_NAME                 "Dr.Doc SoundLab"
    BUNDLE_ID                    "com.drdoc.modelcliponizer"
    PLUGIN_MANUFACTURER_CODE     Drdc
    PLUGIN_CODE                  Clp8
    FORMATS                      VST3 AU Standalone
    PRODUCT_NAME                 "MODEL-CLIPONIZER No.8"
    IS_SYNTH                     FALSE
    NEEDS_MIDI_INPUT             FALSE
    NEEDS_MIDI_OUTPUT            FALSE
    IS_MIDI_EFFECT               FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS  FALSE
    COPY_PLUGIN_AFTER_BUILD      TRUE
)

# Генеруємо JuceHeader.h для компіляції проєкту
juce_generate_juce_header(ClipOnizer)

target_sources(ClipOnizer
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

target_include_directories(ClipOnizer
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_compile_definitions(ClipOnizer
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(ClipOnizer
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
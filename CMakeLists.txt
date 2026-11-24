cmake_minimum_required(VERSION 3.22)

project(VoiceChain VERSION 0.1.0)

# JUCE kitabxanasını tapmaq üçün standart üsul
find_package(JUCE CONFIG REQUIRED)

juce_add_plugin(VoiceChain
    COMPANY_NAME "Minos"
    PLUGIN_MANUFACTURER_CODE Mnos
    PLUGIN_CODE Vcha
    FORMATS VST3 Standalone
    PRODUCT_NAME "VoiceChain"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE)

juce_generate_juce_header(VoiceChain)

target_sources(VoiceChain PRIVATE
    Source/PluginProcessor.h
    Source/PluginProcessor.cpp)

target_compile_definitions(VoiceChain PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(VoiceChain
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

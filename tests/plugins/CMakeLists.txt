# Tiny VST3 + LV2 plugins used by the tests (and handy for trying plugin hosting by hand).
# Bundles end up in ${CMAKE_BINARY_DIR}/tests/plugins/DiyDawTest*_artefacts/<config>/{VST3,LV2}.

foreach(kind Gain Synth)
    if(kind STREQUAL "Synth")
        set(is_synth TRUE)
        set(code DdTs)
    else()
        set(is_synth FALSE)
        set(code DdTg)
    endif()

    juce_add_plugin(DiyDawTest${kind}
        PRODUCT_NAME "diyDaw Test ${kind}"
        COMPANY_NAME "diyDaw"
        BUNDLE_ID "dev.diydaw.test${kind}"
        PLUGIN_MANUFACTURER_CODE DiyD
        PLUGIN_CODE ${code}
        FORMATS VST3 LV2
        IS_SYNTH ${is_synth}
        NEEDS_MIDI_INPUT ${is_synth}
        LV2URI "urn:diydaw:test${kind}"
        COPY_PLUGIN_AFTER_BUILD FALSE)

    target_sources(DiyDawTest${kind} PRIVATE TestPlugins.cpp Test${kind}Entry.cpp)
    target_compile_definitions(DiyDawTest${kind} PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(DiyDawTest${kind} PRIVATE juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endforeach()

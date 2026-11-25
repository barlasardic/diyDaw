cmake_minimum_required(VERSION 3.22)
project(diyDaw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DAW_BUILD_APP "Build the GUI application (needs ALSA/JACK dev packages on Linux)" ON)
option(DAW_BUILD_TESTS "Build the headless engine tests" ON)
option(DAW_AUDIO_BACKENDS "Enable ALSA + JACK audio I/O on Linux (OFF = compile-only, no sound)" ON)

add_subdirectory(external/JUCE EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Engine sources: device-independent DSP core, compiled into every target that
# needs it (each target owns its own copy of the JUCE modules).
# ---------------------------------------------------------------------------
add_library(daw_engine INTERFACE)
target_sources(daw_engine INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/AudioEffect.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/AudioTrack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/Effects.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/Engine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/Metronome.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/SampleData.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/Synth.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/Transport.cpp)
target_include_directories(daw_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/engine)

# Headless document model (tracks/clips, undo, sample pool) on top of the engine.
add_library(daw_model INTERFACE)
target_sources(daw_model INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/model/Project.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/ProjectRenderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/SamplePool.cpp)
target_include_directories(daw_model INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/model)
target_link_libraries(daw_model INTERFACE daw_engine)

set(DAW_COMMON_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

# ---------------------------------------------------------------------------
# Headless tests
# ---------------------------------------------------------------------------
if(DAW_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(DawEngineTests PRODUCT_NAME "DawEngineTests")
    target_sources(DawEngineTests PRIVATE
        tests/TestMain.cpp
        tests/TransportTests.cpp
        tests/MetronomeTests.cpp
        tests/TrackTests.cpp
        tests/ProjectTests.cpp
        tests/PersistenceTests.cpp
        tests/ExportTests.cpp
        tests/SynthTests.cpp
        tests/MidiTests.cpp
        tests/EffectsTests.cpp
        tests/DeviceChainTests.cpp
        tests/LiveNoteTests.cpp)
    target_compile_definitions(DawEngineTests PRIVATE ${DAW_COMMON_DEFINITIONS})
    target_link_libraries(DawEngineTests PRIVATE
        daw_model
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_data_structures
        juce::juce_graphics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    add_test(NAME DawEngineTests COMMAND DawEngineTests)
endif()

# ---------------------------------------------------------------------------
# GUI application
# ---------------------------------------------------------------------------
if(DAW_BUILD_APP)
    juce_add_gui_app(DiyDaw PRODUCT_NAME "diyDaw" COMPANY_NAME "diyDaw")
    target_sources(DiyDaw PRIVATE
        app/Main.cpp
        app/MainComponent.cpp
        app/ExportDialog.cpp
        app/SongPositionBar.cpp
        app/TransportBar.cpp
        app/TrackStrip.cpp
        app/LevelMeter.cpp
        app/DawLookAndFeel.cpp
        app/arrangement/ArrangementView.cpp
        app/arrangement/ClipPainter.cpp
        app/arrangement/TimeRuler.cpp
        app/arrangement/TrackHeader.cpp
        app/clipeditor/PianoRoll.cpp
        app/devices/SynthPanel.cpp
        app/devices/DeviceChain.cpp
        app/DetailView.cpp)
    target_include_directories(DiyDaw PRIVATE app app/arrangement app/clipeditor app/devices)
    target_compile_definitions(DiyDaw PRIVATE
        ${DAW_COMMON_DEFINITIONS}
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:DiyDaw,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:DiyDaw,JUCE_VERSION>")
    if(DAW_AUDIO_BACKENDS)
        target_compile_definitions(DiyDaw PRIVATE JUCE_ALSA=1 JUCE_JACK=1)
    else()
        # Compile-check builds on machines without libasound2-dev / libjack-dev: no audio I/O.
        target_compile_definitions(DiyDaw PRIVATE JUCE_ALSA=0 JUCE_JACK=0)
    endif()
    target_link_libraries(DiyDaw PRIVATE
        daw_model
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
endif()

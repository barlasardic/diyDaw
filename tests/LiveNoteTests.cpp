#include "Project.h"

namespace
{
/** Live notes (piano-roll keys, computer keyboard, MIDI input) must be applied in arrival
    order, even when a note-on and its note-off reach the engine within one audio block. */
struct LiveNoteTests : juce::UnitTest
{
    LiveNoteTests() : juce::UnitTest ("MIDI live notes", "diyDaw") {}

    struct Fixture
    {
        daw::Engine engine;
        daw::SamplePool pool;
        daw::Project project { engine, pool };
        juce::ValueTree track;
        juce::AudioBuffer<float> buffer { 2, 512 };

        Fixture()
        {
            engine.prepare (48000.0, 512);
            track = project.addTrack ({}, daw::Project::TrackKind::midi);
        }

        daw::AudioTrack& engineTrack() { return project.getEngineTrack (track); }

        void renderBlocks (int count)
        {
            for (int i = 0; i < count; ++i)
                engine.render (buffer, 512);
        }
    };

    void runTest() override
    {
        beginTest ("A note-on and note-off in separate blocks: the note ends");
        {
            Fixture f;
            f.engineTrack().pushLiveNote (60, 0.8f, true);
            f.renderBlocks (10);
            expectEquals (f.engineTrack().getSynth().getNumActiveVoices(), 1);
            f.engineTrack().pushLiveNote (60, 0.0f, false);
            f.renderBlocks (400); // > release time
            expectEquals (f.engineTrack().getSynth().getNumActiveVoices(), 0);
        }

        beginTest ("A quick click (on and off within one block) does not hang");
        {
            Fixture f;
            f.engineTrack().pushLiveNote (60, 0.8f, true);
            f.engineTrack().pushLiveNote (60, 0.0f, false);
            f.renderBlocks (400);
            expectEquals (f.engineTrack().getSynth().getNumActiveVoices(), 0);
        }

        beginTest ("Two quick clicks on the same key within one block do not hang");
        {
            Fixture f;
            for (int i = 0; i < 2; ++i)
            {
                f.engineTrack().pushLiveNote (64, 0.8f, true);
                f.engineTrack().pushLiveNote (64, 0.0f, false);
            }
            f.renderBlocks (400);
            expectEquals (f.engineTrack().getSynth().getNumActiveVoices(), 0);
        }

        beginTest ("Gliding across keys (off old, on new) leaves only the last key sounding");
        {
            Fixture f;
            f.engineTrack().pushLiveNote (60, 0.8f, true);
            f.engineTrack().pushLiveNote (60, 0.0f, false);
            f.engineTrack().pushLiveNote (62, 0.8f, true);
            f.renderBlocks (400);
            expectEquals (f.engineTrack().getSynth().getNumActiveVoices(), 1);
            f.engineTrack().pushLiveNote (62, 0.0f, false);
            f.renderBlocks (400);
            expectEquals (f.engineTrack().getSynth().getNumActiveVoices(), 0);
        }
    }
};

LiveNoteTests liveNoteTests;
}

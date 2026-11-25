#pragma once

#include "ProjectRenderer.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace daw
{

/** "Export Audio" panel: range in bars, sample rate, bit depth, normalise, metronome.
    Pressing Export asks for a file name and hands the settings to onExport.
    Message thread only. */
class ExportDialog : public juce::Component
{
public:
    ExportDialog (Project& project, double defaultSampleRate);

    std::function<void (const ExportSettings&, const juce::File&)> onExport;
    std::function<void()> onCancel;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    ExportSettings makeSettings() const;
    void chooseFileAndExport();
    void updateSummary();

    Project& project;

    juce::Label startLabel { {}, "Start bar" }, lengthLabel { {}, "Length (bars)" };
    juce::Slider startBar, lengthBars;
    juce::Label rateLabel { {}, "Sample rate" }, depthLabel { {}, "Bit depth" };
    juce::ComboBox sampleRate, bitDepth;
    juce::ToggleButton normalise { "Normalise to -0.3 dBFS" }, metronome { "Include metronome" };
    juce::Label summary;
    juce::TextButton exportButton { "Export..." }, cancelButton { "Cancel" };

    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportDialog)
};

/** Runs a ProjectRenderer on a background thread behind a progress window, then reports the
    result. Create on the message thread; it calls onFinished (on the message thread) when done,
    after which the owner may delete it. */
class ExportJob : public juce::ThreadWithProgressWindow
{
public:
    ExportJob (Project& project, const ExportSettings& settings, const juce::File& file);

    std::function<void (juce::Result)> onFinished;

private:
    void run() override;
    void threadComplete (bool userPressedCancel) override;

    ProjectRenderer renderer;
    juce::File file;
    juce::Result result { juce::Result::ok() };
};

} // namespace daw

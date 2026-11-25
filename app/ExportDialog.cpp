#include "ExportDialog.h"
#include "DawLookAndFeel.h"

#include <cmath>

namespace daw
{

namespace
{
    const double sampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
    const int bitDepths[] = { 16, 24, 32 };

    void styleLabel (juce::Label& label)
    {
        label.setColour (juce::Label::textColourId, colours::textDim);
        label.setFont (juce::FontOptions (13.0f));
    }

    void styleBarsSlider (juce::Slider& slider, double min, double max, double value)
    {
        slider.setSliderStyle (juce::Slider::IncDecButtons);
        slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 60, 24);
        slider.setRange (min, max, 1.0);
        slider.setValue (value, juce::dontSendNotification);
    }
}

ExportDialog::ExportDialog (Project& p, double defaultSampleRate) : project (p)
{
    const auto beatsPerBar = juce::jmax (1, project.getEngine().getTransport().getBeatsPerBar());
    const auto endBeat = ProjectRenderer::getProjectEndBeat (project);
    const auto bars = juce::jmax (1.0, std::ceil (endBeat / beatsPerBar - 1.0e-9));

    for (auto* l : { &startLabel, &lengthLabel, &rateLabel, &depthLabel })
    {
        styleLabel (*l);
        addAndMakeVisible (*l);
    }

    styleBarsSlider (startBar, 1.0, 9999.0, 1.0);
    styleBarsSlider (lengthBars, 1.0, 9999.0, bars);
    startBar.onValueChange = lengthBars.onValueChange = [this] { updateSummary(); };
    addAndMakeVisible (startBar);
    addAndMakeVisible (lengthBars);

    int defaultRateId = 1;
    for (int i = 0; i < (int) std::size (sampleRates); ++i)
    {
        sampleRate.addItem (juce::String ((int) sampleRates[i]) + " Hz", i + 1);
        if (juce::approximatelyEqual (sampleRates[i], defaultSampleRate))
            defaultRateId = i + 1;
    }
    sampleRate.setSelectedId (defaultRateId, juce::dontSendNotification);
    sampleRate.onChange = [this] { updateSummary(); };
    addAndMakeVisible (sampleRate);

    bitDepth.addItem ("16 bit", 1);
    bitDepth.addItem ("24 bit", 2);
    bitDepth.addItem ("32 bit float", 3);
    bitDepth.setSelectedId (2, juce::dontSendNotification);
    addAndMakeVisible (bitDepth);

    addAndMakeVisible (normalise);
    addAndMakeVisible (metronome);

    summary.setColour (juce::Label::textColourId, colours::textDim);
    summary.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (summary);

    exportButton.onClick = [this] { chooseFileAndExport(); };
    cancelButton.onClick = [this] { if (onCancel) onCancel(); };
    addAndMakeVisible (exportButton);
    addAndMakeVisible (cancelButton);

    updateSummary();
    setSize (380, 290);
}

ExportSettings ExportDialog::makeSettings() const
{
    const auto beatsPerBar = juce::jmax (1, project.getEngine().getTransport().getBeatsPerBar());

    ExportSettings s;
    s.startBeat = (startBar.getValue() - 1.0) * beatsPerBar;
    s.endBeat = s.startBeat + lengthBars.getValue() * beatsPerBar;
    s.sampleRate = sampleRates[juce::jlimit (0, (int) std::size (sampleRates) - 1, sampleRate.getSelectedId() - 1)];
    s.bitDepth = bitDepths[juce::jlimit (0, 2, bitDepth.getSelectedId() - 1)];
    s.normalise = normalise.getToggleState();
    s.includeMetronome = metronome.getToggleState();
    return s;
}

void ExportDialog::updateSummary()
{
    const auto s = makeSettings();
    const auto seconds = project.beatsToSeconds (s.endBeat - s.startBeat);
    const auto total = juce::roundToInt (seconds);
    summary.setText ("Duration " + juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2)
                         + " at " + juce::String (project.getBpm(), 2) + " BPM",
                     juce::dontSendNotification);
}

void ExportDialog::chooseFileAndExport()
{
    auto folder = project.getFile() != juce::File() ? project.getFile().getParentDirectory()
                                                    : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    const auto name = project.getFile() != juce::File() ? project.getFile().getFileNameWithoutExtension()
                                                        : juce::String ("Untitled");

    chooser = std::make_unique<juce::FileChooser> ("Export Audio", folder.getChildFile (name + ".wav"), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer<ExportDialog> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr)
                                  return;

                              if (const auto file = fc.getResult(); file != juce::File() && safe->onExport)
                                  safe->onExport (safe->makeSettings(), file.withFileExtension (".wav"));
                          });
}

void ExportDialog::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);
}

void ExportDialog::resized()
{
    auto area = getLocalBounds().reduced (16);

    auto row = [&area] (juce::Label& label, juce::Component& control)
    {
        auto r = area.removeFromTop (28);
        label.setBounds (r.removeFromLeft (130));
        control.setBounds (r.removeFromLeft (180));
        area.removeFromTop (8);
    };

    row (startLabel, startBar);
    row (lengthLabel, lengthBars);
    row (rateLabel, sampleRate);
    row (depthLabel, bitDepth);

    normalise.setBounds (area.removeFromTop (24));
    metronome.setBounds (area.removeFromTop (24));

    auto buttons = area.removeFromBottom (30);
    exportButton.setBounds (buttons.removeFromRight (100));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (80));
    summary.setBounds (buttons);
}

//==============================================================================
ExportJob::ExportJob (Project& project, const ExportSettings& settings, const juce::File& f)
    : ThreadWithProgressWindow ("Exporting " + f.getFileName() + "...", true, true),
      renderer (project, settings),
      file (f)
{
}

void ExportJob::run()
{
    result = renderer.renderToFile (file, [this] (double fraction)
    {
        setProgress (fraction);
        return ! threadShouldExit();
    });
}

void ExportJob::threadComplete (bool userPressedCancel)
{
    if (userPressedCancel && result.wasOk())
        result = juce::Result::fail ("Export cancelled.");

    if (onFinished)
        onFinished (result);
}

} // namespace daw

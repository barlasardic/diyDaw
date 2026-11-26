#include "DrumRackPanel.h"
#include "BuiltinSamples.h"
#include "DawLookAndFeel.h"
#include "DrumRack.h"
#include "ProjectControls.h"

#include <array>
#include <vector>

namespace daw
{

namespace
{
    constexpr int titleHeight = 26;

    juce::String noteName (int pad)
    {
        static constexpr const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const auto pitch = DrumRack::firstNote + pad;
        return juce::String (names[pitch % 12]) + juce::String (pitch / 12 - 2);
    }

    juce::String soundName (const juce::String& name)
    {
        juce::String result;
        for (auto c : name)
        {
            if (juce::CharacterFunctions::isUpperCase (c))
                result << ' ';
            result << juce::String::charToString (c);
        }
        return result.substring (0, 1).toUpperCase() + result.substring (1);
    }

    juce::String formatParamValue (const ParamInfo& info, double value)
    {
        const auto unit = juce::String (info.unit != nullptr ? info.unit : "");
        if (unit == "s")
            return value < 1.0 ? juce::String::formatted ("%.0f ms", value * 1000.0)
                               : juce::String::formatted ("%.2f s", value);
        if (unit == "dB")
            return juce::String::formatted ("%.1f dB", value);
        if (unit == "st")
            return juce::String (value > 0.0 ? "+" : "") + juce::String (juce::roundToInt (value)) + " st";
        if (info.minValue < 0.0f && info.maxValue > 0.0f)
            return juce::String::formatted ("%+.2f", value);
        return juce::String::formatted ("%.2f", value);
    }

    void styleLabel (juce::Label& label, float height, juce::Colour colour)
    {
        label.setFont (juce::FontOptions (height));
        label.setColour (juce::Label::textColourId, colour);
        label.setJustificationType (juce::Justification::centred);
        label.setInterceptsMouseClicks (false, false);
    }

    class PadLookAndFeel : public DawLookAndFeel
    {
    public:
        juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::FontOptions (10.5f); }
        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            label.setBounds (3, 1, juce::jmax (0, box.getWidth() - 22), box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
        }
    };
}

/** Owns the message-thread pad components and the selected pad's controls. */
struct DrumRackPanel::Controls
{
    class Pad : public juce::Component, private juce::Timer
    {
    public:
        explicit Pad (int index) : pad (index)
        {
            setMouseClickGrabsKeyboardFocus (false);
            setRepaintsOnMouseActivity (true);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void paint (juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
            auto fill = filled ? colours::panelRaised : colours::background.brighter (0.04f);
            if (isMouseOver())
                fill = fill.brighter (0.08f);
            if (selected || dropTarget)
                fill = fill.interpolatedWith (accent, 0.18f);
            if (flashing)
                fill = fill.interpolatedWith (accent, 0.65f);
            g.setColour (fill);
            g.fillRoundedRectangle (bounds, 4.0f);
            g.setColour (selected || dropTarget ? accent : colours::outline);
            g.drawRoundedRectangle (bounds, 4.0f, selected || dropTarget ? 1.5f : 1.0f);

            auto text = getLocalBounds().reduced (7, 4);
            g.setColour (selected || flashing ? accent.brighter (0.3f) : colours::textDim);
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (noteName (pad), text.removeFromTop (13), juce::Justification::centredLeft);
            g.setColour (filled ? colours::text : colours::textDim.withAlpha (0.6f));
            g.setFont (juce::FontOptions (11.0f));
            g.drawFittedText (filled ? name : "Drop sample", text, juce::Justification::centredLeft, 1);
        }

        void mouseDown (const juce::MouseEvent& event) override
        {
            if (! event.mods.isLeftButtonDown())
                return;
            flashing = true;
            startTimer (160);
            repaint();
            if (onPress)
                onPress();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (onRelease)
                onRelease();
        }

        int pad;
        bool selected = false, filled = false, flashing = false, dropTarget = false;
        juce::String name;
        juce::Colour accent;
        std::function<void()> onPress, onRelease;

    private:
        void timerCallback() override
        {
            stopTimer();
            flashing = false;
            repaint();
        }
    };

    struct Knob
    {
        juce::Slider slider;
        juce::Label name, value;
        ParamInfo info {};
        int index = 0;
    };

    PadLookAndFeel lookAndFeel;
    std::array<std::unique_ptr<Pad>, DrumRack::numPads> pads;
    std::vector<std::unique_ptr<Knob>> knobs;
    juce::Label name, note, chokeLabel;
    juce::ComboBox choke;
    juce::TextButton sound { "Sound..." }, clear { "Clear" };
    juce::Rectangle<int> gridBounds, detailBounds;
    int selectedPad = 0, previewPitch = -1;
    size_t infoCount = 0;

    ~Controls() { choke.setLookAndFeel (nullptr); }
};

DrumRackPanel::DrumRackPanel (Project& p) : project (p), controls (std::make_unique<Controls>())
{
    setMouseClickGrabsKeyboardFocus (false);
    for (int pad = 0; pad < DrumRack::numPads; ++pad)
    {
        auto component = std::make_unique<Controls::Pad> (pad);
        component->onPress = [this, pad]
        {
            stopPreview();
            selectPad (pad);
            if (Project::isDrumTrack (track) && onPreviewNote)
            {
                controls->previewPitch = DrumRack::firstNote + pad;
                onPreviewNote (controls->previewPitch, 0.8f, true);
            }
        };
        component->onRelease = [this] { stopPreview(); };
        addAndMakeVisible (*component);
        controls->pads[(size_t) pad] = std::move (component);
    }

    styleLabel (controls->name, 15.0f, colours::text);
    controls->name.setJustificationType (juce::Justification::centredLeft);
    styleLabel (controls->note, 10.5f, colours::textDim);
    controls->note.setJustificationType (juce::Justification::centredLeft);
    styleLabel (controls->chokeLabel, 10.5f, colours::textDim);
    controls->chokeLabel.setText ("Choke", juce::dontSendNotification);
    for (auto* label : { &controls->name, &controls->note, &controls->chokeLabel })
        addAndMakeVisible (*label);

    auto& choke = controls->choke;
    choke.addItem ("None", 1);
    for (int group = 1; group <= 8; ++group)
        choke.addItem (juce::String (group), group + 1);
    choke.setMouseClickGrabsKeyboardFocus (false);
    choke.setLookAndFeel (&controls->lookAndFeel);
    choke.onChange = [this]
    {
        if (! Project::isDrumTrack (track))
            return;
        project.beginTransaction ("Change pad choke");
        project.setDeviceParam (Project::getDevice (track),
                                DrumRack::paramIndex (controls->selectedPad, DrumRack::choke),
                                (float) (controls->choke.getSelectedId() - 1));
    };
    addAndMakeVisible (choke);

    controls->sound.setMouseClickGrabsKeyboardFocus (false);
    controls->sound.onClick = [this] { showSoundMenu(); };
    addAndMakeVisible (controls->sound);
    controls->clear.setMouseClickGrabsKeyboardFocus (false);
    controls->clear.onClick = [this]
    {
        if (! Project::isDrumTrack (track))
            return;
        project.beginTransaction ("Clear drum pad");
        project.clearPad (Project::getDevice (track), controls->selectedPad);
        refresh();
    };
    addAndMakeVisible (controls->clear);
    refresh();
}

DrumRackPanel::~DrumRackPanel() { stopPreview(); }

void DrumRackPanel::stopPreview()
{
    const auto pitch = controls->previewPitch;
    controls->previewPitch = -1;
    if (pitch >= 0 && onPreviewNote)
        onPreviewNote (pitch, 0.0f, false);
}

void DrumRackPanel::visibilityChanged()
{
    if (! isShowing())
        stopPreview();
}

void DrumRackPanel::setTrack (juce::ValueTree newTrack)
{
    if (track != newTrack)
    {
        stopPreview();
        track = newTrack;
        controls->selectedPad = 0;
        rebuildKnobs();
    }
    refresh();
}

void DrumRackPanel::selectPad (int pad)
{
    if (controls->selectedPad != pad)
    {
        controls->selectedPad = pad;
        rebuildKnobs();
    }
    refresh();
}

void DrumRackPanel::rebuildKnobs()
{
    controls->knobs.clear();
    const auto& infos = DrumRack::getParamInfos();
    controls->infoCount = infos.size();
    if (! Project::isDrumTrack (track))
        return;

    for (const auto parameter : { DrumRack::gain, DrumRack::pan, DrumRack::tune, DrumRack::decay })
    {
        const auto index = DrumRack::paramIndex (controls->selectedPad, parameter);
        if (index >= (int) infos.size() || infos[(size_t) index].id == nullptr)
            continue;
        const auto info = infos[(size_t) index];
        if (info.maxValue <= info.minValue)
            continue;
        auto knob = std::make_unique<Controls::Knob>();
        knob->index = index;
        knob->info = info;
        auto& slider = knob->slider;
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        juce::NormalisableRange<double> range (info.minValue, info.maxValue, info.stepped ? 1.0 : 0.0);
        if (! info.stepped && info.centreValue > info.minValue && info.centreValue < info.maxValue)
            range.setSkewForCentre (info.centreValue);
        slider.setNormalisableRange (range);
        slider.setDoubleClickReturnValue (true, info.defaultValue);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setPopupDisplayEnabled (true, true, this);
        slider.setMouseClickGrabsKeyboardFocus (false);
        slider.textFromValueFunction = [info] (double value) { return formatParamValue (info, value); };
        styleLabel (knob->name, 10.5f, colours::text);
        styleLabel (knob->value, 10.0f, colours::textDim);
        knob->name.setText (info.name, juce::dontSendNotification);
        auto* raw = knob.get();
        bindSliderToProject (slider, project, "Change pad " + juce::String (info.name),
                             [this, raw] (double value)
        {
            project.setDeviceParam (Project::getDevice (track), raw->index, (float) value);
            raw->value.setText (formatParamValue (raw->info, value), juce::dontSendNotification);
        });
        addAndMakeVisible (slider);
        addAndMakeVisible (knob->name);
        addAndMakeVisible (knob->value);
        controls->knobs.push_back (std::move (knob));
    }
    resized();
}

void DrumRackPanel::refresh()
{
    const auto valid = Project::isDrumTrack (track);
    const auto device = Project::getDevice (track);
    const auto& infos = DrumRack::getParamInfos();
    if (controls->infoCount != infos.size())
        rebuildKnobs();

    for (auto& pad : controls->pads)
    {
        pad->setEnabled (valid);
        pad->name = valid ? Project::getPadName (device, pad->pad) : juce::String();
        pad->filled = valid && Project::getPadSampleKey (device, pad->pad).isNotEmpty();
        pad->selected = valid && pad->pad == controls->selectedPad;
        pad->accent = valid ? Project::getTrackColour (track) : colours::textDim;
        pad->repaint();
    }
    const auto selectedName = Project::getPadName (device, controls->selectedPad);
    controls->name.setText (valid ? (selectedName.isEmpty() ? "Empty pad" : selectedName) : "Drum Rack",
                            juce::dontSendNotification);
    controls->note.setText (valid ? noteName (controls->selectedPad) + "  /  MIDI "
                                      + juce::String (DrumRack::firstNote + controls->selectedPad)
                                  : "Select a drum track", juce::dontSendNotification);
    controls->sound.setEnabled (valid && ! getBuiltinSampleNames().isEmpty());
    controls->clear.setEnabled (valid && selectedName.isNotEmpty());
    const auto chokeIndex = DrumRack::paramIndex (controls->selectedPad, DrumRack::choke);
    const auto hasChoke = valid && chokeIndex < (int) infos.size() && infos[(size_t) chokeIndex].id != nullptr;
    controls->choke.setVisible (hasChoke);
    controls->chokeLabel.setVisible (hasChoke);
    if (hasChoke && ! controls->choke.isPopupActive())
        controls->choke.setSelectedId (juce::roundToInt (Project::getDeviceParam (device, chokeIndex)) + 1,
                                      juce::dontSendNotification);

    for (auto& knob : controls->knobs)
    {
        const auto target = Project::getDeviceParamTargetId (device, knob->index);
        const auto automated = Project::isAutomated (track, target);
        showAutomationState (knob->slider, automated);
        if (! knob->slider.isMouseButtonDown())
            knob->slider.setValue (automated ? project.getEngineValue (track, target)
                                             : Project::getDeviceParam (device, knob->index),
                                    juce::dontSendNotification);
        knob->value.setText (formatParamValue (knob->info, knob->slider.getValue()), juce::dontSendNotification);
    }
    repaint();
}

void DrumRackPanel::showSoundMenu()
{
    if (! Project::isDrumTrack (track))
        return;
    juce::PopupMenu menu;
    const auto names = getBuiltinSampleNames();
    const auto device = Project::getDevice (track);
    const auto pad = controls->selectedPad;
    for (int i = 0; i < names.size(); ++i)
        menu.addItem (i + 1, soundName (names[i]), true,
                      Project::getPadSampleKey (device, pad) == builtinSampleKey (names[i]));
    juce::Component::SafePointer<DrumRackPanel> safeThis (this);
    const auto targetTrack = track;
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&controls->sound),
                        [safeThis, targetTrack, device, pad, names] (int result)
    {
        if (safeThis == nullptr || result <= 0 || result > names.size()
            || safeThis->project.indexOfTrack (targetTrack) < 0 || Project::getDevice (targetTrack) != device)
            return;
        safeThis->project.beginTransaction ("Change drum sound");
        safeThis->project.setPadSample (device, pad, builtinSampleKey (names[result - 1]));
        safeThis->refresh();
    });
}

void DrumRackPanel::paint (juce::Graphics& g)
{
    g.setColour (colours::panelRaised);
    g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
    g.drawHorizontalLine (titleHeight - 1, 1.0f, (float) getWidth() - 1.0f);
    g.setColour (Project::isDrumTrack (track) ? Project::getTrackColour (track) : colours::textDim);
    g.fillEllipse (10.0f, 9.0f, 8.0f, 8.0f);
    g.setColour (colours::text);
    g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    g.drawText ("Drum Rack", 26, 0, getWidth() - 36, titleHeight, juce::Justification::centredLeft);
    g.setColour (colours::panel);
    g.fillRoundedRectangle (controls->detailBounds.toFloat(), 3.0f);
}

void DrumRackPanel::resized()
{
    auto body = getLocalBounds().withTrimmedTop (titleHeight).reduced (8, 8);
    controls->gridBounds = body.removeFromLeft (body.getWidth() / 2);
    body.removeFromLeft (10);
    controls->detailBounds = body;
    const auto grid = controls->gridBounds;
    for (auto& pad : controls->pads)
    {
        const auto column = pad->pad % 4;
        const auto row = 3 - pad->pad / 4;
        const auto left = grid.getX() + column * grid.getWidth() / 4;
        const auto top = grid.getY() + row * grid.getHeight() / 4;
        pad->setBounds (left, top, (column + 1) * grid.getWidth() / 4 - column * grid.getWidth() / 4 - 4,
                        (row + 1) * grid.getHeight() / 4 - row * grid.getHeight() / 4 - 4);
    }
    body.reduce (10, 6);
    controls->name.setBounds (body.removeFromTop (24));
    controls->note.setBounds (body.removeFromTop (17));
    auto buttons = body.removeFromBottom (26);
    controls->chokeLabel.setBounds (buttons.removeFromLeft (39));
    controls->choke.setBounds (buttons.removeFromLeft (67).reduced (0, 2));
    buttons.removeFromLeft (10);
    controls->clear.setBounds (buttons.removeFromRight (56).reduced (0, 2));
    buttons.removeFromRight (6);
    controls->sound.setBounds (buttons.reduced (0, 2));
    const auto count = (int) controls->knobs.size();
    for (int i = 0; i < count; ++i)
    {
        const auto cell = juce::Rectangle<int> (body.getX() + i * body.getWidth() / count, body.getY(),
                                                 body.getWidth() / count, body.getHeight());
        const auto size = juce::jmax (0, juce::jmin (74, cell.getWidth() - 4, cell.getHeight() - 31));
        const auto top = cell.getCentreY() - (size + 27) / 2;
        auto& knob = *controls->knobs[(size_t) i];
        knob.slider.setBounds (cell.getCentreX() - size / 2, top, size, size);
        knob.name.setBounds (cell.getX(), top + size, cell.getWidth(), 14);
        knob.value.setBounds (cell.getX(), top + size + 14, cell.getWidth(), 13);
    }
}

int DrumRackPanel::padAt (int x, int y) const
{
    for (const auto& pad : controls->pads)
        if (pad->getBounds().contains (x, y))
            return pad->pad;
    return -1;
}

bool DrumRackPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (! Project::isDrumTrack (track))
        return false;
    for (const auto& path : files)
        if (project.getSamplePool().getFormats().findFormatForFileExtension (juce::File (path).getFileExtension()) != nullptr)
            return true;
    return false;
}

void DrumRackPanel::fileDragMove (const juce::StringArray&, int x, int y)
{
    const auto target = padAt (x, y);
    for (auto& pad : controls->pads)
    {
        pad->dropTarget = pad->pad == target;
        pad->repaint();
    }
}

void DrumRackPanel::fileDragExit (const juce::StringArray&)
{
    for (auto& pad : controls->pads)
    {
        pad->dropTarget = false;
        pad->repaint();
    }
}

void DrumRackPanel::filesDropped (const juce::StringArray& files, int x, int y)
{
    fileDragExit (files);
    const auto firstPad = padAt (x, y);
    if (firstPad < 0 || ! Project::isDrumTrack (track))
        return;
    juce::StringArray keys;
    for (const auto& path : files)
    {
        const juce::File file (path);
        if (project.getSamplePool().getFormats().findFormatForFileExtension (file.getFileExtension()) != nullptr)
            keys.add (SamplePool::keyFor (file));
        if (keys.size() >= DrumRack::numPads - firstPad)
            break;
    }
    if (keys.isEmpty())
        return;
    selectPad (firstPad);

    struct LoadBatch
    {
        std::vector<bool> loaded;
        int remaining = 0;
    };
    auto batch = std::make_shared<LoadBatch>();
    batch->remaining = keys.size();
    batch->loaded.resize ((size_t) keys.size(), false);
    juce::Component::SafePointer<DrumRackPanel> safeThis (this);
    const auto targetTrack = track;
    const auto device = Project::getDevice (track);
    for (int i = 0; i < keys.size(); ++i)
        project.getSamplePool().loadAsync (juce::File (keys[i]),
                                           [safeThis, targetTrack, device, firstPad, keys, batch, i] (SamplePool::Ptr sample)
        {
            if (safeThis == nullptr)
                return;
            batch->loaded[(size_t) i] = sample != nullptr;
            if (--batch->remaining != 0 || safeThis->project.indexOfTrack (targetTrack) < 0
                || Project::getDevice (targetTrack) != device)
                return;
            bool started = false;
            for (int j = 0; j < keys.size(); ++j)
                if (batch->loaded[(size_t) j])
                {
                    if (! started)
                    {
                        safeThis->project.beginTransaction ("Load drum samples");
                        started = true;
                    }
                    safeThis->project.setPadSample (device, firstPad + j, keys[j]);
                }
            safeThis->refresh();
        });
}

} // namespace daw

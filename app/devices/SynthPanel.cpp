#include "SynthPanel.h"
#include "DawLookAndFeel.h"
#include "ProjectControls.h"
#include "Synth.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numeric>
#include <vector>

namespace daw
{

namespace
{
    constexpr int titleBarHeight = 26;
    constexpr int sectionTitleHeight = 18;
    constexpr int sectionPadding = 8;
    constexpr int nameHeight = 14;
    constexpr int valueHeight = 13;
    constexpr int rowGap = 4;
    constexpr int maxKnobSize = 84;

    /** Combo boxes need a smaller font and a narrower arrow zone than the LookAndFeel default
        to fit the device faceplate. */
    class DevicePanelLookAndFeel : public DawLookAndFeel
    {
    public:
        juce::Font getComboBoxFont (juce::ComboBox&) override
        {
            return juce::FontOptions (10.5f);
        }

        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            label.setBounds (3, 1, juce::jmax (0, box.getWidth() - 22), box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
        }
    };

    /** Width weight of one grid column: wave selectors get more room than a knob column. */
    constexpr float choiceColumnWeight = 1.4f;
    constexpr float knobColumnWeight = 1.0f;

    /** Short labels for the knob faces (the ParamInfo names are sentences). */
    juce::String shortParamName (int index, const Synth::ParamInfo& info)
    {
        switch (index)
        {
            case Synth::osc1Wave:        return "Osc 1";
            case Synth::osc2Wave:        return "Osc 2";
            case Synth::osc2Semitones:   return "Semi";
            case Synth::osc2Detune:      return "Detune";
            case Synth::oscMix:          return "Mix";
            case Synth::filterCutoff:    return "Cutoff";
            case Synth::filterResonance: return "Resonance";
            case Synth::filterEnvAmount: return "Env Amt";
            case Synth::filterKeyTrack:  return "Key Track";
            case Synth::filterAttack:    return "Attack";
            case Synth::filterDecay:     return "Decay";
            case Synth::filterSustain:   return "Sustain";
            case Synth::filterRelease:   return "Release";
            case Synth::ampAttack:       return "Attack";
            case Synth::ampDecay:        return "Decay";
            case Synth::ampSustain:      return "Sustain";
            case Synth::ampRelease:      return "Release";
            case Synth::velocitySens:    return "Velocity";
            case Synth::volume:          return "Volume";
            default:                     break;
        }

        return info.name != nullptr ? juce::String (info.name) : juce::String();
    }

    juce::String signedNumber (int value, const juce::String& unit)
    {
        return juce::String (value > 0 ? "+" : "") + juce::String (value)
               + (unit.isNotEmpty() ? " " + unit : juce::String());
    }

    /** "1.20 kHz", "350 ms", "-6.0 dB", "+7 st", "-12 ct", "45 %". */
    juce::String formatParamValue (const Synth::ParamInfo& info, double value)
    {
        const auto unit = juce::String (info.unit != nullptr ? info.unit : "");

        if (unit == "Hz")
            return value >= 1000.0 ? juce::String::formatted ("%.2f kHz", value / 1000.0)
                                   : juce::String::formatted ("%.0f Hz", value);

        if (unit == "s")
            return value < 1.0 ? juce::String::formatted ("%.0f ms", value * 1000.0)
                               : juce::String::formatted ("%.2f s", value);

        if (unit == "dB")
            return juce::String::formatted ("%.1f dB", value);

        if (unit == "st")
            return signedNumber (juce::roundToInt (value), unit);

        if (unit == "ct")
            return signedNumber (juce::roundToInt (value), unit);

        if (unit == "%")
            return juce::String::formatted ("%.0f %%", value * 100.0);

        if (info.minValue < 0.0f && info.maxValue > 0.0f)
            return juce::String::formatted ("%+.2f", value);

        return juce::String::formatted ("%.2f", value);
    }

    void styleDeviceLabel (juce::Label& label, float fontHeight, juce::Colour textColour)
    {
        label.setFont (juce::FontOptions (fontHeight));
        label.setColour (juce::Label::textColourId, textColour);
        label.setJustificationType (juce::Justification::centred);
        label.setMinimumHorizontalScale (0.65f);
        label.setInterceptsMouseClicks (false, false);
    }

    struct DeviceCell
    {
        int paramIndex = 0;
        int row = 0;
        int column = 0;
    };
}

//==============================================================================
struct SynthPanel::Controls
{
    struct Knob
    {
        juce::Slider slider;
        juce::Label name, value;
        int paramIndex = -1;
    };

    struct Choice
    {
        juce::Label name;
        juce::ComboBox box;
        int paramIndex = -1;
    };

    struct Section
    {
        juce::String title;
        std::vector<DeviceCell> cells;
        std::vector<float> columnWeights;
        int rows = 1, columns = 1;
        juce::Rectangle<int> bounds;
    };

    DevicePanelLookAndFeel comboLookAndFeel;

    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<Choice>> choices;
    std::array<Knob*, Synth::numParams> knobByParam {};
    std::array<Choice*, Synth::numParams> choiceByParam {};
    std::vector<Section> sections;

    ~Controls()
    {
        for (auto& choice : choices)
            choice->box.setLookAndFeel (nullptr);
    }
};

//==============================================================================
SynthPanel::SynthPanel (Project& p)
    : project (p), controls (std::make_unique<Controls>())
{
    setOpaque (true);

    const auto makeKnob = [this] (int paramIndex, const Synth::ParamInfo& info) -> Controls::Knob*
    {
        auto knob = std::make_unique<Controls::Knob>();
        knob->paramIndex = paramIndex;

        auto& slider = knob->slider;
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);

        juce::NormalisableRange<double> range ((double) info.minValue, (double) info.maxValue,
                                               info.stepped ? 1.0 : 0.0);

        if (! info.stepped)
        {
            const auto lo = (double) info.minValue;
            const auto hi = (double) info.maxValue;
            const auto centre = (double) info.centreValue;

            if (centre > lo && centre < hi)
                range.setSkewForCentre (centre);
        }

        slider.setNormalisableRange (range);
        slider.setDoubleClickReturnValue (true, (double) info.defaultValue);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setPopupDisplayEnabled (true, true, this);
        slider.setMouseClickGrabsKeyboardFocus (false);
        slider.textFromValueFunction = [info] (double value) { return formatParamValue (info, value); };

        styleDeviceLabel (knob->name, 10.5f, colours::text);
        styleDeviceLabel (knob->value, 10.0f, colours::textDim);

        auto* raw = knob.get();
        controls->knobs.push_back (std::move (knob));
        controls->knobByParam[(size_t) paramIndex] = raw;

        const auto valueText = [raw, info] { return formatParamValue (info, raw->slider.getValue()); };
        raw->name.setText (shortParamName (paramIndex, info), juce::dontSendNotification);
        raw->value.setText (valueText(), juce::dontSendNotification);

        const auto actionName = juce::String ("Change ") + shortParamName (paramIndex, info);
        bindSliderToProject (raw->slider, project, actionName, [this, paramIndex, valueText] (double value)
        {
            project.setDeviceParam (track, paramIndex, (float) value);
            controls->knobByParam[(size_t) paramIndex]->value.setText (valueText(), juce::dontSendNotification);
        });

        addAndMakeVisible (raw->slider);
        addAndMakeVisible (raw->name);
        addAndMakeVisible (raw->value);
        return raw;
    };

    const auto makeChoice = [this] (int paramIndex, const Synth::ParamInfo& info)
    {
        auto choice = std::make_unique<Controls::Choice>();
        choice->paramIndex = paramIndex;

        auto& box = choice->box;
        box.addItemList ({ "Saw", "Square", "Triangle", "Sine" }, 1);
        box.setSelectedId (1, juce::dontSendNotification);
        box.setMouseClickGrabsKeyboardFocus (false);
        box.setColour (juce::ComboBox::backgroundColourId, colours::panelRaised);
        box.setColour (juce::ComboBox::textColourId, colours::text);
        box.setColour (juce::ComboBox::outlineColourId, colours::outline);
        box.setColour (juce::ComboBox::arrowColourId, colours::textDim);
        box.setColour (juce::ComboBox::focusedOutlineColourId, colours::accent);
        box.setLookAndFeel (&controls->comboLookAndFeel);

        styleDeviceLabel (choice->name, 10.5f, colours::text);
        choice->name.setText (shortParamName (paramIndex, info), juce::dontSendNotification);

        auto* raw = choice.get();

        box.onChange = [this, paramIndex, raw, name = juce::String (info.name != nullptr ? info.name : "")]
        {
            project.beginTransaction ("Change " + name);
            project.setDeviceParam (track, paramIndex, (float) (raw->box.getSelectedId() - 1));
        };

        controls->choices.push_back (std::move (choice));
        controls->choiceByParam[(size_t) paramIndex] = raw;

        addAndMakeVisible (raw->box);
        addAndMakeVisible (raw->name);
    };

    for (int paramIndex = 0; paramIndex < Synth::numParams; ++paramIndex)
    {
        const auto& info = Synth::getParamInfos()[(size_t) paramIndex];

        if (info.id == nullptr)
            continue; // parameter set not implemented yet: show nothing for this entry

        if (paramIndex == Synth::osc1Wave || paramIndex == Synth::osc2Wave)
            makeChoice (paramIndex, info);
        else
            makeKnob (paramIndex, info);
    }

    const auto addSection = [this] (const juce::String& title, int rows, int columns,
                                    std::initializer_list<DeviceCell> cells)
    {
        Controls::Section section;
        section.title = title;
        section.rows = rows;
        section.columns = columns;

        for (const auto& cell : cells)
            if (controls->knobByParam[(size_t) cell.paramIndex] != nullptr
                || controls->choiceByParam[(size_t) cell.paramIndex] != nullptr)
                section.cells.push_back (cell);

        if (section.cells.empty())
            return;

        // Trim empty trailing rows/columns (e.g. a parameter the synth doesn't expose).
        int usedRows = 1, usedColumns = 1;

        for (const auto& cell : section.cells)
        {
            usedRows = juce::jmax (usedRows, cell.row + 1);
            usedColumns = juce::jmax (usedColumns, cell.column + 1);
        }

        section.rows = usedRows;
        section.columns = usedColumns;
        section.columnWeights.assign ((size_t) usedColumns, knobColumnWeight);

        for (const auto& cell : section.cells)
            if (controls->choiceByParam[(size_t) cell.paramIndex] != nullptr)
                section.columnWeights[(size_t) cell.column] = choiceColumnWeight;

        controls->sections.push_back (std::move (section));
    };

    addSection ("Oscillators", 2, 3, {
        { Synth::osc1Wave,      0, 0 },
        { Synth::osc2Wave,      1, 0 },
        { Synth::osc2Semitones, 0, 1 },
        { Synth::osc2Detune,    1, 1 },
        { Synth::oscMix,        0, 2 },
    });

    addSection ("Filter", 2, 2, {
        { Synth::filterCutoff,    0, 0 },
        { Synth::filterResonance, 0, 1 },
        { Synth::filterEnvAmount, 1, 0 },
        { Synth::filterKeyTrack,  1, 1 },
    });

    addSection ("Filter Envelope", 2, 2, {
        { Synth::filterAttack,  0, 0 },
        { Synth::filterDecay,   0, 1 },
        { Synth::filterSustain, 1, 0 },
        { Synth::filterRelease, 1, 1 },
    });

    addSection ("Amp Envelope", 2, 2, {
        { Synth::ampAttack,  0, 0 },
        { Synth::ampDecay,   0, 1 },
        { Synth::ampSustain, 1, 0 },
        { Synth::ampRelease, 1, 1 },
    });

    addSection ("Output", 2, 1, {
        { Synth::velocitySens, 0, 0 },
        { Synth::volume,       1, 0 },
    });

    refresh();
}

SynthPanel::~SynthPanel() = default;

void SynthPanel::setTrack (juce::ValueTree t)
{
    if (t == track)
        return;

    track = t;

    for (auto& choice : controls->choices)
        choice->box.hidePopup();

    refresh();
    repaint();
}

void SynthPanel::refresh()
{
    if (controls == nullptr)
        return;

    const auto haveTrack = track.isValid();

    for (auto& knob : controls->knobs)
    {
        const auto& info = Synth::getParamInfos()[(size_t) knob->paramIndex];
        const auto targetId = haveTrack ? Project::getDeviceParamTargetId (Project::getDevice (track), knob->paramIndex)
                                        : juce::String();
        const auto automated = haveTrack && Project::isAutomated (track, targetId);
        showAutomationState (knob->slider, automated);

        const auto value = ! haveTrack ? (double) info.defaultValue
                         : automated   ? (double) project.getEngineValue (track, targetId)
                                       : (double) Project::getDeviceParam (track, knob->paramIndex);

        if (! knob->slider.isMouseButtonDown())
            knob->slider.setValue (value, juce::dontSendNotification);

        knob->value.setText (formatParamValue (info, knob->slider.getValue()), juce::dontSendNotification);
    }

    for (auto& choice : controls->choices)
    {
        const auto& info = Synth::getParamInfos()[(size_t) choice->paramIndex];
        const auto value = haveTrack ? Project::getDeviceParam (track, choice->paramIndex) : info.defaultValue;
        const auto id = juce::jlimit (0, 3, juce::roundToInt (value)) + 1;
        choice->box.setSelectedId (id, juce::dontSendNotification);
    }
}

void SynthPanel::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);

    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    auto header = getLocalBounds().removeFromTop (titleBarHeight).reduced (10, 0);

    g.setColour (colours::accent);
    g.fillRect (header.removeFromLeft (3).withSizeKeepingCentre (3, 11));

    g.setColour (colours::text);
    g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    header.removeFromLeft (7);
    g.drawText ("Synth", header, juce::Justification::centredLeft);

    const auto voices = juce::String (Synth::numVoices) + " voices";
    g.setColour (colours::textDim);
    g.setFont (juce::FontOptions (9.5f));
    g.drawText (voices, header, juce::Justification::centredRight);

    g.setColour (colours::outline);
    g.drawHorizontalLine (titleBarHeight - 1, 0.0f, (float) getWidth());

    for (size_t i = 0; i < controls->sections.size(); ++i)
    {
        const auto& section = controls->sections[i];

        if (i > 0)
        {
            g.setColour (colours::outline);
            g.drawVerticalLine (section.bounds.getX(),
                                (float) section.bounds.getY() + 4.0f,
                                (float) section.bounds.getBottom() - 4.0f);
        }

        auto title = section.bounds.withTrimmedLeft (sectionPadding)
                                    .withTrimmedRight (sectionPadding)
                                    .removeFromTop (sectionTitleHeight)
                                    .withTrimmedTop (4);

        g.setColour (colours::textDim);
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (section.title, title, juce::Justification::centredLeft);
    }
}

void SynthPanel::resized()
{
    if (controls == nullptr)
        return;

    auto content = getLocalBounds();
    content.removeFromTop (titleBarHeight);
    content = content.reduced (10, 8);

    float totalWeight = 0.0f;

    for (const auto& section : controls->sections)
        totalWeight += std::accumulate (section.columnWeights.begin(), section.columnWeights.end(), 0.0f);

    if (totalWeight <= 0.0f || content.isEmpty())
        return;

    const auto usableWidth = juce::jmax (1, content.getWidth()
                                               - (int) controls->sections.size() * 2 * sectionPadding);
    const auto columnUnit = (float) usableWidth / totalWeight;

    float x = (float) content.getX();

    for (auto& section : controls->sections)
    {
        const auto sectionWeight = std::accumulate (section.columnWeights.begin(),
                                                    section.columnWeights.end(), 0.0f);
        const auto left = juce::roundToInt (x);
        const auto right = juce::roundToInt (x + columnUnit * sectionWeight) + 2 * sectionPadding;

        section.bounds = juce::Rectangle<int> (left, content.getY(), right - left, content.getHeight());
        x += columnUnit * sectionWeight + 2.0f * (float) sectionPadding;

        auto grid = section.bounds.withTrimmedLeft (sectionPadding)
                                  .withTrimmedRight (sectionPadding)
                                  .withTrimmedTop (sectionTitleHeight);

        if (grid.isEmpty() || section.rows <= 0 || section.columns <= 0)
            continue;

        const auto rowHeight = (grid.getHeight() - rowGap * (section.rows - 1)) / section.rows;

        for (const auto& item : section.cells)
        {
            const auto columnStart = std::accumulate (section.columnWeights.begin(),
                                                      section.columnWeights.begin() + item.column, 0.0f);
            const auto columnEnd = columnStart + section.columnWeights[(size_t) item.column];

            const auto cellLeft = grid.getX()
                                  + juce::roundToInt ((float) grid.getWidth() * columnStart / sectionWeight);
            const auto cellRight = grid.getX()
                                   + juce::roundToInt ((float) grid.getWidth() * columnEnd / sectionWeight);
            const auto cellTop = grid.getY() + item.row * (rowHeight + rowGap);

            const juce::Rectangle<int> cell (cellLeft, cellTop, cellRight - cellLeft, rowHeight);

            const auto knobSpace = cell.getHeight() - nameHeight - valueHeight;
            const auto knobSize = juce::jlimit (16, maxKnobSize,
                                                juce::jmin (cell.getWidth() - 6, knobSpace));
            const auto blockHeight = knobSize + nameHeight + valueHeight;
            const auto top = cell.getY() + juce::jmax (0, (cell.getHeight() - blockHeight) / 2);

            const auto nameBounds = juce::Rectangle<int> (cell.getX(), top + knobSize,
                                                          cell.getWidth(), nameHeight);
            const auto valueBounds = juce::Rectangle<int> (cell.getX(), top + knobSize + nameHeight,
                                                           cell.getWidth(), valueHeight);

            if (auto* knob = controls->knobByParam[(size_t) item.paramIndex])
            {
                knob->slider.setBounds (cell.getCentreX() - knobSize / 2, top, knobSize, knobSize);
                knob->name.setBounds (nameBounds);
                knob->value.setBounds (valueBounds);
            }
            else if (auto* choice = controls->choiceByParam[(size_t) item.paramIndex])
            {
                const auto comboWidth = juce::jmax (24, juce::jmin (cell.getWidth() - 8, 92));
                const auto comboHeight = 22;

                choice->box.setBounds (cell.getCentreX() - comboWidth / 2,
                                       top + (knobSize - comboHeight) / 2,
                                       comboWidth, comboHeight);
                choice->name.setBounds (nameBounds);
            }
        }
    }
}

} // namespace daw

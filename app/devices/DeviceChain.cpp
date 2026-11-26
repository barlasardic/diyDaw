#include "DeviceChain.h"
#include "AudioEffect.h"
#include "DawLookAndFeel.h"
#include "DrumRackPanel.h"
#include "Effects.h"
#include "ProjectControls.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace daw
{

namespace
{
    constexpr int titleBarHeight = 26;
    constexpr int deviceGap = 6;
    constexpr int chainPadding = 8;

    constexpr int effectPadding = 8;
    constexpr int effectColumnWidth = 74;
    constexpr int effectMeterWidth = 16;
    constexpr int maxEffectKnobSize = 62;
    constexpr int minEffectPanelWidth = 150;

    constexpr int addButtonWidth = 108;
    constexpr int addButtonHeight = 26;

    constexpr int nameHeight = 14;
    constexpr int valueHeight = 13;
    constexpr int rowGap = 4;

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

    /** "1.20 kHz", "350 ms", "-6.0 dB", "4.0:1", "45 %". Mirrors the device knobs in SynthPanel. */
    juce::String formatParamValue (const ParamInfo& info, double value)
    {
        const auto unit = juce::String (info.unit != nullptr ? info.unit : "");

        if (unit == "Hz")
            return value >= 1000.0 ? juce::String::formatted ("%.2f kHz", value / 1000.0)
                                   : juce::String::formatted ("%.0f Hz", value);

        if (unit == "s")
            return value < 1.0 ? juce::String::formatted ("%.0f ms", value * 1000.0)
                               : juce::String::formatted ("%.2f s", value);

        if (unit == "ms")
            return juce::String::formatted ("%.0f ms", value);

        if (unit == "dB")
            return juce::String::formatted ("%.1f dB", value);

        if (unit == ":1")
            return juce::String::formatted ("%.1f:1", value);

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

    juce::String paramName (const ParamInfo& info)
    {
        return info.name != nullptr ? juce::String (info.name) : juce::String();
    }

    /** Menu item names for a stepped parameter. The delay's division is the only one with
        musically meaningful names; everything else gets its integer value. */
    juce::StringArray steppedValueNames (const juce::String& effectType, const ParamInfo& info)
    {
        if (effectType == "delay" && juce::String (info.id != nullptr ? info.id : "") == "syncDivision")
            return { "1/16", "1/8", "1/8 D", "1/4", "1/4 D", "1/2", "1 Bar" };

        juce::StringArray names;
        const auto first = juce::roundToInt (info.minValue);
        const auto last = juce::roundToInt (info.maxValue);
        const auto unit = juce::String (info.unit != nullptr ? info.unit : "");

        for (int value = first; value <= last; ++value)
            names.add (unit.isEmpty() ? juce::String (value) : juce::String (value) + " " + unit);

        return names;
    }

    /** Minimal title-bar glyph ("<", ">", "x") that only paints itself on hover. */
    class GlyphButton : public juce::Button
    {
    public:
        explicit GlyphButton (const juce::String& glyphText) : juce::Button (glyphText), glyph (glyphText) {}

        void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
        {
            if ((isMouseOverButton || isButtonDown) && isEnabled())
            {
                g.setColour (colours::panelRaised.brighter (isButtonDown ? 0.0f : 0.1f));
                g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 3.0f);
            }

            g.setColour (isEnabled() ? (isMouseOverButton ? colours::text : colours::textDim)
                                     : colours::textDim.withAlpha (0.35f));
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText (glyph, getLocalBounds(), juce::Justification::centred);
        }

    private:
        juce::String glyph;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GlyphButton)
    };

    /** Small round on/off lamp for a device's title bar. */
    class PowerButton : public juce::Button
    {
    public:
        PowerButton() : juce::Button ("On/Off")
        {
            setClickingTogglesState (true);
        }

        void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override
        {
            auto bounds = getLocalBounds().toFloat().reduced (2.0f);
            auto colour = getToggleState() ? colours::accent : colours::panelRaised;

            if (isButtonDown)
                colour = colour.darker (0.2f);
            else if (isMouseOverButton)
                colour = colour.brighter (0.1f);

            g.setColour (colour);
            g.fillEllipse (bounds);
            g.setColour (colours::outline);
            g.drawEllipse (bounds, 1.0f);
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PowerButton)
    };

    /** One effect DEVICE: a titled, rounded panel with an on/off lamp in the title bar,
        "<" ">" "x" buttons and two rows of parameter controls generated from
        Project::getDeviceParamInfos(). An effect without infos shows only its title bar.

        Message thread only. The owning DeviceChain calls refresh() at ~30 Hz. */
    class EffectPanel : public juce::Component
    {
    public:
        EffectPanel (Project& projectToUse, juce::ValueTree deviceToShow, int indexAmongEffects,
                     int numEffects, DevicePanelLookAndFeel& lookAndFeelToUse)
            : project (projectToUse),
              device (deviceToShow),
              comboLookAndFeel (lookAndFeelToUse),
              type (Project::getDeviceType (deviceToShow)),
              displayName (getEffectDisplayName (type)),
              showsMeter (type == "compressor"),
              enabled (Project::isEffectEnabled (deviceToShow))
        {
            power.setMouseClickGrabsKeyboardFocus (false);
            bindToggleToProject (power, project, "Bypass " + displayName, [this] (bool shouldBeEnabled)
            {
                enabled = shouldBeEnabled;
                project.setEffectEnabled (device, shouldBeEnabled);
                repaint();
            });
            addAndMakeVisible (power);

            moveLeft.setMouseClickGrabsKeyboardFocus (false);
            moveLeft.setEnabled (indexAmongEffects > 0);
            moveLeft.onClick = [this] { if (onMove) onMove (device, -1); };
            addAndMakeVisible (moveLeft);

            moveRight.setMouseClickGrabsKeyboardFocus (false);
            moveRight.setEnabled (indexAmongEffects < numEffects - 1);
            moveRight.onClick = [this] { if (onMove) onMove (device, 1); };
            addAndMakeVisible (moveRight);

            removeButton.setMouseClickGrabsKeyboardFocus (false);
            removeButton.onClick = [this] { if (onRemove) onRemove (device); };
            addAndMakeVisible (removeButton);

            buildControls();
            refresh();
        }

        ~EffectPanel() override = default;

        bool hasControls() const noexcept { return ! cells.empty(); }

        int getPreferredWidth() const
        {
            if (cells.empty())
                return minEffectPanelWidth;

            auto width = effectPadding * 2 + getNumColumns() * effectColumnWidth;

            if (showsMeter)
                width += effectMeterWidth + 6;

            return width;
        }

        /** Resyncs every control from the model without disturbing an active gesture. */
        void refresh()
        {
            const auto nowEnabled = Project::isEffectEnabled (device);

            if (nowEnabled != enabled)
            {
                enabled = nowEnabled;
                repaint();
            }

            if (! power.isMouseButtonDown())
                power.setToggleState (enabled, juce::dontSendNotification);

            for (auto& knob : knobs)
            {
                const auto targetId = Project::getDeviceParamTargetId (device, knob->paramIndex);
                const auto automated = Project::isAutomated (device.getParent(), targetId);
                showAutomationState (knob->slider, automated);

                if (! knob->slider.isMouseButtonDown())
                    knob->slider.setValue (automated ? (double) project.getEngineValue (device.getParent(), targetId)
                                                     : (double) Project::getDeviceParam (device, knob->paramIndex),
                                           juce::dontSendNotification);

                knob->value.setText (formatParamValue (knob->info, knob->slider.getValue()),
                                     juce::dontSendNotification);
            }

            for (auto& choice : choices)
            {
                if (choice->box.isPopupActive())
                    continue;

                const auto value = Project::getDeviceParam (device, choice->paramIndex);
                const auto selected = juce::roundToInt (value - choice->info.minValue) + 1;
                choice->box.setSelectedId (juce::jlimit (1, juce::jmax (1, choice->box.getNumItems()), selected),
                                           juce::dontSendNotification);
            }

            for (auto& toggle : toggles)
            {
                if (! toggle->button.isMouseButtonDown())
                    toggle->button.setToggleState (Project::getDeviceParam (device, toggle->paramIndex) > 0.5f,
                                                   juce::dontSendNotification);
            }

            if (showsMeter)
            {
                auto reduction = 0.0f;

                if (const auto instance = project.getEffectInstance (device); instance != nullptr)
                    if (auto* compressor = dynamic_cast<CompressorEffect*> (instance.get()); compressor != nullptr)
                        reduction = compressor->getGainReductionDb();

                if (std::abs (reduction - gainReduction) > 0.05f)
                {
                    gainReduction = reduction;

                    if (! meterBounds.isEmpty())
                        repaint (meterBounds);
                }
            }
        }

        std::function<void (juce::ValueTree)> onRemove;
        std::function<void (juce::ValueTree, int)> onMove;

        void paint (juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

            g.setColour (colours::panelRaised);
            g.fillRoundedRectangle (bounds, 4.0f);
            g.setColour (colours::outline);
            g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

            g.setColour (colours::outline);
            g.drawHorizontalLine (titleBarHeight - 1, 1.0f, (float) getWidth() - 1.0f);

            auto title = getLocalBounds().removeFromTop (titleBarHeight)
                                          .withTrimmedLeft (32)
                                          .withTrimmedRight (66);
            g.setColour (enabled ? colours::text : colours::textDim);
            g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
            g.drawText (displayName, title, juce::Justification::centredLeft, true);

            if (showsMeter && ! meterBounds.isEmpty())
            {
                auto meter = meterBounds.toFloat();
                g.setColour (colours::outline);
                g.fillRoundedRectangle (meter, 2.0f);

                const auto amount = juce::jlimit (0.0f, 1.0f, gainReduction / 24.0f);

                if (amount > 0.0f)
                {
                    auto fill = meter.reduced (1.5f);
                    fill = fill.removeFromBottom (fill.getHeight() * amount);

                    g.setColour (amount > 0.85f ? colours::meterHigh
                                                : (amount > 0.6f ? colours::meterMid : colours::meterLow));
                    g.fillRoundedRectangle (fill, 1.5f);
                }

                g.setColour (colours::panelRaised);

                for (int tick = 1; tick < 4; ++tick)
                {
                    const auto y = meter.getY() + meter.getHeight() * (float) tick / 4.0f;
                    g.drawHorizontalLine (juce::roundToInt (y), meter.getX() + 3.0f, meter.getRight() - 3.0f);
                }
            }
        }

        void paintOverChildren (juce::Graphics& g) override
        {
            if (enabled)
                return;

            // Bypassed: dim the knobs like Ableton does, but keep the title bar readable.
            g.setColour (colours::panel.withAlpha (0.45f));
            g.fillRect (getLocalBounds().withTrimmedTop (titleBarHeight));
        }

        void resized() override
        {
            auto header = getLocalBounds().removeFromTop (titleBarHeight).reduced (6, 0);
            power.setBounds (header.removeFromLeft (16).withSizeKeepingCentre (16, 16));
            removeButton.setBounds (header.removeFromRight (18).withSizeKeepingCentre (16, 16));
            moveRight.setBounds (header.removeFromRight (18).withSizeKeepingCentre (16, 16));
            moveLeft.setBounds (header.removeFromRight (18).withSizeKeepingCentre (16, 16));

            if (cells.empty())
            {
                meterBounds = {};
                return;
            }

            auto body = getLocalBounds().withTrimmedTop (titleBarHeight).reduced (effectPadding, 6);

            if (showsMeter)
            {
                meterBounds = body.removeFromRight (effectMeterWidth);
                body.removeFromRight (6);
            }

            const auto rows = getNumRows();
            const auto columns = getNumColumns();
            const auto cellWidth = juce::jmax (1, body.getWidth() / columns);
            const auto cellHeight = juce::jmax (1, (body.getHeight() - rowGap * (rows - 1)) / rows);

            for (size_t i = 0; i < cells.size(); ++i)
            {
                const auto column = (int) i % columns;
                const auto row = (int) i / columns;

                const juce::Rectangle<int> cell (body.getX() + column * cellWidth,
                                                 body.getY() + row * (cellHeight + rowGap),
                                                 cellWidth, cellHeight);

                const auto square = juce::jlimit (16, maxEffectKnobSize,
                                                  juce::jmin (cell.getWidth() - 6,
                                                              cell.getHeight() - nameHeight - valueHeight));
                const auto top = cell.getY()
                                 + juce::jmax (0, (cell.getHeight() - (square + nameHeight + valueHeight)) / 2);
                const auto nameBounds = juce::Rectangle<int> (cell.getX(), top + square, cell.getWidth(), nameHeight);
                const auto valueBounds = juce::Rectangle<int> (cell.getX(), top + square + nameHeight,
                                                               cell.getWidth(), valueHeight);

                const auto& control = cells[i];

                switch (control.kind)
                {
                    case Cell::Kind::knob:
                    {
                        auto& knob = *knobs[(size_t) control.index];
                        knob.slider.setBounds (cell.getCentreX() - square / 2, top, square, square);
                        knob.name.setBounds (nameBounds);
                        knob.value.setBounds (valueBounds);
                        break;
                    }

                    case Cell::Kind::choice:
                    {
                        auto& choice = *choices[(size_t) control.index];
                        const auto comboWidth = juce::jmax (24, juce::jmin (cell.getWidth() - 8, 92));
                        const auto comboHeight = juce::jmin (22, square);

                        choice.box.setBounds (cell.getCentreX() - comboWidth / 2,
                                              top + (square - comboHeight) / 2, comboWidth, comboHeight);
                        choice.name.setBounds (nameBounds);
                        break;
                    }

                    case Cell::Kind::toggle:
                    {
                        auto& toggle = *toggles[(size_t) control.index];
                        const auto buttonSize = juce::jlimit (12, 24, square);

                        toggle.button.setBounds (cell.getCentreX() - buttonSize / 2,
                                                 top + (square - buttonSize) / 2, buttonSize, buttonSize);
                        toggle.name.setBounds (nameBounds);
                        break;
                    }
                }
            }
        }

    private:
        struct Knob
        {
            juce::Slider slider;
            juce::Label name, value;
            ParamInfo info {};
            int paramIndex = 0;
        };

        struct Choice
        {
            juce::ComboBox box;
            juce::Label name;
            ParamInfo info {};
            int paramIndex = 0;
        };

        struct Toggle
        {
            juce::TextButton button;
            juce::Label name;
            ParamInfo info {};
            int paramIndex = 0;
        };

        struct Cell
        {
            enum class Kind { knob, choice, toggle };

            Kind kind = Kind::knob;
            int index = 0;
        };

        int getNumRows() const noexcept { return cells.size() > 2 ? 2 : 1; }

        int getNumColumns() const noexcept
        {
            if (cells.empty())
                return 1;

            const auto rows = getNumRows();
            return ((int) cells.size() + rows - 1) / rows;
        }

        void buildControls()
        {
            const auto& infos = Project::getDeviceParamInfos (device);

            for (int paramIndex = 0; paramIndex < (int) infos.size(); ++paramIndex)
            {
                const auto& info = infos[(size_t) paramIndex];

                if (info.id == nullptr)
                    continue;

                if (info.stepped && info.minValue >= 0.0f && info.maxValue <= 1.0f)
                    makeToggle (paramIndex, info);
                else if (info.stepped)
                    makeChoice (paramIndex, info);
                else
                    makeKnob (paramIndex, info);
            }
        }

        void makeKnob (int paramIndex, const ParamInfo& info)
        {
            auto knob = std::make_unique<Knob>();
            knob->info = info;
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
            raw->name.setText (paramName (info), juce::dontSendNotification);
            raw->value.setText (formatParamValue (info, (double) Project::getDeviceParam (device, paramIndex)),
                                juce::dontSendNotification);

            bindSliderToProject (raw->slider, project, "Change " + paramName (info),
                                 [this, raw, paramIndex] (double value)
            {
                project.setDeviceParam (device, paramIndex, (float) value);
                raw->value.setText (formatParamValue (raw->info, value), juce::dontSendNotification);
            });

            addAndMakeVisible (raw->slider);
            addAndMakeVisible (raw->name);
            addAndMakeVisible (raw->value);

            cells.push_back ({ Cell::Kind::knob, (int) knobs.size() });
            knobs.push_back (std::move (knob));
        }

        void makeChoice (int paramIndex, const ParamInfo& info)
        {
            auto choice = std::make_unique<Choice>();
            choice->info = info;
            choice->paramIndex = paramIndex;

            auto& box = choice->box;
            box.addItemList (steppedValueNames (type, info), 1);
            box.setSelectedId (1, juce::dontSendNotification);
            box.setMouseClickGrabsKeyboardFocus (false);
            box.setColour (juce::ComboBox::backgroundColourId, colours::panelRaised);
            box.setColour (juce::ComboBox::textColourId, colours::text);
            box.setColour (juce::ComboBox::outlineColourId, colours::outline);
            box.setColour (juce::ComboBox::arrowColourId, colours::textDim);
            box.setColour (juce::ComboBox::focusedOutlineColourId, colours::accent);
            box.setLookAndFeel (&comboLookAndFeel);

            styleDeviceLabel (choice->name, 10.5f, colours::text);

            auto* raw = choice.get();
            raw->name.setText (paramName (info), juce::dontSendNotification);

            box.onChange = [this, raw, paramIndex]
            {
                project.beginTransaction ("Change " + paramName (raw->info));
                project.setDeviceParam (device, paramIndex,
                                        raw->info.minValue + (float) (raw->box.getSelectedId() - 1));
            };

            addAndMakeVisible (raw->box);
            addAndMakeVisible (raw->name);

            cells.push_back ({ Cell::Kind::choice, (int) choices.size() });
            choices.push_back (std::move (choice));
        }

        void makeToggle (int paramIndex, const ParamInfo& info)
        {
            auto toggle = std::make_unique<Toggle>();
            toggle->info = info;
            toggle->paramIndex = paramIndex;

            auto& button = toggle->button;
            button.setClickingTogglesState (true);
            button.setMouseClickGrabsKeyboardFocus (false);
            button.setColour (juce::TextButton::buttonColourId, colours::panelRaised);
            button.setColour (juce::TextButton::buttonOnColourId, colours::accent);
            button.setColour (juce::TextButton::textColourOffId, colours::textDim);
            button.setColour (juce::TextButton::textColourOnId, juce::Colours::black);

            styleDeviceLabel (toggle->name, 10.5f, colours::text);

            auto* raw = toggle.get();
            raw->name.setText (paramName (info), juce::dontSendNotification);

            bindToggleToProject (button, project, "Change " + paramName (info),
                                 [this, paramIndex] (bool isOn)
            {
                project.setDeviceParam (device, paramIndex, isOn ? 1.0f : 0.0f);
            });

            addAndMakeVisible (raw->button);
            addAndMakeVisible (raw->name);

            cells.push_back ({ Cell::Kind::toggle, (int) toggles.size() });
            toggles.push_back (std::move (toggle));
        }

        Project& project;
        juce::ValueTree device;
        DevicePanelLookAndFeel& comboLookAndFeel;
        juce::String type, displayName;
        bool showsMeter = false;
        bool enabled = true;
        float gainReduction = 0.0f;
        juce::Rectangle<int> meterBounds;

        PowerButton power;
        GlyphButton moveLeft { "<" }, moveRight { ">" }, removeButton { "x" };

        std::vector<std::unique_ptr<Knob>> knobs;
        std::vector<std::unique_ptr<Choice>> choices;
        std::vector<std::unique_ptr<Toggle>> toggles;
        std::vector<Cell> cells;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectPanel)
    };
}

//==============================================================================
struct DeviceChain::Panels
{
    explicit Panels (Project& project) : drumRackPanel (project) {}

    DrumRackPanel drumRackPanel;
    DevicePanelLookAndFeel comboLookAndFeel;
    std::vector<std::unique_ptr<EffectPanel>> effects;
    juce::TextButton addEffect { "+ Add Effect" };
    bool rebuildPending = false;
};

//==============================================================================
DeviceChain::DeviceChain (Project& p)
    : project (p), synthPanel (p), panels (std::make_unique<Panels> (p))
{
    viewport.setViewedComponent (&content, false);
    viewport.setScrollBarsShown (false, true);
    content.addChildComponent (synthPanel);
    content.addChildComponent (panels->drumRackPanel);
    panels->drumRackPanel.onPreviewNote = [this] (int pitch, float velocity, bool isNoteOn)
    {
        if (onPreviewNote)
            onPreviewNote (pitch, velocity, isNoteOn);
    };

    panels->addEffect.setMouseClickGrabsKeyboardFocus (false);
    panels->addEffect.setTooltip ("Add an effect to this track");
    panels->addEffect.onClick = [this]
    {
        juce::PopupMenu menu;
        const auto& types = getEffectTypes();

        for (int i = 0; i < types.size(); ++i)
            menu.addItem (i + 1, getEffectDisplayName (types[i]));

        juce::Component::SafePointer<DeviceChain> safeThis (this);
        const auto targetTrack = track;

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&panels->addEffect),
                            [safeThis, targetTrack] (int result)
        {
            if (safeThis == nullptr || result <= 0)
                return;

            const auto& effectTypes = getEffectTypes();

            if (result > effectTypes.size())
                return;

            const auto effectType = effectTypes[result - 1];
            safeThis->project.beginTransaction ("Add " + getEffectDisplayName (effectType));
            safeThis->project.addEffect (targetTrack, effectType);
            safeThis->rebuild();
        });
    };
    content.addAndMakeVisible (panels->addEffect);

    addAndMakeVisible (viewport);
}

DeviceChain::~DeviceChain() { track.removeListener (this); }

void DeviceChain::setTrack (juce::ValueTree newTrack)
{
    track.removeListener (this);
    track = newTrack;
    track.addListener (this);
    panels->rebuildPending = false;
    rebuildNow();
}

void DeviceChain::rebuild()
{
    // Deferred: listeners fire while the model changes and panel buttons fire from inside the
    // component that this rebuild deletes.
    if (panels->rebuildPending)
        return;

    panels->rebuildPending = true;
    juce::Component::SafePointer<DeviceChain> safeThis (this);

    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis == nullptr)
            return;

        safeThis->panels->rebuildPending = false;
        safeThis->rebuildNow();
    });
}

void DeviceChain::rebuildNow()
{
    const auto isMidi = track.isValid() && Project::isMidiTrack (track);
    const auto isDrum = isMidi && Project::isDrumTrack (track);
    synthPanel.setTrack (isMidi && ! isDrum ? track : juce::ValueTree());
    synthPanel.setVisible (isMidi && ! isDrum);
    panels->drumRackPanel.setTrack (isDrum ? track : juce::ValueTree());
    panels->drumRackPanel.setVisible (isDrum);

    for (auto& effect : panels->effects)
        content.removeChildComponent (effect.get());

    panels->effects.clear();

    const auto effects = project.getEffects (track);

    for (int i = 0; i < effects.size(); ++i)
    {
        auto panel = std::make_unique<EffectPanel> (project, effects[i], i, effects.size(),
                                                    panels->comboLookAndFeel);

        panel->onRemove = [this] (juce::ValueTree device)
        {
            project.beginTransaction ("Remove " + getEffectDisplayName (Project::getDeviceType (device)));
            project.removeEffect (device);
            rebuild();
        };

        panel->onMove = [this] (juce::ValueTree device, int delta)
        {
            const auto currentEffects = project.getEffects (track);
            const auto index = currentEffects.indexOf (device);

            if (index < 0)
                return;

            project.beginTransaction ("Move " + getEffectDisplayName (Project::getDeviceType (device)));
            project.moveEffect (device, index + delta);
            rebuild();
        };

        content.addAndMakeVisible (*panel);
        panels->effects.push_back (std::move (panel));
    }

    panels->addEffect.setVisible (track.isValid());
    resized();
    repaint();
}

void DeviceChain::refresh()
{
    if (synthPanel.isVisible())
        synthPanel.refresh();

    if (panels->drumRackPanel.isVisible())
        panels->drumRackPanel.refresh();

    for (auto& effect : panels->effects)
        effect->refresh();
}

void DeviceChain::paint (juce::Graphics& g) { g.fillAll (colours::panel); }

void DeviceChain::resized()
{
    viewport.setBounds (getLocalBounds());

    const auto isMidi = track.isValid() && Project::isMidiTrack (track);
    const auto isDrum = isMidi && Project::isDrumTrack (track);
    const auto instrumentWidth = isDrum ? DrumRackPanel::preferredWidth : SynthPanel::preferredWidth;

    auto neededWidth = chainPadding * 2;

    if (isMidi)
        neededWidth += instrumentWidth + deviceGap;

    for (const auto& effect : panels->effects)
        neededWidth += effect->getPreferredWidth() + deviceGap;

    if (track.isValid())
        neededWidth += addButtonWidth + chainPadding;

    const auto scrollBarThickness = viewport.getScrollBarThickness();
    const auto height = juce::jmax (1, viewport.getHeight() - (neededWidth > viewport.getWidth()
                                                                   ? scrollBarThickness : 0));

    auto x = chainPadding;

    if (isMidi)
    {
        if (isDrum)
            panels->drumRackPanel.setBounds (x, 0, instrumentWidth, height);
        else
            synthPanel.setBounds (x, 0, instrumentWidth, height);
        x += instrumentWidth + deviceGap;
    }

    for (const auto& effect : panels->effects)
    {
        const auto panelHeight = effect->hasControls() ? height : juce::jmin (height, titleBarHeight);
        effect->setBounds (x, 0, effect->getPreferredWidth(), panelHeight);
        x += effect->getPreferredWidth() + deviceGap;
    }

    if (track.isValid())
    {
        panels->addEffect.setBounds (x, (height - addButtonHeight) / 2, addButtonWidth, addButtonHeight);
        x += addButtonWidth + chainPadding;
    }
    else
    {
        panels->addEffect.setBounds (0, 0, 0, 0);
    }

    content.setSize (juce::jmax (viewport.getWidth(), x), height);
}

void DeviceChain::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (parent == track && child.hasType (ids::DEVICE))
        rebuild();
}

void DeviceChain::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (parent == track && child.hasType (ids::DEVICE))
        rebuild();
}

void DeviceChain::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    if (parent == track)
        rebuild();
}

} // namespace daw

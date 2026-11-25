#pragma once

#include "Project.h"
#include "SynthPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace daw
{

/** Contents of the detail view's Device tab (Ableton device chain): a horizontally scrolling
    row of device panels for one track, left to right in signal order:

    - the instrument (a SynthPanel, MIDI tracks only, ~SynthPanel::preferredWidth wide),
    - one EffectPanel per effect DEVICE (Project::getEffects), each with a title bar holding an
      on/off toggle (lit = enabled, Project::setEffectEnabled), the effect's display name
      (getEffectDisplayName), small "<" ">" buttons (Project::moveEffect) and an "x" button
      (Project::removeEffect); below it knobs generated from Project::getDeviceParamInfos(device),
      bound with bindSliderToProject to Project::setDeviceParam (device, index, value); stepped
      params with range 0..1 are toggle buttons, other stepped params are combo boxes (the delay's
      syncDivision: "1/16", "1/8", "1/8 D", "1/4", "1/4 D", "1/2", "1 Bar"); the compressor panel
      also shows a vertical gain-reduction meter (Project::getEffectInstance -> CompressorEffect
      ::getGainReductionDb),
    - an "+ Add Effect" button at the end that pops up getEffectTypes() by display name and
      calls Project::addEffect (track, type) in its own transaction.

    Every edit is one undo step (beginTransaction first). The chain rebuilds its panels when the
    track's DEVICE children are added, removed or reordered (ValueTree listener on the track) and
    refresh() pulls parameter values back from the model (undo, file load).

    Message thread only. */
class DeviceChain : public juce::Component,
                    private juce::ValueTree::Listener
{
public:
    explicit DeviceChain (Project& project);
    ~DeviceChain() override;

    /** The track whose chain to show (audio or MIDI), or invalid for an empty view. */
    void setTrack (juce::ValueTree track);
    juce::ValueTree getTrack() const { return track; }

    /** Called ~30 Hz. */
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    /** Defers a rebuild to the message queue (safe from ValueTree and panel button callbacks). */
    void rebuild();

    /** Recreates the effect panels and lays the chain out. Never call from a panel callback. */
    void rebuildNow();

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;

    Project& project;
    juce::ValueTree track;

    juce::Viewport viewport;
    juce::Component content;
    SynthPanel synthPanel;

    struct Panels;
    std::unique_ptr<Panels> panels;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceChain)
};

} // namespace daw

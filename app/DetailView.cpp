#include "DetailView.h"
#include "DawLookAndFeel.h"

namespace daw
{

DetailView::DetailView (Project& p) : project (p), pianoRoll (p), synthPanel (p)
{
    for (auto* b : { &clipTab, &deviceTab })
    {
        b->setClickingTogglesState (false);
        b->setMouseClickGrabsKeyboardFocus (false);
        b->setColour (juce::TextButton::buttonOnColourId, colours::accent);
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        addAndMakeVisible (*b);
    }

    clipTab.setTooltip ("Clip editor (Shift+Tab switches)");
    deviceTab.setTooltip ("Instrument of the track (Shift+Tab switches)");
    clipTab.onClick = [this] { setTab (Tab::clip); };
    deviceTab.onClick = [this] { setTab (Tab::device); };

    pianoRoll.onPreviewNote = [this] (int pitch, float velocity, bool on)
    {
        if (onPreviewNote)
            onPreviewNote (pitch, velocity, on);
    };

    addChildComponent (pianoRoll);
    addChildComponent (synthPanel);
    updateVisibility();
}

void DetailView::showClip (juce::ValueTree newClip)
{
    clip = newClip;
    if (clip.isValid())
        showTrack (clip.getParent());

    pianoRoll.setClip (Project::isMidiClip (clip) ? clip : juce::ValueTree());
    setTab (Tab::clip);
}

void DetailView::showTrack (juce::ValueTree newTrack)
{
    track = newTrack;
    synthPanel.setTrack (Project::isMidiTrack (track) ? track : juce::ValueTree());

    if (clip.isValid() && clip.getParent() != track)
    {
        clip = {};
        pianoRoll.setClip ({});
    }

    updateVisibility();
}

void DetailView::setTab (Tab newTab)
{
    tab = newTab;
    updateVisibility();
}

void DetailView::updateVisibility()
{
    // Forget a clip or track that was deleted (or replaced by loading a file).
    if (clip.isValid() && ! clip.getParent().isValid())
    {
        clip = {};
        pianoRoll.setClip ({});
    }

    if (track.isValid() && project.indexOfTrack (track) < 0)
    {
        track = {};
        synthPanel.setTrack ({});
    }

    clipTab.setToggleState (tab == Tab::clip, juce::dontSendNotification);
    deviceTab.setToggleState (tab == Tab::device, juce::dontSendNotification);
    pianoRoll.setVisible (tab == Tab::clip && Project::isMidiClip (clip));
    synthPanel.setVisible (tab == Tab::device && Project::isMidiTrack (track));
    repaint();
}

void DetailView::refresh()
{
    if ((clip.isValid() && ! clip.getParent().isValid()) || (track.isValid() && project.indexOfTrack (track) < 0))
        updateVisibility();

    if (pianoRoll.isVisible())
        pianoRoll.refresh();

    if (synthPanel.isVisible())
        synthPanel.refresh();
}

void DetailView::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);

    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (colours::panelRaised);
    g.fillRect (header);
    g.setColour (colours::outline);
    g.drawHorizontalLine (0, 0.0f, (float) getWidth());
    g.drawHorizontalLine (headerHeight - 1, 0.0f, (float) getWidth());

    if (track.isValid())
    {
        g.setColour (Project::getTrackColour (track));
        g.fillRect (header.getX() + 140, header.getY() + 7, 12, 12);
    }

    juce::String title;
    if (tab == Tab::clip && clip.isValid())
        title = Project::getClipName (clip) + "  -  " + track[ids::name].toString();
    else if (track.isValid())
        title = track[ids::name].toString() + (Project::isMidiTrack (track) ? "  -  Synth" : "");

    g.setColour (colours::text);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText (title, header.withTrimmedLeft (160).reduced (4, 0), juce::Justification::centredLeft);

    // Empty states
    const auto body = getLocalBounds().withTrimmedTop (headerHeight);
    juce::String hint;

    if (tab == Tab::clip && ! Project::isMidiClip (clip))
        hint = clip.isValid() ? "Audio clip editing is not available yet."
                              : "Double-click a MIDI clip to edit its notes. Double-click an empty spot on a MIDI track to create one.";
    else if (tab == Tab::device && ! Project::isMidiTrack (track))
        hint = track.isValid() ? "Audio tracks have no instrument." : "Select a MIDI track to see its instrument.";

    if (hint.isNotEmpty())
    {
        g.setColour (colours::textDim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (hint, body, juce::Justification::centred);
    }
}

void DetailView::resized()
{
    auto area = getLocalBounds();
    auto header = area.removeFromTop (headerHeight).reduced (4, 3);
    clipTab.setBounds (header.removeFromLeft (60));
    header.removeFromLeft (4);
    deviceTab.setBounds (header.removeFromLeft (64));

    pianoRoll.setBounds (area);
    synthPanel.setBounds (area);
}

} // namespace daw

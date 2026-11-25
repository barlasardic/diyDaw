#pragma once

#include "ClipPainter.h"
#include "Project.h"
#include "TimeRuler.h"
#include "TimelineGeometry.h"
#include "TrackHeader.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace daw
{

/** Ableton-style arrangement: ruler on top, one lane per track (header on the left,
    clips on the right), playhead line across all lanes.

    See AGENTS.md "Arrangement view" for the interaction spec.

    Message thread only. All edits go through Project (never touch the ValueTree directly),
    with Project::beginTransaction() once per user gesture. */
class ArrangementView : public juce::Component,
                        public juce::FileDragAndDropTarget,
                        private juce::ValueTree::Listener
{
public:
    explicit ArrangementView (Project& project);
    ~ArrangementView() override;

    /** Called ~30 Hz by the owner: playhead, follow mode, header meters. */
    void refresh();

    /** Message-thread layout state. The callback fires once after a width drag. */
    int getHeaderWidth() const { return headerWidth; }
    void setHeaderWidth (int);
    std::function<void (int)> onHeaderWidthChanged;

    /** Snapshot helper: expand tracks containing automation lanes (UI state only). */
    void showAutomationForTracksWithAutomation();

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    /** Synchronously adds an audio track (used when files are dropped below the last lane or
        onto a MIDI track). Set by the owner. */
    std::function<void()> onAddTrackRequested;

    /** A clip was clicked / double-clicked (double-click on an empty MIDI lane spot creates a
        clip first), and a lane was clicked (its track). */
    std::function<void (juce::ValueTree clip)> onClipSelected, onClipDoubleClicked;
    std::function<void (juce::ValueTree track)> onTrackSelected;

private:
    class Lanes;
    struct ImportBatch;

    void geometryChanged();
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void scrollOrZoom (const juce::MouseEvent&, const juce::MouseWheelDetails&, bool overLanes);
    void finishImport (const ImportBatch&);
    void modelChanged (bool tracksChanged);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override;

    Project& project;
    TimelineGeometry geometry;
    juce::ValueTree state;
    int headerWidth = TrackHeader::preferredWidth;
    TimeRuler ruler;
    juce::Viewport viewport;
    std::unique_ptr<Lanes> lanes;
    std::unique_ptr<juce::Component> headerResizeHandle;
    double playheadBeat = 0.0;
    double lastBpm = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangementView)
};

} // namespace daw

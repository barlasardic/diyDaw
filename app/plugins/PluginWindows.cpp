#include "PluginWindows.h"
#include "DawLookAndFeel.h"
#include <algorithm>

namespace daw
{

struct PluginWindows::Window : juce::DocumentWindow, private juce::ValueTree::Listener
{
    Window (juce::ValueTree d, juce::AudioProcessorEditor& e)
        : DocumentWindow ({}, colours::panel, DocumentWindow::closeButton), device (d), owner (d.getParent()), editor (&e)
    {
        owner.addListener (this);
        setUsingNativeTitleBar (true);
        setResizable (e.isResizable(), false);
        if (e.isResizable())
            setConstrainer (e.getConstrainer());
        setContentNonOwned (&e, true);
        // Offscreen tooling has no monitor or native plugin parent to attach to.
        if (juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr)
        {
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }
    }

    ~Window() override
    {
        owner.removeListener (this);
        closeEditor();
    }

    void closeEditor()
    {
        // A SafePointer also handles an editor that has deleted itself.
        setConstrainer (nullptr);
        clearContentComponent();
        if (editor != nullptr)
        {
            editor->processor.editorBeingDeleted (editor.getComponent());
            delete editor.getComponent();
        }
    }

    void closeButtonPressed() override
    {
        closing = true;
        setVisible (false);
        closeEditor();
    }

    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        // A listener on the owning track runs before the Project's root listener can
        // retire the processor, including during undo and instrument replacement.
        if (parent == owner && child == device)
            closeButtonPressed();
    }

    juce::ValueTree device, owner;
    juce::Component::SafePointer<juce::AudioProcessorEditor> editor;
    bool closing = false;
};

PluginWindows::PluginWindows (Project& p) : project (p) {}
PluginWindows::~PluginWindows() { closeAll(); }
void PluginWindows::closeAll() { windows.clear(); }

void PluginWindows::prune()
{
    windows.erase (std::remove_if (windows.begin(), windows.end(), [this] (const auto& w)
    {
        const auto parent = w->device.getParent();
        return w->closing || w->editor == nullptr || ! parent.isValid()
            || (parent != project.getMaster() && project.indexOfTrack (parent) < 0)
            || project.getPluginInstance (w->device) != &w->editor->processor;
    }), windows.end());
    for (auto& w : windows)
        w->setName (Project::getDeviceDisplayName (w->device) + " - "
                    + (Project::isMaster (w->device.getParent()) ? juce::String ("Master")
                                                               : w->device.getParent()[ids::name].toString()));
}

void PluginWindows::open (juce::ValueTree device)
{
    prune();
    for (auto& w : windows)
        if (w->device == device)
        {
            w->toFront (true);
            return;
        }
    if (auto* plugin = project.getPluginInstance (device))
        if (plugin->hasEditor())
            if (auto* editor = plugin->createEditorAndMakeActive())
            {
                windows.push_back (std::make_unique<Window> (device, *editor));
                prune();
            }
}

} // namespace daw

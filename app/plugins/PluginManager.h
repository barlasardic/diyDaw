#pragma once

#include "PluginHost.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace daw
{

/** Searchable plugin catalogue and incremental scan controls. Message thread only. */
class PluginManager : public juce::Component,
                      private juce::TableListBoxModel,
                      private juce::Timer
{
public:
    PluginManager (PluginHost&, const juce::FileSearchPath& folders = {});
    ~PluginManager() override;

    std::function<void (const juce::FileSearchPath&)> onFoldersChanged;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int, int, int, bool) override;
    void paintCell (juce::Graphics&, int, int, int, int, bool) override;
    void timerCallback() override;
    void updateCatalogue();
    void saveFolders();

    PluginHost& host;
    juce::Array<juce::PluginDescription> visiblePlugins;
    juce::TextEditor search, failures;
    juce::TableListBox table { "Plugins", this };
    juce::TextButton scan { "Scan" }, rescan { "Rescan all" }, foldersButton { "Folders..." };
    juce::Label heading, subtitle, status, failedHeading, folderHeading;
    double progress = 0.0;
    juce::ProgressBar progressBar { progress };
    juce::FileSearchPathListComponent folders;
    juce::String savedFolders;
    bool showingFolders = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManager)
};

} // namespace daw

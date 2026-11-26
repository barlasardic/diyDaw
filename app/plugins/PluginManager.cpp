#include "PluginManager.h"
#include "DawLookAndFeel.h"
#include <algorithm>

namespace daw
{

PluginManager::PluginManager (PluginHost& h, const juce::FileSearchPath& paths) : host (h)
{
    heading.setText ("Plugin Manager", juce::dontSendNotification);
    heading.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    subtitle.setText ("VST3 / LV2  |  Instruments and effects", juce::dontSendNotification);
    subtitle.setColour (juce::Label::textColourId, colours::textDim);
    search.setTextToShowWhenEmpty ("Search name, vendor, format or category", colours::textDim);
    search.onTextChange = [this] { updateCatalogue(); };
    search.setEscapeAndReturnKeysConsumed (true);
    table.getHeader().addColumn ("Name", 1, 240, 100);
    table.getHeader().addColumn ("Vendor", 2, 160, 80);
    table.getHeader().addColumn ("Format", 3, 85, 60);
    table.getHeader().addColumn ("Type", 4, 110, 80);
    table.getHeader().addColumn ("Category", 5, 140, 80);
    table.getHeader().setStretchToFitActive (true);
    table.getHeader().setColour (juce::TableHeaderComponent::backgroundColourId, colours::panelRaised);
    table.getHeader().setColour (juce::TableHeaderComponent::textColourId, colours::text);
    table.getHeader().setColour (juce::TableHeaderComponent::outlineColourId, colours::outline);
    table.setRowHeight (30);
    table.setColour (juce::ListBox::backgroundColourId, colours::background);
    scan.setTooltip ("Scan standard plugin locations and extra folders for new files");
    rescan.setTooltip ("Scan all files again, including known plugins");
    scan.onClick = [this] { saveFolders(); host.startScan(); timerCallback(); };
    rescan.onClick = [this] { saveFolders(); host.startScan (true); timerCallback(); };
    progressBar.setColour (juce::ProgressBar::backgroundColourId, colours::outline);
    progressBar.setColour (juce::ProgressBar::foregroundColourId, colours::accent);
    folders.setPath (paths);
    savedFolders = paths.toString();
    folderHeading.setText ("Extra search folders (in addition to standard plugin locations)", juce::dontSendNotification);
    foldersButton.onClick = [this]
    {
        showingFolders = ! showingFolders;
        resized();
    };
    failures.setMultiLine (true);
    failures.setReadOnly (true);
    failures.setScrollbarsShown (true);
    failures.setColour (juce::TextEditor::textColourId, colours::textDim);
    failedHeading.setText ("Files that failed to load / blacklisted", juce::dontSendNotification);
    for (auto* c : std::initializer_list<juce::Component*> { &heading, &subtitle, &search, &table, &scan,
             &rescan, &foldersButton, &status, &progressBar, &failedHeading, &failures })
        addAndMakeVisible (c);
    addChildComponent (folders);
    addChildComponent (folderHeading);
    setSize (800, 580);
    timerCallback();
    startTimerHz (10);
}

PluginManager::~PluginManager()
{
    stopTimer();
    saveFolders();
    table.setModel (nullptr);
}

void PluginManager::updateCatalogue()
{
    visiblePlugins.clear();
    const auto query = search.getText().trim();
    for (const auto& d : host.getKnownPlugins().getTypes())
        if ((d.name + " " + d.manufacturerName + " " + d.pluginFormatName + " "
             + d.category + (d.isInstrument ? " Instrument" : " Effect")).containsIgnoreCase (query))
            visiblePlugins.add (d);
    std::sort (visiblePlugins.begin(), visiblePlugins.end(), [] (const auto& a, const auto& b)
    {
        return a.name.compareIgnoreCase (b.name) < 0;
    });
    table.updateContent();
    table.repaint();
}

void PluginManager::saveFolders()
{
    if (savedFolders != folders.getPath().toString())
    {
        savedFolders = folders.getPath().toString();
        host.setExtraSearchPaths (folders.getPath());
        if (onFoldersChanged)
            onFoldersChanged (folders.getPath());
    }
}

void PluginManager::timerCallback()
{
    saveFolders();
    const auto scanning = host.isScanning();
    scan.setEnabled (! scanning);
    rescan.setEnabled (! scanning);
    foldersButton.setEnabled (! scanning);
    folders.setEnabled (! scanning);
    progress = scanning ? (double) host.getScanProgress() : 1.0;
    progressBar.setVisible (scanning);
    status.setText (scanning ? host.getScanStatus()
                            : (host.getKnownPlugins().getNumTypes() == 0
                                   ? "No plugins yet. Press Scan to build your catalogue."
                                   : juce::String (host.getKnownPlugins().getNumTypes()) + " plugins available"),
                    juce::dontSendNotification);
    auto failed = host.getFailedFiles();
    failed.addArray (host.getKnownPlugins().getBlacklistedFiles());
    failed.removeDuplicates (false);
    const auto failedText = failed.isEmpty() ? "None" : failed.joinIntoString ("\n");
    if (failures.getText() != failedText)
        failures.setText (failedText, false);
    updateCatalogue();
}

int PluginManager::getNumRows() { return visiblePlugins.size(); }

void PluginManager::paintRowBackground (juce::Graphics& g, int row, int width, int height, bool selected)
{
    g.fillAll (selected ? colours::accent.withAlpha (0.2f)
                       : (row % 2 == 0 ? colours::background : colours::panel));
    g.setColour (colours::outline.withAlpha (0.4f));
    g.drawHorizontalLine (height - 1, 0.0f, (float) width);
}

void PluginManager::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    if (! juce::isPositiveAndBelow (row, visiblePlugins.size()))
        return;
    const auto& d = visiblePlugins.getReference (row);
    juce::String text;
    switch (column)
    {
        case 1: text = d.name; break;
        case 2: text = d.manufacturerName; break;
        case 3: text = d.pluginFormatName; break;
        case 4: text = d.isInstrument ? "Instrument" : "Effect"; break;
        case 5: text = d.category; break;
        default: break;
    }
    g.setColour (column == 1 ? colours::text : colours::textDim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (text, 10, 0, width - 16, height, juce::Justification::centredLeft, true);
}

void PluginManager::paint (juce::Graphics& g) { g.fillAll (colours::panel); }

void PluginManager::resized()
{
    auto area = getLocalBounds().reduced (20);
    heading.setBounds (area.removeFromTop (30));
    subtitle.setBounds (area.removeFromTop (25));
    area.removeFromTop (12);
    auto controls = area.removeFromTop (30);
    foldersButton.setBounds (controls.removeFromRight (100));
    controls.removeFromRight (8);
    rescan.setBounds (controls.removeFromRight (100));
    controls.removeFromRight (8);
    scan.setBounds (controls.removeFromRight (72));
    controls.removeFromRight (12);
    search.setBounds (controls);
    area.removeFromTop (12);
    auto footer = area.removeFromBottom (130);
    status.setBounds (footer.removeFromTop (28));
    progressBar.setBounds (footer.removeFromTop (18));
    footer.removeFromTop (8);
    failedHeading.setBounds (footer.removeFromTop (22));
    failures.setBounds (footer);
    folders.setVisible (showingFolders);
    folderHeading.setVisible (showingFolders);
    if (showingFolders)
    {
        auto folderArea = area.removeFromBottom (135);
        folderHeading.setBounds (folderArea.removeFromTop (25));
        folders.setBounds (folderArea.reduced (0, 4));
    }
    table.setBounds (area);
}

} // namespace daw

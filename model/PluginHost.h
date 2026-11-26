#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>

namespace daw
{

/** Finds and loads VST3 / LV2 plugins.

    - The format manager is filled by the owner: the app adds the formats *with* editors
      (juce::addDefaultFormatsToManager), tests the headless ones.
    - Scanning runs on the message thread, one plugin file per timer tick, so the UI stays
      responsive. A plugin that crashes the scan is recorded in the dead-man's-pedal file and
      blacklisted on the next start.
    - The KnownPluginList is the catalogue the UI shows; the owner persists it
      (createKnownListXml / restoreKnownList).

    Message thread only. */
class PluginHost : private juce::Timer
{
public:
    using FormatSetup = std::function<void (juce::AudioPluginFormatManager&)>;

    explicit PluginHost (FormatSetup addFormats);
    ~PluginHost() override;

    juce::AudioPluginFormatManager& getFormatManager() noexcept { return formats; }
    juce::KnownPluginList& getKnownPlugins() noexcept { return knownPlugins; }

    /** Folders searched in addition to each format's standard locations. */
    void setExtraSearchPaths (const juce::FileSearchPath& paths) { extraPaths = paths; }
    juce::FileSearchPath getSearchPath (juce::AudioPluginFormat& format) const;

    void setDeadMansPedalFile (const juce::File& file) { deadMansPedal = file; }

    // ---- scanning -----------------------------------------------------------------
    /** Starts an incremental scan of every format (already known files are skipped unless
        'rescanAll'). */
    void startScan (bool rescanAll = false);
    void stopScan();
    bool isScanning() const noexcept { return scanning; }
    float getScanProgress() const noexcept;           // 0..1 over all formats
    juce::String getScanStatus() const { return status; }
    juce::StringArray getFailedFiles() const { return failedFiles; }

    /** Scans everything right now (blocks; for tests and the command line). */
    void scanNow (bool rescanAll = false);

    std::function<void()> onScanFinished;

    // ---- catalogue ------------------------------------------------------------------
    juce::Array<juce::PluginDescription> getInstruments() const;
    juce::Array<juce::PluginDescription> getEffects() const;
    bool findDescription (const juce::String& identifierString, juce::PluginDescription& result) const;

    std::unique_ptr<juce::XmlElement> createKnownListXml() const { return knownPlugins.createXml(); }
    void restoreKnownList (const juce::XmlElement& xml) { knownPlugins.recreateFromXml (xml); }

    // ---- instances --------------------------------------------------------------------
    /** Creates a plugin synchronously (some plugins take a moment). Returns nullptr and sets
        'error' on failure. */
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const juce::PluginDescription& description,
                                                               double sampleRate, int blockSize,
                                                               juce::String& error) const;

private:
    void timerCallback() override;
    bool scanStep();
    void finishScan();

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList knownPlugins;
    juce::FileSearchPath extraPaths;
    juce::File deadMansPedal;

    // scan state
    bool scanning = false, rescan = false;
    int formatIndex = 0;
    std::unique_ptr<juce::PluginDirectoryScanner> scanner;
    juce::String status;
    juce::StringArray failedFiles;

    JUCE_DECLARE_NON_COPYABLE (PluginHost)
};

} // namespace daw

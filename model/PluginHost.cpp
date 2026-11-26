#include "PluginHost.h"

namespace daw
{

PluginHost::PluginHost (FormatSetup addFormats)
{
    if (addFormats)
        addFormats (formats);
}

PluginHost::~PluginHost()
{
    stopTimer();
}

juce::FileSearchPath PluginHost::getSearchPath (juce::AudioPluginFormat& format) const
{
    auto path = format.getDefaultLocationsToSearch();

    for (int i = 0; i < extraPaths.getNumPaths(); ++i)
        path.addIfNotAlreadyThere (extraPaths[i]);

    path.removeNonExistentPaths();
    return path;
}

void PluginHost::startScan (bool rescanAll)
{
    if (scanning)
        return;

    if (deadMansPedal != juce::File())
        juce::PluginDirectoryScanner::applyBlacklistingsFromDeadMansPedal (knownPlugins, deadMansPedal);

    scanning = true;
    rescan = rescanAll;
    formatIndex = -1;
    scanner.reset();
    failedFiles.clear();
    status = "Scanning...";
    startTimer (1);
}

void PluginHost::stopScan()
{
    if (scanning)
        finishScan();
}

float PluginHost::getScanProgress() const noexcept
{
    const auto numFormats = juce::jmax (1, formats.getNumFormats());
    const auto within = scanner != nullptr ? scanner->getProgress() : 0.0f;
    return juce::jlimit (0.0f, 1.0f, ((float) juce::jmax (0, formatIndex) + within) / (float) numFormats);
}

bool PluginHost::scanStep()
{
    // Move to the next format when the current one is done.
    while (scanner == nullptr)
    {
        if (++formatIndex >= formats.getNumFormats())
            return false;

        auto* format = formats.getFormat (formatIndex);
        scanner = std::make_unique<juce::PluginDirectoryScanner> (knownPlugins, *format, getSearchPath (*format),
                                                                  true, deadMansPedal, false);
    }

    juce::String name;
    status = "Scanning " + scanner->getNextPluginFileThatWillBeScanned();

    if (! scanner->scanNextFile (! rescan, name))
    {
        failedFiles.addArray (scanner->getFailedFiles());
        scanner.reset();
    }

    return true;
}

void PluginHost::timerCallback()
{
    if (! scanStep())
        finishScan();
}

void PluginHost::finishScan()
{
    stopTimer();

    if (scanner != nullptr)
        failedFiles.addArray (scanner->getFailedFiles());

    scanner.reset();
    scanning = false;
    status = juce::String (knownPlugins.getNumTypes()) + " plugins";

    if (onScanFinished)
        onScanFinished();
}

void PluginHost::scanNow (bool rescanAll)
{
    startScan (rescanAll);
    stopTimer();

    while (scanStep()) {}

    finishScan();
}

juce::Array<juce::PluginDescription> PluginHost::getInstruments() const
{
    juce::Array<juce::PluginDescription> result;
    for (const auto& d : knownPlugins.getTypes())
        if (d.isInstrument)
            result.add (d);
    return result;
}

juce::Array<juce::PluginDescription> PluginHost::getEffects() const
{
    juce::Array<juce::PluginDescription> result;
    for (const auto& d : knownPlugins.getTypes())
        if (! d.isInstrument)
            result.add (d);
    return result;
}

bool PluginHost::findDescription (const juce::String& identifierString, juce::PluginDescription& result) const
{
    if (auto d = knownPlugins.getTypeForIdentifierString (identifierString))
    {
        result = *d;
        return true;
    }

    return false;
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance (const juce::PluginDescription& description,
                                                                       double sampleRate, int blockSize,
                                                                       juce::String& error) const
{
    return formats.createPluginInstance (description, sampleRate, blockSize, error);
}

} // namespace daw

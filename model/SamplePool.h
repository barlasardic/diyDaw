#pragma once

#include "SampleData.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace daw
{

/** Cache of decoded samples, keyed by absolute file path.
    Decoding happens on background threads so the UI never blocks on disk I/O.

    Message thread only (callbacks are also delivered on the message thread).
*/
class SamplePool
{
public:
    using Ptr = std::shared_ptr<const SampleData>;
    using Callback = std::function<void (Ptr)>;

    SamplePool();
    ~SamplePool();

    static juce::String keyFor (const juce::File& file) { return file.getFullPathName(); }

    /** The decoded sample, or nullptr if it isn't loaded (yet). */
    Ptr get (const juce::String& key) const;
    Ptr get (const juce::File& file) const { return get (keyFor (file)); }

    /** Adds an already decoded sample (e.g. generated in memory, or in tests). */
    void insert (const juce::String& key, Ptr sample);

    /** Decodes 'file' in the background, then calls onLoaded on the message thread with the
        result (nullptr on failure). Calls back asynchronously even if it is already cached.
        Concurrent requests for the same file share one decode. */
    void loadAsync (const juce::File& file, Callback onLoaded);

    bool isLoading (const juce::File& file) const { return pending.count (keyFor (file)) > 0; }

    juce::AudioFormatManager& getFormats() noexcept { return formats; }

    /** Called on the message thread whenever a new sample becomes available. */
    std::function<void (const juce::String& key)> onSampleAdded;

private:
    void finishLoad (const juce::String& key, Ptr sample);

    juce::AudioFormatManager formats;
    std::map<juce::String, Ptr> samples;
    std::map<juce::String, std::vector<Callback>> pending;
    juce::ThreadPool decoders { juce::ThreadPoolOptions{}.withNumberOfThreads (2) };

    // Lets background jobs know whether this pool still exists when they finish.
    std::shared_ptr<SamplePool*> alive = std::make_shared<SamplePool*> (this);

    JUCE_DECLARE_NON_COPYABLE (SamplePool)
};

} // namespace daw

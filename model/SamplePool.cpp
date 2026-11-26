#include "SamplePool.h"
#include "BuiltinSamples.h"

namespace daw
{

SamplePool::SamplePool()
{
    formats.registerBasicFormats();
}

SamplePool::~SamplePool()
{
    alive.reset();                        // jobs that finish from now on drop their result
    decoders.removeAllJobs (true, 10000); // and wait for running decodes to finish
}

SamplePool::Ptr SamplePool::get (const juce::String& key) const
{
    if (const auto it = samples.find (key); it != samples.end())
        return it->second;

    if (isBuiltinSampleKey (key))
        if (auto sample = createBuiltinSample (builtinSampleName (key)))
            return samples[key] = std::move (sample);

    return nullptr;
}

void SamplePool::insert (const juce::String& key, Ptr sample)
{
    if (sample == nullptr)
        return;

    samples[key] = std::move (sample);

    if (onSampleAdded)
        onSampleAdded (key);
}

void SamplePool::loadAsync (const juce::File& file, Callback onLoaded)
{
    const auto key = keyFor (file);

    if (auto cached = get (key))
    {
        juce::MessageManager::callAsync ([cb = std::move (onLoaded), cached] { if (cb) cb (cached); });
        return;
    }

    auto& waiting = pending[key];
    waiting.push_back (std::move (onLoaded));

    if (waiting.size() > 1)
        return; // a decode for this file is already running

    std::weak_ptr<SamplePool*> weak = alive;

    decoders.addJob ([weak, file, key, fmts = &formats]
    {
        Ptr result = loadSample (file, *fmts);

        juce::MessageManager::callAsync ([weak, key, result]
        {
            if (auto self = weak.lock())
                (*self)->finishLoad (key, result);
        });
    });
}

void SamplePool::finishLoad (const juce::String& key, Ptr sample)
{
    auto callbacks = std::move (pending[key]);
    pending.erase (key);

    insert (key, sample);

    if (sample == nullptr && onLoadFailed)
        onLoadFailed (key);

    for (auto& cb : callbacks)
        if (cb)
            cb (sample);
}

} // namespace daw

#include "DrumRack.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace daw
{

const std::vector<ParamInfo>& DrumRack::getParamInfos()
{
    // The strings and their c_str() addresses live as long as the descriptors.
    static const auto ids = []
    {
        std::array<std::string, numParams> result;
        const char* suffixes[] { "Gain", "Pan", "Tune", "Decay", "Choke" };
        for (int pad = 0; pad < numPads; ++pad)
            for (int p = 0; p < numPadParams; ++p)
                result[static_cast<size_t> (pad * numPadParams + p)] =
                    "pad" + std::to_string (pad + 1) + suffixes[p];
        return result;
    }();
    static const auto infos = []
    {
        const ParamInfo templates[] {
            { nullptr, "Gain", -60, 6, 0, -12, false, "dB" },
            { nullptr, "Pan", -1, 1, 0, 0, false, "" },
            { nullptr, "Tune", -24, 24, 0, 0, true, "st" },
            { nullptr, "Decay", 0.01f, 10, 10, 0.5f, false, "s" },
            { nullptr, "Choke", 0, 8, 0, 0, true, "" }
        };
        std::vector<ParamInfo> result;
        result.reserve (numParams);
        for (int i = 0; i < numParams; ++i)
        {
            auto info = templates[i % numPadParams];
            info.id = ids[static_cast<size_t> (i)].c_str();
            result.push_back (info);
        }
        return result;
    }();
    return infos;
}

/** Render state belongs to the audio thread; buffers are sized only by prepare().
    Each sound's ring contains the NEXT 5 ms of unpanned audio, with no output latency.
    Before acquire(), the old kit still owns every source we read. After acquire(), we
    only compare raw pointers against the NEW kit, never dereferencing a missing source:
    detach it and play the cached continuation with a 5 ms fade. collectGarbage() may
    therefore delete the old kit concurrently, even before acquire() returns. Shared
    samples may continue because the new active kit owns them. No shared_ptr operations
    (including last-owner destruction) occur on the audio thread. */
struct DrumRack::Impl
{
    struct Frame
    {
        float left = 0, right = 0;
    };

    struct Sound
    {
        std::vector<Frame> future;
        const SampleData* sample = nullptr;
        double position = 0, step = 1, decayFrames = 0, generated = 0;
        double remaining = 0;
        int cursor = 0, pad = 0, group = 0, release = -1, attack = 0;
        float velocity = 0;
        std::uint64_t age = 0;

        bool active() const noexcept { return remaining > 0; }

        void clear() noexcept
        {
            sample = nullptr;
            remaining = 0;
            release = -1;
        }

        Frame generate() noexcept
        {
            if (sample == nullptr)
                return {};
            const auto frames = sample->getNumFrames();
            if (position >= frames || (decayFrames > 0 && generated >= decayFrames))
            {
                sample = nullptr;
                return {};
            }
            const auto index = static_cast<int> (position);
            const auto fraction = static_cast<float> (position - index);
            const auto envelope = decayFrames > 0 ? std::pow (std::max (0.0, 1.0 - generated / decayFrames), 3.0) : 1.0;
            const auto read = [&] (int channel)
            {
                const auto* data = sample->buffer.getReadPointer (channel);
                const auto next = index + 1 < frames ? data[index + 1] : 0.0f;
                return (data[index] + fraction * (next - data[index])) * static_cast<float> (envelope);
            };
            Frame result { read (0), read (sample->buffer.getNumChannels() > 1 ? 1 : 0) };
            position += step;
            generated += 1;
            return result;
        }

        void start (const SampleData* source, int padIndex, int chokeGroup, float strength,
                    double rate, float semitones, float seconds, std::uint64_t order, bool stolen) noexcept
        {
            sample = source;
            position = generated = 0;
            step = source->sampleRate / rate * std::exp2 (semitones / 12.0);
            decayFrames = seconds < 10.0f ? static_cast<double> (seconds) * rate : 0;
            remaining = std::ceil (source->getNumFrames() / step);
            if (decayFrames > 0)
                remaining = std::min (remaining, std::ceil (decayFrames));
            cursor = 0;
            pad = padIndex;
            group = chokeGroup;
            velocity = strength;
            age = order;
            release = -1;
            attack = stolen ? static_cast<int> (future.size()) : 0;
            // Tune and decay are captured per hit; gain and pan remain live and smoothed.
            for (auto& frame : future)
                frame = generate();
        }

        void fade() noexcept
        {
            if (active() && release < 0)
                release = static_cast<int> (future.size());
        }

        void validate (const Kit* current) noexcept
        {
            if (sample == nullptr)
                return;
            if (current != nullptr)
                for (const auto& candidate : current->samples)
                    if (candidate.get() == sample)
                        return;
            sample = nullptr; // Only cached audio may be used after this point.
            fade();
        }

        Frame tick() noexcept
        {
            if (! active())
                return {};
            auto result = future[static_cast<size_t> (cursor)];
            future[static_cast<size_t> (cursor)] = generate();
            cursor = (cursor + 1) % static_cast<int> (future.size());
            auto level = velocity;
            if (attack > 0)
                level *= 1.0f - static_cast<float> (attack--) / static_cast<float> (future.size());
            if (release >= 0)
            {
                level *= static_cast<float> (release) / static_cast<float> (future.size());
                if (--release == 0)
                    clear();
            }
            remaining = std::max (0.0, remaining - 1);
            if (! active())
                sample = nullptr;
            return { result.left * level, result.right * level };
        }
    };

    struct Voice
    {
        Sound sound, tail;
        Frame residual, lastTail;
        int residualRemaining = 0;
    };

    std::array<Voice, numVoices> voices;
    std::array<juce::SmoothedValue<float>, numPads> gains, pans;
    double sampleRate = 44100;
    int fadeSamples = 221;
    std::uint64_t order = 0;
    std::atomic<int> activeVoices { 0 };

    void event (const NoteEvent& e, const Kit* current,
                const std::array<float, numParams>& values) noexcept
    {
        if (! e.isNoteOn || ! std::isfinite (e.velocity) || e.velocity <= 0
            || e.pitch < firstNote || e.pitch >= firstNote + numPads || current == nullptr)
            return;
        const auto pad = e.pitch - firstNote;
        const auto* source = current->samples[static_cast<size_t> (pad)].get();
        if (source == nullptr || source->getNumFrames() == 0 || source->buffer.getNumChannels() == 0
            || ! std::isfinite (source->sampleRate) || source->sampleRate <= 0)
            return;
        const auto base = pad * numPadParams;
        const auto group = static_cast<int> (values[static_cast<size_t> (base + choke)]);
        Voice* chosen = nullptr;
        for (auto& v : voices)
        {
            // Same-pad retriggers ring; only OTHER pads in the group are choked.
            for (auto* sound : { &v.sound, &v.tail })
                if (group != 0 && sound->pad != pad && sound->group == group)
                    sound->fade();
            if (! v.sound.active() && chosen == nullptr)
                chosen = &v;
        }
        if (chosen == nullptr)
            chosen = &*std::min_element (voices.begin(), voices.end(), [] (const auto& a, const auto& b)
            {
                return a.sound.age < b.sound.age;
            });
        const bool stolen = chosen->sound.active();
        if (stolen)
        {
            // Retain the old sound without copying/allocating its ring. Repeated steals
            // fold the displaced tail into a bounded-storage, decaying residual.
            chosen->residual = chosen->lastTail;
            chosen->residualRemaining = fadeSamples;
            std::swap (chosen->sound, chosen->tail);
            chosen->tail.fade();
        }
        chosen->sound.start (source, pad, group, std::clamp (e.velocity, 0.0f, 1.0f), sampleRate,
                             values[static_cast<size_t> (base + tune)], values[static_cast<size_t> (base + decay)],
                             ++order, stolen);
    }
};

DrumRack::DrumRack() : impl (std::make_unique<Impl>())
{
    for (int i = 0; i < numParams; ++i)
        params[static_cast<size_t> (i)].store (getParamInfos()[static_cast<size_t> (i)].defaultValue);
    prepare (44100, 512);
}

DrumRack::~DrumRack() = default;
void DrumRack::setKit (std::unique_ptr<Kit> k) { kit.publish (std::move (k)); }
void DrumRack::collectGarbage() { kit.collectGarbage(); }

void DrumRack::setParam (int index, float value) noexcept
{
    if (index < 0 || index >= numParams || std::isnan (value))
        return;
    const auto& info = getParamInfos()[static_cast<size_t> (index)];
    value = std::clamp (value, info.minValue, info.maxValue);
    params[static_cast<size_t> (index)].store (info.stepped ? std::round (value) : value, std::memory_order_relaxed);
}

float DrumRack::getParam (int index) const noexcept
{
    return index >= 0 && index < numParams ? params[static_cast<size_t> (index)].load (std::memory_order_relaxed) : 0;
}

void DrumRack::prepare (double sampleRate, int maxBlockSize)
{
    juce::ignoreUnused (maxBlockSize);
    impl->sampleRate = std::isfinite (sampleRate) && sampleRate >= 1000 ? sampleRate : 44100;
    impl->fadeSamples = std::max (1, static_cast<int> (std::ceil (impl->sampleRate * 0.005)));
    for (auto& v : impl->voices)
        for (auto* sound : { &v.sound, &v.tail })
            sound->future.resize (static_cast<size_t> (impl->fadeSamples));
    for (int pad = 0; pad < numPads; ++pad)
    {
        auto& g = impl->gains[static_cast<size_t> (pad)];
        auto& p = impl->pans[static_cast<size_t> (pad)];
        g.reset (impl->sampleRate, 0.005);
        p.reset (impl->sampleRate, 0.005);
        g.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (getParam (paramIndex (pad, gain))));
        p.setCurrentAndTargetValue (getParam (paramIndex (pad, pan)));
    }
    reset();
}

void DrumRack::process (juce::AudioBuffer<float>& out, int numSamples,
                       const NoteEvent* events, int numEvents) noexcept
{
    const auto* current = kit.acquire(); // Exactly once, including silent blocks.
    for (auto& v : impl->voices)
    {
        v.sound.validate (current);
        v.tail.validate (current);
    }
    std::array<float, numParams> values;
    for (int i = 0; i < numParams; ++i)
        values[static_cast<size_t> (i)] = getParam (i);
    for (int pad = 0; pad < numPads; ++pad)
    {
        impl->gains[static_cast<size_t> (pad)].setTargetValue (
            juce::Decibels::decibelsToGain (values[static_cast<size_t> (paramIndex (pad, gain))]));
        impl->pans[static_cast<size_t> (pad)].setTargetValue (values[static_cast<size_t> (paramIndex (pad, pan))]);
    }
    int nextEvent = 0;
    const auto count = std::clamp (numSamples, 0, out.getNumSamples());
    for (int i = 0; i < count; ++i)
    {
        while (events != nullptr && nextEvent < numEvents && events[nextEvent].sampleOffset <= i)
            impl->event (events[nextEvent++], current, values);
        std::array<Impl::Frame, numPads> levels;
        for (int pad = 0; pad < numPads; ++pad)
        {
            const auto g = impl->gains[static_cast<size_t> (pad)].getNextValue();
            const auto angle = (impl->pans[static_cast<size_t> (pad)].getNextValue() + 1.0f)
                               * juce::MathConstants<float>::pi / 4.0f;
            levels[static_cast<size_t> (pad)] = { g * std::cos (angle), g * std::sin (angle) };
        }
        Impl::Frame sum;
        for (auto& v : impl->voices)
        {
            const auto mix = [&] (Impl::Sound& sound)
            {
                const auto frame = sound.tick();
                const auto level = levels[static_cast<size_t> (sound.pad)];
                return Impl::Frame { frame.left * level.left, frame.right * level.right };
            };
            const auto main = mix (v.sound);
            auto tail = mix (v.tail);
            if (v.residualRemaining > 0)
            {
                const auto scale = static_cast<float> (v.residualRemaining--) / static_cast<float> (impl->fadeSamples);
                tail.left += v.residual.left * scale;
                tail.right += v.residual.right * scale;
            }
            v.lastTail = tail;
            sum.left += main.left + tail.left;
            sum.right += main.right + tail.right;
        }
        if (out.getNumChannels() > 0)
            out.addSample (0, i, sum.left);
        if (out.getNumChannels() > 1)
            out.addSample (1, i, sum.right);
    }
    int active = 0;
    for (const auto& v : impl->voices)
        if (v.sound.active() || v.tail.active() || v.residualRemaining > 0)
            ++active;
    impl->activeVoices.store (active, std::memory_order_relaxed);
}

void DrumRack::allNotesOff() noexcept
{
    for (auto& v : impl->voices)
    {
        v.sound.fade();
        v.tail.fade();
    }
}

void DrumRack::reset() noexcept
{
    for (auto& v : impl->voices)
    {
        v.sound.clear();
        v.tail.clear();
        v.residual = v.lastTail = {};
        v.residualRemaining = 0;
    }
    impl->order = 0;
    impl->activeVoices.store (0, std::memory_order_relaxed);
}

int DrumRack::getNumActiveVoices() const noexcept
{
    return impl->activeVoices.load (std::memory_order_relaxed);
}

} // namespace daw

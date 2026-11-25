#include "Synth.h"

namespace daw
{

// Stub: to be implemented.
const std::array<Synth::ParamInfo, Synth::numParams>& Synth::getParamInfos()
{
    static const std::array<ParamInfo, numParams> infos {};
    return infos;
}

struct Synth::Impl {};

Synth::Synth() : impl (std::make_unique<Impl>()) {}
Synth::~Synth() = default;
void Synth::setParam (int, float) noexcept {}
float Synth::getParam (int) const noexcept { return 0.0f; }
void Synth::prepare (double, int) {}
void Synth::process (juce::AudioBuffer<float>&, int, const NoteEvent*, int) noexcept {}
void Synth::allNotesOff() noexcept {}
void Synth::reset() noexcept {}
int Synth::getNumActiveVoices() const noexcept { return 0; }

} // namespace daw

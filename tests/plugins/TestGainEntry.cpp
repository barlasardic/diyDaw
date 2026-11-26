#include "TestPlugins.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return createTestGainPlugin(); }

#pragma once

namespace daw
{

/** Static description of one device parameter (instrument or effect). Used by the model to
    store values by id and by the UI to build controls. */
struct ParamInfo
{
    const char* id;          // stable identifier, used in project files
    const char* name;        // display name
    float minValue, maxValue, defaultValue;
    float centreValue;       // value at the middle of a knob's travel (for skewed ranges)
    bool stepped;            // integer steps
    const char* unit;        // "Hz", "s", "ms", "dB", "st", "ct", "%", ":1" or ""
};

} // namespace daw

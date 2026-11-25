#include "Automation.h"

#include <algorithm>

namespace daw
{

float AutomationLane::valueAt (double beat) const noexcept
{
    if (points.empty())
        return 0.0f;

    if (beat <= points.front().beat)
        return points.front().value;

    if (beat >= points.back().beat)
        return points.back().value;

    // First point after 'beat'; the segment is [it - 1, it].
    const auto it = std::upper_bound (points.begin(), points.end(), beat,
                                      [] (double b, const AutomationPoint& p) { return b < p.beat; });
    const auto& a = *(it - 1);
    const auto& b = *it;
    const auto span = b.beat - a.beat;

    if (span <= 0.0)
        return b.value;

    const auto t = (float) ((beat - a.beat) / span);
    return a.value + t * (b.value - a.value);
}

} // namespace daw

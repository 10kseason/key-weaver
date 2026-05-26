#pragma once

#include <keyconv/chart.hpp>

namespace keyconv::adapter {

// Adapter contract for future engine integrations such as Qwilight or TenRiff.
// Do not include engine headers here. Implement adapters in separate targets.
template <typename ExternalChart>
struct ChartAdapter {
    static Chart toKeyconvChart(const ExternalChart& input);
    static ExternalChart fromKeyconvChart(const Chart& converted, const ExternalChart& original);
};

}  // namespace keyconv::adapter


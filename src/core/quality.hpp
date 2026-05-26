#pragma once

#include "core/chart.hpp"
#include "core/report.hpp"

namespace keyconv {

QualityReport computeQualityReport(const Chart& original,
                                   const Chart& converted,
                                   int sourceKeyCount,
                                   int targetKeyCount);

}  // namespace keyconv


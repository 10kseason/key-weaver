#pragma once

#include "core/chart.hpp"
#include "core/report.hpp"

namespace keyconv {

QualityReport computeQualityReport(const Chart& original,
                                   const Chart& converted,
                                   int sourceKeyCount,
                                   int targetKeyCount,
                                   int jackWindowMs = 500);

TargetKProfile targetKProfileFor(int sourceKeyCount, int targetKeyCount);

void finalizeTargetKLikenessReport(QualityReport& report,
                                   const Chart& original,
                                   const Chart& converted,
                                   int sourceKeyCount,
                                   int targetKeyCount,
                                   const TargetKProfile* profileOverride = nullptr);

}  // namespace keyconv

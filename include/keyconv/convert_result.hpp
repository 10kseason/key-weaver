#pragma once

#include <keyconv/chart.hpp>
#include <keyconv/quality_report.hpp>

namespace keyconv {

struct ConvertResult {
    Chart chart;
    ConversionReport report;
};

}  // namespace keyconv


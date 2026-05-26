#pragma once

#include <optional>
#include <string>

#include <keyconv/chart.hpp>

namespace keyconv {

std::string exportOsu(const Chart& chart, std::optional<int> targetKeyCount = std::nullopt);

}  // namespace keyconv


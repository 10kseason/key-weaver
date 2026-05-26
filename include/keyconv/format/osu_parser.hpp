#pragma once

#include <optional>
#include <string>

#include <keyconv/chart.hpp>

namespace keyconv {

struct ParseOptions {
    std::optional<int> sourceKeyCount;
};

Chart parseOsu(const std::string& text, const ParseOptions& options = {});

}  // namespace keyconv


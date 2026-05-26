#pragma once

#include <optional>
#include <string>

#include <keyconv/chart.hpp>
#include <keyconv/format/osu_parser.hpp>

namespace keyconv {

Chart parseBms(const std::string& text, const ParseOptions& options = {});

}  // namespace keyconv

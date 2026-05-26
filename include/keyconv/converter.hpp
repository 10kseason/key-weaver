#pragma once

#include <keyconv/convert_options.hpp>
#include <keyconv/convert_result.hpp>

namespace keyconv {

class Converter {
public:
    ConvertResult convert(const Chart& input, const ConvertOptions& options) const;
};

}  // namespace keyconv


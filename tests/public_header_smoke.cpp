#include <keyconv/adapter/adapter_concepts.hpp>
#include <keyconv/chart.hpp>
#include <keyconv/convert_options.hpp>
#include <keyconv/convert_result.hpp>
#include <keyconv/converter.hpp>
#include <keyconv/format/bms_exporter.hpp>
#include <keyconv/format/bms_parser.hpp>
#include <keyconv/format/osu_exporter.hpp>
#include <keyconv/format/osu_parser.hpp>
#include <keyconv/note.hpp>
#include <keyconv/quality_report.hpp>
#include <keyconv/timing.hpp>

int main() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 4;
    chart.notes.push_back(keyconv::Note{"n0", 1000, 0, keyconv::NoteType::Tap});

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;

    const keyconv::Converter converter;
    const keyconv::ConvertResult result = converter.convert(chart, options);
    if (result.chart.notes.empty()) {
        return 1;
    }

    const auto style = keyconv::parseConversionStyle("training");
    const auto optimizer = keyconv::parseOptimizerKind("beam");
    const auto compressPolicy = keyconv::parseCompressPolicy("no-overlap-hybrid");
    const auto distancePolicy = keyconv::parseDistancePolicy("aimod-safe");
    const auto expansionPolicy = keyconv::parseExpansionPolicy("preserve-tap-plus");
    const auto lowExpansionPolicy = keyconv::parseExpansionPolicy("auto-low");
    const auto echoPolicy = keyconv::parseEchoPolicy("stair-trill");
    const auto streamEchoProfile = keyconv::parseStreamEchoProfile("balanced");
    const auto jackPreservePolicy = keyconv::parseJackPreservePolicy("preserve-playable");
    if (!style.has_value() || !optimizer.has_value() || !compressPolicy.has_value() ||
        !distancePolicy.has_value() || !expansionPolicy.has_value() ||
        !lowExpansionPolicy.has_value() || !echoPolicy.has_value() || !streamEchoProfile.has_value() ||
        !jackPreservePolicy.has_value()) {
        return 2;
    }

    const auto text = keyconv::exportOsu(result.chart, 10);
    const auto reparsed = keyconv::parseOsu(text);
    if (reparsed.notes.empty()) {
        return 3;
    }
    const auto bms = keyconv::exportBms(result.chart, 10);
    const auto parsedBms = keyconv::parseBms(bms);
    if (parsedBms.notes.empty()) {
        return 4;
    }
    return reparsed.meta.version == "KeyWeaver10K" ? 0 : 5;
}

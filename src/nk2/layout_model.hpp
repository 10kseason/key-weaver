#pragma once

#include <string>

namespace keyconv::nk2 {

struct LayoutWeights {
    int panel = 3;
    int bridge = 2;
    int fullField = 6;
};

struct TargetLayoutSummary {
    int targetKeyCount = 0;
    bool hasPanels = false;
    bool hasBridge = false;
    int leftPanelStart = 0;
    int leftPanelEnd = 0;
    int rightPanelStart = 0;
    int rightPanelEnd = 0;
    int bridgeStart = 0;
    int bridgeEnd = 0;
    LayoutWeights weights;
};

TargetLayoutSummary buildTargetLayoutSummary(int targetKeyCount, LayoutWeights weights);
std::string layoutKindName(const TargetLayoutSummary& layout);

}  // namespace keyconv::nk2

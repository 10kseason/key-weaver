#include "nk2/layout_model.hpp"

namespace keyconv::nk2 {

TargetLayoutSummary buildTargetLayoutSummary(int targetKeyCount, LayoutWeights weights) {
    TargetLayoutSummary layout;
    layout.targetKeyCount = targetKeyCount;
    layout.weights = weights;

    if (targetKeyCount >= 8 && targetKeyCount % 2 == 0) {
        layout.hasPanels = true;
        const int split = targetKeyCount / 2;
        layout.leftPanelStart = 0;
        layout.leftPanelEnd = split - 1;
        layout.rightPanelStart = split;
        layout.rightPanelEnd = targetKeyCount - 1;
    }

    if (targetKeyCount == 10) {
        layout.hasBridge = true;
        layout.bridgeStart = 3;
        layout.bridgeEnd = 6;
    } else if (targetKeyCount >= 7) {
        layout.hasBridge = true;
        layout.bridgeStart = targetKeyCount / 2 - 1;
        layout.bridgeEnd = targetKeyCount / 2 + 1;
    }

    return layout;
}

std::string layoutKindName(const TargetLayoutSummary& layout) {
    if (layout.targetKeyCount == 10) {
        return "10K panel-bridge-fullfield";
    }
    if (layout.hasPanels && layout.hasBridge) {
        return "panel-bridge";
    }
    if (layout.hasPanels) {
        return "panel";
    }
    if (layout.hasBridge) {
        return "bridge";
    }
    return "fullfield";
}

}  // namespace keyconv::nk2

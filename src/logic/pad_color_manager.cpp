#include "crosspad-sampler/logic/pad_color_manager.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"

#include "crosspad/pad/PadManager.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"

namespace crosspad_sampler {

// ── Private helper ────────────────────────────────────────────────────────────

crosspad::RgbColor PadColorManager::uint32ToRgb(uint32_t color) {
    uint8_t r = static_cast<uint8_t>((color >> 16) & 0xFF);
    uint8_t g = static_cast<uint8_t>((color >>  8) & 0xFF);
    uint8_t b = static_cast<uint8_t>( color        & 0xFF);
    return crosspad::RgbColor(r, g, b);
}

void PadColorManager::restorePadKitColor(int padIdx) {
    if (padIdx < 0 || padIdx >= PAD_COUNT) return;

    crosspad::IKitManager* km = crosspad::getKitManager();
    if (!km) return;

    crosspad::KitInfo* kit = km->getCurrentKit();
    if (!kit) return;

    crosspad::getPadManager().setPadColor(
        static_cast<uint8_t>(padIdx),
        uint32ToRgb(kit->pads[padIdx].color)
    );
}

// ── Public API ────────────────────────────────────────────────────────────────

void PadColorManager::loadColorsFromKit(const crosspad::KitInfo& kit) {
    crosspad::PadManager& pm = crosspad::getPadManager();
    for (int i = 0; i < PAD_COUNT; ++i) {
        pm.setPadColor(static_cast<uint8_t>(i), uint32ToRgb(kit.pads[i].color));
    }
}

void PadColorManager::applyPadColor(uint8_t padIdx, const crosspad::Pad& pad) {
    if (padIdx >= PAD_COUNT) return;
    crosspad::getPadManager().setPadColor(padIdx, uint32ToRgb(pad.color));
}

void PadColorManager::setSelectedPad(int padIdx) {
    if (padIdx == selectedPad_) return;

    // Restore previous selection's kit color before highlighting the new one.
    if (selectedPad_ >= 0) {
        restorePadKitColor(selectedPad_);
    }

    selectedPad_ = padIdx;

    if (padIdx >= 0 && padIdx < PAD_COUNT) {
        crosspad::getPadManager().setPadColor(
            static_cast<uint8_t>(padIdx),
            crosspad::RgbColor(255, 255, 255)
        );
    }
}

int PadColorManager::getSelectedPad() const {
    return selectedPad_;
}

void PadColorManager::resetAllColors() {
    crosspad::PadManager& pm = crosspad::getPadManager();
    const crosspad::RgbColor gray(50, 50, 50);
    for (int i = 0; i < PAD_COUNT; ++i) {
        pm.setPadColor(static_cast<uint8_t>(i), gray);
    }
}

} // namespace crosspad_sampler

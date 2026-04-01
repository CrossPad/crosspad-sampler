#pragma once

#include <cstdint>
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/led/RgbColor.hpp"
#include "crosspad/pad/Pad.hpp"

namespace crosspad_sampler {

/**
 * @brief Manages pad LED colors based on kit config and selection state.
 *
 * Provides helpers for bulk-loading kit colors onto pads, applying
 * individual pad colors, highlighting a selected pad, and resetting
 * all pads to a neutral state.
 */
class PadColorManager {
public:
    PadColorManager() = default;

    /**
     * @brief Load pad colors from a kit and push them to the pad manager.
     *
     * Iterates all 16 pads in @p kit, converts each pad's uint32_t color
     * (RRGGBB) to RgbColor, and calls crosspad::getPadManager().setPadColor().
     */
    void loadColorsFromKit(const crosspad::KitInfo& kit);

    /**
     * @brief Set the LED color for a single pad from its Pad config.
     *
     * @param padIdx  Zero-based pad index (0–15).
     * @param pad     Pad whose color field is applied.
     */
    void applyPadColor(uint8_t padIdx, const crosspad::Pad& pad);

    /**
     * @brief Highlight @p padIdx white and restore the previously selected pad.
     *
     * If a pad was already selected its kit color is reloaded from the current
     * kit before the new selection is highlighted. Passing the same index that
     * is already selected is a no-op.
     *
     * @param padIdx  Zero-based pad index to select (-1 to clear selection).
     */
    void setSelectedPad(int padIdx);

    /**
     * @brief Return the currently selected pad index, or -1 if none.
     */
    int getSelectedPad() const;

    /**
     * @brief Reset all 16 pads to neutral gray (50, 50, 50).
     */
    void resetAllColors();

private:
    int selectedPad_ = -1;

    /// Convert a packed RRGGBB uint32_t to an RgbColor.
    static crosspad::RgbColor uint32ToRgb(uint32_t color);

    /// Restore a single pad's kit color (uses current kit from PlatformServices).
    void restorePadKitColor(int padIdx);
};

} // namespace crosspad_sampler

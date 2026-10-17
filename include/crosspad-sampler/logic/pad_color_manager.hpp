#pragma once

#include <atomic>
#include <cstdint>

#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/led/RgbColor.hpp"
#include "crosspad/pad/Pad.hpp"

namespace crosspad_sampler {

/// What a reloaded pad turned out to hold.
enum class PadLoad : uint8_t {
    Ready,    ///< a hit sounds
    Empty,    ///< the kit gives this pad no sample
    Failed,   ///< the kit names a sample the engine could not take
};

/**
 * @brief Manages pad LED colors based on kit config and selection state.
 *
 * Provides helpers for bulk-loading kit colors onto pads, applying
 * individual pad colors, highlighting a selected pad, and resetting
 * all pads to a neutral state.
 */
class PadColorManager {
public:
    /// Pads this manager tracks. Checked against SamplerConfig's PAD_COUNT in
    /// the implementation; kept local so this header pulls in no sampler
    /// config, whose names collide with the sample player's C macros.
    static constexpr int kPads = 16;

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

    /**
     * @brief Start showing which pads still hold the outgoing kit.
     *
     * A live swap reloads one pad at a time, so a bulk repaint would make
     * every pad claim the new kit while half of them still play the old one.
     * Instead the pads in @p pendingMask keep the outgoing colors, dimmed and
     * breathing, until markPadReloaded() says the sample underneath changed.
     *
     * Called from the loader thread; it only publishes state. Every LED write
     * happens in tickKitSwap()/endKitSwap() on the LVGL thread, so the two
     * threads never paint the same pad.
     *
     * @param incoming     The kit being loaded — the colors pads settle on.
     * @param pendingMask  Bit per pad that still has to be reloaded.
     */
    void beginKitSwap(const crosspad::KitInfo& incoming, uint16_t pendingMask);

    /**
     * @brief Mark @p padIdx as reloaded, with what it now holds. Loader thread.
     *
     * Only a Ready pad takes its kit color: an Empty one goes dark and a
     * Failed one dim red, so no pad looks loaded that a hit would not sound.
     * The state outlasts the swap -- every later repaint of the kit's colors
     * asks shownColor(), which reads emptiness off the kit model and keeps a
     * pad red only while the engine still cannot play it.
     */
    void markPadReloaded(uint8_t padIdx, PadLoad state);

    /**
     * @brief The color a pad shows for @p kitColor, given what it holds.
     */
    uint32_t shownColor(uint8_t padIdx, uint32_t kitColor) const;

    /**
     * @brief Advance the breath and push the pending/settled colors. LVGL thread.
     */
    void tickKitSwap();

    /**
     * @brief End the swap and settle every pad on the incoming kit colors.
     */
    void endKitSwap();

    /**
     * @brief True between beginKitSwap() and endKitSwap().
     */
    bool kitSwapActive() const { return swapActive_; }

    /**
     * @brief Paint the swap state again now, every pad. LVGL thread.
     *
     * For a writer that repainted the pads with the kit's colors mid-swap
     * (the panel taking the new kit's pads) -- the swap's state goes back on
     * at once rather than at the next tick.
     */
    void repaintKitSwap();

private:
    int selectedPad_ = -1;

    /// Convert a packed RRGGBB uint32_t to an RgbColor.
    static crosspad::RgbColor uint32ToRgb(uint32_t color);

    /// Restore a single pad's kit color (uses current kit from PlatformServices).
    void restorePadKitColor(int padIdx);

    /// Scale a color to @p percent of its brightness.
    static crosspad::RgbColor dim(uint32_t color, uint8_t percent);

    /// Every pad's swap color at the current breath.
    void paintKitSwap();

    /// Push one pad's swap color, unless it is the white-highlighted selection.
    void paintSwapPad(uint8_t padIdx, uint16_t pending, uint8_t breathPct);

    /// What a settled pad shows — the colors of the kit that is loaded.
    uint32_t kitColor_[kPads] = {0};

    /// What the pads showed before the swap started; the outgoing kit.
    uint32_t staleColor_[kPads] = {0};

    /// Pads that still hold the outgoing sample. Cleared by the loader thread,
    /// read by the LVGL thread — the only state the two share.
    std::atomic<uint16_t> swapPending_{0};

    /// Pads already repainted in their settled color, so the tick writes each
    /// hand-over once instead of every 100 ms.
    uint16_t swapSettled_ = 0;

    /// Pads whose last reload found a sample the engine could not take.
    /// Written by the loader thread, read by the LVGL thread.
    std::atomic<uint16_t> failedMask_{0};

    bool    swapActive_ = false;
    uint8_t breathStep_ = 0;
};

} // namespace crosspad_sampler

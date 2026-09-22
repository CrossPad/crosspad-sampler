#pragma once

#include <crosspad/pad/IPadLogicHandler.hpp>
#include <crosspad/event/IEventBus.hpp>

namespace crosspad_sampler {

/**
 * @brief Pad logic handler for the sampler app.
 *
 * Implements crosspad::IPadLogicHandler and manages pad interactions
 * for the CrossPad sampler: triggering sample playback on press,
 * applying kit pad colors on activate, and cleaning up on deactivate.
 *
 * Playback modes (from kit pad config):
 *   0 = one-shot  — sample plays to end regardless of release
 *   1 = hold      — sample stops on pad release
 *   2 = loop      — reserved (treated as one-shot for now)
 *
 * Two performance modes, MPC-style, neither persisted:
 *   Full level  — every hit plays at velocity 127, whatever the pad measured.
 *   16 levels   — the pad hit last before the mode went on becomes the
 *                 source; every pad then plays that source at one of sixteen
 *                 velocities, pad 0 the softest, pad 15 full. The LEDs show
 *                 the source's colour as a brightness ramp while it is on.
 */
class SamplerPadLogic : public crosspad::IPadLogicHandler {
public:
    SamplerPadLogic() = default;
    ~SamplerPadLogic() override = default;

    void setFullLevel(bool on) { fullLevel_ = on; }
    bool fullLevel() const { return fullLevel_; }

    /// Turning it on takes the last pad hit as the source (pad 0 when none
    /// was hit yet) and paints the ramp; turning it off restores the kit's
    /// colours.
    void setSixteenLevels(bool on);
    bool sixteenLevels() const { return sixteenLevels_; }
    uint8_t sixteenLevelsSource() const { return levelsSource_; }

    /// Repaints the pads for the active mode — the ramp, or the kit's own
    /// colours. For the kit loader, whose swap repaint ends on the kit's
    /// colours regardless of the mode.
    void refreshColors();

    /// Subscribe to EventBus events and load kit pad colors.
    void onActivate(crosspad::PadManager& padManager) override;

    /// Unsubscribe from EventBus events and reset pad colors to neutral gray.
    void onDeactivate(crosspad::PadManager& padManager) override;

    /// Trigger sample playback and provide visual press feedback.
    void onPadPress(crosspad::PadManager& padManager, uint8_t padIdx, uint8_t velocity) override;

    /// Provide visual release feedback; stop sample for Hold mode pads.
    void onPadRelease(crosspad::PadManager& padManager, uint8_t padIdx) override;

    /// Reserved for future aftertouch use — no-op.
    void onPadPressure(crosspad::PadManager& padManager, uint8_t padIdx, uint8_t pressure) override;

private:
    /// Load per-pad colors from the current kit into PadLedController.
    void loadKitColors(crosspad::PadManager& padManager);

    /// The source pad's colour on every pad, brightness rising with the level.
    void loadLevelsColors();

    /// Velocity a pad plays at under 16 levels: 7, 15, … 127.
    static uint8_t levelVelocity(uint8_t padIdx) { return static_cast<uint8_t>((padIdx + 1) * 8 - 1); }

    bool    fullLevel_     = false;
    bool    sixteenLevels_ = false;
    uint8_t levelsSource_  = 0;
    uint8_t lastPad_       = 0;

    // EventBus subscription handles
    crosspad::SubscriptionHandle samplePlaybackHandle_ = nullptr;
    crosspad::SubscriptionHandle noteOnHandle_         = nullptr;
    crosspad::SubscriptionHandle noteOffHandle_        = nullptr;

    bool subscribed_ = false;

    // Static event callbacks
    static void onSamplePlayback(const void* event_data, crosspad::EventType type, void* user_ctx);
    static void onNoteOn(const void* event_data, crosspad::EventType type, void* user_ctx);
    static void onNoteOff(const void* event_data, crosspad::EventType type, void* user_ctx);
};

} // namespace crosspad_sampler

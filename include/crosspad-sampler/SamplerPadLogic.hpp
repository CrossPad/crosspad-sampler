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
 */
class SamplerPadLogic : public crosspad::IPadLogicHandler {
public:
    SamplerPadLogic() = default;
    ~SamplerPadLogic() override = default;

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

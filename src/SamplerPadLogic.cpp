/**
 * @file SamplerPadLogic.cpp
 * @brief IPadLogicHandler implementation for the CrossPad sampler app.
 *
 * Handles pad press/release interactions for sample triggering, subscribes to
 * EventBus events for playback state feedback, and loads kit pad colors on
 * activation.
 */

#include "crosspad-sampler/SamplerPadLogic.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"

#include <crosspad/event/EventData.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/pad/PadLedController.hpp>
#include <crosspad/led/RgbColor.hpp>
#include <crosspad/platform/PlatformServices.hpp>

namespace crosspad_sampler {

// ── Helpers ──────────────────────────────────────────────────────────────────

/// Convert a 24-bit RRGGBB uint32_t to RgbColor.
static crosspad::RgbColor colorFromUint32(uint32_t rrggbb) {
    uint8_t r = (rrggbb >> 16) & 0xFF;
    uint8_t g = (rrggbb >>  8) & 0xFF;
    uint8_t b = (rrggbb      ) & 0xFF;
    return crosspad::RgbColor(r, g, b);
}

// ── Static event callbacks ────────────────────────────────────────────────────

void SamplerPadLogic::onSamplePlayback(const void* event_data,
                                        crosspad::EventType /*type*/,
                                        void* /*user_ctx*/) {
    auto* data = static_cast<const crosspad::SamplePlaybackData*>(event_data);
    if (data->note >= PAD_COUNT) return;

    crosspad::getPadLedController().notifyNotePlay(data->note, data->isOn);
}

void SamplerPadLogic::onNoteOn(const void* event_data,
                                crosspad::EventType /*type*/,
                                void* user_ctx) {
    // Reserved for future use (e.g. MIDI-driven visual feedback).
    (void)event_data;
    (void)user_ctx;
}

void SamplerPadLogic::onNoteOff(const void* event_data,
                                 crosspad::EventType /*type*/,
                                 void* user_ctx) {
    // Reserved for future use.
    (void)event_data;
    (void)user_ctx;
}

// ── IPadLogicHandler ─────────────────────────────────────────────────────────

void SamplerPadLogic::onActivate(crosspad::PadManager& padManager) {
    // Subscribe to EventBus events
    if (!subscribed_) {
        auto& bus = crosspad::getEventBus();

        samplePlaybackHandle_ = bus.subscribe(
            crosspad::EventType::SamplePlayback, onSamplePlayback, this);

        noteOnHandle_ = bus.subscribe(
            crosspad::EventType::NoteOn, onNoteOn, this);

        noteOffHandle_ = bus.subscribe(
            crosspad::EventType::NoteOff, onNoteOff, this);

        subscribed_ = true;
    }

    // Load pad colors from current kit
    loadKitColors(padManager);
}

void SamplerPadLogic::onDeactivate(crosspad::PadManager& padManager) {
    // Unsubscribe from EventBus
    if (subscribed_) {
        auto& bus = crosspad::getEventBus();

        if (samplePlaybackHandle_) { bus.unsubscribe(samplePlaybackHandle_); samplePlaybackHandle_ = nullptr; }
        if (noteOnHandle_)         { bus.unsubscribe(noteOnHandle_);          noteOnHandle_         = nullptr; }
        if (noteOffHandle_)        { bus.unsubscribe(noteOffHandle_);         noteOffHandle_        = nullptr; }

        subscribed_ = false;
    }

    // Reset all pad colors to neutral gray
    const crosspad::RgbColor neutral(50, 50, 50);
    for (int i = 0; i < PAD_COUNT; ++i) {
        padManager.setPadColor(i, neutral);
    }
}

void SamplerPadLogic::onPadPress(crosspad::PadManager& padManager,
                                  uint8_t padIdx,
                                  uint8_t velocity) {
    if (padIdx >= PAD_COUNT) return;

    // Trigger sample playback via EventBus
    crosspad::getEventBus().postSamplePlayback(
        padIdx, /*wavIdx=*/0, /*isOn=*/true, crosspad::EventSource::App);

    // Visual feedback — light the pad as pressed
    padManager.notifyNoteOn(padIdx, velocity, /*fromSampler=*/true);
}

void SamplerPadLogic::onPadRelease(crosspad::PadManager& padManager,
                                    uint8_t padIdx) {
    if (padIdx >= PAD_COUNT) return;

    // Visual feedback — return pad to resting state
    padManager.notifyNoteOff(padIdx, /*fromSampler=*/true);

    // For Hold mode (playbackMode == 1): send stop event so the audio engine
    // cuts playback when the pad is released.
    crosspad::IKitManager* kitMgr = crosspad::getKitManager();
    if (kitMgr) {
        crosspad::KitInfo* kit = kitMgr->getCurrentKit();
        if (kit && kit->parsed && kit->pads[padIdx].playbackMode == 1) {
            crosspad::getEventBus().postSamplePlayback(
                padIdx, /*wavIdx=*/0, /*isOn=*/false, crosspad::EventSource::App);
        }
    }
}

void SamplerPadLogic::onPadPressure(crosspad::PadManager& /*padManager*/,
                                     uint8_t /*padIdx*/,
                                     uint8_t /*pressure*/) {
    // Reserved for future aftertouch use — no-op.
}

// ── Private helpers ───────────────────────────────────────────────────────────

void SamplerPadLogic::loadKitColors(crosspad::PadManager& padManager) {
    crosspad::IKitManager* kitMgr = crosspad::getKitManager();
    if (!kitMgr) return;

    crosspad::KitInfo* kit = kitMgr->getCurrentKit();
    if (!kit || !kit->parsed) return;

    auto& ledCtrl = crosspad::getPadLedController();
    for (int i = 0; i < PAD_COUNT; ++i) {
        ledCtrl.setPadColor(i, colorFromUint32(kit->pads[i].color));
        ledCtrl.setPadPressedColor(i, colorFromUint32(kit->pads[i].color_pressed));
        // Playing color: dimmed version of the pad's default color
        ledCtrl.setPadPlayingColor(i, colorFromUint32(kit->pads[i].color).Dim(127));
    }
}

} // namespace crosspad_sampler

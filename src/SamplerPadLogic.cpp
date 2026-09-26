/**
 * @file SamplerPadLogic.cpp
 * @brief IPadLogicHandler for the sampler: sample triggering, EventBus
 *        playback feedback, and kit pad colours on activation.
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
#include <crosspad/midi/IMidiOutput.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/sequencer/SequencerController.hpp>
#include <crosspad/settings/CrosspadSettings.hpp>

namespace crosspad_sampler {

// ── Helpers ──────────────────────────────────────────────────────────────────

/// Convert a 24-bit RRGGBB uint32_t to RgbColor.
static crosspad::RgbColor colorFromUint32(uint32_t rrggbb) {
    uint8_t r = (rrggbb >> 16) & 0xFF;
    uint8_t g = (rrggbb >>  8) & 0xFF;
    uint8_t b = (rrggbb      ) & 0xFF;
    return crosspad::RgbColor(r, g, b);
}

/// The sampler is PadManager's *base* pad logic, so a handler exists from boot
/// and handlePadPress() returns before its own MIDI branch runs. The note is
/// emitted here instead, under the same keypad gating; routing stays with
/// MidiInputHandler behind getMidiOutput().
static bool padMidiEnabled() {
    auto* settings = crosspad::CrosspadSettings::getInstance();
    return settings && settings->keypad.enableKeypad && !settings->keypad.sendCC;
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

    loadKitColors(padManager);
}

void SamplerPadLogic::onDeactivate(crosspad::PadManager& padManager) {
    if (subscribed_) {
        auto& bus = crosspad::getEventBus();

        if (samplePlaybackHandle_) { bus.unsubscribe(samplePlaybackHandle_); samplePlaybackHandle_ = nullptr; }
        if (noteOnHandle_)         { bus.unsubscribe(noteOnHandle_);          noteOnHandle_         = nullptr; }
        if (noteOffHandle_)        { bus.unsubscribe(noteOffHandle_);         noteOffHandle_        = nullptr; }

        subscribed_ = false;
    }

    const crosspad::RgbColor neutral(50, 50, 50);
    for (int i = 0; i < PAD_COUNT; ++i) {
        padManager.setPadColor(i, neutral);
    }
}

void SamplerPadLogic::onPadPress(crosspad::PadManager& padManager,
                                  uint8_t padIdx,
                                  uint8_t velocity) {
    if (padIdx >= PAD_COUNT) return;

    /* Erase held in the sequencer: a player's hit only names the pad to take
     * out of the scene. The LED still answers, so you see which one went. The
     * loop's own playback is not a hit and keeps sounding -- silencing it too
     * turned held erase into a mute. */
    auto* seq = crosspad::getPlatformServices().sequencer;
    if (seq && seq->isErasing() && !crosspad::PadManager::isSequencedPress()) {
        padManager.notifyNoteOn(padIdx, velocity, /*fromSampler=*/true);
        return;
    }

    // The velocity is already post-curve (the STM32 applies the per-pad
    // response), so it passes through linearly.
    crosspad::getEventBus().postSamplePlayback(
        padIdx, /*wavIdx=*/0, /*isOn=*/true, crosspad::EventSource::App, velocity);

    padManager.notifyNoteOn(padIdx, velocity, /*fromSampler=*/true);

    if (padMidiEnabled()) {
        crosspad::getMidiOutput().sendNoteOn(padManager.getMidiNoteForPad(padIdx),
                                             velocity,
                                             padManager.getPadChannel(padIdx));
    }
}

void SamplerPadLogic::onPadRelease(crosspad::PadManager& padManager,
                                    uint8_t padIdx) {
    if (padIdx >= PAD_COUNT) return;

    padManager.notifyNoteOff(padIdx, /*fromSampler=*/true);

    if (padMidiEnabled()) {
        crosspad::getMidiOutput().sendNoteOff(padManager.getMidiNoteForPad(padIdx),
                                              /*velocity=*/0,
                                              padManager.getPadChannel(padIdx));
    }

    // Hold mode: the release has to tell the engine to cut playback.
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
        // Playing: the pad's default colour, dimmed.
        ledCtrl.setPadPlayingColor(i, colorFromUint32(kit->pads[i].color).Dim(127));
    }
}

} // namespace crosspad_sampler

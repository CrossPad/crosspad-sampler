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
    if (seq && seq->isErasing() &&
        padManager.pressSource(padIdx) != crosspad::EventSource::SequencerCallback) {
        padManager.notifyNoteOn(padIdx, velocity, /*fromSampler=*/true);
        return;
    }

    // The velocity is already post-curve (the STM32 applies the per-pad
    // response), so it passes through linearly — unless a performance mode
    // overrides it. Under 16 levels the pad hit only picks the level; what
    // sounds (and what goes out over MIDI) is the source pad -- at a velocity
    // of the ramp, or at a pitch of the scale. The pad hit still gets the
    // press feedback and the held state, so the LEDs light where the finger is.
    uint8_t soundPad = padIdx;
    int8_t transpose = 0;
    uint8_t key = crosspad::kSamplePlaybackOwnKey;
    if (levels_ == Levels::Velocity) {
        soundPad = levelsSource_;
        velocity = levelVelocity(padIdx);
    } else if (levels_ == Levels::Pitch) {
        soundPad = levelsSource_;
        transpose = levelTranspose(padIdx);
        key = static_cast<uint8_t>(kLevelsKey + padIdx);
    } else {
        lastPad_ = padIdx;
    }
    if (fullLevel_ && levels_ != Levels::Velocity) velocity = 127;

    crosspad::getEventBus().postSamplePlayback(
        soundPad, /*wavIdx=*/0, /*isOn=*/true, crosspad::EventSource::App, velocity, transpose, key);

    padManager.notifyNoteOn(padIdx, velocity, /*fromSampler=*/true);

    if (padMidiEnabled()) {
        crosspad::getMidiOutput().sendNoteOn(midiNote(padManager, soundPad, transpose),
                                             velocity,
                                             padManager.getPadChannel(soundPad));
    }
}

void SamplerPadLogic::onPadRelease(crosspad::PadManager& padManager,
                                    uint8_t padIdx) {
    if (padIdx >= PAD_COUNT) return;

    const uint8_t soundPad = sixteenLevels() ? levelsSource_ : padIdx;
    const bool pitch = levels_ == Levels::Pitch;
    const int8_t transpose = pitch ? levelTranspose(padIdx) : 0;
    const uint8_t key = pitch ? static_cast<uint8_t>(kLevelsKey + padIdx) : crosspad::kSamplePlaybackOwnKey;

    padManager.notifyNoteOff(padIdx, /*fromSampler=*/true);

    // Hold mode: the release has to tell the engine to cut playback. Before
    // the MIDI echo, as the press posts before its own: a USB send can wait
    // on a host that reads slowly.
    crosspad::IKitManager* kitMgr = crosspad::getKitManager();
    if (kitMgr) {
        crosspad::KitInfo* kit = kitMgr->getCurrentKit();
        if (kit && kit->parsed && kit->pads[soundPad].playbackMode == 1) {
            crosspad::getEventBus().postSamplePlayback(
                soundPad, /*wavIdx=*/0, /*isOn=*/false, crosspad::EventSource::App, 127, transpose, key);
        }
    }

    if (padMidiEnabled()) {
        crosspad::getMidiOutput().sendNoteOff(midiNote(padManager, soundPad, transpose),
                                              /*velocity=*/0,
                                              padManager.getPadChannel(soundPad));
    }
}

// ── Performance modes ────────────────────────────────────────────────────────

void SamplerPadLogic::setLevels(Levels mode) {
    if (mode == levels_) return;
    /* From off, the pad hit last is the source; between the two levels it
     * stays the one it was. */
    if (levels_ == Levels::Off) levelsSource_ = lastPad_;
    levels_ = mode;
    refreshColors();
}

void SamplerPadLogic::refreshColors() {
    if (sixteenLevels()) loadLevelsColors();
    else                 loadKitColors(crosspad::getPadManager());
}

uint8_t SamplerPadLogic::midiNote(crosspad::PadManager& padManager, uint8_t pad, int8_t transpose) {
    const int note = static_cast<int>(padManager.getMidiNoteForPad(pad)) + transpose;
    return static_cast<uint8_t>(note < 0 ? 0 : (note > 127 ? 127 : note));
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

    if (sixteenLevels()) {
        loadLevelsColors();
        return;
    }

    auto& ledCtrl = crosspad::getPadLedController();
    for (int i = 0; i < PAD_COUNT; ++i) {
        ledCtrl.setPadColor(i, colorFromUint32(kit->pads[i].color));
        ledCtrl.setPadPressedColor(i, colorFromUint32(kit->pads[i].color_pressed));
        // Playing: the pad's default colour, dimmed.
        ledCtrl.setPadPlayingColor(i, colorFromUint32(kit->pads[i].color).Dim(127));
    }
}

void SamplerPadLogic::loadLevelsColors() {
    crosspad::IKitManager* kitMgr = crosspad::getKitManager();
    crosspad::KitInfo* kit = kitMgr ? kitMgr->getCurrentKit() : nullptr;
    // No kit: a white ramp still says which pad plays how loud.
    const crosspad::RgbColor base = (kit && kit->parsed)
        ? colorFromUint32(kit->pads[levelsSource_].color) : crosspad::RgbColor(255, 255, 255);
    const crosspad::RgbColor pressed = (kit && kit->parsed)
        ? colorFromUint32(kit->pads[levelsSource_].color_pressed) : crosspad::RgbColor(255, 255, 255);

    auto& ledCtrl = crosspad::getPadLedController();
    for (uint8_t i = 0; i < PAD_COUNT; ++i) {
        // Dim() takes 0..255; the softest pad keeps a floor so it is not off.
        uint8_t brightness = static_cast<uint8_t>(40 + (215 * i) / (PAD_COUNT - 1));
        if (levels_ == Levels::Pitch) {
            /* A keyboard: the source's own note in every octave full, the
             * other naturals half, the sharps low. */
            const int semi = levelTranspose(i) % 12;
            const bool sharp = semi == 1 || semi == 3 || semi == 6 || semi == 8 || semi == 10;
            brightness = semi == 0 ? 255 : (sharp ? 40 : 130);
        }
        ledCtrl.setPadColor(i, base.Dim(brightness));
        ledCtrl.setPadPressedColor(i, pressed);
        ledCtrl.setPadPlayingColor(i, base.Dim(brightness / 2));
    }
}

} // namespace crosspad_sampler

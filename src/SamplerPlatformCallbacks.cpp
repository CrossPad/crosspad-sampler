// SPDX-License-Identifier: MIT

#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"

#include <crosspad/pad/PadManager.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/instrument/PitchedInstrument.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/platform/PlatformServices.hpp>

namespace crosspad_sampler {


static SamplerPlatformCallbacks s_callbacks = {};

void sampler_set_platform_callbacks(const SamplerPlatformCallbacks& cb) {
    s_callbacks = cb;
}

const SamplerPlatformCallbacks& sampler_get_platform_callbacks() {
    return s_callbacks;
}

namespace {
class KitPadMix : public crosspad::IPadMix {
public:
    void setPadMix(uint8_t pad, int volume, int pan) override {
        auto* km  = crosspad::getKitManager();
        auto* kit = km ? km->getCurrentKit() : nullptr;
        if (kit && kit->engine == crosspad::KitEngine::Pitched) {
            if (auto* inst = crosspad::getPlatformServices().pitchedInstrument) {
                inst->setPadMix(pad, volume, pan);
            }
            return;
        }
        if (s_callbacks.player.setVolume) s_callbacks.player.setVolume(pad, volume);
        if (s_callbacks.player.setPan)    s_callbacks.player.setPan(pad, pan);
    }
};
} // namespace

crosspad::IPadMix& sampler_pad_mix() {
    static KitPadMix mix;
    return mix;
}

// Engine → Sampler callbacks, wired into SampleStreamPlayer_NoteOnCb/NoteOffCb
// by the platform. They update PadManager state and post EventBus feedback so
// the GUI tracks playback with minimal latency.

static void engine_note_on(uint8_t note, uint8_t wavIdx) {
    if (note >= PAD_COUNT) return;

    crosspad::getPadManager().handleSamplePlay(note, true, wavIdx);

    crosspad::getEventBus().postSamplePlayback(
        note, wavIdx, true, crosspad::EventSource::SamplePlayback);
}

static void engine_note_off(uint8_t note, uint8_t wavIdx) {
    if (note >= PAD_COUNT) return;

    crosspad::getPadManager().handleSamplePlay(note, false, wavIdx);
    crosspad::getEventBus().postSamplePlayback(
        note, wavIdx, false, crosspad::EventSource::SamplePlayback);
}

SamplerEngineCallbacks sampler_get_engine_callbacks() {
    SamplerEngineCallbacks cb;
    cb.onNoteOn  = engine_note_on;
    cb.onNoteOff = engine_note_off;
    return cb;
}

} // namespace crosspad_sampler

// SPDX-License-Identifier: MIT

#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"

#include <crosspad/pad/PadManager.hpp>
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/platform/PlatformServices.hpp>

namespace crosspad_sampler {

// ---------------------------------------------------------------------------
// Static storage
// ---------------------------------------------------------------------------

static SamplerPlatformCallbacks s_callbacks = {};

void sampler_set_platform_callbacks(const SamplerPlatformCallbacks& cb) {
    s_callbacks = cb;
}

const SamplerPlatformCallbacks& sampler_get_platform_callbacks() {
    return s_callbacks;
}

// ---------------------------------------------------------------------------
// Engine → Sampler callbacks (direct path for low-latency fingerdrumming)
//
// The platform wires these into SampleStreamPlayer_NoteOnCb / NoteOffCb.
// They update PadManager state and post EventBus feedback so the GUI can
// track playback (playhead, pad colors) with minimal latency.
// ---------------------------------------------------------------------------

static void engine_note_on(uint8_t note, uint8_t wavIdx) {
    if (note >= PAD_COUNT) return;

    // Update PadManager play state
    crosspad::getPadManager().handleSamplePlay(note, true, wavIdx);

    // Post EventBus feedback for RGB and GUI synchronization
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

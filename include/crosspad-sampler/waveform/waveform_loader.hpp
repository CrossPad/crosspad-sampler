// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace crosspad_sampler {

/// Start async waveform loader task (FreeRTOS task on configurable core).
bool waveform_loader_start();

/// Stop loader task gracefully.
void waveform_loader_stop();

/// Queue a waveform load request.
void waveform_loader_request(const char* filename, bool highPriority = false,
                             uint8_t padIndex = 0,
                             uint32_t sampleStart = 0, uint32_t sampleEnd = 0);

/// Queue every sample of every pad in the current kit that is not cached yet.
/// Every layer, not only the first: the panel shows the layer that actually
/// fired, so a round-robin or velocity-split pad drew nothing for its other
/// samples. Returns how many requests were queued.
int waveform_loader_preload_kit(int maxPads = 16);

/// Is the loader task running?
bool waveform_loader_is_running();

/// What the loader has done since boot. "missing waveform" used to be
/// invisible — a failed open was a printf and nothing else — so every outcome
/// is counted, and HIL reads them over CDC (WAVE_STATUS).
struct WaveformLoaderStats {
    uint32_t requested  = 0;   ///< requests accepted into the queue
    uint32_t dropped    = 0;   ///< queue full: request lost
    uint32_t loaded     = 0;   ///< cached successfully
    uint32_t retried    = 0;   ///< a failed read re-queued
    uint32_t failedInfo = 0;   ///< header/open failed for good
    uint32_t failedRead = 0;   ///< sample read failed for good
    uint32_t allocFail  = 0;   ///< no memory for the temp buffer
    uint32_t sweeps     = 0;   ///< queue ran dry, the kit was walked again
    uint32_t startFail  = 0;   ///< the task could not be created (internal RAM)
    uint32_t queued     = 0;   ///< waiting right now
};
void waveform_loader_get_stats(WaveformLoaderStats* out);

/// Progress over the requests made since the current kit was picked, 0..100,
/// or -1 when there is nothing outstanding. Drives the status bar's loading
/// ring, the same one the kit loader uses.
int waveform_loader_progress();

/// Clear pending load requests.
void waveform_loader_clear_queue();

} // namespace crosspad_sampler

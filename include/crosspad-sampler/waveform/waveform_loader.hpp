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

/// Preload waveforms for all pads in the current kit (queues requests).
void waveform_loader_preload_kit(int maxPads = 16);

/// Is the loader task running?
bool waveform_loader_is_running();

/// Clear pending load requests.
void waveform_loader_clear_queue();

} // namespace crosspad_sampler

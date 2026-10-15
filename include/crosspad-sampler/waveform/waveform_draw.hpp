// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"
#include <cstdint>

namespace crosspad_sampler {

/// Draw a chunk of waveform on an LVGL canvas (stereo interleaved int16_t).
void waveform_draw_chunk(lv_obj_t* canvas, int16_t* waveform, uint32_t sampleCount,
                         int startX, int endX, int width, int height, bool clearFirst);

/// Draw complete waveform synchronously.
void waveform_draw_on_canvas(lv_obj_t* canvas, int16_t* waveform, uint32_t sampleCount,
                             int width, int height);

/// Start async (chunked) waveform drawing via LVGL timer.
void waveform_start_async_draw(lv_obj_t* canvas, int16_t* waveformData,
                               uint32_t sampleCount, bool useLinearScale = false);

/// Load waveform from cache and draw (async).
/// Returns true if drawn from cache; false if cache miss (will request async load).
bool waveform_load_and_draw(lv_obj_t* canvas, const char* filename, bool forceSync);

/// Update waveform display with new file.
void waveform_update_display(lv_obj_t* canvas, const char* filename);

/// Create waveform display widget (canvas + styling).
lv_obj_t* waveform_create_display(lv_obj_t* parent, const char* filename = nullptr);

/// Clear drawing state (call before cache clear).
void waveform_clear_drawing_state();

/// Draw placeholder sine waves (when caching is declined).
void waveform_draw_empty(lv_obj_t* canvas);

} // namespace crosspad_sampler

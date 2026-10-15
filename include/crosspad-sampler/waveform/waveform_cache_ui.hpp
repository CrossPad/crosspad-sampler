// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

namespace crosspad_sampler {

// User interaction state
extern bool userAskedAboutCache;
extern bool userWantsCaching;

/// Draw placeholder sine waves (when user declines caching).
void waveform_ui_draw_empty(lv_obj_t* canvas);

/// Create cache prompt overlay ("Cache all .wav files at once?").
void waveform_ui_create_prompt(lv_obj_t* canvas);

/// Destroy cache prompt overlay.
void waveform_ui_destroy_prompt();

/// Create progress bar overlay for cache loading.
void waveform_ui_create_progress(lv_obj_t* canvas);

/// Update cache progress bar.
void waveform_ui_update_progress(int current, int total);

/// Destroy cache progress bar and trigger disk save.
void waveform_ui_destroy_progress();

} // namespace crosspad_sampler

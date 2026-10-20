// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef USE_LVGL

#include <lvgl.h>

// The sampler's own styles: the waveform box and the kit browser (cover panel
// on the left, one row per kit on the right).
extern lv_style_t styleWaveformBox;
extern lv_style_t styleKitBrowserScreen;
extern lv_style_t styleKitBrowserStatus;
extern lv_style_t styleKitBrowserRow;
extern lv_style_t styleKitBrowserRowSelected;
extern lv_style_t styleKitBrowserCover;
extern lv_style_t styleKitBrowserThumb;

namespace crosspad_sampler {

/// Build the styles above from the current theme colour, before anything
/// that uses them is created. Cheap when they are up to date; rebuilds them
/// when the theme colour changed since.
void sampler_styles_init();

} // namespace crosspad_sampler

#endif // USE_LVGL

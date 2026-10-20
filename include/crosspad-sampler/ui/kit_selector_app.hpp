// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef USE_LVGL

#include <lvgl.h>

class App;

/// LVGL app factory for the kit selector overlay.
lv_obj_t* lv_CreateKitSelector(lv_obj_t* parent, App* app);

/// Destroy the kit selector overlay app.
void lv_KitSelector_destroy(lv_obj_t* app);

/// The next selector this app creates picks for @p fn (kit_selector.hpp
/// KitSelectorConfig::onPicked) and closes, instead of loading the kit; null
/// for the usual load. Taken by that selector, so one after it loads again.
void kit_selector_app_pick_for(void (*fn)(int kitId, void* ctx), void* ctx);

#endif // USE_LVGL

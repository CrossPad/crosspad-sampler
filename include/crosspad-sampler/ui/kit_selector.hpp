// SPDX-License-Identifier: MIT
#pragma once

#ifdef USE_LVGL

#include <lvgl.h>

namespace crosspad_sampler {

/// Callback invoked when a kit button is selected (before loading).
using KitSelectedFn = void (*)(int kitId, void* userData);

/// Callback invoked when kit loading finishes.
using KitLoadedFn = void (*)(void* userData);

/// Configuration for creating a kit selector overlay.
struct KitSelectorConfig {
    lv_obj_t* parent = nullptr;
    KitSelectedFn onSelected = nullptr;
    KitLoadedFn   onLoaded = nullptr;
    void* userData = nullptr;
    bool showCacheDots = true;
};

/// Create a full-screen kit selector overlay.
lv_obj_t* kit_selector_create(const KitSelectorConfig& config);

/// Destroy the kit selector overlay and clean up LVGL groups.
void kit_selector_destroy(lv_obj_t* selector);

/// Repopulate the kit list from IKitManager.
void kit_selector_refresh(lv_obj_t* selector);

// Legacy API — retained for backward compat
void kit_selector_set_callback(KitSelectedFn fn, void* userData);

} // namespace crosspad_sampler

#endif // USE_LVGL

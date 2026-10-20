// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#ifdef USE_LVGL

#include <lvgl.h>

namespace crosspad_sampler {

/// Callback invoked when a kit button is selected (before loading).
using KitSelectedFn = void (*)(int kitId, void* userData);

/// Callback invoked when kit loading finishes.
using KitLoadedFn = void (*)(void* userData);

/// Callback invoked when the user dismisses the overlay (Back / power button).
/// If set, the host owns teardown — typically by destroying the wrapping App.
/// If null, the overlay calls `kit_selector_destroy` itself.
using KitClosedFn = void (*)(void* userData);

/// Callback asking the host to close the browser and open @p app (its
/// registered name): Pitch Play for an instrument, Arrange for a song that has
/// opened, the Slicer for a take.
using KitOpenAppFn = void (*)(const char* app, void* userData);

/// Configuration for creating a kit selector overlay.
struct KitSelectorConfig {
    lv_obj_t* parent = nullptr;
    /// Set, the selector picks for someone: a kit chosen is handed here and
    /// not loaded (onSelected and onLoaded are not called), and the host
    /// closes it. A song's kit track's kit is picked so.
    KitSelectedFn onPicked = nullptr;
    KitSelectedFn onSelected = nullptr;
    KitLoadedFn   onLoaded = nullptr;
    KitClosedFn   onClose = nullptr;
    KitOpenAppFn  onOpenApp = nullptr;
    void* userData = nullptr;
};

/// Create the browser over the whole app area: KITS / DRUMS / PITCH / SONGS /
/// USER down the left, five rows of the chosen list, what the highlighted thing
/// is across the top, LOAD and EDIT along the bottom. Kits come from
/// IKitManager (DRUMS: "category": "drums"), instruments from Pitch Play's
/// library, songs from ISongEngine, takes from ISampleTools. While it is up the
/// pads play and light the highlighted kit.
lv_obj_t* kit_selector_create(const KitSelectorConfig& config);

/// Destroy the kit selector overlay and clean up LVGL groups.
void kit_selector_destroy(lv_obj_t* selector);

/// Repopulate the kit list from IKitManager.
void kit_selector_refresh(lv_obj_t* selector);

/// True if a kit selector overlay is currently visible.
bool kit_selector_is_open();

/// Close the active kit selector overlay (same effect as the Back button).
void kit_selector_close();

// Legacy API — retained for backward compat
void kit_selector_set_callback(KitSelectedFn fn, void* userData);

} // namespace crosspad_sampler

#endif // USE_LVGL

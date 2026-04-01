// SPDX-License-Identifier: MIT
#pragma once

#ifdef USE_LVGL

#include <lvgl.h>

namespace crosspad_sampler {

/// Callback invoked when a kit button is selected.
/// @param kitId   Index of the selected kit.
/// @param userData Opaque user context pointer.
using KitSelectedFn = void (*)(int kitId, void* userData);

/// Create a full-screen kit selector overlay attached to @p parent.
/// Internally calls kit_selector_refresh() to populate kit buttons.
/// @return Pointer to the created LVGL object.
lv_obj_t* kit_selector_create(lv_obj_t* parent);

/// Asynchronously delete the selector object.
void kit_selector_destroy(lv_obj_t* selector);

/// Repopulate the kit button list from IKitManager.
/// Removes all buttons (preserving the title label at index 0) and recreates
/// them from the current kit list. Highlights the active kit.
void kit_selector_refresh(lv_obj_t* selector);

/// Register the callback invoked when the user selects a kit.
void kit_selector_set_callback(KitSelectedFn fn, void* userData);

} // namespace crosspad_sampler

#endif // USE_LVGL

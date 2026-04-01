// SPDX-License-Identifier: MIT
#pragma once

#ifdef USE_LVGL

#include <lvgl.h>

namespace crosspad_sampler {

/// Create the main sampler panel inside @p parent.
/// @return Pointer to the top-level lv_obj_t for the panel.
lv_obj_t* sampler_panel_create(lv_obj_t* parent);

/// Destroy the sampler panel previously created by sampler_panel_create().
/// @param panel The lv_obj_t returned by sampler_panel_create().
void sampler_panel_destroy(lv_obj_t* panel);

} // namespace crosspad_sampler

#endif // USE_LVGL

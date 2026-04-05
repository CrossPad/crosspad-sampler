// SPDX-License-Identifier: MIT
#pragma once

#ifdef USE_LVGL

#include <lvgl.h>

class App;

/// LVGL app factory for the kit selector overlay.
lv_obj_t* lv_CreateKitSelector(lv_obj_t* parent, App* app);

/// Destroy the kit selector overlay app.
void lv_KitSelector_destroy(lv_obj_t* app);

#endif // USE_LVGL

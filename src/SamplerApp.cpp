// SPDX-License-Identifier: MIT

#ifdef USE_LVGL

#include <crosspad-sampler/SamplerApp.hpp>
#include <crosspad/app/AppRegistry.hpp>
#include <crosspad/app/AppRegistrar.hpp>
#include <crosspad/app/AppEntry.hpp>
#include <crosspad-sampler/ui/sampler_panel.hpp>

#include <lvgl.h>

namespace crosspad_sampler {

// Wrapper matching the generic createLVGL signature (App* parameter is unused).
static lv_obj_t* sampler_create_fn(lv_obj_t* parent, void* /*app*/) {
    return sampler_panel_create(parent);
}

crosspad::AppEntry sampler_get_entry() {
    crosspad::AppEntry entry;
    entry.name     = "Sampler";
    entry.icon     = LV_SYMBOL_AUDIO;
    entry.createLVGL  = reinterpret_cast<decltype(entry.createLVGL)>(sampler_create_fn);
    entry.destroyLVGL = sampler_panel_destroy;
    entry.priority = 0;
    return entry;
}

void sampler_register() {
    crosspad::AppRegistry::getInstance().registerApp(sampler_get_entry());
}

} // namespace crosspad_sampler

// Auto-registration via crosspad-core AppRegistrar (cross-platform static constructor).
// Kept alive by --whole-archive when library.json has "appLibrary": true.
static crosspad::AppRegistrar s_sampler_reg(crosspad_sampler::sampler_get_entry());

#endif // USE_LVGL

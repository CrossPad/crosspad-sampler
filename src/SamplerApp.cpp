// SPDX-License-Identifier: MIT

#ifdef USE_LVGL

#include <crosspad-sampler/SamplerApp.hpp>
#include <crosspad/app/AppRegistry.hpp>
#include <crosspad/app/AppEntry.hpp>
#include <crosspad-sampler/ui/sampler_panel.hpp>
#include <crosspad-sampler/SamplerPadLogic.hpp>
#include <crosspad-gui/app/App.hpp>
#include <crosspad/pad/PadManager.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <lvgl.h>
#include <memory>

namespace crosspad_sampler {

static App* s_samplerApp = nullptr;

static void sampler_back_cb() {
    if (s_samplerApp && s_samplerApp->isStarted()) {
        s_samplerApp->destroyApp();
    }
    s_samplerApp = nullptr;
}

static lv_obj_t* sampler_create_fn(lv_obj_t* parent, App* app) {
    s_samplerApp = app;
    return sampler_panel_create(parent, sampler_back_cb);
}

crosspad::AppEntry sampler_get_entry() {
    crosspad::AppEntry entry;
    entry.name     = "Sampler";
    entry.icon     = LV_SYMBOL_AUDIO;
    entry.createLVGL  = sampler_create_fn;
    entry.destroyLVGL = sampler_panel_destroy;
    entry.priority = 0;
    return entry;
}

void sampler_service_init() {
    // Non-owning: the handler is a function-local static owned by the sampler
    // panel translation unit and outlives every app instance, which is exactly
    // what a system instrument needs.
    auto logic = std::shared_ptr<SamplerPadLogic>(&getSamplerPadLogic(),
                                                  [](SamplerPadLogic*) {});
    auto& pads = crosspad::getPadManager();
    pads.registerPadLogic("Sampler", logic);
    pads.setBasePadLogic("Sampler");
}

void sampler_register() {
    crosspad::AppRegistry::getInstance().registerApp(sampler_get_entry());
}

} // namespace crosspad_sampler

// Discovered by the platform registry generator.
void _register_Sampler_app() {
    crosspad_sampler::sampler_register();
}

#endif // USE_LVGL

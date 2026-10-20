// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef USE_LVGL

#include <crosspad-sampler/SamplerApp.hpp>
#include <crosspad/app/AppRegistry.hpp>
#include <crosspad/app/AppEntry.hpp>
#include <crosspad-sampler/ui/sampler_panel.hpp>
#include <crosspad-sampler/SamplerPadLogic.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
// The App class differs by platform but not by shape: the PC one derives from
// crosspad_gui::AppBase<App> just as the device one does, so isStarted() /
// destroyApp() mean the same thing on both.
#if __has_include("pc_stubs/PcApp.hpp")
  #include "pc_stubs/PcApp.hpp"
#else
  #include <crosspad-gui/app/App.hpp>
#endif
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

/* One level up from the sampler is the kit selector, not the launcher: the
 * selector is where the running kit was chosen, and bouncing between the two
 * is the loop a player actually works in. Leaving the app takes a second back,
 * from the selector. A platform without a selector keeps the old behaviour by
 * declining here. */
static bool sampler_handle_back(lv_obj_t* app) {
    (void)app;
    /* Unsaved pad edits first. The `<` that used to ask is gone with the
     * UX 2000 chrome, and this click is now the only way off this screen that
     * the screen itself sees -- so nothing is lost without a question. */
    if (sampler_panel_confirm_exit()) return true;
    const auto& cb = sampler_get_platform_callbacks().kitSelector;
    const auto open = cb.openSelectorForBack ? cb.openSelectorForBack : cb.openSelector;
    if (!open) return false;
    open();
    return true;
}

static lv_obj_t* sampler_create_fn(lv_obj_t* parent, App* app) {
    s_samplerApp = app;
    app->setOnBack(sampler_handle_back);
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
    crosspad::getPlatformServices().padMix = &sampler_pad_mix();
    /* 16 levels, for the Sequencer to record a part in. */
    crosspad::getPlatformServices().padPerformance = &getSamplerPadLogic();
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

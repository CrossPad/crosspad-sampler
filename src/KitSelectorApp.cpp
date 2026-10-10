// SPDX-License-Identifier: GPL-3.0-or-later
//
// Kit Selector app — wraps kit_selector UI with platform callbacks
// and LVGL app lifecycle. Registered via a named platform entry point.

#ifdef USE_LVGL

#include <crosspad-sampler/ui/kit_selector_app.hpp>
#include <crosspad-sampler/ui/kit_selector.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
#include <crosspad-sampler/waveform/waveform_loader.hpp>
#include <crosspad-sampler/waveform/waveform_cache.hpp>
#include <crosspad/app/AppRegistry.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad-gui/platform/IGuiPlatform.h>
#include <crosspad-gui/app/App.hpp>

#include <lvgl.h>

static App* s_thisApp = nullptr;

static void on_kit_selected(int kitId, void* /*userData*/) {
    auto& cb = crosspad_sampler::sampler_get_platform_callbacks().kitSelector;
    if (cb.onBeforeKitLoad) cb.onBeforeKitLoad(kitId);

    auto* kitMgr = crosspad::getPlatformServices().kitManager;
    if (kitMgr) {
        auto* kit = kitMgr->getKit(kitId);
        /* The loader is not stopped here any more. Its stack is 6 kB of
         * internal RAM in one piece, and by the time a kit is picked the
         * largest free internal block is often smaller than that: the task
         * died on every kit pick and the restart failed silently, so the kit
         * ended up with no waveform at all. It stays alive; the queue is
         * cleared inside clear_if_kit_changed(), which is what the stop was
         * really for. */
        crosspad_sampler::waveform_cache_clear_if_kit_changed(
            kit, kit ? kit->path.c_str() : nullptr);
    }
}

/* Both exits go through here, deferred: the caller is an LVGL callback on a
 * widget the teardown deletes. onClosed fires after destroyApp() so whoever
 * opened the selector as a gate can launch what was waiting behind it. */
static void close_app_async() {
    lv_async_call([](void*) {
        if (s_thisApp && s_thisApp->isStarted()) {
            s_thisApp->destroyApp();
        }
        auto& cb = crosspad_sampler::sampler_get_platform_callbacks().kitSelector;
        if (cb.onClosed) cb.onClosed();
    }, nullptr);
}

static void on_kit_loaded(void* /*userData*/) {
    auto& cb = crosspad_sampler::sampler_get_platform_callbacks().kitSelector;
    if (cb.onKitLoaded) cb.onKitLoaded();
    close_app_async();
}

static void on_kit_closed(void* /*userData*/) {
    close_app_async();
}

lv_obj_t* lv_CreateKitSelector(lv_obj_t* parent, App* app) {
    s_thisApp = app;

    crosspad_sampler::KitSelectorConfig config;
    config.parent        = parent;
    config.onSelected    = on_kit_selected;
    config.onLoaded      = on_kit_loaded;
    config.onClose       = on_kit_closed;
    return crosspad_sampler::kit_selector_create(config);
}

void lv_KitSelector_destroy(lv_obj_t* app) {
    s_thisApp = nullptr;
    crosspad_sampler::kit_selector_destroy(app);
}

static crosspad::AppEntry kit_selector_get_entry() {
    crosspad::AppEntry entry;
    entry.name        = "KitSelector";
    entry.icon        = "notepick.png";
    entry.createLVGL  = lv_CreateKitSelector;
    entry.destroyLVGL = lv_KitSelector_destroy;
    entry.priority    = 50;
    entry.recolorIcon = true;  // notepick.png is a black glyph
    return entry;
}

void _register_KitSelector_app() {
    crosspad::AppRegistry::getInstance().registerApp(kit_selector_get_entry());
}

#endif // USE_LVGL

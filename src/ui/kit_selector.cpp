// SPDX-License-Identifier: MIT

#ifdef USE_LVGL

#include <crosspad-sampler/ui/kit_selector.hpp>

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/platform/PlatformServices.hpp>

namespace crosspad_sampler {

// ── Static state ─────────────────────────────────────────────────────────────

static KitSelectedFn s_callback  = nullptr;
static void*         s_userData  = nullptr;

// ── Internal event handler ────────────────────────────────────────────────────

static void on_kit_btn_clicked(lv_event_t* e)
{
    if (!s_callback) {
        return;
    }
    // Kit id was stored as userdata via (void*)(intptr_t)i
    int kitId = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    s_callback(kitId, s_userData);
}

// ── Public API ────────────────────────────────────────────────────────────────

lv_obj_t* kit_selector_create(lv_obj_t* parent)
{
    // Full-screen overlay
    lv_obj_t* overlay = lv_obj_create(parent);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));

    // Dark semi-transparent background
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, LV_PART_MAIN);

    // Flex column layout with padding
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(overlay, 8, LV_PART_MAIN);

    // Title label (index 0 — must remain first child)
    lv_obj_t* title = lv_label_create(overlay);
    lv_label_set_text(title, "Select Kit");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    // Populate kit buttons
    kit_selector_refresh(overlay);

    return overlay;
}

void kit_selector_destroy(lv_obj_t* selector)
{
    lv_obj_delete_async(selector);
}

void kit_selector_refresh(lv_obj_t* selector)
{
    // Remove all children except the title at index 0
    uint32_t childCount = lv_obj_get_child_count(selector);
    for (uint32_t i = childCount; i > 1; --i) {
        lv_obj_t* child = lv_obj_get_child(selector, static_cast<int32_t>(i - 1));
        lv_obj_delete(child);
    }

    crosspad::IKitManager* kitManager = crosspad::getKitManager();
    if (!kitManager) {
        return;
    }

    int kitCount      = kitManager->getKitCount();
    int currentKitId  = kitManager->getCurrentKitId();

    for (int i = 0; i < kitCount; ++i) {
        crosspad::KitInfo* kit = kitManager->getKit(i);
        if (!kit) {
            continue;
        }

        lv_obj_t* btn = lv_button_create(selector);
        lv_obj_set_width(btn, LV_PCT(100));

        // Highlight the active kit
        if (i == currentKitId) {
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x004488), LV_PART_MAIN);
        }

        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, kit->name.c_str());

        lv_obj_add_event_cb(btn, on_kit_btn_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }
}

void kit_selector_set_callback(KitSelectedFn fn, void* userData)
{
    s_callback = fn;
    s_userData = userData;
}

} // namespace crosspad_sampler

#endif // USE_LVGL

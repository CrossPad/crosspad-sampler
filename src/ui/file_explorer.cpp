// SPDX-License-Identifier: MIT

#ifdef USE_LVGL

#include <crosspad-sampler/ui/file_explorer.hpp>
#include <cstdint>
#include <cstring>
#include <lvgl.h>

namespace crosspad_sampler {

// ── Static state ────────────────────────────────────────────────────────

static FileListProvider s_provider    = nullptr;
static FileSelectedFn   s_callback    = nullptr;
static void*            s_userData    = nullptr;
static lv_obj_t*        s_fileList    = nullptr;
static char             s_currentPath[256] = "/crosspad/kits";

// ── Internal callbacks ──────────────────────────────────────────────────

static void on_file_clicked(lv_event_t* e)
{
    const char* filePath = static_cast<const char*>(lv_event_get_user_data(e));
    if (s_callback && filePath) {
        s_callback(filePath, s_userData);
    }
}

static void on_file_list_result(const char** entries, uint32_t count, void* ctx)
{
    (void)ctx;

    if (!s_fileList) {
        return;
    }

    lv_obj_clean(s_fileList);

    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* btn = lv_list_add_button(s_fileList, LV_SYMBOL_FILE, entries[i]);
        if (btn) {
            // Store file path as user data; entries[i] lifetime is managed by the caller.
            lv_obj_add_event_cb(btn, on_file_clicked, LV_EVENT_CLICKED,
                                const_cast<void*>(static_cast<const void*>(entries[i])));
        }
    }
}

// ── Public API ──────────────────────────────────────────────────────────

void file_explorer_set_provider(FileListProvider fn)
{
    s_provider = fn;
}

void file_explorer_set_callback(FileSelectedFn fn, void* userData)
{
    s_callback = fn;
    s_userData = userData;
}

lv_obj_t* file_explorer_create(lv_obj_t* parent, const char* initialPath)
{
    if (initialPath && initialPath[0] != '\0') {
        strncpy(s_currentPath, initialPath, sizeof(s_currentPath) - 1);
        s_currentPath[sizeof(s_currentPath) - 1] = '\0';
    }

    lv_obj_t* explorer = lv_obj_create(parent);
    lv_obj_set_size(explorer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(explorer, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(explorer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(explorer, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(explorer, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* pathLabel = lv_label_create(explorer);
    lv_obj_set_style_text_color(pathLabel, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_label_set_text(pathLabel, s_currentPath);

    s_fileList = lv_list_create(explorer);
    lv_obj_set_width(s_fileList, LV_PCT(100));
    lv_obj_set_flex_grow(s_fileList, 1);

    if (s_provider) {
        s_provider(s_currentPath, on_file_list_result, nullptr);
    } else {
        lv_list_add_text(s_fileList, "(no file provider)");
    }

    return explorer;
}

void file_explorer_destroy(lv_obj_t* explorer)
{
    s_fileList = nullptr;
    if (explorer) {
        lv_obj_delete_async(explorer);
    }
}

} // namespace crosspad_sampler

#endif // USE_LVGL

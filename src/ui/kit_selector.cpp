// SPDX-License-Identifier: MIT
//
// Kit browser — cover on the left, one row per kit on the right.
//
// Everything shown comes from IKitManager: the rows are its kit list, the
// artwork is what a kit ships beside its JSON (cover.png 122x122, thumb.png
// 20x20), and a kit without any gets a lettered chip. Loading goes through the
// same platform callback as every other way into a kit change.

#ifdef USE_LVGL

#include <crosspad-sampler/ui/kit_selector.hpp>
#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>
#include <crosspad-sampler/logic/kit_load_task.hpp>

#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad-gui/crosspad_gui.h>
#include <lvgl_private.h>   // lv_image_decoder_dsc_t, for warming the image cache

#include <cstdio>
#include <string>
#include <vector>

namespace crosspad_sampler {

namespace {

constexpr int32_t kStatusH   = 24;
constexpr int32_t kRowH      = 30;
constexpr int32_t kCoverSize = 122;
constexpr int32_t kThumbSize = 20;

/* The cover is a 122x122 PNG read off the card and decoded on the LVGL
 * thread. It is loaded only once the encoder has been still for this long: a
 * flick through the list never pays for covers it scrolled past. */
constexpr uint32_t kCoverDelayMs = 750;
/* Row thumbnails are decoded into LVGL's image cache one per tick by a timer,
 * nearest to the focused row first, and a row shows its initial until its
 * thumbnail is cached. Decoding at draw time -- the old way -- put an SD read
 * and a PNG decode on every row that scrolled into view. */
constexpr uint32_t kThumbTickMs = 10;

struct Row {
    lv_obj_t* obj    = nullptr;
    lv_obj_t* name   = nullptr;
    lv_obj_t* author = nullptr;
    lv_obj_t* thumb  = nullptr;   ///< image, hidden until its PNG is in the cache
    lv_obj_t* letter = nullptr;   ///< the initial, shown until then (or for good)
    std::string thumbPath;
    bool thumbTried = false;
};

KitSelectedFn s_onSelected = nullptr;
KitLoadedFn   s_onLoaded   = nullptr;
KitClosedFn   s_onClose    = nullptr;
void*         s_userData   = nullptr;

lv_obj_t*    s_overlay     = nullptr;
lv_obj_t*    s_list        = nullptr;
lv_obj_t*    s_coverBox    = nullptr;
lv_obj_t*    s_coverImg    = nullptr;
lv_obj_t*    s_coverLetter = nullptr;
lv_obj_t*    s_coverName   = nullptr;
lv_obj_t*    s_coverAuthor = nullptr;
lv_obj_t*    s_statusLabel = nullptr;
lv_obj_t*    s_backBtn     = nullptr;
lv_obj_t*    s_busy        = nullptr;

lv_group_t*  s_group       = nullptr;
lv_group_t*  s_prevGroup   = nullptr;

std::vector<Row> s_rows;
int  s_selected  = -1;   ///< row under the highlight
int  s_loaded    = -1;   ///< row carrying the "this one is playing" edge
int  s_coverWanted = -1;
int  s_coverShown  = -1;

/* lv_group_add_obj() fires LV_EVENT_FOCUSED synchronously for its first
 * object, landing in row_focused_cb() mid-assembly. Focus callbacks are
 * ignored until the list is complete, and again during teardown. */
bool s_building = false;
bool s_loadingStarted = false;

lv_timer_t* s_coverTimer = nullptr;
lv_timer_t* s_loadTimer  = nullptr;
lv_timer_t* s_thumbTimer = nullptr;

lv_indev_t* encoder() {
    return crosspad_gui::getGuiPlatform().getNavigationEncoder();
}

bool fileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/// First character of the kit name — the whole UTF-8 sequence, so a name that
/// starts with a multibyte glyph does not become a broken byte on the chip.
const char* initialOf(const crosspad::KitInfo& kit) {
    static char buf[5];
    size_t n = 0;
    if (!kit.name.empty()) {
        buf[n++] = kit.name[0];
        while (n < 4 && n < kit.name.size() && (kit.name[n] & 0xC0) == 0x80) {
            buf[n] = kit.name[n];
            ++n;
        }
    } else {
        buf[n++] = '?';
    }
    buf[n] = '\0';
    return buf;
}

// ── Thumbnails ───────────────────────────────────────────────────────────────

/// The pending row closest to the highlight, or -1 when every row is done.
int thumb_next_pending() {
    const int n = (int)s_rows.size();
    if (n == 0) return -1;
    const int centre = (s_selected >= 0 && s_selected < n) ? s_selected : 0;
    for (int d = 0; d < n; ++d) {
        const int below = centre + d;
        const int above = centre - d;
        if (below < n && !s_rows[below].thumbTried) return below;
        if (above >= 0 && !s_rows[above].thumbTried) return above;
    }
    return -1;
}

void thumb_timer_cb(lv_timer_t*) {
    const int idx = thumb_next_pending();
    if (idx < 0) {
        lv_timer_pause(s_thumbTimer);
        return;
    }
    Row& row = s_rows[idx];
    row.thumbTried = true;
    if (!row.thumb || row.thumbPath.empty()) return;
    /* open+close leaves the decoded bitmap in the image cache, so the draw
     * that follows is a blit. A missing or broken file keeps the initial. */
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, row.thumbPath.c_str(), nullptr) != LV_RESULT_OK) return;
    lv_image_decoder_close(&dsc);
    lv_image_set_src(row.thumb, row.thumbPath.c_str());
    lv_obj_remove_flag(row.thumb, LV_OBJ_FLAG_HIDDEN);
    if (row.letter) lv_obj_add_flag(row.letter, LV_OBJ_FLAG_HIDDEN);
}

/// Keep the warmer running; it pauses itself once nothing is pending.
void thumb_warm() {
    if (!s_thumbTimer) s_thumbTimer = lv_timer_create(thumb_timer_cb, kThumbTickMs, nullptr);
    lv_timer_resume(s_thumbTimer);
}

// ── Cover pane ───────────────────────────────────────────────────────────────

void cover_show(int idx) {
    auto* mgr = crosspad::getKitManager();
    auto* kit = mgr ? mgr->getKit(idx) : nullptr;
    if (!kit || !s_coverImg || !s_coverLetter) return;

    const std::string path = crosspad::kitAssetPath(kit->path, "cover.png");
    if (fileExists(path)) {
        lv_image_set_src(s_coverImg, path.c_str());
        lv_obj_remove_flag(s_coverImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_coverLetter, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_coverLetter, initialOf(*kit));
        lv_obj_add_flag(s_coverImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_coverLetter, LV_OBJ_FLAG_HIDDEN);
    }
    s_coverShown = idx;
}

void cover_timer_cb(lv_timer_t*) {
    lv_timer_pause(s_coverTimer);
    if (s_coverWanted < 0 || s_coverWanted == s_coverShown) return;
    cover_show(s_coverWanted);
}

void cover_request(int idx) {
    s_coverWanted = idx;
    if (!s_coverTimer) s_coverTimer = lv_timer_create(cover_timer_cb, kCoverDelayMs, nullptr);
    /* Restart the countdown: while the encoder keeps turning this keeps
     * pushing the decode out instead of queueing one per step. */
    lv_timer_reset(s_coverTimer);
    lv_timer_resume(s_coverTimer);
}

// ── Selection ────────────────────────────────────────────────────────────────

void row_set_selected(int idx, bool selected) {
    if (idx < 0 || idx >= (int)s_rows.size() || !s_rows[idx].obj) return;
    Row& r = s_rows[idx];
    /* A state flip, not lv_obj_add_style(): adding or removing a style
     * refreshes every property and marks the flex column's layout dirty,
     * which re-measured all rows (two DOT labels each) on every detent. */
    if (selected) {
        lv_obj_add_state(r.obj, LV_STATE_CHECKED);
        lv_obj_set_style_text_opa(r.author, LV_OPA_COVER, 0);
    } else {
        lv_obj_remove_state(r.obj, LV_STATE_CHECKED);
        lv_obj_set_style_text_opa(r.author, LV_OPA_50, 0);
    }
}

/// The loaded kit keeps a primary-coloured edge, so it stays readable while
/// the selection highlight moves elsewhere.
void row_mark_loaded(int idx) {
    if (s_loaded >= 0 && s_loaded < (int)s_rows.size() && s_rows[s_loaded].obj) {
        lv_obj_set_style_border_width(s_rows[s_loaded].obj, 0, 0);
    }
    s_loaded = idx;
    if (idx < 0 || idx >= (int)s_rows.size() || !s_rows[idx].obj) return;
    lv_obj_t* row = s_rows[idx].obj;
    lv_obj_set_style_border_color(row, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(row, 3, 0);
}

void select_row(int idx) {
    if (s_building || idx < 0 || idx >= (int)s_rows.size()) return;

    row_set_selected(s_selected, false);
    s_selected = idx;
    row_set_selected(s_selected, true);

    auto* mgr = crosspad::getKitManager();
    auto* kit = mgr ? mgr->getKit(idx) : nullptr;
    if (kit) {
        lv_label_set_text(s_coverName, kit->name.c_str());
        lv_label_set_text(s_coverAuthor, kit->author.c_str());
    }
    lv_obj_scroll_to_view(s_rows[idx].obj, LV_ANIM_OFF);
    cover_request(idx);
    thumb_warm();
}

// ── Loading ──────────────────────────────────────────────────────────────────

/* Hidden rather than deleted between loads: deleting would race the app
 * teardown, which drops the whole tree asynchronously. */
void busy_show() {
    if (!s_coverBox) return;
    if (!s_busy) {
        s_busy = lv_obj_create(s_coverBox);
        lv_obj_remove_style_all(s_busy);
        lv_obj_set_size(s_busy, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_busy, CROSSPAD_THEME_BG_DEEP, 0);
        lv_obj_set_style_bg_opa(s_busy, LV_OPA_70, 0);
        lv_obj_remove_flag(s_busy, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_busy, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* sp = lv_spinner_create(s_busy);
        lv_obj_set_size(sp, 44, 44);
        lv_obj_center(sp);
        lv_obj_set_style_arc_color(sp, CROSSPAD_THEME_CARD_DEEP, 0);
        lv_obj_set_style_arc_color(sp, CROSSPAD_THEME_PRIMARY, LV_PART_INDICATOR);
    }
    lv_obj_remove_flag(s_busy, LV_OBJ_FLAG_HIDDEN);
}

void busy_hide() {
    if (s_busy) lv_obj_add_flag(s_busy, LV_OBJ_FLAG_HIDDEN);
}

void load_kit(int kitId) {
    auto* mgr = crosspad::getKitManager();
    auto* kit = mgr ? mgr->getKit(kitId) : nullptr;
    if (!kit) return;

    const auto& cb = sampler_get_platform_callbacks();
    if (!cb.audio.startLoadKit) return;

    // Platform pre-load: stop the sequencer, drop the old waveform cache.
    if (s_onSelected) s_onSelected(kitId, s_userData);

    cb.audio.startLoadKit(kitId, false);
    s_loadingStarted = true;
    busy_show();
    row_mark_loaded(kitId);
    if (s_statusLabel) lv_label_set_text(s_statusLabel, kit->name.c_str());
}

void load_poll_cb(lv_timer_t*) {
    if (!s_loadingStarted || kit_load_task_is_loading()) return;
    s_loadingStarted = false;
    busy_hide();
    // The host owns teardown from here (KitSelectorApp destroys the App).
    if (s_onLoaded) s_onLoaded(s_userData);
}

// ── Events ───────────────────────────────────────────────────────────────────

void row_focused_cb(lv_event_t* e) {
    select_row((int)(intptr_t)lv_event_get_user_data(e));
}

/* Encoder press and touch both land here, and both load on the first click:
 * requiring a second press made touch feel like the first tap did nothing. */
void row_clicked_cb(lv_event_t* e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    select_row(idx);
    load_kit(idx);
}

void back_clicked_cb(lv_event_t*) {
    if (s_onClose) {
        s_onClose(s_userData);
        return;
    }
    if (s_overlay) kit_selector_destroy(s_overlay);
}

// ── Building ─────────────────────────────────────────────────────────────────

void build_row(int idx, const crosspad::KitInfo& kit) {
    void* tag = (void*)(intptr_t)idx;

    lv_obj_t* row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &styleKitBrowserRow, 0);
    lv_obj_add_style(row, &styleKitBrowserRowSelected, LV_STATE_CHECKED);
    lv_obj_set_size(row, LV_PCT(100), kRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, tag);
    lv_obj_add_event_cb(row, row_focused_cb, LV_EVENT_FOCUSED, tag);
    if (s_group) lv_group_add_obj(s_group, row);

    lv_obj_t* thumbBox = lv_obj_create(row);
    lv_obj_remove_style_all(thumbBox);
    lv_obj_add_style(thumbBox, &styleKitBrowserThumb, 0);
    lv_obj_set_size(thumbBox, kThumbSize, kThumbSize);
    lv_obj_remove_flag(thumbBox, LV_OBJ_FLAG_SCROLLABLE);

    /* No file access here: the initial goes up now, the thumbnail once the
     * warmer has it cached. Building 80 rows used to cost 80 fopen()s. */
    lv_obj_t* letter = lv_label_create(thumbBox);
    lv_label_set_text(letter, initialOf(kit));
    lv_obj_center(letter);
    lv_obj_t* img = lv_image_create(thumbBox);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(img);

    lv_obj_t* texts = lv_obj_create(row);
    lv_obj_remove_style_all(texts);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_height(texts, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(texts, LV_OBJ_FLAG_SCROLLABLE);

    /* One-line fixed height each: DOT only truncates when the height cannot
     * grow, and a wrapped name would overflow the 30 px row. */
    lv_obj_t* name = lv_label_create(texts);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(name, LV_PCT(100), lv_font_get_line_height(&lv_font_montserrat_10));
    lv_label_set_text(name, kit.name.c_str());

    lv_obj_t* author = lv_label_create(texts);
    lv_obj_set_style_text_font(author, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_opa(author, LV_OPA_50, 0);
    lv_label_set_long_mode(author, LV_LABEL_LONG_DOT);
    lv_obj_set_size(author, LV_PCT(100), lv_font_get_line_height(&lv_font_montserrat_10));
    lv_label_set_text(author, kit.author.c_str());

    Row r;
    r.obj = row; r.name = name; r.author = author;
    r.thumb = img; r.letter = letter;
    r.thumbPath = crosspad::kitAssetPath(kit.path, "thumb.png");
    s_rows.push_back(std::move(r));
}

void build_rows() {
    auto* mgr = crosspad::getKitManager();
    const int count = mgr ? mgr->getKitCount() : 0;

    s_building = true;
    s_rows.clear();
    s_rows.reserve(count);
    s_selected = -1;
    s_loaded   = -1;
    s_coverShown = -1;

    for (int i = 0; i < count; ++i) {
        if (auto* kit = mgr->getKit(i)) build_row(i, *kit);
    }
    s_building = false;

    if (s_statusLabel) lv_label_set_text_fmt(s_statusLabel, "%d kits", (int)s_rows.size());

    if (s_rows.empty()) {
        lv_obj_t* msg = lv_label_create(s_list);
        lv_label_set_text(msg, "No kits found\non the SD card");
        lv_obj_set_style_text_opa(msg, LV_OPA_50, 0);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(msg);
        return;
    }

    const int current = mgr->getCurrentKitId();
    if (current >= 0 && current < (int)s_rows.size()) row_mark_loaded(current);

    /* Back holds focus by default, having been added first. Focus the list
     * instead, which also puts Back one notch counter-clockwise of the top. */
    const int first = current >= 0 && current < (int)s_rows.size() ? current : 0;
    lv_group_focus_obj(s_rows[first].obj);
    select_row(first);
    cover_show(first);
    thumb_warm();
}

void reset_state();
void overlay_deleted_cb(lv_event_t* e);

} // namespace

// ── Public API ───────────────────────────────────────────────────────────────

lv_obj_t* kit_selector_create(const KitSelectorConfig& config) {
    s_onSelected = config.onSelected;
    s_onLoaded   = config.onLoaded;
    s_onClose    = config.onClose;
    s_userData   = config.userData;
    s_loadingStarted = false;
    s_coverWanted = s_coverShown = -1;

    lv_obj_t* scr = lv_obj_create(config.parent);
    lv_obj_add_style(scr, &styleKitBrowserScreen, 0);
    lv_obj_set_size(scr, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /* Status bar on top, body filling whatever height the host container
     * actually has — hardcoding CP_LCD_VER_RES here overflowed the app
     * container, which sits below the system status bar. */
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scr, 0, 0);
    s_overlay = scr;
    lv_obj_add_event_cb(scr, overlay_deleted_cb, LV_EVENT_DELETE, nullptr);

    s_group = lv_group_create();
    if (lv_indev_t* enc = encoder()) {
        s_prevGroup = lv_indev_get_group(enc);
        lv_indev_set_group(enc, s_group);
    }

    // ── Status bar ──────────────────────────────────────────────────────────
    lv_obj_t* status = lv_obj_create(scr);
    lv_obj_add_style(status, &styleKitBrowserStatus, 0);
    lv_obj_set_size(status, LV_PCT(100), kStatusH);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_SCROLLABLE);

    /* The title doubles as the way out: the browser owns the encoder while it
     * is open, so without a focusable control here nothing leaves. */
    s_backBtn = lv_obj_create(status);
    lv_obj_remove_style_all(s_backBtn);
    lv_obj_set_size(s_backBtn, 78, kStatusH - 5);
    lv_obj_align(s_backBtn, LV_ALIGN_LEFT_MID, -3, 0);
    lv_obj_set_style_radius(s_backBtn, 5, 0);
    lv_obj_add_style(s_backBtn, &styleKitBrowserRowSelected, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(s_backBtn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_remove_flag(s_backBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_backBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_backBtn, back_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_group_add_obj(s_group, s_backBtn);

    lv_obj_t* backLabel = lv_label_create(s_backBtn);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT "  KITS");
    lv_obj_set_style_text_letter_space(backLabel, 1, 0);
    lv_obj_center(backLabel);

    s_statusLabel = lv_label_create(status);
    lv_obj_set_style_text_font(s_statusLabel, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_opa(s_statusLabel, LV_OPA_50, 0);
    lv_obj_align(s_statusLabel, LV_ALIGN_RIGHT_MID, 0, 0);

    // ── Body: cover | list ──────────────────────────────────────────────────
    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_column(body, 10, 0);

    lv_obj_t* left = lv_obj_create(body);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, kCoverSize, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    /* Centre the stack on the main axis; the cross axis stays at START so the
     * captions line up with the cover's left edge. */
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(left, 7, 0);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    s_coverBox = lv_obj_create(left);
    lv_obj_remove_style_all(s_coverBox);
    lv_obj_add_style(s_coverBox, &styleKitBrowserCover, 0);
    lv_obj_set_size(s_coverBox, kCoverSize, kCoverSize);
    lv_obj_remove_flag(s_coverBox, LV_OBJ_FLAG_SCROLLABLE);

    s_coverImg = lv_image_create(s_coverBox);
    lv_obj_center(s_coverImg);
    lv_obj_add_flag(s_coverImg, LV_OBJ_FLAG_HIDDEN);

    s_coverLetter = lv_label_create(s_coverBox);
    lv_label_set_text(s_coverLetter, "");
    lv_obj_center(s_coverLetter);

    /* Fixed one-line height: with LV_SIZE_CONTENT a long name wraps instead
     * of truncating, growing the column and pushing the cover off the top. */
    s_coverName = lv_label_create(left);
    lv_obj_set_style_text_font(s_coverName, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_coverName, lv_color_white(), 0);
    lv_label_set_long_mode(s_coverName, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_coverName, kCoverSize, lv_font_get_line_height(&lv_font_montserrat_14));
    lv_label_set_text(s_coverName, "");

    s_coverAuthor = lv_label_create(left);
    lv_obj_set_style_text_font(s_coverAuthor, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_coverAuthor, CROSSPAD_THEME_PRIMARY, 0);
    lv_label_set_long_mode(s_coverAuthor, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_coverAuthor, kCoverSize, lv_font_get_line_height(&lv_font_montserrat_10));
    lv_label_set_text(s_coverAuthor, "");

    s_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_height(s_list, LV_PCT(100));
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 5, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    build_rows();

    s_loadTimer = lv_timer_create(load_poll_cb, 100, nullptr);
    return scr;
}

namespace {

/* Everything that must not outlive the widget tree: both lv_timers and every
 * static handle into it. Shared with the LV_EVENT_DELETE fallback below, so a
 * host that drops the tree without calling kit_selector_destroy cannot leave
 * the cover timer writing into freed memory. */
void reset_state() {
    /* Stop answering focus events first: deleting a focused row refocuses
     * another, into widgets that are part of the same teardown. */
    s_building = true;
    s_loadingStarted = false;

    if (s_loadTimer)  { lv_timer_delete(s_loadTimer);  s_loadTimer = nullptr; }
    if (s_coverTimer) { lv_timer_delete(s_coverTimer); s_coverTimer = nullptr; }
    if (s_thumbTimer) { lv_timer_delete(s_thumbTimer); s_thumbTimer = nullptr; }

    // Restore the previous encoder group (the launcher's) before deleting ours
    if (lv_indev_t* enc = encoder()) {
        if (lv_indev_get_group(enc) == s_group) lv_indev_set_group(enc, s_prevGroup);
    }
    s_prevGroup = nullptr;
    if (s_group) { lv_group_delete(s_group); s_group = nullptr; }

    s_rows.clear();
    s_selected = s_loaded = -1;
    s_coverWanted = s_coverShown = -1;
    s_overlay = s_list = s_coverBox = s_coverImg = s_coverLetter = nullptr;
    s_coverName = s_coverAuthor = s_statusLabel = s_backBtn = s_busy = nullptr;
}

void overlay_deleted_cb(lv_event_t* e) {
    if (lv_event_get_target_obj(e) == s_overlay) reset_state();
}

} // namespace

void kit_selector_destroy(lv_obj_t* selector) {
    reset_state();
    lv_obj_delete_async(selector);
}

void kit_selector_refresh(lv_obj_t* selector) {
    if (selector != s_overlay || !s_list) return;
    s_building = true;
    lv_obj_clean(s_list);
    s_busy = nullptr;
    build_rows();
}

void kit_selector_set_callback(KitSelectedFn fn, void* userData) {
    s_onSelected = fn;
    s_userData = userData;
}

bool kit_selector_is_open() {
    return s_overlay != nullptr;
}

void kit_selector_close() {
    if (!s_overlay) return;
    if (s_onClose) {
        s_onClose(s_userData);
        return;
    }
    kit_selector_destroy(s_overlay);
}

} // namespace crosspad_sampler

#endif // USE_LVGL

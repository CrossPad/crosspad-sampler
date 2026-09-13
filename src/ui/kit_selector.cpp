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
constexpr int32_t kRowGap    = 5;
constexpr int32_t kRowPitch  = kRowH + kRowGap;
constexpr int32_t kCoverSize = 122;
constexpr int32_t kThumbSize = 20;
/* The gutter between the cover and the list, holding nothing but the caret.
 * It narrows the rows by its own width plus one column gap; the caret has to
 * sit outside the list because the list scrolls and the caret does not. */
constexpr int32_t kCaretW    = 14;

/* The list is virtual: every kit has a Row entry, but only the rows within
 * this many of the highlight have LVGL objects. A row is ~3 kB of PSRAM and
 * ~50 allocations (seven widgets, their styles, texts and event lists); 79
 * of them were 237 kB, most of what the launcher leaves outside the sample
 * engine's arena, and the kit load that followed had no room for its FFT.
 * Rows are placed at idx * kRowPitch, so the scroll extent, the thumbnail
 * warmer and the encoder order never depend on which rows exist. */
constexpr int kRowWindow = 8;

/* The cover is a 122x122 PNG read off the card and decoded on the LVGL
 * thread. It is loaded only once the encoder has been still for this long: a
 * flick through the list never pays for covers it scrolled past. */
constexpr uint32_t kCoverDelayMs = 750;
/* Row thumbnails are decoded into LVGL's image cache one per tick by a timer,
 * nearest to the focused row first, and a row shows its initial until its
 * thumbnail is cached. Decoding at draw time -- the old way -- put an SD read
 * and a PNG decode on every row that scrolled into view. */
constexpr uint32_t kThumbTickMs = 10;
/* ...but not while the list is moving. Each tick is an SD read and a PNG
 * decode on the LVGL thread, and with the encoder turning that lands between
 * every two frames of the scroll: the list stuttered its way down the kits and
 * the decodes were mostly of rows nobody stopped on. The warmer waits for the
 * selection to stand still this long instead. */
constexpr uint32_t kThumbQuietMs = 350;

/* How far the selection may drift inside the built window before the window
 * moves with it. Sliding by one row per detent meant building and destroying a
 * row -- seven widgets and some fifty allocations -- on every click. */
constexpr int kRowSlack = 4;

/* Seventy kits at one row per click is a seventy-click job, and every click
 * costs a repaint whatever the row count -- so the way to cross the list
 * quickly is to cover more ground per repaint, not to repaint faster. Clicks
 * arriving inside this window step by twice as many rows as the last one, up
 * to kMaxStep; a click on its own always moves exactly one. */
constexpr uint32_t kFastStepMs = 140;
constexpr int      kMaxStep    = 8;

struct Row {
    lv_obj_t* obj    = nullptr;   ///< null while the row is outside the window
    lv_obj_t* name   = nullptr;
    lv_obj_t* author = nullptr;
    lv_obj_t* thumb  = nullptr;   ///< image, hidden until its PNG is in the cache
    lv_obj_t* letter = nullptr;   ///< the initial, shown until then (or for good)
    std::string thumbPath;
    bool thumbTried = false;      ///< for the built row; reset when it is rebuilt
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
lv_obj_t*    s_caret       = nullptr;   ///< the grot beside the row the cover belongs to
lv_obj_t*    s_frame       = nullptr;   ///< the fixed selection frame, over the first row slot
lv_obj_t*    s_moreCard    = nullptr;   ///< where-to-get-kits note, past the last row

lv_group_t*  s_group       = nullptr;
lv_group_t*  s_prevGroup   = nullptr;

std::vector<Row> s_rows;
int  s_winFirst  = 0;    ///< rows [s_winFirst, s_winLast] have objects
int  s_winLast   = -1;
int  s_selected  = -1;   ///< row under the highlight
int  s_loaded    = -1;   ///< row carrying the "this one is playing" edge
int  s_coverWanted = -1;
int  s_coverShown  = -1;

/* lv_group_add_obj() fires LV_EVENT_FOCUSED synchronously for its first
 * object, landing in row_focused_cb() mid-assembly. Focus callbacks are
 * ignored until the list is complete, and again during teardown. */
uint32_t s_backToken = 0;   ///< power-button back registration
uint32_t s_lastStepMs = 0;  ///< when the selection last moved by a click
int      s_lastStepDir = 0;
int      s_stepRun = 1;     ///< rows per click, doubling while the spin holds
bool     s_refocusing = false;
bool s_building = false;
bool s_loadingStarted = false;

lv_timer_t* s_coverTimer = nullptr;
lv_timer_t* s_loadTimer  = nullptr;
lv_timer_t* s_thumbTimer = nullptr;
lv_timer_t* s_thumbQuietTimer = nullptr;
std::string s_coverPathShown;   ///< what the cover image is decoded from

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

/// The pending row closest to the highlight, or -1 when every visible row is
/// done.
///
/// On screen only, plus a row of margin. Warming the whole built window meant a
/// dozen decodes -- and a dozen row repaints -- for rows nobody had scrolled
/// to, and each one pushed something else out of LVGL's image cache, which the
/// visible rows then had to decode again at draw time, inside the refresh.
int thumb_next_pending() {
    const int n = (int)s_rows.size();
    if (n == 0 || !s_list) return -1;
    const int32_t y = lv_obj_get_scroll_y(s_list);
    const int32_t h = lv_obj_get_height(s_list);
    int first = (int)(y / kRowPitch) - 1;
    int last  = (int)((y + h) / kRowPitch) + 1;
    if (first < 0) first = 0;
    if (last >= n) last = n - 1;
    if (last < first) return -1;
    const int centre = (s_selected >= first && s_selected <= last) ? s_selected : first;
    for (int d = 0; d <= last - first; ++d) {
        const int below = centre + d;
        const int above = centre - d;
        if (below <= last && s_rows[below].obj && !s_rows[below].thumbTried) return below;
        if (above >= first && s_rows[above].obj && !s_rows[above].thumbTried) return above;
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

void thumb_quiet_cb(lv_timer_t*) {
    if (s_thumbQuietTimer) lv_timer_pause(s_thumbQuietTimer);
    thumb_warm();
}

/// Decoding stops now and starts again once the list has stood still. Called
/// from everything that moves the selection.
void thumb_defer() {
    if (s_thumbTimer) lv_timer_pause(s_thumbTimer);
    if (!s_thumbQuietTimer) {
        s_thumbQuietTimer = lv_timer_create(thumb_quiet_cb, kThumbQuietMs, nullptr);
    }
    lv_timer_reset(s_thumbQuietTimer);
    lv_timer_resume(s_thumbQuietTimer);
}

// ── Cover pane ───────────────────────────────────────────────────────────────

/* One cover in the image cache at a time. A decoded 122x122 is ~59 kB of the
 * 192 kB LVGL keeps for images, so a few of them evict every row thumbnail --
 * and the next scroll decodes those again at draw time, inside the refresh,
 * which is what made a single click of the encoder take a third of a second. */
void cover_show(int idx) {
    auto* mgr = crosspad::getKitManager();
    auto* kit = mgr ? mgr->getKit(idx) : nullptr;
    if (!kit || !s_coverImg || !s_coverLetter) return;

    const std::string path = crosspad::kitAssetPath(kit->path, "cover.png");
    const std::string previous = s_coverPathShown;
    if (fileExists(path)) {
        /* Decode here, not at draw time. Setting the source alone leaves the
         * PNG to be read and decoded inside the next refresh -- one frame in
         * the middle of a scroll taking over a tenth of a second, which is
         * felt as the list catching. The same open+close the thumbnails use. */
        lv_image_decoder_dsc_t dsc;
        if (lv_image_decoder_open(&dsc, path.c_str(), nullptr) == LV_RESULT_OK) {
            lv_image_decoder_close(&dsc);
        }
        lv_image_set_src(s_coverImg, path.c_str());
        s_coverPathShown = path;
        lv_obj_remove_flag(s_coverImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_coverLetter, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_coverLetter, initialOf(*kit));
        lv_obj_add_flag(s_coverImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_coverLetter, LV_OBJ_FLAG_HIDDEN);
        s_coverPathShown.clear();
    }
    /* After the swap, never before: dropping what is still on screen only buys
     * the decode back. */
    if (!previous.empty() && previous != s_coverPathShown) {
        lv_image_cache_drop(previous.c_str());
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

/// Put the frame and the caret on the selected row, wherever the list is.
///
/// They used to be nailed to the first row slot and the list was scrolled to
/// bring the selection under them -- which repainted every visible row, their
/// thumbnails and their twelve labels, for a selection that moved by one. The
/// marker moving instead repaints the row it leaves and the row it lands on.
void selection_mark() {
    if (s_selected < 0 || !s_list) return;
    const int32_t off = (int32_t)s_selected * kRowPitch - lv_obj_get_scroll_y(s_list);
    if (s_frame) lv_obj_set_pos(s_frame, lv_obj_get_x(s_list), lv_obj_get_y(s_list) + off);
    if (s_caret) {
        const int32_t h = lv_font_get_line_height(&lv_font_montserrat_14);
        lv_obj_set_y(s_caret, off + (kRowH - h) / 2);
        lv_obj_remove_flag(s_caret, LV_OBJ_FLAG_HIDDEN);
    }
}

/// First and last row fully inside the viewport.
void rows_in_view(int& first, int& last) {
    const int n = (int)s_rows.size();
    const int32_t y = lv_obj_get_scroll_y(s_list);
    const int32_t h = lv_obj_get_height(s_list);
    first = (int)((y + kRowPitch - 1) / kRowPitch);
    last  = (int)((y + h) / kRowPitch) - 1;
    if (first < 0) first = 0;
    if (last >= n) last = n - 1;
    if (last < first) last = first;
}

/* select_row() scrolls, and scrolling re-enters through LV_EVENT_SCROLL. The
 * handler reads the row under the caret and would select it again — same index,
 * but the round trip fights an animation and re-runs the window work. */
bool s_scrolling = false;

void row_apply_loaded_edge(lv_obj_t* row) {
    lv_obj_set_style_border_color(row, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(row, 3, 0);
}

/// The loaded kit keeps a primary-coloured edge, so it stays readable while
/// the selection highlight moves elsewhere.
void row_mark_loaded(int idx) {
    if (s_loaded >= 0 && s_loaded < (int)s_rows.size() && s_rows[s_loaded].obj) {
        lv_obj_set_style_border_width(s_rows[s_loaded].obj, 0, 0);
    }
    s_loaded = idx;
    if (idx < 0 || idx >= (int)s_rows.size() || !s_rows[idx].obj) return;
    row_apply_loaded_edge(s_rows[idx].obj);
}

void build_row(int idx);
void destroy_row(int idx);
void group_reorder();

/// Give rows [centre - kRowWindow, centre + kRowWindow] objects and take them
/// from every other row. Focus callbacks are muted meanwhile: creating a row
/// into the group and deleting a focused one both fire them.
void ensure_window(int centre) {
    const int n = (int)s_rows.size();
    if (n == 0) return;
    if (centre < 0) centre = 0;
    if (centre >= n) centre = n - 1;
    int first = centre - kRowWindow;
    int last  = centre + kRowWindow;
    if (first < 0) first = 0;
    if (last >= n) last = n - 1;
    if (first == s_winFirst && last == s_winLast) return;

    /* Still comfortably inside what is built -- including at the ends of the
     * list, where the clamp is what keeps the selection away from an edge the
     * window cannot pass. Nothing to do until it nears a real edge. */
    const int needFirst = centre - kRowSlack < 0 ? 0 : centre - kRowSlack;
    const int needLast  = centre + kRowSlack >= n ? n - 1 : centre + kRowSlack;
    if (s_winLast >= s_winFirst && s_winFirst <= needFirst && s_winLast >= needLast) return;

    const int prevFirst = s_winFirst;
    const bool wasBuilding = s_building;
    s_building = true;
    for (int i = s_winFirst; i <= s_winLast; ++i) {
        if (i < first || i > last) destroy_row(i);
    }
    /* The group walks its objects in insertion order, so a row appended below
     * the last one is already in the right place; only a row built above the
     * window's old start puts the order out, and only that needs the rebuild. */
    bool addedAbove = false;
    for (int i = first; i <= last; ++i) {
        if (!s_rows[i].obj) {
            build_row(i);
            if (i < prevFirst || s_winLast < s_winFirst) addedAbove = true;
        }
    }
    s_winFirst = first;
    s_winLast  = last;
    if (addedAbove) group_reorder();
    s_building = wasBuilding;
    thumb_defer();
}

void select_row(int idx) {
    if (s_building || idx < 0 || idx >= (int)s_rows.size()) return;

    s_selected = idx;
    ensure_window(idx);

    auto* mgr = crosspad::getKitManager();
    auto* kit = mgr ? mgr->getKit(idx) : nullptr;
    if (kit) {
        lv_label_set_text(s_coverName, kit->name.c_str());
        lv_label_set_text(s_coverAuthor, kit->author.c_str());
    }
    /* Into view, and no further: the list only moves when the selection would
     * leave it, which is what keeps a click of the encoder to two repainted
     * rows instead of a whole screen of them. */
    const int32_t top  = (int32_t)idx * kRowPitch;
    const int32_t view = lv_obj_get_height(s_list);
    const int32_t sy   = lv_obj_get_scroll_y(s_list);
    int32_t want = sy;
    if (top < sy)                         want = top;
    else if (top + kRowH > sy + view)     want = top + kRowH - view;
    if (want != sy) {
        s_scrolling = true;
        lv_obj_scroll_to_y(s_list, want, LV_ANIM_OFF);
        s_scrolling = false;
    }
    selection_mark();
    cover_request(idx);
    thumb_defer();
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

/* LVGL moves the focus one row per detent; this turns a fast run of them into
 * a longer jump before anything is drawn, so a spin crosses the list in a
 * handful of repaints instead of seventy. */
void row_focused_cb(lv_event_t* e) {
    if (s_refocusing) return;
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const int n = (int)s_rows.size();
    if (s_building || idx < 0 || idx >= n) { select_row(idx); return; }

    const uint32_t now = lv_tick_get();
    const int dir = (s_selected < 0) ? 0 : (idx > s_selected) - (idx < s_selected);
    int target = idx;

    /* Only a single-row move is a detent; a click on a row, or a drag, is not
     * something to accelerate. */
    if (dir != 0 && idx - s_selected == dir) {
        /* Strictly later, not merely soon: a quick flick reaches LVGL as
         * several key presses inside one tick, and those already carry their
         * own distance -- doubling on top of them would fling the list. */
        const uint32_t since = now - s_lastStepMs;
        if (dir == s_lastStepDir && since > 0 && since <= kFastStepMs) {
            s_stepRun = s_stepRun < kMaxStep ? s_stepRun * 2 : kMaxStep;
        } else {
            s_stepRun = 1;
        }
        target = idx + dir * (s_stepRun - 1);
        if (target < 0) target = 0;
        if (target >= n) target = n - 1;
    } else {
        s_stepRun = 1;
    }
    s_lastStepDir = dir;
    s_lastStepMs = now;

    if (target != idx) {
        /* The jump lands on a row that may not be built yet, and moving the
         * focus fires this callback again. */
        s_refocusing = true;
        ensure_window(target);
        if (s_rows[target].obj) lv_group_focus_obj(s_rows[target].obj);
        s_refocusing = false;
    }
    select_row(target);
}

/* Encoder press and touch both land here, and both load on the first click:
 * requiring a second press made touch feel like the first tap did nothing. */
void row_clicked_cb(lv_event_t* e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    select_row(idx);
    load_kit(idx);
}

/* A touch flick lands wherever it lands; the rows there have to exist. */
void list_scrolled_cb(lv_event_t*) {
    if (s_building || s_scrolling || !s_list || s_rows.empty()) return;
    /* Rows first: a flick lands wherever it lands and the row it lands on has
     * to exist before anything reads it. */
    const int32_t y = lv_obj_get_scroll_y(s_list);
    const int32_t h = lv_obj_get_height(s_list);
    ensure_window((int)((y + h / 2) / kRowPitch));

    /* A drag keeps the kit it had selected for as long as it is on screen;
     * dragged past it, the nearest row still in view takes over. */
    int first = 0, last = 0;
    rows_in_view(first, last);
    if (s_selected < first)     select_row(first);
    else if (s_selected > last) select_row(last);
    else                        selection_mark();
}

/* A flick comes to rest wherever momentum ends, which is usually between two
 * rows. LVGL's own scroll snap is not usable here: it snaps to any child, and
 * the tail spacer is one. */
void list_scroll_end_cb(lv_event_t*) {
    if (s_building || s_scrolling || !s_list || s_rows.empty()) return;
    const int32_t y = lv_obj_get_scroll_y(s_list);
    const int32_t want = ((y + kRowPitch / 2) / kRowPitch) * kRowPitch;
    if (y == want) return;
    s_scrolling = true;
    lv_obj_scroll_to_y(s_list, want, LV_ANIM_ON);
    s_scrolling = false;
}

/* The power button's short click has to leave through the browser's own close
 * path. Without this hook powerBack() falls through to "destroy the running
 * app", which tears the overlay down directly -- and the hook that puts the
 * app the user came from back on screen is raised by the close path and by
 * nothing else, so back from the browser landed on the launcher. */
bool selector_back_cb(void*) {
    if (!s_overlay) return false;
    kit_selector_close();
    return true;
}

void back_clicked_cb(lv_event_t*) {
    if (s_onClose) {
        s_onClose(s_userData);
        return;
    }
    if (s_overlay) kit_selector_destroy(s_overlay);
}

// ── Building ─────────────────────────────────────────────────────────────────

/// Widgets for row `idx`, placed at its slot. The entry in s_rows must exist.
void build_row(int idx) {
    auto* mgr = crosspad::getKitManager();
    auto* kitp = mgr ? mgr->getKit(idx) : nullptr;
    if (!kitp || idx < 0 || idx >= (int)s_rows.size() || s_rows[idx].obj) return;
    const crosspad::KitInfo& kit = *kitp;
    void* tag = (void*)(intptr_t)idx;

    lv_obj_t* row = lv_obj_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &styleKitBrowserRow, 0);
    lv_obj_set_size(row, LV_PCT(100), kRowH);
    lv_obj_set_pos(row, 0, idx * kRowPitch);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, tag);
    lv_obj_add_event_cb(row, row_focused_cb, LV_EVENT_FOCUSED, tag);
    if (s_group) lv_group_add_obj(s_group, row);

    /* The row is the click target, so nothing inside it may take a press: a
     * tap that landed on the text column hit this container instead and the
     * row never saw LV_EVENT_CLICKED — the kit under the finger did nothing. */
    lv_obj_t* thumbBox = lv_obj_create(row);
    lv_obj_remove_style_all(thumbBox);
    lv_obj_add_style(thumbBox, &styleKitBrowserThumb, 0);
    lv_obj_set_size(thumbBox, kThumbSize, kThumbSize);
    lv_obj_remove_flag(thumbBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(thumbBox, LV_OBJ_FLAG_CLICKABLE);

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
    lv_obj_remove_flag(texts, LV_OBJ_FLAG_CLICKABLE);

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

    Row& r = s_rows[idx];
    r.obj = row; r.name = name; r.author = author;
    r.thumb = img; r.letter = letter;
    r.thumbTried = false;
    if (idx == s_loaded) row_apply_loaded_edge(row);
}

void destroy_row(int idx) {
    if (idx < 0 || idx >= (int)s_rows.size()) return;
    Row& r = s_rows[idx];
    if (!r.obj) return;
    lv_obj_delete(r.obj);   // takes it out of the group as well
    r.obj = r.name = r.author = r.thumb = r.letter = nullptr;
}

/// The group walks its objects in insertion order; a row built ahead of an
/// older one would come after it. Back first, then the window in row order.
void group_reorder() {
    if (!s_group) return;
    lv_obj_t* focused = lv_group_get_focused(s_group);
    lv_group_remove_all_objs(s_group);
    if (s_backBtn) lv_group_add_obj(s_group, s_backBtn);
    for (int i = s_winFirst; i <= s_winLast; ++i) {
        if (s_rows[i].obj) lv_group_add_obj(s_group, s_rows[i].obj);
    }
    if (focused) lv_group_focus_obj(focused);
}

void build_rows() {
    auto* mgr = crosspad::getKitManager();
    const int count = mgr ? mgr->getKitCount() : 0;

    s_building = true;
    s_rows.clear();
    s_rows.resize(count);
    s_winFirst = 0;
    s_winLast  = -1;
    s_selected = -1;
    s_loaded   = -1;
    s_coverShown = -1;
    s_moreCard = nullptr;

    for (int i = 0; i < count; ++i) {
        if (auto* kit = mgr->getKit(i)) {
            s_rows[i].thumbPath = crosspad::kitAssetPath(kit->path, "thumb.png");
        }
    }
    if (count > 0) {
        /* Past the last row, and it is also the list's scrollable extent —
         * the rows come and go, so something permanent has to define it. Its
         * height is a viewport minus one row for a second reason: the frame is
         * fixed against the first slot, so the last kit can only reach it if
         * there is that much left to scroll past it. */
        const int32_t tail = lv_obj_get_height(s_list) - kRowH;
        s_moreCard = lv_obj_create(s_list);
        lv_obj_remove_style_all(s_moreCard);
        lv_obj_set_size(s_moreCard, LV_PCT(100), tail > kRowH ? tail : kRowH);
        lv_obj_set_pos(s_moreCard, 0, count * kRowPitch);
        lv_obj_remove_flag(s_moreCard, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_moreCard, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(s_moreCard, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_top(s_moreCard, 8, 0);
        lv_obj_set_style_pad_row(s_moreCard, 3, 0);

        lv_obj_t* moreTitle = lv_label_create(s_moreCard);
        lv_obj_set_style_text_font(moreTitle, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(moreTitle, CROSSPAD_THEME_PRIMARY, 0);
        lv_label_set_text(moreTitle, "More kits");

        /* The path, not a URL: this is where the firmware actually looks, and
         * it is the one instruction that stays true whatever the shop is
         * called. */
        lv_obj_t* moreBody = lv_label_create(s_moreCard);
        lv_obj_set_style_text_font(moreBody, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_opa(moreBody, LV_OPA_50, 0);
        lv_label_set_long_mode(moreBody, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(moreBody, LV_PCT(100));
        lv_label_set_text(moreBody, "Put kit folders in\ncrosspad/kits\non the SD card.");
    }
    const int current = mgr->getCurrentKitId();
    ensure_window(current >= 0 && current < count ? current : 0);
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

    /* Whatever the last screen decoded — launcher icons, a previous kit's
     * thumbnails — goes now, so the covers and thumbnails about to be decoded
     * get one contiguous run of PSRAM instead of the gaps between them. The
     * launcher re-decodes its icons from the assets partition on the way back. */
    lv_image_cache_drop(NULL);

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

    /* The caret's gutter. It is a sibling of the list, not a child: the list
     * scrolls and the caret must not, and a row's own leading glyph would
     * vanish with the row whenever the virtual window dropped it. Both are
     * PCT(100) tall flex items, so a y inside the gutter is a y inside the
     * list's viewport with no offset to carry. */
    lv_obj_t* gutter = lv_obj_create(body);
    lv_obj_remove_style_all(gutter);
    lv_obj_set_size(gutter, kCaretW, LV_PCT(100));
    lv_obj_remove_flag(gutter, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(gutter, LV_OBJ_FLAG_CLICKABLE);

    s_caret = lv_label_create(gutter);
    lv_label_set_text(s_caret, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(s_caret, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_caret, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_x(s_caret, 0);
    lv_obj_add_flag(s_caret, LV_OBJ_FLAG_HIDDEN);

    s_list = lv_obj_create(body);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_height(s_list, LV_PCT(100));
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    /* On every scroll step, not only at the end: a flick must find rows
     * where it lands. ensure_window() returns at once while the window holds. */
    lv_obj_add_event_cb(s_list, list_scrolled_cb, LV_EVENT_SCROLL, nullptr);
    lv_obj_add_event_cb(s_list, list_scroll_end_cb, LV_EVENT_SCROLL_END, nullptr);

    /* The selection frame. Outside the list, because the list scrolls and this
     * must not, and out of the flex flow (IGNORE_LAYOUT) so it can be laid on
     * top of the list's first row slot. Not clickable: the row underneath has
     * to keep receiving the press. */
    s_frame = lv_obj_create(body);
    lv_obj_remove_style_all(s_frame);
    lv_obj_add_flag(s_frame, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(s_frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_frame, 6, 0);
    lv_obj_set_style_border_color(s_frame, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_style_border_opa(s_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_frame, 2, 0);

    /* The caret's y comes from the list's height and scroll position, and
     * select_row() runs inside build_rows(); without this the flex pass has
     * not happened yet and the list still measures zero. */
    lv_obj_update_layout(scr);
    lv_obj_set_pos(s_frame, lv_obj_get_x(s_list), lv_obj_get_y(s_list));
    lv_obj_set_size(s_frame, lv_obj_get_width(s_list), kRowH);
    build_rows();

    s_loadTimer = lv_timer_create(load_poll_cb, 100, nullptr);
    s_backToken = crosspad_gui::ui_back_push(selector_back_cb, nullptr);
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

    if (s_backToken) { crosspad_gui::ui_back_remove(s_backToken); s_backToken = 0; }
    if (s_loadTimer)  { lv_timer_delete(s_loadTimer);  s_loadTimer = nullptr; }
    if (s_coverTimer) { lv_timer_delete(s_coverTimer); s_coverTimer = nullptr; }
    if (s_thumbTimer) { lv_timer_delete(s_thumbTimer); s_thumbTimer = nullptr; }
    if (s_thumbQuietTimer) { lv_timer_delete(s_thumbQuietTimer); s_thumbQuietTimer = nullptr; }

    // Restore the previous encoder group (the launcher's) before deleting ours
    if (lv_indev_t* enc = encoder()) {
        if (lv_indev_get_group(enc) == s_group) lv_indev_set_group(enc, s_prevGroup);
    }
    s_prevGroup = nullptr;
    if (s_group) { lv_group_delete(s_group); s_group = nullptr; }

    s_rows.clear();
    s_winFirst = 0; s_winLast = -1;
    s_selected = s_loaded = -1;
    s_coverWanted = s_coverShown = -1;
    s_coverPathShown.clear();
    s_lastStepMs = 0;
    s_lastStepDir = 0;
    s_stepRun = 1;
    s_refocusing = false;
    s_overlay = s_list = s_coverBox = s_coverImg = s_coverLetter = nullptr;
    s_coverName = s_coverAuthor = s_statusLabel = s_backBtn = s_busy = nullptr;
    s_caret = s_frame = s_moreCard = nullptr;
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
    s_winFirst = 0; s_winLast = -1;
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

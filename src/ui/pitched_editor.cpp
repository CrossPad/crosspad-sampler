// SPDX-License-Identifier: MIT

#ifdef USE_LVGL

// Edit window for a pitched kit: the pad's note/volume/pan on one tab, the
// instrument's zones and envelope on the other.

#include <lvgl.h>

#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"
#include "crosspad-sampler/ui/pitched_editor.hpp"
#include "crosspad-sampler/ui/waveform_view.hpp"
#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/waveform/waveform_loader.hpp"

#include "crosspad-gui/crosspad_gui.h"

#include <crosspad/instrument/PitchedInstrument.hpp>
#include <crosspad/instrument/SampleBank.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitInfo.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/platform/PlatformServices.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace crosspad_sampler {

using crosspad::InstrumentZoneSpec;
using crosspad::KitInfo;

extern lv_subject_t s_pad_sampler; // last pressed pad, owned by sampler_panel.cpp

namespace {

// ── Shared accessors ────────────────────────────────────────────────────

lv_indev_t* getEncoder() {
    return crosspad_gui::getGuiPlatform().getNavigationEncoder();
}

KitInfo* currentKit() {
    auto* mgr = crosspad::getKitManager();
    KitInfo* kit = mgr ? mgr->getCurrentKit() : nullptr;
    if (kit && kit->engine != crosspad::KitEngine::Pitched) return nullptr;
    return kit;
}

/// The pad the panel last reported, pad 0 until one is pressed — a pitched
/// kit's Edit window always edits *some* pad, unlike the sampler's, which has
/// nothing to show before a hit.
int currentPad() {
    const int pad = lv_subject_get_int(&s_pad_sampler) - 1;
    return (pad >= 0 && pad < KIT_PADS) ? pad : 0;
}

crosspad::PitchedInstrument* instrument() {
    return crosspad::getPlatformServices().pitchedInstrument;
}

/// The zone the engine actually built for @p idx, or null when no bank is
/// installed. Its rootMidi is the detected pitch of an "auto" zone.
const crosspad::Zone* bankZone(uint8_t idx) {
    auto* inst = instrument();
    auto* bank = inst ? inst->bank() : nullptr;
    if (!bank || idx >= bank->zoneCount()) return nullptr;
    return &bank->zone(idx);
}

const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/// MIDI note -> "C4". Buffer must hold at least 6 bytes.
void noteName(int note, char* out, size_t len) {
    if (note < 0) note = 0;
    if (note > 127) note = 127;
    snprintf(out, len, "%s%d", kNoteNames[note % 12], (note / 12) - 1);
}

const char* baseName(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

// ── Editor state ────────────────────────────────────────────────────────

lv_obj_t*   s_cont      = nullptr;
lv_group_t* s_group     = nullptr;
lv_group_t* s_priorGroup = nullptr;
lv_timer_t* s_timer     = nullptr;
uint32_t    s_backToken = 0;

lv_obj_t* s_tabPadBtn  = nullptr;
lv_obj_t* s_tabInstBtn = nullptr;
lv_obj_t* s_padPanel   = nullptr;
lv_obj_t* s_instPanel  = nullptr;
int       s_tab        = 0;   // 0 = Pad, 1 = Instrument

lv_obj_t* s_noteSpin  = nullptr;
lv_obj_t* s_noteLabel = nullptr;
lv_obj_t* s_volSpin   = nullptr;
lv_obj_t* s_panSpin   = nullptr;
lv_obj_t* s_zoneInfo  = nullptr;

lv_obj_t* s_zoneList   = nullptr;
lv_obj_t* s_rootSpin   = nullptr;
lv_obj_t* s_autoCheck  = nullptr;
lv_obj_t* s_startSpin  = nullptr;
lv_obj_t* s_endSpin    = nullptr;
lv_obj_t* s_stepBtn    = nullptr;
lv_obj_t* s_attackSpin = nullptr;
lv_obj_t* s_releaseSpin = nullptr;
lv_obj_t* s_voicesSpin = nullptr;
lv_obj_t* s_removeBtn  = nullptr;

lv_obj_t* s_waveform = nullptr;

int  s_zoneSel  = 0;      // index into instrument.zones, or zones.size() for "+ add zone"
int  s_lastPad  = -1;
bool s_syncing  = false;  // suppress the value-changed handlers while filling widgets
bool s_wavePending = false;
char s_wavePath[160] = {};

const int  kStepMultipliers[] = {1, 10, 100, 1000, 10000};
const int  kStepCount = 5;
int        s_stepIdx  = 2;   // x100: loops on a one-second zone move audibly

// Spinbox ceiling for a loop point. 9 999 999 samples is ~3.8 minutes at
// 44.1 kHz — past anything the bank can hold, and it keeps the field inside
// the 140 px panel.
constexpr int32_t kLoopMax = 9999999;

void markDirty() {
    KitInfo* kit = currentKit();
    if (kit) kit->instrument.dirty = true;
}

bool hasUnsavedEdits() {
    KitInfo* kit = currentKit();
    if (!kit) return false;
    if (kit->instrument.dirty) return true;
    for (int i = 0; i < KIT_PADS; ++i) {
        if (kit->pads[i].dirtyModified) return true;
    }
    return false;
}

// ── Waveform by path ────────────────────────────────────────────────────

/// The same cache-or-request path the panel's pad provider takes, keyed by a
/// resolved file name instead of a pad slot.
void fileWaveformProvider(const char* path, int16_t* outData, uint32_t maxSamples,
                          uint32_t* actualSamples, void* /*userData*/) {
    *actualSamples = 0;
    if (!path || !path[0]) return;

    WaveformCacheEntry* entry = waveform_cache_find(path);
    if (!entry || !entry->valid || !entry->waveformData || entry->sampleCount == 0) {
        waveform_loader_start();
        waveform_loader_request(path, true, 0, 0, 0);
        s_wavePending = true;
        return;
    }

    uint32_t count = entry->sampleCount < maxSamples ? entry->sampleCount : maxSamples;
    if (entry->sampleCount <= maxSamples) {
        memcpy(outData, entry->waveformData, count * sizeof(int16_t));
    } else {
        for (uint32_t i = 0; i < maxSamples; ++i) {
            outData[i] = entry->waveformData[(uint64_t)i * entry->sampleCount / maxSamples];
        }
        count = maxSamples;
    }
    *actualSamples = count;
}

void showZoneWaveform(int zoneIdx) {
    KitInfo* kit = currentKit();
    if (!s_waveform || !kit) return;
    if (zoneIdx < 0 || zoneIdx >= (int)kit->instrument.zones.size()) {
        waveform_view_clear(s_waveform);
        s_wavePath[0] = '\0';
        return;
    }

    const std::string resolved =
        crosspad::resolveKitSamplePath(kit->path, kit->instrument.zones[zoneIdx].sample);
    if (resolved.empty()) return;

    s_wavePending = false;
    snprintf(s_wavePath, sizeof(s_wavePath), "%s", resolved.c_str());
    waveform_view_load_file(s_waveform, s_wavePath);

    // Loop points are in samples of the zone the engine built, so that is what
    // they are drawn against; without a bank there is nothing to scale by.
    const crosspad::Zone* z = bankZone((uint8_t)zoneIdx);
    const InstrumentZoneSpec& spec = kit->instrument.zones[zoneIdx];
    if (z && z->length > 0 && spec.loopEnd > spec.loopStart) {
        const float total = (float)z->length;
        waveform_view_set_markers(s_waveform, (float)spec.loopStart / total,
                                  (float)spec.loopEnd / total);
    } else {
        waveform_view_set_markers(s_waveform, 0.0f, 1.0f);
    }
}

// ── Live application ────────────────────────────────────────────────────

/// Push the selected zone's root and loop into the bank that is playing.
void applyZoneEdit() {
    KitInfo* kit = currentKit();
    auto* inst = instrument();
    if (!kit || !inst || s_zoneSel < 0 || s_zoneSel >= (int)kit->instrument.zones.size()) return;

    const InstrumentZoneSpec& spec = kit->instrument.zones[s_zoneSel];
    // "auto" has no root of its own: the bank's detected pitch is what the
    // engine keeps playing at, so that is what the edit re-asserts.
    float root = spec.root;
    if (root < 0.0f) {
        const crosspad::Zone* z = bankZone((uint8_t)s_zoneSel);
        root = z ? z->rootMidi : 69.0f;
    }
    inst->editZone((uint8_t)s_zoneSel, root, spec.loopStart, spec.loopEnd, 10, nullptr);
}

void applyParams() {
    KitInfo* kit = currentKit();
    auto* inst = instrument();
    if (!kit || !inst) return;
    crosspad::InstrumentParams p;
    p.maxVoices = kit->instrument.maxVoices;
    p.attackMs  = (float)kit->instrument.attackMs;
    p.releaseMs = (float)kit->instrument.releaseMs;
    inst->setParams(p);
}

/// Rebuild the bank from the current zone list. The pitched engine builds the
/// whole bank on pad 0; the other fifteen pads carry no per-engine state.
void rebuildBank() {
    auto& cbs = sampler_get_platform_callbacks();
    if (cbs.audio.reloadPad) cbs.audio.reloadPad(0);
}

// ── Zone list ───────────────────────────────────────────────────────────

void refreshZoneList();
void rebuildGroup();
void syncInstrumentTab();

void zoneRowClicked(lv_event_t* e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_zoneSel = idx;
    syncInstrumentTab();
    refreshZoneList();
    showZoneWaveform(idx);
}

void refreshZoneList() {
    KitInfo* kit = currentKit();
    if (!s_zoneList || !kit) return;

    const int zoneCount = (int)kit->instrument.zones.size();
    if (s_zoneSel > zoneCount) s_zoneSel = zoneCount;
    if (s_zoneSel < 0) s_zoneSel = 0;

    lv_list_cleanup(s_zoneList);

    char row[64];
    for (int i = 0; i < zoneCount; ++i) {
        const InstrumentZoneSpec& spec = kit->instrument.zones[i];
        const crosspad::Zone* z = bankZone((uint8_t)i);
        if (spec.root < 0.0f) {
            snprintf(row, sizeof(row), "%s " LV_SYMBOL_RIGHT " auto %.1f",
                     baseName(spec.sample), z ? z->rootMidi : 0.0f);
        } else {
            char nn[8];
            noteName((int)(spec.root + 0.5f), nn, sizeof(nn));
            snprintf(row, sizeof(row), "%s " LV_SYMBOL_RIGHT " %s (%d)",
                     baseName(spec.sample), nn, (int)spec.root);
        }
        lv_obj_t* item = lv_list_add_button(s_zoneList, nullptr, row);
        lv_obj_add_style(item, &styleFileExplorerEntry, 0);
        lv_obj_set_style_text_font(item, &lv_font_montserrat_10, 0);
        lv_obj_set_style_pad_ver(item, 2, 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(i == s_zoneSel ? 0x2255AA : 0x111111), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(item, lv_color_white(), 0);
        lv_obj_add_event_cb(item, zoneRowClicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    if (zoneCount < crosspad::kInstrumentMaxZones) {
        lv_obj_t* add = lv_list_add_button(s_zoneList, LV_SYMBOL_PLUS, "add zone");
        lv_obj_add_style(add, &styleFileExplorerEntry, 0);
        lv_obj_set_style_text_font(add, &lv_font_montserrat_10, 0);
        lv_obj_set_style_pad_ver(add, 2, 0);
        lv_obj_set_style_bg_color(add, lv_color_hex(s_zoneSel == zoneCount ? 0x2255AA : 0x111111), 0);
        lv_obj_set_style_bg_opa(add, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(add, lv_color_white(), 0);
        lv_obj_add_event_cb(add, zoneRowClicked, LV_EVENT_CLICKED, (void*)(intptr_t)zoneCount);
    }

    if (s_removeBtn) {
        if (zoneCount > 1 && s_zoneSel < zoneCount) lv_obj_remove_state(s_removeBtn, LV_STATE_DISABLED);
        else                                        lv_obj_add_state(s_removeBtn, LV_STATE_DISABLED);
    }

    // The rows are group members, and lv_list_cleanup() freed the old ones.
    rebuildGroup();
}

// ── Widget sync ─────────────────────────────────────────────────────────

void syncInstrumentTab() {
    KitInfo* kit = currentKit();
    if (!kit || !s_rootSpin) return;
    const int zoneCount = (int)kit->instrument.zones.size();
    const bool haveZone = s_zoneSel >= 0 && s_zoneSel < zoneCount;

    const bool wasSyncing = s_syncing;
    s_syncing = true;
    if (haveZone) {
        const InstrumentZoneSpec& spec = kit->instrument.zones[s_zoneSel];
        const bool automatic = spec.root < 0.0f;
        const crosspad::Zone* z = bankZone((uint8_t)s_zoneSel);
        const int shownRoot = automatic ? (int)((z ? z->rootMidi : 69.0f) + 0.5f)
                                        : (int)(spec.root + 0.5f);
        lv_spinbox_set_value(s_rootSpin, shownRoot);
        if (automatic) lv_obj_add_state(s_autoCheck, LV_STATE_CHECKED);
        else           lv_obj_remove_state(s_autoCheck, LV_STATE_CHECKED);
        lv_spinbox_set_value(s_startSpin, (int32_t)(spec.loopStart > (uint32_t)kLoopMax
                                                        ? kLoopMax : spec.loopStart));
        lv_spinbox_set_value(s_endSpin, (int32_t)(spec.loopEnd > (uint32_t)kLoopMax
                                                      ? kLoopMax : spec.loopEnd));
    }
    lv_spinbox_set_value(s_attackSpin, kit->instrument.attackMs);
    lv_spinbox_set_value(s_releaseSpin, kit->instrument.releaseMs);
    lv_spinbox_set_value(s_voicesSpin, kit->instrument.maxVoices ? kit->instrument.maxVoices : 24);
    s_syncing = wasSyncing;
}

/// The note name beside the spinbox, and which zone that note actually reaches
/// — the nearest root, the same choice the engine makes on a hit.
void showNoteAndZone(int note) {
    KitInfo* kit = currentKit();
    if (!kit || !s_noteLabel || !s_zoneInfo) return;

    char nn[8];
    noteName(note, nn, sizeof(nn));
    lv_label_set_text(s_noteLabel, nn);

    const char* file = "-";
    float root = 0.0f;
    auto* inst = instrument();
    auto* bank = inst ? inst->bank() : nullptr;
    if (bank) {
        const crosspad::Zone* nearest = bank->nearestZone((float)note);
        for (uint8_t i = 0; nearest && i < bank->zoneCount(); ++i) {
            if (&bank->zone(i) == nearest) {
                root = nearest->rootMidi;
                if (i < kit->instrument.zones.size())
                    file = baseName(kit->instrument.zones[i].sample);
                break;
            }
        }
    }
    lv_label_set_text_fmt(s_zoneInfo, "Zone: %s\nroot %.1f", file, root);
}

void syncPadTab(int padIdx) {
    KitInfo* kit = currentKit();
    if (!kit || !s_noteSpin || padIdx < 0 || padIdx >= KIT_PADS) return;

    const bool wasSyncing = s_syncing;
    s_syncing = true;
    const int note = kit->instrument.noteForPad((uint8_t)padIdx);
    lv_spinbox_set_value(s_noteSpin, note);
    lv_spinbox_set_value(s_volSpin, kit->pads[padIdx].volume);
    lv_spinbox_set_value(s_panSpin, kit->pads[padIdx].pan);
    s_syncing = wasSyncing;

    showNoteAndZone(note);
}

// ── Tabs and encoder group ──────────────────────────────────────────────

void addIfPresent(lv_obj_t* obj) {
    if (obj) lv_group_add_obj(s_group, obj);
}

void rebuildGroup() {
    if (!s_group) return;
    lv_group_remove_all_objs(s_group);

    addIfPresent(s_tabPadBtn);
    addIfPresent(s_tabInstBtn);

    if (s_tab == 0) {
        addIfPresent(s_noteSpin);
        addIfPresent(s_volSpin);
        addIfPresent(s_panSpin);
    } else {
        if (s_zoneList) {
            uint32_t rows = lv_obj_get_child_count(s_zoneList);
            for (uint32_t i = 0; i < rows; ++i) addIfPresent(lv_obj_get_child(s_zoneList, i));
        }
        addIfPresent(s_rootSpin);
        addIfPresent(s_autoCheck);
        addIfPresent(s_startSpin);
        addIfPresent(s_endSpin);
        addIfPresent(s_stepBtn);
        addIfPresent(s_attackSpin);
        addIfPresent(s_releaseSpin);
        addIfPresent(s_voicesSpin);
        addIfPresent(s_removeBtn);
    }

    // Footer last, so the encoder cycles the rows before Close/Save/Browse.
    uint32_t children = lv_obj_get_child_count(s_cont);
    for (uint32_t i = 0; i < children; ++i) {
        lv_obj_t* child = lv_obj_get_child(s_cont, i);
        if (child && lv_obj_has_flag(child, LV_OBJ_FLAG_USER_1)) lv_group_add_obj(s_group, child);
    }
}

void setTab(int tab) {
    s_tab = tab;
    // Own the tab's look outright: a CHECKABLE button's state is toggled by
    // LVGL as well as by us, and the two disagreed after a click that focused
    // without firing CLICKED.
    lv_obj_t* tabs[2] = {s_tabPadBtn, s_tabInstBtn};
    for (int i = 0; i < 2; ++i) {
        const bool on = (i == tab);
        lv_obj_set_style_bg_color(tabs[i], lv_color_hex(on ? 0x2255AA : 0x9A9A9A), 0);
        lv_obj_t* label = lv_obj_get_child(tabs[i], 0);
        if (label) lv_obj_set_style_text_color(label, on ? lv_color_white() : lv_color_black(), 0);
    }
    if (tab == 0) {
        lv_obj_remove_flag(s_padPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_instPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_zoneList, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_waveform, LV_OBJ_FLAG_HIDDEN);
        s_lastPad = -1;   // force the pad row to refill
    } else {
        lv_obj_add_flag(s_padPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_instPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_zoneList, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_waveform, LV_OBJ_FLAG_HIDDEN);
        syncInstrumentTab();
        refreshZoneList();
        showZoneWaveform(s_zoneSel);
    }
    rebuildGroup();
}

// ── Close / Save ────────────────────────────────────────────────────────

void closeNow() {
    if (s_cont) lv_obj_delete_async(s_cont);
}

void saveKitNow() {
    KitInfo* kit = currentKit();
    auto* mgr = crosspad::getKitManager();
    if (!kit || !mgr) return;
    const bool ok = mgr->saveKit(*kit);
    kit->instrument.dirty = false;
    for (int i = 0; i < KIT_PADS; ++i) {
        kit->pads[i].dirty = false;
        kit->pads[i].dirtyModified = false;
    }
    crosspad_gui::statusbar_show_loader_message(ok ? "Kit" : "Error",
                                                ok ? "Kit saved" : "Save failed", !ok);
}

void unsavedModalCb(bool save, void*) {
    if (save) {
        saveKitNow();
    } else {
        // Discard: the card still holds the last saved kit, so re-reading it
        // undoes the edits, and the bank has to be rebuilt from what came back.
        auto* mgr = crosspad::getKitManager();
        KitInfo* kit = currentKit();
        if (mgr && kit) {
            mgr->loadKit(*kit);
            kit->instrument.dirty = false;
            for (int i = 0; i < KIT_PADS; ++i) kit->pads[i].dirtyModified = false;
            rebuildBank();
        }
    }
    closeNow();
}

void requestClose() {
    if (!hasUnsavedEdits()) { closeNow(); return; }
    ui_modal_confirm("Unsaved changes", "Write the edits to the kit?",
                     "Save", "Discard", unsavedModalCb, nullptr, s_group);
}

bool backCb(void*) {
    if (!s_cont) return false;
    requestClose();
    return true;
}

// ── Browse ──────────────────────────────────────────────────────────────

/// kit.json names a zone by a path the loader resolves against the kit's own
/// directory; the explorer hands back whatever this platform's filesystem
/// calls the file, which on the simulator is a host path. A file inside the
/// kit goes back in relative, so the kit stays portable between card and
/// desktop; anything else is stored as picked.
std::string zonePathFor(const KitInfo& kit, const std::string& picked) {
    const size_t kitSlash = kit.path.rfind('/');
    if (kitSlash == std::string::npos || kitSlash == 0) return picked;
    const size_t nameStart = kit.path.rfind('/', kitSlash - 1);
    if (nameStart == std::string::npos) return picked;

    const std::string needle = kit.path.substr(nameStart, kitSlash - nameStart + 1);
    const size_t at = picked.rfind(needle);
    if (at == std::string::npos) return picked;

    const std::string relative = picked.substr(at + needle.size());
    return crosspad::resolveKitSamplePath(kit.path, relative).empty() ? picked : relative;
}

void browseLoadCb(lv_event_t* e) {
    lv_obj_t* btn = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_delete_async(lv_obj_get_parent(btn));
    lv_indev_set_group(getEncoder(), s_group);

    crosspad_gui::FileItem picked = GetCurrentFile();
    if (picked.path.empty() || picked.isFolder) return;

    KitInfo* kit = currentKit();
    if (!kit) return;

    const int zoneCount = (int)kit->instrument.zones.size();
    if (s_zoneSel >= zoneCount) {
        if (zoneCount >= crosspad::kInstrumentMaxZones) return;
        InstrumentZoneSpec spec;
        spec.sample = zonePathFor(*kit, picked.path);
        spec.root = -1.0f;   // let the loader detect it
        kit->instrument.zones.push_back(spec);
        s_zoneSel = zoneCount;
    } else {
        InstrumentZoneSpec& spec = kit->instrument.zones[s_zoneSel];
        spec.sample = zonePathFor(*kit, picked.path);
        spec.root = -1.0f;
        spec.loopStart = 0;
        spec.loopEnd = 0;
    }

    markDirty();
    rebuildBank();
    syncInstrumentTab();
    refreshZoneList();
    showZoneWaveform(s_zoneSel);
}

void browseCb(lv_event_t*) {
    if (s_tab != 1) setTab(1);
    // The kit's own directory, not the kits root: a zone is almost always
    // replaced by another take sitting beside the ones already in the kit, and
    // "Go back..." reaches the rest of the card in one step.
    KitInfo* kit = currentKit();
    std::string start = DEFAULT_KIT_PATH;
    if (kit && !kit->path.empty()) {
        const size_t slash = kit->path.rfind('/');
        if (slash != std::string::npos && slash > 0) start = kit->path.substr(0, slash);
    }
    lv_CreateFileExplorer(s_cont, FILTER_AUDIO, start, browseLoadCb, nullptr);
}

// ── Periodic refresh ────────────────────────────────────────────────────

void updateCb(lv_timer_t*) {
    if (s_tab == 0) {
        const int pad = currentPad();
        if (pad != s_lastPad) {
            s_lastPad = pad;
            syncPadTab(pad);
        }
    } else if (s_wavePending && s_wavePath[0]) {
        // The loader was asked for this file; redraw once it lands.
        WaveformCacheEntry* entry = waveform_cache_find(s_wavePath);
        if (entry && entry->valid) {
            s_wavePending = false;
            showZoneWaveform(s_zoneSel);
        }
    }
}

// ── Widget builders ─────────────────────────────────────────────────────

lv_obj_t* makeFooterButton(lv_obj_t* parent, const char* text, lv_align_t align, int dx,
                           lv_event_cb_t cb) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_align(btn, align, dx, -8);
    lv_obj_add_style(btn, &styleButton, 0);
    lv_obj_add_style(btn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_add_style(btn, &styleButtonInactive, LV_STATE_DISABLED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_USER_1);   // rebuildGroup() picks these up last
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
    return btn;
}

/// A label plus a spinbox on one row of a panel. @p prev anchors the row below
/// the previous one; null puts it at the top.
lv_obj_t* makeRow(lv_obj_t* panel, lv_obj_t* prev, const char* text, int32_t min, int32_t max,
                  lv_event_cb_t cb, lv_obj_t** outLabel) {
    lv_obj_t* label = lv_label_create(panel);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_label_set_text(label, text);
    if (prev) lv_obj_align_to(label, prev, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
    else      lv_obj_align(label, LV_ALIGN_TOP_LEFT, 2, 2);

    lv_obj_t* spin = lv_CreateStandardSpinbox(panel, min, max, min, cb, s_group, nullptr,
                                              &lv_font_montserrat_14);
    if (min < 0) {
        const uint8_t digits = CalcDigits(LV_MAX(LV_ABS(min), LV_ABS(max)));
        lv_obj_set_width(spin, 5 + (8 * digits) + 8);
    }
    lv_obj_align_to(spin, label, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    if (outLabel) *outLabel = label;
    return spin;
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────────

lv_obj_t* pitched_editor_create(lv_obj_t* parent) {
    KitInfo* kit = currentKit();
    if (!kit) return nullptr;

    s_cont = lv_obj_create(parent);
    lv_obj_set_size(s_cont, lv_obj_get_content_width(parent), lv_obj_get_content_height(parent));
    lv_obj_center(s_cont);
    lv_obj_add_style(s_cont, &styleAppContainer, 0);
    lv_obj_remove_flag(s_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_group = lv_group_create();
    lv_group_set_wrap(s_group, true);
    s_priorGroup = lv_indev_get_group(getEncoder());
    lv_indev_set_group(getEncoder(), s_group);

    // Creating a spinbox fires its own VALUE_CHANGED, and the widget a handler
    // reaches may not exist yet -- the build is a sync like any other.
    s_syncing = true;
    s_zoneSel = 0;
    s_lastPad = -1;
    s_stepIdx = 2;
    s_wavePending = false;
    s_wavePath[0] = '\0';

    lv_obj_update_layout(parent);
    const int contW = lv_obj_get_content_width(s_cont);
    const int contH = lv_obj_get_content_height(s_cont);
    const int panelW = (contW / 2) - 12;
    const int tabTop = 24;
    const int footer = 50;   // clears the Close/Save/Browse band

    // ── Footer ──────────────────────────────────────────────────────────
    makeFooterButton(s_cont, "Close", LV_ALIGN_BOTTOM_LEFT, 6,
                     [](lv_event_t*) { requestClose(); });
    makeFooterButton(s_cont, "Save", LV_ALIGN_BOTTOM_MID, 0,
                     [](lv_event_t*) { saveKitNow(); });
    makeFooterButton(s_cont, "Browse", LV_ALIGN_BOTTOM_RIGHT, -6, browseCb);

    // ── Tabs ────────────────────────────────────────────────────────────
    auto makeTab = [&](const char* text, int dx, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_button_create(s_cont);
        lv_obj_set_size(btn, (panelW / 2) - 2, 20);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, dx, 0);
        lv_obj_add_style(btn, &styleButton, 0);
        lv_obj_add_style(btn, &styleButtonPressed, LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
        lv_obj_center(label);
        return btn;
    };
    s_tabPadBtn  = makeTab("Pad", 4, [](lv_event_t*) { setTab(0); });
    s_tabInstBtn = makeTab("Instrument", 4 + (panelW / 2), [](lv_event_t*) { setTab(1); });

    // ── Right column: zone list over the waveform ───────────────────────
    s_waveform = waveform_view_create(s_cont);
    lv_obj_align(s_waveform, LV_ALIGN_BOTTOM_RIGHT, -4, -footer);

    s_zoneList = lv_list_create(s_cont);
    lv_obj_add_style(s_zoneList, &styleFileExplorerList, 0);
    lv_obj_add_style(s_zoneList, &styleFileExplorerScrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_width(s_zoneList, WAVEFORM_CANVAS_WIDTH);
    lv_obj_set_style_max_height(s_zoneList, contH - footer - WAVEFORM_CANVAS_HEIGHT - 8, 0);
    lv_obj_set_style_pad_all(s_zoneList, 1, 0);
    // The file-explorer list is theme green, which next to the editor's dark
    // panel reads as the loudest thing on screen; the rows are the content.
    lv_obj_set_style_bg_color(s_zoneList, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_color(s_zoneList, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(s_zoneList, 1, 0);
    lv_obj_align_to(s_zoneList, s_waveform, LV_ALIGN_OUT_TOP_MID, 0, -4);

    // ── Pad tab ─────────────────────────────────────────────────────────
    s_padPanel = lv_obj_create(s_cont);
    lv_obj_add_style(s_padPanel, &styleAppContainer, 0);
    lv_obj_set_style_bg_opa(s_padPanel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_padPanel, 0, 0);
    lv_obj_set_size(s_padPanel, panelW, contH - tabTop - footer);
    lv_obj_align(s_padPanel, LV_ALIGN_TOP_LEFT, 4, tabTop);
    lv_obj_set_scroll_dir(s_padPanel, LV_DIR_VER);

    lv_obj_t* noteRowLabel = nullptr;
    s_noteSpin = makeRow(s_padPanel, nullptr, "Note:", 0, 127, [](lv_event_t* e) {
        if (s_syncing) return;
        KitInfo* kit = currentKit();
        const int pad = currentPad();
        if (!kit) return;
        const int v = lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        if (!kit->instrument.padNotesSet) {
            // The kit ran chromatically from base_note; freeze that map before
            // one pad leaves it, or the other fifteen would move too.
            for (int i = 0; i < KIT_PADS; ++i)
                kit->instrument.padNotes[i] = kit->instrument.noteForPad((uint8_t)i);
            kit->instrument.padNotesSet = true;
        }
        kit->instrument.padNotes[pad] = (uint8_t)v;
        markDirty();
        showNoteAndZone(v);
    }, &noteRowLabel);

    s_noteLabel = lv_label_create(s_padPanel);
    lv_obj_set_style_text_font(s_noteLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_noteLabel, lv_color_hex(0x22CCFF), 0);
    lv_label_set_text(s_noteLabel, "-");
    lv_obj_align_to(s_noteLabel, s_noteSpin, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    lv_obj_t* volRowLabel = nullptr;
    s_volSpin = makeRow(s_padPanel, noteRowLabel, "Volume:", 0, 100, [](lv_event_t* e) {
        if (s_syncing) return;
        KitInfo* kit = currentKit();
        const int pad = currentPad();
        if (!kit) return;
        kit->pads[pad].volume = lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        kit->pads[pad].dirtyModified = true;
        auto* inst = instrument();
        if (inst) inst->setPadMix((uint8_t)pad, kit->pads[pad].volume, kit->pads[pad].pan);
    }, &volRowLabel);

    lv_obj_t* panRowLabel = nullptr;
    s_panSpin = makeRow(s_padPanel, volRowLabel, "Pan:", -100, 100, [](lv_event_t* e) {
        if (s_syncing) return;
        KitInfo* kit = currentKit();
        const int pad = currentPad();
        if (!kit) return;
        kit->pads[pad].pan = lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        kit->pads[pad].dirtyModified = true;
        auto* inst = instrument();
        if (inst) inst->setPadMix((uint8_t)pad, kit->pads[pad].volume, kit->pads[pad].pan);
    }, &panRowLabel);

    s_zoneInfo = lv_label_create(s_padPanel);
    lv_obj_set_style_text_font(s_zoneInfo, &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(s_zoneInfo, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_zoneInfo, panelW - 4);
    lv_label_set_text(s_zoneInfo, "Zone: -");
    lv_obj_align_to(s_zoneInfo, panRowLabel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);

    // ── Instrument tab ──────────────────────────────────────────────────
    s_instPanel = lv_obj_create(s_cont);
    lv_obj_add_style(s_instPanel, &styleAppContainer, 0);
    lv_obj_set_style_bg_opa(s_instPanel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_instPanel, 0, 0);
    lv_obj_set_size(s_instPanel, panelW, contH - tabTop - footer);
    lv_obj_align(s_instPanel, LV_ALIGN_TOP_LEFT, 4, tabTop);
    lv_obj_set_scroll_dir(s_instPanel, LV_DIR_VER);

    lv_obj_t* rootRowLabel = nullptr;
    s_rootSpin = makeRow(s_instPanel, nullptr, "Root:", 0, 127, [](lv_event_t* e) {
        if (s_syncing) return;
        KitInfo* kit = currentKit();
        if (!kit || s_zoneSel < 0 || s_zoneSel >= (int)kit->instrument.zones.size()) return;
        kit->instrument.zones[s_zoneSel].root =
            (float)lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        lv_obj_remove_state(s_autoCheck, LV_STATE_CHECKED);
        markDirty();
        applyZoneEdit();
        refreshZoneList();
    }, &rootRowLabel);

    s_autoCheck = lv_checkbox_create(s_instPanel);
    lv_checkbox_set_text(s_autoCheck, "Auto");
    lv_obj_set_style_text_font(s_autoCheck, &lv_font_montserrat_10, 0);
    lv_obj_align_to(s_autoCheck, s_rootSpin, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_add_event_cb(s_autoCheck, [](lv_event_t* e) {
        if (s_syncing) return;
        KitInfo* kit = currentKit();
        if (!kit || s_zoneSel < 0 || s_zoneSel >= (int)kit->instrument.zones.size()) return;
        lv_obj_t* box = (lv_obj_t*)lv_event_get_target(e);
        InstrumentZoneSpec& spec = kit->instrument.zones[s_zoneSel];
        if (lv_obj_has_state(box, LV_STATE_CHECKED)) {
            spec.root = -1.0f;
        } else {
            const crosspad::Zone* z = bankZone((uint8_t)s_zoneSel);
            spec.root = z ? z->rootMidi : (float)lv_spinbox_get_value(s_rootSpin);
        }
        markDirty();
        applyZoneEdit();
        syncInstrumentTab();
        refreshZoneList();
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* startRowLabel = nullptr;
    s_startSpin = makeRow(s_instPanel, rootRowLabel, "Loop A:", 0, kLoopMax, [](lv_event_t* e) {
        if (s_syncing) return;
        KitInfo* kit = currentKit();
        if (!kit || s_zoneSel < 0 || s_zoneSel >= (int)kit->instrument.zones.size()) return;
        kit->instrument.zones[s_zoneSel].loopStart =
            (uint32_t)lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        markDirty();
        applyZoneEdit();
        showZoneWaveform(s_zoneSel);
    }, &startRowLabel);
    lv_spinbox_set_step(s_startSpin, kStepMultipliers[s_stepIdx]);

    lv_obj_t* endRowLabel = nullptr;
    s_endSpin = makeRow(s_instPanel, startRowLabel, "Loop B:", 0, kLoopMax, [](lv_event_t* e) {
        if (s_syncing) return;
        KitInfo* kit = currentKit();
        if (!kit || s_zoneSel < 0 || s_zoneSel >= (int)kit->instrument.zones.size()) return;
        kit->instrument.zones[s_zoneSel].loopEnd =
            (uint32_t)lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        markDirty();
        applyZoneEdit();
        showZoneWaveform(s_zoneSel);
    }, &endRowLabel);
    lv_spinbox_set_step(s_endSpin, kStepMultipliers[s_stepIdx]);

    lv_obj_t* stepLabel = lv_label_create(s_instPanel);
    lv_obj_set_style_text_font(stepLabel, &lv_font_montserrat_14, 0);
    lv_label_set_text(stepLabel, "Step:");
    lv_obj_align_to(stepLabel, endRowLabel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

    s_stepBtn = lv_button_create(s_instPanel);
    lv_obj_add_style(s_stepBtn, &styleSpinbox, 0);
    lv_obj_add_style(s_stepBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_set_width(s_stepBtn, 5 + (8 * 4));
    lv_obj_align_to(s_stepBtn, stepLabel, LV_ALIGN_OUT_RIGHT_MID, 2, 0);
    lv_obj_t* stepValue = lv_label_create(s_stepBtn);
    lv_obj_set_style_text_font(stepValue, &lv_font_montserrat_14, 0);
    lv_label_set_text_fmt(stepValue, "x%d", kStepMultipliers[s_stepIdx]);
    lv_obj_center(stepValue);
    lv_obj_add_event_cb(s_stepBtn, [](lv_event_t* e) {
        s_stepIdx = (s_stepIdx + 1) % kStepCount;
        lv_obj_t* label = lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0);
        if (label) lv_label_set_text_fmt(label, "x%d", kStepMultipliers[s_stepIdx]);
        lv_spinbox_set_step(s_startSpin, kStepMultipliers[s_stepIdx]);
        lv_spinbox_set_step(s_endSpin, kStepMultipliers[s_stepIdx]);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* attackRowLabel = nullptr;
    s_attackSpin = makeRow(s_instPanel, stepLabel, "Attack:", 0, 500, [](lv_event_t* e) {
        if (s_syncing) return;
        KitInfo* kit = currentKit();
        if (!kit) return;
        kit->instrument.attackMs =
            (uint16_t)lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        markDirty();
        applyParams();
    }, &attackRowLabel);

    lv_obj_t* releaseRowLabel = nullptr;
    s_releaseSpin = makeRow(s_instPanel, attackRowLabel, "Release:", 0, 5000, [](lv_event_t* e) {
        if (s_syncing) return;
        KitInfo* kit = currentKit();
        if (!kit) return;
        kit->instrument.releaseMs =
            (uint16_t)lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        markDirty();
        applyParams();
    }, &releaseRowLabel);

    lv_obj_t* voicesRowLabel = nullptr;
    s_voicesSpin = makeRow(s_instPanel, releaseRowLabel, "Voices:", 1,
                           crosspad::PitchedInstrument::kMaxSounding, [](lv_event_t* e) {
        if (s_syncing) return;
        KitInfo* kit = currentKit();
        if (!kit) return;
        kit->instrument.maxVoices =
            (uint8_t)lv_spinbox_get_value((lv_obj_t*)lv_event_get_target(e));
        markDirty();
        applyParams();
    }, &voicesRowLabel);

    s_removeBtn = lv_button_create(s_instPanel);
    lv_obj_add_style(s_removeBtn, &styleButton, 0);
    lv_obj_add_style(s_removeBtn, &styleButtonPressed, LV_STATE_PRESSED);
    lv_obj_add_style(s_removeBtn, &styleButtonInactive, LV_STATE_DISABLED);
    lv_obj_align_to(s_removeBtn, voicesRowLabel, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    lv_obj_t* removeLabel = lv_label_create(s_removeBtn);
    lv_obj_set_style_text_font(removeLabel, &lv_font_montserrat_10, 0);
    lv_label_set_text(removeLabel, "Remove zone");
    lv_obj_set_style_text_color(removeLabel, lv_color_hex(0x000000), 0);
    lv_obj_add_event_cb(s_removeBtn, [](lv_event_t*) {
        KitInfo* kit = currentKit();
        if (!kit) return;
        // One zone must remain: a bank with none plays nothing at all.
        if ((int)kit->instrument.zones.size() <= 1) return;
        if (s_zoneSel < 0 || s_zoneSel >= (int)kit->instrument.zones.size()) return;
        kit->instrument.zones.erase(kit->instrument.zones.begin() + s_zoneSel);
        if (s_zoneSel >= (int)kit->instrument.zones.size())
            s_zoneSel = (int)kit->instrument.zones.size() - 1;
        markDirty();
        rebuildBank();
        syncInstrumentTab();
        refreshZoneList();
        showZoneWaveform(s_zoneSel);
    }, LV_EVENT_CLICKED, nullptr);

    waveform_view_set_file_provider(fileWaveformProvider, nullptr);

    lv_obj_add_event_cb(s_cont, [](lv_event_t*) {
        waveform_view_set_file_provider(nullptr, nullptr);
        crosspad_gui::ui_back_remove(s_backToken);
        s_backToken = 0;
        if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
        lv_indev_restore_group(getEncoder(), s_priorGroup);
        s_priorGroup = nullptr;
        if (s_group) {
            lv_group_remove_all_objs(s_group);
            lv_group_delete(s_group);
            s_group = nullptr;
        }
        s_cont = nullptr;
        s_padPanel = s_instPanel = s_zoneList = s_waveform = nullptr;
        s_tabPadBtn = s_tabInstBtn = nullptr;
        s_noteSpin = s_noteLabel = s_volSpin = s_panSpin = s_zoneInfo = nullptr;
        s_rootSpin = s_autoCheck = s_startSpin = s_endSpin = s_stepBtn = nullptr;
        s_attackSpin = s_releaseSpin = s_voicesSpin = s_removeBtn = nullptr;
    }, LV_EVENT_DELETE, nullptr);

    setTab(0);
    syncInstrumentTab();
    syncPadTab(currentPad());
    s_lastPad = currentPad();
    s_syncing = false;

    s_timer = lv_timer_create(updateCb, 200, nullptr);
    s_backToken = crosspad_gui::ui_back_push(backCb, nullptr);
    return s_cont;
}

void pitched_editor_destroy() {
    if (s_cont) lv_obj_delete_async(s_cont);
}

} // namespace crosspad_sampler

#endif // USE_LVGL

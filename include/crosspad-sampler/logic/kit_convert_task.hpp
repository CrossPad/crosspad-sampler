// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file kit_convert_task.hpp
 * @brief A kit's samples converted to the engine's rate, off the UI thread.
 *
 * One long-lived worker, as the kit loader has: a conversion is minutes of
 * card reads, FFTs and writes, a tidy is a walk over every kit on the card.
 * The worker runs crosspad::KitRateConverter and tidyKit(); everything that
 * touches the kit model or the loader happens here on the LVGL thread, from a
 * timer, once the worker reports:
 *
 *  1. The commit retargeted the kit's JSON: the same edits go into the kit
 *     model in memory (no card read), so a kit that is parsed plays what the
 *     card says.
 *  2. The kit is current: its pads are reloaded (kit_load_task_start), the
 *     engine swapping each to its converted samples while it plays.
 *  3. Then the tidy removes the originals nothing names any more. A file a
 *     voice still reads stays and is tried again, every 10 s for a minute,
 *     and at the next conversion or load of the kit after that.
 */

#include <crosspad/kit/KitRateConvert.hpp>

#include <cstdint>

namespace crosspad_sampler {

enum class KitConvertJob : uint8_t { None, Convert, Tidy };

struct KitConvertStatus {
    KitConvertJob job  = KitConvertJob::None;   ///< running, or the last one
    bool     busy      = false;                 ///< the worker has it, or its follow-up is pending
    int      kitId     = -1;
    crosspad::KitRateConverter::Progress progress;   ///< the last Convert
    crosspad::KitTidyResult tidy;                    ///< the last Tidy
    bool     reloaded  = false;                 ///< the last Convert's kit was current and reloaded
    uint32_t elapsedMs = 0;                     ///< of the running job, or what the last one took
    uint32_t seq       = 0;                     ///< jobs finished since boot
};

/// Convert kit @p kitId's samples to @p rate (0: the engine's). LVGL thread.
/// False when a job or its follow-up is running, the platform has no
/// conversion, or there is no such kit.
bool kit_convert_start(int kitId, uint32_t rate = 0);

/// Stop the running conversion after the piece at hand. Any thread.
void kit_convert_cancel();

bool kit_convert_busy();
KitConvertStatus kit_convert_status();

/// Never offer the conversion for this kit (@p keep), or offer it again: the
/// model, saved as the Sampler's Save saves it unless pads hold unsaved
/// edits (their save then carries it). A loaded sampler kit only. LVGL thread.
bool kit_convert_keep_rate(int kitId, bool keep);

/// Remove the originals a finished conversion of @p kitId left, if any.
/// LVGL thread.
bool kit_convert_tidy(int kitId);

/// The rate a conversion goes to: the engine's, as the platform reports it,
/// 44100 when it cannot say.
uint32_t kit_convert_engine_rate();

} // namespace crosspad_sampler

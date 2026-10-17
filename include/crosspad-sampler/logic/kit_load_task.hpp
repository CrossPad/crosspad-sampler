// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include <crosspad-sampler/SamplerPlatformCallbacks.hpp>

namespace crosspad_sampler {

/// Initialize kit load task resources. Call once at startup.
/// On platforms using lv_async_call this is a no-op.
void kit_load_task_init();

/// Poll for kit load progress updates. Call from LVGL loop.
/// On platforms using lv_async_call this is a no-op.
void kit_load_task_poll();

/// Start the background kit load. Must be called from the LVGL thread.
///
/// Requesting a load while one is running does not fail and is not dropped:
/// the request is held and started when the running load reports. Only the
/// most recent one is kept, so spinning through the selector lands on the kit
/// you stopped at rather than replaying every kit you passed.
///
/// @param onlyModified  reload only pads the kit marks as modified.
/// @param kitId         when >= 0, the task runs KitOrchestrator::changeKit()
///                      itself before touching the pads. That call parses the
///                      kit JSON off storage and takes seconds on a cold SD
///                      card, so running it on the caller's thread froze the
///                      UI long enough to trip the task watchdog. Pass -1 when
///                      a kit is already current and only the pads need work.
void kit_load_task_start(bool onlyModified, int kitId = -1);

/// True while a background load task is running.
bool kit_load_task_is_loading();

/// True when called on the kit_load worker itself. The platform's pad reload
/// is also reached from the UI and the CDC task (pad editor, SMPL_PITCH);
/// work that must only ever run on the worker -- a pitched bank build: seconds
/// of card reads and FFT, behind the engine arena's owner lock -- checks this
/// and hands itself to the worker (kit_load_task_start) when it is not.
bool kit_load_task_is_worker();

/// Kit id held back until the running load finishes, or -1 if none is waiting.
int kit_load_task_pending_kit();

/// Why a load failed, for the swap log (the UI gets it in words).
enum class KitLoadWhy : uint8_t {
    None = 0,
    Parse,        ///< the kit could not be read or made current
    NoSamples,    ///< no pad of the kit names a sample
    NoneReadable, ///< pads name samples, the engine could take none of them
};

/// What one kit load did and what it cost. Kept for the last few loads so a
/// bench run can read them back after the fact (CDC KIT_SWAP_LOG): a swap
/// that sounded wrong is then a row with numbers, not a memory.
struct KitSwapRecord {
    uint32_t seq       = 0;      ///< 1-based, counts every load since boot
    int      kitId     = -1;     ///< -1 when only the pads were reloaded
    bool     ok        = false;
    uint32_t reqMs     = 0;      ///< uptime at the request
    uint32_t parseMs   = 0;      ///< request → kit parsed and current
    uint32_t doneMs    = 0;      ///< request → every pad reloaded
    int32_t  padDoneMs[16] = {}; ///< request → that pad reloaded; -1 = untouched
    uint16_t deferred  = 0;      ///< pads that waited for their voice to end
    uint16_t forced    = 0;      ///< pads reloaded while still sounding
    uint16_t empty     = 0;      ///< pads the kit gives no sample
    uint16_t failed    = 0;      ///< pads with a sample the engine could not take
    KitLoadWhy why     = KitLoadWhy::None;
    EngineLoadStats engine;      ///< deltas over the pad loop (gateMaxUs: peak)
};

/// How many loads the log holds (≤ 8, oldest first).
int kit_load_task_swap_log_count();
/// Copy the i-th (0 = oldest) record of the log; false when out of range.
bool kit_load_task_swap_log(int i, KitSwapRecord* out);


} // namespace crosspad_sampler

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file SamplerRtos.hpp
 * @brief The concurrency primitives the sampler needs, on every platform.
 *
 * The waveform cache, the waveform loader and the kit load worker need a
 * mutex, a doorbell, a bounded queue and tasks. Called directly, FreeRTOS and
 * ESP-IDF (`xTaskCreatePinnedToCoreWithCaps`, `heap_caps_malloc`,
 * `freertos/…` include paths) compile on two of the three platforms this app
 * runs on. These wrappers have no platform header.
 *
 * The shapes are the FreeRTOS ones (a timed take, a bounded queue with a
 * front-insert, a task with a stack size, priority and core), so the
 * behaviour tuned on hardware — PSRAM stacks and queue storage, pinned cores,
 * the priorities — is what the board still gets.
 *
 * There is one implementation and it is FreeRTOS, on every platform: the
 * desktop simulator links the kernel too (POSIX port) and runs its LVGL task
 * on it. What differs is real platform difference: the include path, the
 * stack unit (ESP-IDF counts bytes, vanilla FreeRTOS counts words), where a
 * stack or a queue's storage is allocated from, and how many priority levels
 * exist.
 */

#include <cstddef>
#include <cstdint>

namespace crosspad_sampler {

/// Block until the operation succeeds.
inline constexpr uint32_t kWaitForever = 0xFFFFFFFFu;

// ── Mutex ────────────────────────────────────────────────────────────────

/// Created on first take(), like the lazily created FreeRTOS mutex it
/// replaces: the waveform cache is reached from whichever kit-load path ran
/// first, and an init-only lock left half of them silently uncached.
class SamplerMutex {
public:
    SamplerMutex() = default;
    ~SamplerMutex();

    SamplerMutex(const SamplerMutex&)            = delete;
    SamplerMutex& operator=(const SamplerMutex&) = delete;

    /// @param timeoutMs 0 = try once, kWaitForever = block.
    bool take(uint32_t timeoutMs);
    void give();
    /// True once a take() has created it.
    bool valid() const { return impl_ != nullptr; }

private:
    void* impl_ = nullptr;
    void* ensure();
};

// ── Binary signal (a doorbell, not a counter) ────────────────────────────

class SamplerSignal {
public:
    SamplerSignal() = default;
    ~SamplerSignal();

    SamplerSignal(const SamplerSignal&)            = delete;
    SamplerSignal& operator=(const SamplerSignal&) = delete;

    bool create();
    void give();
    bool take(uint32_t timeoutMs);
    bool valid() const { return impl_ != nullptr; }

private:
    void* impl_ = nullptr;
};

// ── Bounded queue of fixed-size items ────────────────────────────────────

/// Storage from PSRAM on ESP-IDF: the waveform loader's queue is 4.5 kB.
class SamplerQueueBase {
public:
    SamplerQueueBase() = default;
    ~SamplerQueueBase();

    SamplerQueueBase(const SamplerQueueBase&)            = delete;
    SamplerQueueBase& operator=(const SamplerQueueBase&) = delete;

    /// Creates the queue once; later calls only report whether it exists.
    bool create(size_t depth, size_t itemSize);
    bool valid() const { return impl_ != nullptr; }

    bool send(const void* item);        ///< to the back, never blocks
    bool sendToFront(const void* item); ///< jumps the queue, never blocks
    bool receive(void* item, uint32_t timeoutMs);

    void   reset();       ///< discard everything queued
    size_t count() const; ///< items waiting

private:
    void* impl_ = nullptr;
};

/// Typed wrapper. The item type must be trivially copyable — these queues copy
/// bytes, exactly as the FreeRTOS ones do.
template <typename T>
class SamplerQueue : public SamplerQueueBase {
public:
    bool create(size_t depth) { return SamplerQueueBase::create(depth, sizeof(T)); }
    bool send(const T& item) { return SamplerQueueBase::send(&item); }
    bool sendToFront(const T& item) { return SamplerQueueBase::sendToFront(&item); }
    bool receive(T& item, uint32_t timeoutMs) {
        return SamplerQueueBase::receive(&item, timeoutMs);
    }
};

// ── Tasks ────────────────────────────────────────────────────────────────

/// A task. On ESP-IDF its stack is in PSRAM (`xTaskCreatePinnedToCoreWithCaps`),
/// and such a task is *static*: only `vTaskDeleteWithCaps` frees its stack and
/// TCB, which is what exitSelf() and kill() use there.
///
/// @param stackBytes / @param priority / @param core are honoured on the
///        board; on the desktop the stack is converted to words, the priority
///        clamped to the kernel's levels and the core ignored.
class SamplerTask {
public:
    SamplerTask() = default;

    SamplerTask(const SamplerTask&)            = delete;
    SamplerTask& operator=(const SamplerTask&) = delete;

    bool start(void (*fn)(void*), void* arg, const char* name,
               size_t stackBytes, int priority, int core);
    /// True from start() until exitSelf() or kill().
    bool running() const { return impl_ != nullptr; }
    /// True when called from this task.
    bool isCurrent() const;
    /// The task's own last statement: forgets the handle, then deletes itself.
    void exitSelf();
    /// Deletes the task from another one.
    void kill();

private:
    void* volatile impl_ = nullptr;
};

// ── Misc ─────────────────────────────────────────────────────────────────

void sampler_delay_ms(uint32_t ms);

/// Large buffer: PSRAM on the ESP32-S3 (nullptr when it has none left), the
/// default heap elsewhere. Freed with free().
void* sampler_alloc_big(size_t bytes);

} // namespace crosspad_sampler

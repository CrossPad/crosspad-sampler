// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file SamplerRtos.hpp
 * @brief The concurrency primitives the sampler needs, on every platform.
 *
 * The waveform cache, the waveform loader and the kit load worker used to call
 * FreeRTOS and ESP-IDF directly — `xTaskCreatePinnedToCore`, `heap_caps_malloc`,
 * `freertos/…` include paths. That is exactly what an app in a shared repo must
 * not do: it compiled on two of the three platforms it is supposed to run on.
 *
 * The shapes here are deliberately the FreeRTOS ones (a timed take, a bounded
 * queue with a front-insert, a long-lived worker with a stack size, priority
 * and core), so the embedded behaviour that was tuned on hardware — the PSRAM
 * stack, the pinned core, priority 5 for the loader — survives verbatim and the
 * call sites barely change.
 *
 * There is one implementation and it is FreeRTOS, on every platform. The
 * desktop simulator links the kernel too (POSIX port) and already runs its
 * LVGL task on it, so a second concurrency model would only make the simulator
 * behave unlike the board — which is the one thing a simulator must not do.
 * The platform differences that remain are real ones: the include path, the
 * stack unit (ESP-IDF counts bytes, vanilla FreeRTOS counts words), where the
 * stack is allocated from, and how many priority levels exist.
 */

#include <cstddef>
#include <cstdint>

namespace crosspad_sampler {

/// Block until the operation succeeds.
inline constexpr uint32_t kWaitForever = 0xFFFFFFFFu;

// ── Mutex ────────────────────────────────────────────────────────────────

/// Created on first use, like the FreeRTOS mutex it replaces — the waveform
/// cache depends on that: it is reached from whichever kit-load path ran
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

class SamplerQueueBase {
public:
    SamplerQueueBase() = default;
    ~SamplerQueueBase();

    SamplerQueueBase(const SamplerQueueBase&)            = delete;
    SamplerQueueBase& operator=(const SamplerQueueBase&) = delete;

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
/// bytes, exactly as the FreeRTOS ones did.
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

// ── Long-lived worker ────────────────────────────────────────────────────

/// Started once and parked between jobs. Never ended: on ESP-IDF a task built
/// by `xTaskCreatePinnedToCoreWithCaps` is *static*, and only
/// `vTaskDeleteWithCaps` frees its stack and TCB — a worker spawned per job
/// and killed with `vTaskDelete` leaked 8 kB of PSRAM and 372 B of internal
/// RAM every time, which is what starved the SD driver after ten kit swaps.
class SamplerWorker {
public:
    SamplerWorker() = default;

    SamplerWorker(const SamplerWorker&)            = delete;
    SamplerWorker& operator=(const SamplerWorker&) = delete;

    /// @param stackBytes / @param priority / @param core are honoured on
    ///        FreeRTOS and ignored on the desktop, where the OS decides.
    bool start(void (*fn)(void*), void* arg, const char* name,
               size_t stackBytes, int priority, int core);
    bool started() const { return impl_ != nullptr; }

private:
    void* impl_ = nullptr;
};

// ── Misc ─────────────────────────────────────────────────────────────────

void sampler_delay_ms(uint32_t ms);

/// Large, short-lived buffer. Routed to PSRAM where there is one; the callers
/// that have a platform allocator (the waveform callbacks) still prefer it.
void* sampler_alloc_big(size_t bytes);
void  sampler_free_big(void* ptr);

} // namespace crosspad_sampler

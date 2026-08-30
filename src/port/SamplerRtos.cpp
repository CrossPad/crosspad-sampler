// SPDX-License-Identifier: MIT

#include "crosspad-sampler/port/SamplerRtos.hpp"

#include <cstdlib>
#include <cstring>

// One implementation, FreeRTOS, on every platform the sampler runs on. The
// desktop simulator links the FreeRTOS kernel too (POSIX port) and runs the
// LVGL task on it, so there is no reason for a second concurrency model here —
// and a good reason against one: anything that behaves differently between the
// simulator and the board makes the simulator worth less as a test bench.
//
// Only three things actually differ, and each is a real platform difference
// rather than a style choice:
//   • the include path (`freertos/…` under ESP-IDF and Arduino, bare on the
//     vanilla kernel),
//   • the stack unit — ESP-IDF's xTaskCreate takes BYTES, vanilla FreeRTOS
//     takes WORDS, which is a factor of four waiting to bite,
//   • where the stack comes from: PSRAM on the ESP32-S3, the FreeRTOS heap
//     elsewhere.
#if defined(ESP_PLATFORM) || defined(ARDUINO)
  #define SAMPLER_RTOS_IDF_PATHS 1
  #include <freertos/FreeRTOS.h>
  #include <freertos/task.h>
  #include <freertos/semphr.h>
  #include <freertos/queue.h>
  #if defined(ESP_PLATFORM)
    #include <freertos/idf_additions.h>
    #include <esp_heap_caps.h>
  #endif
#else
  #define SAMPLER_RTOS_IDF_PATHS 0
  #include "FreeRTOS.h"
  #include "task.h"
  #include "semphr.h"
  #include "queue.h"
#endif

namespace crosspad_sampler {

namespace {

inline TickType_t ticks(uint32_t ms) {
    return (ms == kWaitForever) ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}

/// configMAX_PRIORITIES is 5 on the simulator and 25 on ESP-IDF. A priority at
/// or above the cap is a configASSERT inside xTaskCreate, so the tuned
/// embedded numbers are clamped rather than carried over blindly.
inline UBaseType_t clampPriority(int priority) {
    if (priority < 0) priority = 0;
    if (priority > (int)configMAX_PRIORITIES - 1) priority = (int)configMAX_PRIORITIES - 1;
    return (UBaseType_t)priority;
}

} // namespace

// ── Mutex ────────────────────────────────────────────────────────────────

void* SamplerMutex::ensure() {
    if (!impl_) impl_ = xSemaphoreCreateMutex();
    return impl_;
}
SamplerMutex::~SamplerMutex() {
    if (impl_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(impl_));
}
bool SamplerMutex::take(uint32_t timeoutMs) {
    void* h = ensure();
    if (!h) return false;
    return xSemaphoreTake(static_cast<SemaphoreHandle_t>(h), ticks(timeoutMs)) == pdTRUE;
}
void SamplerMutex::give() {
    if (impl_) xSemaphoreGive(static_cast<SemaphoreHandle_t>(impl_));
}

// ── Signal ───────────────────────────────────────────────────────────────

SamplerSignal::~SamplerSignal() {
    if (impl_) vSemaphoreDelete(static_cast<SemaphoreHandle_t>(impl_));
}
bool SamplerSignal::create() {
    if (!impl_) impl_ = xSemaphoreCreateBinary();
    return impl_ != nullptr;
}
void SamplerSignal::give() {
    if (impl_) xSemaphoreGive(static_cast<SemaphoreHandle_t>(impl_));
}
bool SamplerSignal::take(uint32_t timeoutMs) {
    if (!impl_) return false;
    return xSemaphoreTake(static_cast<SemaphoreHandle_t>(impl_), ticks(timeoutMs)) == pdTRUE;
}

// ── Queue ────────────────────────────────────────────────────────────────

SamplerQueueBase::~SamplerQueueBase() {
    if (impl_) vQueueDelete(static_cast<QueueHandle_t>(impl_));
}
bool SamplerQueueBase::create(size_t depth, size_t itemSize) {
    if (!impl_) impl_ = xQueueCreate(depth, itemSize);
    return impl_ != nullptr;
}
bool SamplerQueueBase::send(const void* item) {
    return impl_ && xQueueSend(static_cast<QueueHandle_t>(impl_), item, 0) == pdTRUE;
}
bool SamplerQueueBase::sendToFront(const void* item) {
    return impl_ && xQueueSendToFront(static_cast<QueueHandle_t>(impl_), item, 0) == pdTRUE;
}
bool SamplerQueueBase::receive(void* item, uint32_t timeoutMs) {
    return impl_ &&
           xQueueReceive(static_cast<QueueHandle_t>(impl_), item, ticks(timeoutMs)) == pdTRUE;
}
void SamplerQueueBase::reset() {
    if (impl_) xQueueReset(static_cast<QueueHandle_t>(impl_));
}
size_t SamplerQueueBase::count() const {
    return impl_ ? uxQueueMessagesWaiting(static_cast<QueueHandle_t>(impl_)) : 0;
}

// ── Worker ───────────────────────────────────────────────────────────────

bool SamplerWorker::start(void (*fn)(void*), void* arg, const char* name,
                          size_t stackBytes, int priority, int core) {
    if (impl_) return true;
    TaskHandle_t h = nullptr;
    const UBaseType_t prio = clampPriority(priority);

#if defined(ESP_PLATFORM)
    // The stack comes from PSRAM: internal RAM is the scarce one, and these
    // workers spend their lives blocked on the card rather than on the CPU.
    // ESP-IDF sizes the stack in bytes.
    if (xTaskCreatePinnedToCoreWithCaps(fn, name, stackBytes, arg, prio, &h,
                                        static_cast<BaseType_t>(core),
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        return false;
    }
#elif defined(ARDUINO)
    if (xTaskCreatePinnedToCore(fn, name, stackBytes, arg, prio, &h,
                                static_cast<BaseType_t>(core)) != pdPASS) {
        return false;
    }
#else
    // Vanilla FreeRTOS sizes the stack in words, and has no core affinity.
    //
    // On the POSIX port the number is close to cosmetic: pxPortInitialiseStack()
    // calls pthread_create() with a default attribute and never sets a stack
    // size, so the task actually runs on the system's 8 MB pthread stack and
    // this buffer only has to be larger than the port's Thread_t. Keeping the
    // conversion honest anyway means the same figure means the same thing on
    // every port, including ones that do use it.
    (void)core;
    const configSTACK_DEPTH_TYPE words =
        (configSTACK_DEPTH_TYPE)(stackBytes / sizeof(StackType_t));
    if (xTaskCreate(fn, name, words, arg, prio, &h) != pdPASS) {
        return false;
    }
#endif
    impl_ = h;
    return true;
}

void sampler_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void* sampler_alloc_big(size_t bytes) {
#if defined(ESP_PLATFORM)
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(bytes);
#else
    return malloc(bytes);
#endif
}
void sampler_free_big(void* ptr) { free(ptr); }

} // namespace crosspad_sampler

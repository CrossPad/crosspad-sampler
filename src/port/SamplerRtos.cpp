// SPDX-License-Identifier: GPL-3.0-or-later

#include "crosspad-sampler/port/SamplerRtos.hpp"

#include <cstdlib>

// Arduino-ESP32 defines ESP_PLATFORM too, but has neither the *WithCaps task
// and queue calls nor a heap for this loader's stacks, so it is tested first.
#if defined(ARDUINO)
  #define SAMPLER_RTOS_ARDUINO 1
#elif defined(ESP_PLATFORM)
  #define SAMPLER_RTOS_IDF 1
#endif

#if defined(SAMPLER_RTOS_ARDUINO) || defined(SAMPLER_RTOS_IDF)
  #include <freertos/FreeRTOS.h>
  #include <freertos/task.h>
  #include <freertos/semphr.h>
  #include <freertos/queue.h>
#else
  #include "FreeRTOS.h"
  #include "task.h"
  #include "semphr.h"
  #include "queue.h"
#endif

#if defined(SAMPLER_RTOS_IDF)
  #include <freertos/idf_additions.h>
#endif
#if defined(ESP_PLATFORM)
  #include <esp_heap_caps.h>
#endif

namespace crosspad_sampler {

namespace {

inline TickType_t ticks(uint32_t ms) {
    return (ms == kWaitForever) ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}

/// configMAX_PRIORITIES is 5 on the simulator and 25 on ESP-IDF; a priority at
/// or above it is a configASSERT inside xTaskCreate.
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
    if (!impl_) return;
#if defined(SAMPLER_RTOS_IDF)
    vQueueDeleteWithCaps(static_cast<QueueHandle_t>(impl_));
#else
    vQueueDelete(static_cast<QueueHandle_t>(impl_));
#endif
}
bool SamplerQueueBase::create(size_t depth, size_t itemSize) {
    if (!impl_) {
#if defined(SAMPLER_RTOS_IDF)
        impl_ = xQueueCreateWithCaps(depth, itemSize, MALLOC_CAP_SPIRAM);
#else
        impl_ = xQueueCreate(depth, itemSize);
#endif
    }
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

// ── Task ─────────────────────────────────────────────────────────────────

bool SamplerTask::start(void (*fn)(void*), void* arg, const char* name,
                        size_t stackBytes, int priority, int core) {
    TaskHandle_t h = nullptr;
    const UBaseType_t prio = clampPriority(priority);
#if defined(SAMPLER_RTOS_IDF)
    if (xTaskCreatePinnedToCoreWithCaps(fn, name, stackBytes, arg, prio, &h,
                                        static_cast<BaseType_t>(core),
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        return false;
    }
#elif defined(SAMPLER_RTOS_ARDUINO)
    if (xTaskCreatePinnedToCore(fn, name, stackBytes, arg, prio, &h,
                                static_cast<BaseType_t>(core)) != pdPASS) {
        return false;
    }
#else
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

bool SamplerTask::isCurrent() const {
    return impl_ != nullptr && xTaskGetCurrentTaskHandle() == static_cast<TaskHandle_t>(impl_);
}

void SamplerTask::exitSelf() {
    impl_ = nullptr;
#if defined(SAMPLER_RTOS_IDF)
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

void SamplerTask::kill() {
    TaskHandle_t h = static_cast<TaskHandle_t>(impl_);
    if (!h) return;
#if defined(SAMPLER_RTOS_IDF)
    vTaskDeleteWithCaps(h);
#else
    vTaskDelete(h);
#endif
    impl_ = nullptr;
}

// ── Misc ─────────────────────────────────────────────────────────────────

void sampler_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void* sampler_alloc_big(size_t bytes) {
#if defined(ESP_PLATFORM)
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}

} // namespace crosspad_sampler

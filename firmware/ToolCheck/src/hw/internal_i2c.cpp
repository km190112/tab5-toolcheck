#include "internal_i2c.h"

#include <Arduino.h>

namespace toolcheck {
namespace internal_i2c {
namespace {

SemaphoreHandle_t g_mutex = nullptr;

}  // namespace

void begin() {
  if (g_mutex == nullptr) g_mutex = xSemaphoreCreateMutex();
}

bool tryLock() { return g_mutex != nullptr && xSemaphoreTake(g_mutex, 0) == pdTRUE; }

bool lock(uint32_t timeout_ms) { return g_mutex != nullptr && xSemaphoreTake(g_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE; }

void unlock() {
  if (g_mutex != nullptr) xSemaphoreGive(g_mutex);
}

}  // namespace internal_i2c
}  // namespace toolcheck

/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    debug_log.h
  * @brief   UART-backed debug logging helpers.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __DEBUG_LOG_H__
#define __DEBUG_LOG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stddef.h>

typedef enum {
  DEBUG_LEVEL_CRITICAL = 0,
  DEBUG_LEVEL_ERROR,
  DEBUG_LEVEL_WARN,
  DEBUG_LEVEL_INFO,
  DEBUG_LEVEL_DEBUG,
} DebugLevel_t;

typedef enum {
  DEBUG_CLASS_SYSTEM = 0,
  DEBUG_CLASS_IMU,
  DEBUG_CLASS_COMM,
  DEBUG_CLASS_WIFI
} DebugClass_t;

void DebugLog_Init(UART_HandleTypeDef *huart);
void Debug_Log(DebugLevel_t level, DebugClass_t debug_class, const char *fmt, ...);
void Debug_Print(const char *msg);
void Debug_WriteRaw(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __DEBUG_LOG_H__ */

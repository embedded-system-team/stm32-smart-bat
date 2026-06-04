/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    debug_log.c
  * @brief   UART-backed debug logging helpers.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "debug_log.h"
#include "cmsis_os2.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DEBUG_LOG_BUFFER_SIZE 256U

static UART_HandleTypeDef *debug_uart;
static osMutexId_t debug_uart_mutex;

static const char *DebugLevelName(DebugLevel_t level);
static const char *DebugClassName(DebugClass_t debug_class);
static void DebugTransmit(const uint8_t *data, uint16_t len);

void DebugLog_Init(UART_HandleTypeDef *huart)
{
  debug_uart = huart;

  if (debug_uart_mutex == NULL)
  {
    debug_uart_mutex = osMutexNew(NULL);
  }
}

void Debug_Log(DebugLevel_t level, DebugClass_t debug_class, const char *fmt, ...)
{
  char msg[DEBUG_LOG_BUFFER_SIZE];
  va_list args;

  if (fmt == NULL)
  {
    return;
  }

  int len = snprintf(msg,
                     sizeof(msg),
                     "[%s][%s] ",
                     DebugLevelName(level),
                     DebugClassName(debug_class));

  if (len < 0)
  {
    return;
  }

  if ((size_t)len >= sizeof(msg))
  {
    len = (int)(sizeof(msg) - 1U);
  }

  va_start(args, fmt);
  int body_len = vsnprintf(&msg[len], sizeof(msg) - (size_t)len, fmt, args);
  va_end(args);

  if (body_len < 0)
  {
    return;
  }

  size_t total_len = (size_t)len + (size_t)body_len;
  if (total_len >= sizeof(msg))
  {
    total_len = sizeof(msg) - 1U;
  }

  if ((total_len < 2U) ||
      (msg[total_len - 2U] != '\r') ||
      (msg[total_len - 1U] != '\n'))
  {
    if (total_len < (sizeof(msg) - 2U))
    {
      msg[total_len++] = '\r';
      msg[total_len++] = '\n';
    }
  }

  DebugTransmit((const uint8_t *)msg, (uint16_t)total_len);
}

void Debug_Print(const char *msg)
{
  if (msg == NULL)
  {
    return;
  }

  Debug_WriteRaw((const uint8_t *)msg, strlen(msg));
}

void Debug_WriteRaw(const uint8_t *data, size_t len)
{
  if ((data == NULL) || (len == 0U))
  {
    return;
  }

  if (len > UINT16_MAX)
  {
    len = UINT16_MAX;
  }

  DebugTransmit(data, (uint16_t)len);
}

static const char *DebugLevelName(DebugLevel_t level)
{
  switch (level)
  {
    case DEBUG_LEVEL_ERROR:
      return "ERROR";
    case DEBUG_LEVEL_CRITICAL:
      return "CRITICAL";
    case DEBUG_LEVEL_WARN:
      return "WARN";
    case DEBUG_LEVEL_INFO:
      return "INFO";
    case DEBUG_LEVEL_DEBUG:
      return "DEBUG";
    default:
      return "UNKNOWN";
  }
}

static const char *DebugClassName(DebugClass_t debug_class)
{
  switch (debug_class)
  {
    case DEBUG_CLASS_SYSTEM:
      return "SYS";
    case DEBUG_CLASS_IMU:
      return "IMU";
    case DEBUG_CLASS_COMM:
      return "COMM";
    default:
      return "UNKNOWN";
  }
}

static void DebugTransmit(const uint8_t *data, uint16_t len)
{
  if ((debug_uart == NULL) || (data == NULL) || (len == 0U))
  {
    return;
  }

  if ((debug_uart_mutex != NULL) && (osKernelGetState() == osKernelRunning))
  {
    if (osMutexAcquire(debug_uart_mutex, osWaitForever) == osOK)
    {
      HAL_UART_Transmit(debug_uart, (uint8_t *)data, len, HAL_MAX_DELAY);
      (void)osMutexRelease(debug_uart_mutex);
    }

    return;
  }

  HAL_UART_Transmit(debug_uart, (uint8_t *)data, len, HAL_MAX_DELAY);
}

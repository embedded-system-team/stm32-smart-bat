/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "b_l475e_iot01a1_bus.h"
#include "lsm6dsl.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct {
  uint32_t timestamp_ms;

  int32_t ax;
  int32_t ay;
  int32_t az;

  int32_t gx;
  int32_t gy;
  int32_t gz;
} IMUSample_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
static LSM6DSL_Object_t imu;
static uint32_t dropped_samples = 0;
static osMessageQueueId_t imuQueueHandle;
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for sensorTask */
osThreadId_t sensorTaskHandle;
const osThreadAttr_t sensorTask_attributes = {
  .name = "sensorTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for commTask */
osThreadId_t commTaskHandle;
const osThreadAttr_t commTask_attributes = {
  .name = "commTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
static void ImuDelay(uint32_t ms);
static void UartPrint(const char *msg);
static int32_t ImuInit(void);

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void StartSensorTask(void *argument);
void StartCommTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of sensorTask */
  sensorTaskHandle = osThreadNew(StartSensorTask, NULL, &sensorTask_attributes);

  /* creation of commTask */
  commTaskHandle = osThreadNew(StartCommTask, NULL, &commTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  imuQueueHandle = osMessageQueueNew(32, sizeof(IMUSample_t), NULL);
  if (imuQueueHandle == NULL)
  {
    Error_Handler();
  }
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  for (;;) {
    osDelay(1000);
  }
  /* USER CODE END StartDefaultTask */
}

/* USER CODE BEGIN Header_StartSensorTask */
/**
* @brief Function implementing the sensorTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartSensorTask */
void StartSensorTask(void *argument)
{
  IMUSample_t sample;
  LSM6DSL_Axes_t acc;
  LSM6DSL_Axes_t gyro;

  if (ImuInit() != LSM6DSL_OK)
  {
    UartPrint("LSM6DSL init failed\r\n");

    for (;;)
    {
      osDelay(1000);
    }
  }

  UartPrint("LSM6DSL ready\r\n");

  for (;;)
  {
    if ((LSM6DSL_ACC_GetAxes(&imu, &acc) == LSM6DSL_OK) &&
        (LSM6DSL_GYRO_GetAxes(&imu, &gyro) == LSM6DSL_OK))
    {
      sample.timestamp_ms = HAL_GetTick();

      sample.ax = acc.x;
      sample.ay = acc.y;
      sample.az = acc.z;

      sample.gx = gyro.x;
      sample.gy = gyro.y;
      sample.gz = gyro.z;

      if (osMessageQueuePut(imuQueueHandle, &sample, 0U, 0U) != osOK)
      {
        dropped_samples++;
      }
    }

    osDelay(10);
  }
}

/* USER CODE BEGIN Header_StartCommTask */
/**
* @brief Function implementing the commTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartCommTask */
void StartCommTask(void *argument)
{
  IMUSample_t sample;
  char msg[160];

  for (;;)
  {
    if (osMessageQueueGet(imuQueueHandle, &sample, NULL, osWaitForever) == osOK)
    {
      int len = snprintf(
          msg,
          sizeof(msg),
          "%lu,%ld,%ld,%ld,%ld,%ld,%ld,%lu\r\n",
          (unsigned long)sample.timestamp_ms,
          (long)sample.ax,
          (long)sample.ay,
          (long)sample.az,
          (long)sample.gx,
          (long)sample.gy,
          (long)sample.gz,
          (unsigned long)dropped_samples
      );

      if (len > 0)
      {
        HAL_UART_Transmit(&huart1, (uint8_t *)msg, (uint16_t)len, HAL_MAX_DELAY);
      }
    }
  }
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
static void ImuDelay(uint32_t ms)
{
  osDelay(ms);
}

static void UartPrint(const char *msg)
{
  HAL_UART_Transmit(&huart1, (uint8_t *)msg, strlen(msg), HAL_MAX_DELAY);
}

static int32_t ImuInit(void)
{
  LSM6DSL_IO_t io_ctx = {
    .Init = BSP_I2C2_Init,
    .DeInit = BSP_I2C2_DeInit,
    .BusType = LSM6DSL_I2C_BUS,
    .Address = (uint8_t)(LSM6DSL_I2C_ADD_H & 0xFEU),
    .WriteReg = BSP_I2C2_WriteReg,
    .ReadReg = BSP_I2C2_ReadReg,
    .GetTick = BSP_GetTick,
    .Delay = ImuDelay,
  };
  const uint8_t addresses[] = {
    (uint8_t)(LSM6DSL_I2C_ADD_H & 0xFEU),
    (uint8_t)(LSM6DSL_I2C_ADD_L & 0xFEU),
  };
  uint8_t id = 0U;

  for (uint32_t i = 0U; i < (sizeof(addresses) / sizeof(addresses[0])); i++)
  {
    io_ctx.Address = addresses[i];

    if ((LSM6DSL_RegisterBusIO(&imu, &io_ctx) == LSM6DSL_OK) &&
        (LSM6DSL_ReadID(&imu, &id) == LSM6DSL_OK) &&
        (id == LSM6DSL_ID) &&
        (LSM6DSL_Init(&imu) == LSM6DSL_OK) &&
        (LSM6DSL_ACC_SetOutputDataRate(&imu, 104.0f) == LSM6DSL_OK) &&
        (LSM6DSL_ACC_SetFullScale(&imu, 4) == LSM6DSL_OK) &&
        (LSM6DSL_GYRO_SetOutputDataRate(&imu,104.0f) == LSM6DSL_OK) &&
        (LSM6DSL_GYRO_SetFullScale(&imu, 500) == LSM6DSL_OK) &&
        (LSM6DSL_ACC_Enable(&imu) == LSM6DSL_OK) &&
        (LSM6DSL_GYRO_Enable(&imu) == LSM6DSL_OK))
    {
      return LSM6DSL_OK;
    }
  }

  return LSM6DSL_ERROR;
}

/* USER CODE END Application */


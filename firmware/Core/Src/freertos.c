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
#include "debug_log.h"
#include "lsm6dsl.h"
#include "usart.h"
#include <stdio.h>
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

static int32_t gyro_bias_x = 0;
static int32_t gyro_bias_y = 0;
static int32_t gyro_bias_z = 0;

static uint32_t imu_read_failures = 0;
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
static int32_t ImuInit(void);

static int32_t ImuCalibrateGyroBias(void);
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
  DebugLog_Init(&huart1);
  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_SYSTEM, "FreeRTOS init");
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
  imuQueueHandle = osMessageQueueNew(32, sizeof(IMUSample_t), NULL);
  if (imuQueueHandle == NULL)
  {
    Debug_Log(DEBUG_LEVEL_CRITICAL, DEBUG_CLASS_SYSTEM, "imuQueue create failed");
    Error_Handler();
  }
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);
  if (defaultTaskHandle == NULL)
  {
    Debug_Log(DEBUG_LEVEL_CRITICAL, DEBUG_CLASS_SYSTEM, "defaultTask create failed");
    Error_Handler();
  }

  /* creation of sensorTask */
  sensorTaskHandle = osThreadNew(StartSensorTask, NULL, &sensorTask_attributes);
  if (sensorTaskHandle == NULL)
  {
    Debug_Log(DEBUG_LEVEL_CRITICAL, DEBUG_CLASS_SYSTEM, "sensorTask create failed");
    Error_Handler();
  }

  /* creation of commTask */
  commTaskHandle = osThreadNew(StartCommTask, NULL, &commTask_attributes);
  if (commTaskHandle == NULL)
  {
    Debug_Log(DEBUG_LEVEL_CRITICAL, DEBUG_CLASS_SYSTEM, "commTask create failed");
    Error_Handler();
  }

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
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

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_IMU, "Sensor task start");

  if (ImuInit() != LSM6DSL_OK)
  {
    Debug_Log(DEBUG_LEVEL_ERROR, DEBUG_CLASS_IMU, "LSM6DSL init failed");

    for (;;)
    {
      osDelay(1000);
    }
  }

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_IMU, "LSM6DSL ready");

  if (ImuCalibrateGyroBias() != LSM6DSL_OK)
  {
    Debug_Log(DEBUG_LEVEL_ERROR, DEBUG_CLASS_IMU, "Gyro calibration failed");

    for (;;)
    {
      osDelay(1000);
    }
  }

  for (;;)
  {
    int32_t acc_status = LSM6DSL_ACC_GetAxes(&imu, &acc);
    int32_t gyro_status = LSM6DSL_GYRO_GetAxes(&imu, &gyro);

    if ((acc_status == LSM6DSL_OK) && (gyro_status == LSM6DSL_OK))
    {
      sample.timestamp_ms = HAL_GetTick();

      sample.ax = acc.x;
      sample.ay = acc.y;
      sample.az = acc.z;

      sample.gx = gyro.x - gyro_bias_x;
      sample.gy = gyro.y - gyro_bias_y;
      sample.gz = gyro.z - gyro_bias_z;

      if (osMessageQueuePut(imuQueueHandle, &sample, 0U, 0U) != osOK)
      {
        dropped_samples++;
      }
    }
    else
    {
      imu_read_failures++;
      if ((imu_read_failures % 100U) == 1U)
      {
        Debug_Log(DEBUG_LEVEL_WARN,
                  DEBUG_CLASS_IMU,
                  "IMU read failed acc=%ld gyro=%ld count=%lu",
                  (long)acc_status,
                  (long)gyro_status,
                  (unsigned long)imu_read_failures);
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

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_COMM, "Comm task start");

  for (;;)
  {
    if (osMessageQueueGet(imuQueueHandle, &sample, NULL, osWaitForever) == osOK)
    {
      Debug_Log(DEBUG_LEVEL_DEBUG,
                DEBUG_CLASS_COMM,
                "imu t=%lu acc=%ld,%ld,%ld gyro=%ld,%ld,%ld dropped=%lu",
                (unsigned long)sample.timestamp_ms,
                (long)sample.ax,
                (long)sample.ay,
                (long)sample.az,
                (long)sample.gx,
                (long)sample.gy,
                (long)sample.gz,
                (unsigned long)dropped_samples);
    }
  }
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
static void ImuDelay(uint32_t ms)
{
  osDelay(ms);
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

static int32_t ImuCalibrateGyroBias(void)
{
  const uint32_t sample_count = 100;
  int64_t sum_x = 0;
  int64_t sum_y = 0;
  int64_t sum_z = 0;
  LSM6DSL_Axes_t gyro;

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_IMU, "Gyro calibration start. Keep board still...");

  for (uint32_t i = 0; i < sample_count; i++)
  {
    if (LSM6DSL_GYRO_GetAxes(&imu, &gyro) != LSM6DSL_OK)
    {
      return LSM6DSL_ERROR;
    }

    sum_x += gyro.x;
    sum_y += gyro.y;
    sum_z += gyro.z;

    osDelay(10);
  }

  gyro_bias_x = (int32_t)(sum_x / (int64_t)sample_count);
  gyro_bias_y = (int32_t)(sum_y / (int64_t)sample_count);
  gyro_bias_z = (int32_t)(sum_z / (int64_t)sample_count);

  Debug_Log(DEBUG_LEVEL_INFO,
            DEBUG_CLASS_IMU,
            "Gyro bias: %ld,%ld,%ld",
            (long)gyro_bias_x,
            (long)gyro_bias_y,
            (long)gyro_bias_z);

  return LSM6DSL_OK;
}

/* USER CODE END Application */


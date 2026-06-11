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
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "b_l475e_iot01a1_bus.h"
#include "debug_log.h"
#include "lsm6dsl.h"
#include "usart.h"
#include "arm_math.h"
#include <math.h>
#include <stdio.h>
#include "cmsis_os2.h"
#include <string.h>
#include "dma.h"
#include "wifi.h"
#include "wifi_credentials.h"
#include <stdarg.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef struct {
  uint32_t timestamp_ms;
  uint32_t dt_ms;

  int32_t ax;
  int32_t ay;
  int32_t az;

  int32_t gx;
  int32_t gy;
  int32_t gz;
} IMUSample_t;

typedef struct {
  float rms_dps;
  float energy;
  float cmsis_peak_dps;
  uint32_t peak_index;
  uint32_t sample_count;
  uint32_t dropped_count;
} SwingDspMetrics_t;

typedef struct {
  char line[256];
  uint8_t repeat_count;
} UdpTxMessage_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define IMU_ODR_HZ                       104.0f
#define IMU_ACCEL_FULL_SCALE_G           16
#define IMU_GYRO_FULL_SCALE_DPS          2000
#define IMU_SENSOR_SETTLE_DELAY_MS       100U
#define IMU_SAMPLE_PERIOD_MS             10U
#define IMU_GYRO_CALIBRATION_SAMPLES     500U

#define ENABLE_IMU_RAW_LOG               0

#define SWING_START_THRESHOLD_MDPS      400000LL
#define SWING_END_THRESHOLD_MDPS        120000LL

#define SWING_START_THRESHOLD2 \
  (SWING_START_THRESHOLD_MDPS * SWING_START_THRESHOLD_MDPS)

#define SWING_END_THRESHOLD2 \
  (SWING_END_THRESHOLD_MDPS * SWING_END_THRESHOLD_MDPS)

#define SWING_START_ACC_THRESHOLD_MG    1800LL
#define SWING_START_ACC_THRESHOLD2 \
  (SWING_START_ACC_THRESHOLD_MG * SWING_START_ACC_THRESHOLD_MG)

#define SWING_MIN_DURATION_MS           180U
#define SWING_COOLDOWN_MS              1200U

#define SWING_CONFIRM_MS                48U
#define MOTOR_PULSE_MS                  300U 

#define BAT_SWEET_SPOT_DISTANCE_MM  650UL

// DMA
#define COMM_RX_DMA_SIZE     64U
#define COMM_RX_LINE_SIZE    64U
#define COMM_FLAG_RX_READY   (1U << 0)
#define COMM_FLAG_UDP_PITCH  (1U << 1)
#define UDP_FLAG_WIFI_READY  (1U << 0)
#define UDP_TX_QUEUE_DEPTH   16U
#define UDP_TX_LINE_SIZE     256U
#define UDP_GAME_EVENT_REPEATS 3U
#define UDP_GAME_EVENT_REPEAT_DELAY_MS 20U

#define SWING_DSP_MAX_SAMPLES 128U

static uint8_t comm_rx_dma_buf[COMM_RX_DMA_SIZE];
static char comm_rx_line[COMM_RX_LINE_SIZE];
static volatile uint16_t comm_rx_line_len = 0U;
static volatile uint8_t comm_rx_line_ready = 0U;
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
static LSM6DSL_Object_t imu;

static uint32_t dropped_samples = 0;
static osMessageQueueId_t imuQueueHandle;
static osMessageQueueId_t udpTxQueueHandle;

static int32_t gyro_bias_x = 0;
static int32_t gyro_bias_y = 0;
static int32_t gyro_bias_z = 0;

static uint32_t imu_read_failures = 0;

static volatile uint8_t udp_pitch_pending = 0U;
static volatile uint32_t udp_pitch_round = 0U;

static float swing_gyro_mag_buf[SWING_DSP_MAX_SAMPLES];
static uint32_t swing_dsp_count = 0U;
static uint32_t swing_dsp_dropped = 0U;
/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityLow,
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
/* Definitions for wifiTask */
osThreadId_t wifiTaskHandle;
const osThreadAttr_t wifiTask_attributes = {
  .name = "wifiTask",
  .stack_size = 2048 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for udpTask */
osThreadId_t udpTaskHandle;
const osThreadAttr_t udpTask_attributes = {
  .name = "udpTask",
  .stack_size = 2048 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
static void ImuDelay(uint32_t ms);
static int32_t ImuInit(void);

static int32_t ImuCalibrateGyroBias(void);
static void FormatInt64(char *buffer, size_t buffer_size, int64_t value);
static uint32_t Isqrt64(uint64_t x);
static void SwingDsp_Reset(void);
static void SwingDsp_AppendSample(const IMUSample_t *sample);
static void SwingDsp_Compute(SwingDspMetrics_t *metrics);

static void Comm_StartRxDmaIdle(void);
static void Comm_ProcessRxLine(uint8_t *pitch_active, uint32_t *pitch_time_ms);
static void Comm_SendGameEventRepeated(uint8_t repeat_count, const char *fmt, ...);
static void Comm_QueueGameEvent(uint8_t repeat_count, const char *fmt, va_list args);
static void Udp_SendQueuedMessages(uint16_t *sent_len);
static const char *WifiEcnName(WIFI_Ecn_t ecn);
static const char *WifiStatusName(WIFI_Status_t status);
static void Wifi_LogConfiguredNetwork(void);
static void Wifi_LogAccessPoints(void);
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void StartSensorTask(void *argument);
void StartCommTask(void *argument);
void StartWifiTask(void *argument);
void StartUdpTask(void *argument);

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

  udpTxQueueHandle = osMessageQueueNew(UDP_TX_QUEUE_DEPTH, sizeof(UdpTxMessage_t), NULL);
  if (udpTxQueueHandle == NULL)
  {
    Debug_Log(DEBUG_LEVEL_CRITICAL, DEBUG_CLASS_SYSTEM, "udpTxQueue create failed");
    Error_Handler();
  }
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of sensorTask */
  sensorTaskHandle = osThreadNew(StartSensorTask, NULL, &sensorTask_attributes);

  /* creation of commTask */
  commTaskHandle = osThreadNew(StartCommTask, NULL, &commTask_attributes);

  /* creation of wifiTask */
  wifiTaskHandle = osThreadNew(StartWifiTask, NULL, &wifiTask_attributes);

  /* creation of udpTask */
  udpTaskHandle = osThreadNew(StartUdpTask, NULL, &udpTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  if (defaultTaskHandle == NULL)
  {
    Debug_Log(DEBUG_LEVEL_CRITICAL, DEBUG_CLASS_SYSTEM, "defaultTask create failed");
    Error_Handler();
  }
  if (sensorTaskHandle == NULL)
  {
    Debug_Log(DEBUG_LEVEL_CRITICAL, DEBUG_CLASS_SYSTEM, "sensorTask create failed");
    Error_Handler();
  }
  if (commTaskHandle == NULL)
  {
    Debug_Log(DEBUG_LEVEL_CRITICAL, DEBUG_CLASS_SYSTEM, "commTask create failed");
    Error_Handler();
  }
  if (wifiTaskHandle == NULL)
  {
    Debug_Log(DEBUG_LEVEL_CRITICAL, DEBUG_CLASS_SYSTEM, "wifiTask create failed");
    Error_Handler();
  }
  if (udpTaskHandle == NULL)
  {
    Debug_Log(DEBUG_LEVEL_CRITICAL, DEBUG_CLASS_SYSTEM, "udpTask create failed");
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
  /* USER CODE BEGIN StartSensorTask */
  IMUSample_t sample;
  LSM6DSL_Axes_t acc;
  LSM6DSL_Axes_t gyro;

  uint32_t last_sample_tick = 0U;

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
  osDelay(IMU_SENSOR_SETTLE_DELAY_MS);

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
      uint32_t now = HAL_GetTick();

      sample.timestamp_ms = now;

      if (last_sample_tick == 0U)
      {
        sample.dt_ms = 0U;
      }
      else
      {
        sample.dt_ms = now - last_sample_tick;
      }

      last_sample_tick = now;

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

    osDelay(IMU_SAMPLE_PERIOD_MS);
  }
  /* USER CODE END StartSensorTask */
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
  /* USER CODE BEGIN StartCommTask */
  IMUSample_t sample;

  uint8_t pitch_active = 0U;
  uint32_t pitch_time_ms = 0U;

  typedef enum {
    SWING_STATE_IDLE = 0,
    SWING_STATE_CANDIDATE,
    SWING_STATE_SWINGING,
    SWING_STATE_COOLDOWN
  } SwingState_t;

  SwingState_t swing_state = SWING_STATE_IDLE;

  uint32_t swing_start_time = 0U;
  uint32_t swing_end_time = 0U;
  uint32_t cooldown_start_time = 0U;
  uint32_t swing_peak_time = 0U;

  int64_t swing_peak_gmag2 = 0;

  uint32_t candidate_start_time = 0U;

  /* 馬達短脈衝設定 */
  uint8_t  motor_on = 0U;
  uint32_t motor_on_start = 0U;

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_COMM, "Comm task start");

  Comm_StartRxDmaIdle();


  for (;;)
  {
    uint32_t flags = osThreadFlagsWait(COMM_FLAG_RX_READY | COMM_FLAG_UDP_PITCH,
                                      osFlagsWaitAny,
                                      0U);

    if ((flags & COMM_FLAG_RX_READY) != 0U)
    {
      Comm_ProcessRxLine(&pitch_active, &pitch_time_ms);
    }

    if (((flags & COMM_FLAG_UDP_PITCH) != 0U) || (udp_pitch_pending != 0U)) {
      uint32_t round = udp_pitch_round;
      uint32_t now_ms = HAL_GetTick();

      udp_pitch_pending = 0U;

      pitch_active = 1U;
      pitch_time_ms = now_ms;

      Comm_SendGameEventRepeated(UDP_GAME_EVENT_REPEATS,
                                 "PITCH_SYNC round=%lu t=%lu",
                                 (unsigned long)round,
                                 (unsigned long)pitch_time_ms);
    }

    if (osMessageQueueGet(imuQueueHandle, &sample, NULL, 1U) == osOK)
    {
      int64_t acc_mag2 =
          (int64_t)sample.ax * (int64_t)sample.ax +
          (int64_t)sample.ay * (int64_t)sample.ay +
          (int64_t)sample.az * (int64_t)sample.az;

      int64_t gyro_mag2 =
          (int64_t)sample.gx * (int64_t)sample.gx +
          (int64_t)sample.gy * (int64_t)sample.gy +
          (int64_t)sample.gz * (int64_t)sample.gz;

      /* 脈衝時間到就關掉(非阻塞)*/
      if (motor_on && (uint32_t)(sample.timestamp_ms - motor_on_start) >= MOTOR_PULSE_MS)
      {
        HAL_GPIO_WritePin(ARD_D8_GPIO_Port, ARD_D8_Pin, GPIO_PIN_RESET);
        motor_on = 0U;
      }


      switch (swing_state)
      {
        case SWING_STATE_IDLE:
        {
          if ((gyro_mag2 > SWING_START_THRESHOLD2) &&
              (acc_mag2 > SWING_START_ACC_THRESHOLD2))
          {
            swing_state = SWING_STATE_CANDIDATE;

            candidate_start_time = sample.timestamp_ms;
            swing_start_time = sample.timestamp_ms;
            swing_peak_time = sample.timestamp_ms;
            swing_peak_gmag2 = gyro_mag2;
            SwingDsp_Reset();
            SwingDsp_AppendSample(&sample);

          }
          break;
        }

        case SWING_STATE_CANDIDATE:
        {
          uint8_t swing_condition =
            (gyro_mag2 > SWING_START_THRESHOLD2) &&
            (acc_mag2 > SWING_START_ACC_THRESHOLD2);

          if (gyro_mag2 > swing_peak_gmag2)
          {
            swing_peak_gmag2 = gyro_mag2;
            swing_peak_time = sample.timestamp_ms;

          }

          if (!swing_condition)
          {
            SwingDsp_Reset();
            swing_state = SWING_STATE_IDLE;
          }
          else
          {
            SwingDsp_AppendSample(&sample);

            if ((sample.timestamp_ms - candidate_start_time) >= SWING_CONFIRM_MS)
            {
              swing_state = SWING_STATE_SWINGING;

              Debug_Log(DEBUG_LEVEL_INFO,
                        DEBUG_CLASS_COMM,
                        "SWING_START t=%lu",
                        (unsigned long)swing_start_time);

              Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_COMM, "MOTOR_ON");

              HAL_GPIO_WritePin(ARD_D8_GPIO_Port, ARD_D8_Pin, GPIO_PIN_SET);
              motor_on = 1U;
              motor_on_start = sample.timestamp_ms;
              Comm_SendGameEventRepeated(UDP_GAME_EVENT_REPEATS,
                                         "SWING_START t=%lu",
                                         (unsigned long)swing_start_time);
            }
          }
          break;
        }

        case SWING_STATE_SWINGING:
        {
          SwingDsp_AppendSample(&sample);

          if (gyro_mag2 > swing_peak_gmag2)
          {
            swing_peak_gmag2 = gyro_mag2;
            swing_peak_time = sample.timestamp_ms;

          }

          if (gyro_mag2 < SWING_END_THRESHOLD2)
          {
            uint32_t duration = sample.timestamp_ms - swing_start_time;

            if (duration >= SWING_MIN_DURATION_MS)
            {
              char peak_gmag2_text[32];
              SwingDspMetrics_t dsp_metrics;

              swing_end_time = sample.timestamp_ms;
              cooldown_start_time = sample.timestamp_ms;
              swing_state = SWING_STATE_COOLDOWN;
              SwingDsp_Compute(&dsp_metrics);

              FormatInt64(peak_gmag2_text,
                          sizeof(peak_gmag2_text),
                          swing_peak_gmag2);

              uint32_t peak_mdps = Isqrt64((uint64_t)swing_peak_gmag2);
              uint32_t peak_dps = peak_mdps / 1000U;

              uint32_t estimated_speed_mm_s =
                (uint32_t)(((uint64_t)BAT_SWEET_SPOT_DISTANCE_MM *
                            (uint64_t)peak_mdps * 355ULL) / (113ULL * 180000ULL));

              uint32_t estimated_speed_m_s_x100 =
                estimated_speed_mm_s / 10U;

              uint32_t start_rt = 0U;
              uint32_t peak_rt = 0U;

              if (pitch_active != 0U)
              {
                start_rt = swing_start_time - pitch_time_ms;
                peak_rt = swing_peak_time - pitch_time_ms;
              }

              Comm_SendGameEventRepeated(UDP_GAME_EVENT_REPEATS,
                                         "SWING_END t=%lu dur=%lu peak_t=%lu start_rt=%lu peak_rt=%lu peak_dps=%lu speed_x100=%lu rms_dps=%lu energy=%lu cmsis_peak=%lu dsp_n=%lu drop=%lu dsp_drop=%lu",
                                         (unsigned long)swing_end_time,
                                         (unsigned long)duration,
                                         (unsigned long)swing_peak_time,
                                         (unsigned long)start_rt,
                                         (unsigned long)peak_rt,
                                         (unsigned long)peak_dps,
                                         (unsigned long)estimated_speed_m_s_x100,
                                         (unsigned long)(uint32_t)(dsp_metrics.rms_dps + 0.5f),
                                         (unsigned long)(uint32_t)(dsp_metrics.energy + 0.5f),
                                         (unsigned long)(uint32_t)(dsp_metrics.cmsis_peak_dps + 0.5f),
                                         (unsigned long)dsp_metrics.sample_count,
                                         (unsigned long)dropped_samples,
                                         (unsigned long)dsp_metrics.dropped_count);

              pitch_active = 0U;
            }
            else
            {
              SwingDsp_Reset();
              cooldown_start_time = sample.timestamp_ms;
              swing_state = SWING_STATE_COOLDOWN;
            }
          }
          break;
        }

        case SWING_STATE_COOLDOWN:
        {
          if ((sample.timestamp_ms - cooldown_start_time) >= SWING_COOLDOWN_MS)
          {
            swing_state = SWING_STATE_IDLE;
          }
          break;
        }

        default:
        {
          swing_state = SWING_STATE_IDLE;
          break;
        }
      }

#if ENABLE_IMU_RAW_LOG
      {
        char acc_mag2_text[24];
        char gyro_mag2_text[24];

        FormatInt64(acc_mag2_text, sizeof(acc_mag2_text), acc_mag2);
        FormatInt64(gyro_mag2_text, sizeof(gyro_mag2_text), gyro_mag2);

        Debug_Log(DEBUG_LEVEL_DEBUG,
                  DEBUG_CLASS_COMM,
                  "imu t=%lu dt=%lu acc_mg=%ld,%ld,%ld gyro_mdps=%ld,%ld,%ld amag2=%s gmag2=%s dropped=%lu",
                  (unsigned long)sample.timestamp_ms,
                  (unsigned long)sample.dt_ms,
                  (long)sample.ax,
                  (long)sample.ay,
                  (long)sample.az,
                  (long)sample.gx,
                  (long)sample.gy,
                  (long)sample.gz,
                  acc_mag2_text,
                  gyro_mag2_text,
                  (unsigned long)dropped_samples);
      }
#endif
    }
  }
  /* USER CODE END StartCommTask */
}

/* USER CODE BEGIN Header_StartWifiTask */
/**
* @brief Function implementing the wifiTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartWifiTask */
void StartWifiTask(void *argument)
{
  /* USER CODE BEGIN StartWifiTask */

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "wifiTask start");

  osDelay(10000);

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "WIFI_Init start");

  if (WIFI_Init() != WIFI_STATUS_OK) {
    Debug_Log(DEBUG_LEVEL_ERROR, DEBUG_CLASS_WIFI, "WIFI_Init failed");

    for (;;) {
      osDelay(1000);
    }
  }

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "WIFI_Init ok");

  Wifi_LogConfiguredNetwork();
  Wifi_LogAccessPoints();

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "WIFI_Connect start");

  WIFI_Status_t connect_status = WIFI_Connect(WIFI_SSID, WIFI_PASSWORD, WIFI_ECN_WPA2_PSK);

  if (connect_status != WIFI_STATUS_OK) {
    Debug_Log(DEBUG_LEVEL_ERROR,
              DEBUG_CLASS_WIFI,
              "WIFI_Connect failed status=%s(%d)",
              WifiStatusName(connect_status),
              (int)connect_status);

    Wifi_LogAccessPoints();

    for (;;) {
      osDelay(1000);
    }
  }

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "WIFI_Connect ok");

  uint8_t ipaddr[4] = {0};

  if (WIFI_GetIP_Address(ipaddr, 4) == WIFI_STATUS_OK) {
    char msg[96];

    snprintf(msg, sizeof(msg),
            "IP = %u.%u.%u.%u",
            ipaddr[0], ipaddr[1], ipaddr[2], ipaddr[3]);

    Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, msg);
  } else {
    Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "WIFI_GetIP_Address failed");
  }

  osThreadFlagsSet(udpTaskHandle, UDP_FLAG_WIFI_READY);

  for (;;) {
    osDelay(1000);
  }
  /* USER CODE END StartWifiTask */
}

/* USER CODE BEGIN Header_StartUdpTask */
/**
* @brief Function implementing the udpTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartUdpTask */
void StartUdpTask(void *argument)
{
  /* USER CODE BEGIN StartUdpTask */

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "udpTask start");

  osThreadFlagsWait(UDP_FLAG_WIFI_READY,
                    osFlagsWaitAny,
                    osWaitForever);

  uint8_t server_ip[4] = SERVER_IP;

  uint16_t sent_len = 0;
  uint16_t recv_len = 0;

  uint8_t rx_buf[128];
  uint8_t remote_ip[4] = {0};
  uint16_t remote_port = 0;

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "UDP open client start");

  /*
   * remote = PC_IP:SERVER_PORT
   * local  = STM32_UDP_PORT
   */
  if (WIFI_OpenClientConnection(0,
                                WIFI_UDP_PROTOCOL,
                                "smartbat_udp",
                                server_ip,
                                SERVER_PORT,
                                STM32_UDP_PORT) != WIFI_STATUS_OK) {
    Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "UDP open client failed");

    for (;;) {
      osDelay(1000);
    }
  }

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "UDP open client ok");

  const char *hello = "HELLO_FROM_STM32\n";

  if (WIFI_SendData(0,
                    (const uint8_t *)hello,
                    strlen(hello),
                    &sent_len,
                    1000) == WIFI_STATUS_OK) {
    Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "UDP HELLO sent");
  } else {
    Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "UDP HELLO send failed");
  }

  Udp_SendQueuedMessages(&sent_len);

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "UDP receive loop start");

  for (;;) {
    Udp_SendQueuedMessages(&sent_len);

    memset(rx_buf, 0, sizeof(rx_buf));
    memset(remote_ip, 0, sizeof(remote_ip));
    recv_len = 0;
    remote_port = 0;

    WIFI_Status_t st = WIFI_ReceiveDataFrom(0,
                                            rx_buf,
                                            sizeof(rx_buf) - 1,
                                            &recv_len,
                                            500,
                                            remote_ip,
                                            4,
                                            &remote_port);

    if (st == WIFI_STATUS_OK && recv_len > 0) {
      rx_buf[recv_len] = '\0';

      Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "UDP rx ok");
      Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, (char *)rx_buf);

      if (strncmp((char *)rx_buf, "PING", 4) == 0) {
        const char *pong = "PONG_FROM_STM32\n";

        if (WIFI_SendData(0,
                          (const uint8_t *)pong,
                          strlen(pong),
                          &sent_len,
                          1000) == WIFI_STATUS_OK) {
          Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "UDP PONG sent");
        } else {
          Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "UDP PONG send failed");
        }
      }
      else if (strncmp((char *)rx_buf, "PITCH", 5) == 0) {
        unsigned long round = 0UL;

        /*
        * Expected format:
        * PITCH round=1
        */
        sscanf((char *)rx_buf, "PITCH round=%lu", &round);

        udp_pitch_round = (uint32_t)round;
        udp_pitch_pending = 1U;
        osThreadFlagsSet(commTaskHandle, COMM_FLAG_UDP_PITCH);

        Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "UDP PITCH queued");
      }
    } else {
      static uint32_t last_log = 0;
      uint32_t now = HAL_GetTick();

      if (now - last_log > 2000) {
        Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "UDP rx timeout/no data");
        last_log = now;
      }
    }

    osDelay(10);
  }

  /* USER CODE END StartUdpTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
static void ImuDelay(uint32_t ms)
{
  osDelay(ms);
}

static void FormatInt64(char *buffer, size_t buffer_size, int64_t value)
{
  char reversed[21];
  uint64_t magnitude;
  size_t len = 0U;
  size_t out = 0U;

  if (buffer_size == 0U)
  {
    return;
  }
  buffer[0] = '\0';

  if (buffer_size == 1U)
  {
    return;
  }

  if (value < 0)
  {
    magnitude = (uint64_t)(-(value + 1)) + 1U;
    buffer[out++] = '-';
  }
  else
  {
    magnitude = (uint64_t)value;
  }

  do
  {
    reversed[len++] = (char)('0' + (magnitude % 10U));
    magnitude /= 10U;
  } while ((magnitude > 0U) && (len < sizeof(reversed)));

  while ((len > 0U) && (out < (buffer_size - 1U)))
  {
    buffer[out++] = reversed[--len];
  }

  buffer[out] = '\0';
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
        (LSM6DSL_ACC_SetOutputDataRate(&imu, IMU_ODR_HZ) == LSM6DSL_OK) &&
        (LSM6DSL_ACC_SetFullScale(&imu, IMU_ACCEL_FULL_SCALE_G) == LSM6DSL_OK) &&
        (LSM6DSL_GYRO_SetOutputDataRate(&imu, IMU_ODR_HZ) == LSM6DSL_OK) &&
        (LSM6DSL_GYRO_SetFullScale(&imu, IMU_GYRO_FULL_SCALE_DPS) == LSM6DSL_OK) &&
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
  const uint32_t sample_count = IMU_GYRO_CALIBRATION_SAMPLES;

  int64_t sum_x = 0;
  int64_t sum_y = 0;
  int64_t sum_z = 0;
  LSM6DSL_Axes_t gyro;

  Debug_Log(DEBUG_LEVEL_INFO,
            DEBUG_CLASS_IMU,
            "Gyro calibration start (%lu samples). Keep board still...",
            (unsigned long)sample_count);

  // Wait for gyro to become stable
  osDelay(1000);

  for (uint32_t i = 0; i < sample_count; i++)
  {
    if (LSM6DSL_GYRO_GetAxes(&imu, &gyro) != LSM6DSL_OK)
    {
      return LSM6DSL_ERROR;
    }

    sum_x += gyro.x;
    sum_y += gyro.y;
    sum_z += gyro.z;

    osDelay(IMU_SAMPLE_PERIOD_MS);
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

  Comm_SendGameEventRepeated(UDP_GAME_EVENT_REPEATS, "GAME_READY");

  return LSM6DSL_OK;
}

static uint32_t Isqrt64(uint64_t x)
{
  uint64_t op = x;
  uint64_t res = 0;
  uint64_t one = 1ULL << 62;

  while (one > op)
  {
    one >>= 2;
  }

  while (one != 0)
  {
    if (op >= res + one)
    {
      op -= res + one;
      res = res + 2 * one;
    }

    res >>= 1;
    one >>= 2;
  }

  return (uint32_t)res;
}

static void SwingDsp_Reset(void)
{
  swing_dsp_count = 0U;
  swing_dsp_dropped = 0U;
}

static void SwingDsp_AppendSample(const IMUSample_t *sample)
{
  if (sample == NULL)
  {
    return;
  }

  if (swing_dsp_count >= SWING_DSP_MAX_SAMPLES)
  {
    swing_dsp_dropped++;
    return;
  }

  float gx_dps = (float)sample->gx / 1000.0f;
  float gy_dps = (float)sample->gy / 1000.0f;
  float gz_dps = (float)sample->gz / 1000.0f;

  swing_gyro_mag_buf[swing_dsp_count] =
    sqrtf((gx_dps * gx_dps) + (gy_dps * gy_dps) + (gz_dps * gz_dps));
  swing_dsp_count++;
}

static void SwingDsp_Compute(SwingDspMetrics_t *metrics)
{
  if (metrics == NULL)
  {
    return;
  }

  metrics->rms_dps = 0.0f;
  metrics->energy = 0.0f;
  metrics->cmsis_peak_dps = 0.0f;
  metrics->peak_index = 0U;
  metrics->sample_count = swing_dsp_count;
  metrics->dropped_count = swing_dsp_dropped;

  if (swing_dsp_count == 0U)
  {
    return;
  }

  arm_rms_f32(swing_gyro_mag_buf, swing_dsp_count, &metrics->rms_dps);
  arm_power_f32(swing_gyro_mag_buf, swing_dsp_count, &metrics->energy);
  arm_max_f32(swing_gyro_mag_buf,
              swing_dsp_count,
              &metrics->cmsis_peak_dps,
              &metrics->peak_index);
}

static void Comm_StartRxDmaIdle(void)
{
  HAL_UARTEx_ReceiveToIdle_DMA(&huart1,
                               comm_rx_dma_buf,
                               COMM_RX_DMA_SIZE);

  __HAL_DMA_DISABLE_IT(huart1.hdmarx, DMA_IT_HT);
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  if (huart->Instance == USART1)
  {
    uint16_t copy_len = Size;

    if (copy_len >= COMM_RX_LINE_SIZE)
    {
      copy_len = COMM_RX_LINE_SIZE - 1U;
    }

    for (uint16_t i = 0U; i < copy_len; i++)
    {
      comm_rx_line[i] = (char)comm_rx_dma_buf[i];
    }

    comm_rx_line[copy_len] = '\0';
    comm_rx_line_len = copy_len;
    comm_rx_line_ready = 1U;

    osThreadFlagsSet(commTaskHandle, COMM_FLAG_RX_READY);

    Comm_StartRxDmaIdle();
  }
}

static void Comm_ProcessRxLine(uint8_t *pitch_active, uint32_t *pitch_time_ms)
{
  char line[COMM_RX_LINE_SIZE];

  if (comm_rx_line_ready == 0U)
  {
    return;
  }

  __disable_irq();

  strncpy(line, comm_rx_line, sizeof(line));
  line[sizeof(line) - 1U] = '\0';
  comm_rx_line_ready = 0U;

  __enable_irq();

  for (uint32_t i = 0U; line[i] != '\0'; i++)
  {
    if ((line[i] == '\r') || (line[i] == '\n'))
    {
      line[i] = '\0';
      break;
    }
  }

  Debug_Log(DEBUG_LEVEL_INFO,
            DEBUG_CLASS_COMM,
            "RX_LINE [%s]",
            line);

  if (strcmp(line, "PITCH") == 0)
  {
    *pitch_active = 1U;
    *pitch_time_ms = HAL_GetTick();

    Comm_SendGameEventRepeated(UDP_GAME_EVENT_REPEATS,
                               "PITCH_SYNC t=%lu",
                               (unsigned long)(*pitch_time_ms));
  }
}

static void Comm_SendGameEventRepeated(uint8_t repeat_count, const char *fmt, ...)
{
  va_list args;

  if (fmt == NULL)
  {
    return;
  }

  va_start(args, fmt);
  Comm_QueueGameEvent(repeat_count, fmt, args);
  va_end(args);
}

static void Comm_QueueGameEvent(uint8_t repeat_count, const char *fmt, va_list args)
{
  UdpTxMessage_t msg;
  int len = vsnprintf(msg.line, sizeof(msg.line), fmt, args);

  if (len < 0)
  {
    return;
  }

  if (repeat_count == 0U)
  {
    repeat_count = 1U;
  }

  msg.line[sizeof(msg.line) - 1U] = '\0';
  msg.repeat_count = repeat_count;

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_COMM, "%s", msg.line);

  if (udpTxQueueHandle != NULL)
  {
    (void)osMessageQueuePut(udpTxQueueHandle, &msg, 0U, 0U);
  }
}

static void Udp_SendQueuedMessages(uint16_t *sent_len)
{
  UdpTxMessage_t msg;
  char tx_line[UDP_TX_LINE_SIZE + 2U];

  if ((udpTxQueueHandle == NULL) || (sent_len == NULL))
  {
    return;
  }

  while (osMessageQueueGet(udpTxQueueHandle, &msg, NULL, 0U) == osOK)
  {
    size_t len = 0U;

    while ((len < sizeof(msg.line)) && (msg.line[len] != '\0'))
    {
      len++;
    }

    if (len >= UDP_TX_LINE_SIZE)
    {
      len = UDP_TX_LINE_SIZE - 1U;
    }

    memcpy(tx_line, msg.line, len);
    tx_line[len++] = '\n';
    tx_line[len] = '\0';

    if (WIFI_SendData(0,
                      (const uint8_t *)tx_line,
                      (uint16_t)len,
                      sent_len,
                      1000) != WIFI_STATUS_OK)
    {
      Debug_Log(DEBUG_LEVEL_WARN, DEBUG_CLASS_WIFI, "UDP event send failed");
      break;
    }

    for (uint8_t i = 1U; i < msg.repeat_count; i++)
    {
      osDelay(UDP_GAME_EVENT_REPEAT_DELAY_MS);

      if (WIFI_SendData(0,
                        (const uint8_t *)tx_line,
                        (uint16_t)len,
                        sent_len,
                        1000) != WIFI_STATUS_OK)
      {
        Debug_Log(DEBUG_LEVEL_WARN, DEBUG_CLASS_WIFI, "UDP event resend failed");
        break;
      }
    }
  }
}

static const char *WifiEcnName(WIFI_Ecn_t ecn)
{
  switch (ecn)
  {
    case WIFI_ECN_OPEN:
      return "OPEN";
    case WIFI_ECN_WEP:
      return "WEP";
    case WIFI_ECN_WPA_PSK:
      return "WPA";
    case WIFI_ECN_WPA2_PSK:
      return "WPA2";
    case WIFI_ECN_WPA_WPA2_PSK:
      return "WPA/WPA2";
    default:
      return "UNKNOWN";
  }
}

static const char *WifiStatusName(WIFI_Status_t status)
{
  switch (status)
  {
    case WIFI_STATUS_OK:
      return "OK";
    case WIFI_STATUS_ERROR:
      return "ERROR";
    case WIFI_STATUS_NOT_SUPPORTED:
      return "NOT_SUPPORTED";
    case WIFI_STATUS_JOINED:
      return "JOINED";
    case WIFI_STATUS_ASSIGNED:
      return "ASSIGNED";
    case WIFI_STATUS_TIMEOUT:
      return "TIMEOUT";
    default:
      return "UNKNOWN";
  }
}

static void Wifi_LogConfiguredNetwork(void)
{
  Debug_Log(DEBUG_LEVEL_INFO,
            DEBUG_CLASS_WIFI,
            "WiFi config ssid=\"%s\" ssid_len=%lu pass_len=%lu security=%s",
            WIFI_SSID,
            (unsigned long)strlen(WIFI_SSID),
            (unsigned long)strlen(WIFI_PASSWORD),
            WifiEcnName(WIFI_ECN_WPA2_PSK));
}

static void Wifi_LogAccessPoints(void)
{
  static WIFI_APs_t aps;
  WIFI_Status_t scan_status;
  uint8_t found_target = 0U;

  memset(&aps, 0, sizeof(aps));

  Debug_Log(DEBUG_LEVEL_INFO, DEBUG_CLASS_WIFI, "WIFI scan start");

  scan_status = WIFI_ListAccessPoints(&aps, WIFI_MAX_APS);

  if (scan_status != WIFI_STATUS_OK)
  {
    Debug_Log(DEBUG_LEVEL_WARN,
              DEBUG_CLASS_WIFI,
              "WIFI scan failed status=%s(%d)",
              WifiStatusName(scan_status),
              (int)scan_status);
    return;
  }

  Debug_Log(DEBUG_LEVEL_INFO,
            DEBUG_CLASS_WIFI,
            "WIFI scan ok count=%u",
            aps.count);

  for (uint8_t i = 0U; i < aps.count; i++)
  {
    const WIFI_AP_t *ap = &aps.ap[i];
    uint8_t is_target = (strcmp(ap->SSID, WIFI_SSID) == 0);

    if (is_target != 0U)
    {
      found_target = 1U;
    }

    Debug_Log(DEBUG_LEVEL_INFO,
              DEBUG_CLASS_WIFI,
              "AP[%u]%s ssid=\"%s\" rssi=%d ecn=%s(%d) ch=%u",
              i,
              (is_target != 0U) ? " TARGET" : "",
              ap->SSID,
              (int)ap->RSSI,
              WifiEcnName(ap->Ecn),
              (int)ap->Ecn,
              ap->Channel);
  }

  if (found_target == 0U)
  {
    Debug_Log(DEBUG_LEVEL_WARN,
              DEBUG_CLASS_WIFI,
              "Configured SSID not found in scan: \"%s\"",
              WIFI_SSID);
  }
}

/* USER CODE END Application */


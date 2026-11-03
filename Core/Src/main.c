/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : 智慧飲料店 POS 系統主程式 (STM32G474RE)
  * @hardware       :
  *   - 綠燈 (PC6, GPIO Output) - 正常待機指示 (平時恆亮)
  *   - 黃燈 (PC8, GPIO Output) - 插隊態指示 (PC8 閃爍 3 下後自動恢復)
  *   - 紅燈 (PC9, TIM3_CH4 PWM) - 原料預警呼吸燈 (插隊時凍結亮度)
  *   - 奧客插隊按鈕 (PA10, EXTI10 下降沿觸發 + 內部上拉)
  *   - 外送平台 (LPUART1, RXNE 中斷 + Ring Buffer 背景處理)
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <stdio.h>
#include <string.h>

/* Private define ------------------------------------------------------------*/
#define RING_BUFFER_SIZE  128  // UART 環形緩衝區大小
#define MAX_PWM_PULSE     500  // PWM 亮度上限
#define DEBOUNCE_DELAY_MS 200  // 按鈕軟體防彈跳時間 (ms)

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef hlpuart1;
TIM_HandleTypeDef htim3;

/* --- UART 環形緩衝區相關變數 (背景外送持單) --- */
uint8_t rx_byte = 0;                          // 中斷單位元組接收暫存
uint8_t ring_buffer[RING_BUFFER_SIZE];        // 環形緩衝區陣列
volatile uint16_t ring_head = 0;              // 寫入指標
volatile uint16_t ring_tail = 0;              // 讀取指標
uint8_t cmd_line[RING_BUFFER_SIZE];           // 提取出來的單行訂單文字
uint16_t cmd_idx = 0;

/* --- 狀態轉移與呼吸燈變數 --- */
volatile uint8_t g_urgent_flag = 0;           // 黃燈奧客插隊態 (URGENT) 旗標
volatile uint32_t g_last_btn_tick = 0;       // 按鈕上次有效觸發時間

static uint16_t g_pulse = 0;                  // 當前 PWM 脈寬值 (凍結亮度用)
static int8_t g_pulse_dir = 5;                // PWM 變化方向 (+5 漸亮 / -5 漸暗)

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_LPUART1_UART_Init(void);
static void MX_TIM3_Init(void);

void UART_SendString(const char *str);
void Process_Urgent_State(void);
void Process_Delivery_Order(void);
void Update_Breathing_LED(void);
uint8_t RingBuffer_Read(uint8_t *data);

/**
  * @brief  主程式進入點
  */
int main(void)
{
  /* 1. 重置所有外設，初始化 Flash 介面與 Systick */
  HAL_Init();

  /* 2. 設定系統時脈 */
  SystemClock_Config();

  /* 3. 初始化所有硬體外設 */
  MX_GPIO_Init();
  MX_LPUART1_UART_Init();
  MX_TIM3_Init();

  /* 4. STATE 1: 常態運作態 (IDLE + ALERT) 起始設定 */
  // 綠燈 (PC6) 恆亮，黃燈 (PC8) 熄滅
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_RESET);

  // 開啟 LPUART1 接收中斷 (背景外送持單態)
  HAL_UART_Receive_IT(&hlpuart1, &rx_byte, 1);

  // 啟動 TIM3 CH4 PWM 輸出 (PC9 紅燈呼吸燈)
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);

  UART_SendString("\r\n=== 智慧飲料店 POS 系統 (STATE 1 常態運作中 - 按鈕切換至 PA10) ===\r\n");

  /* 5. 主迴圈 (Super Loop) */
  while (1)
  {
    /* --- STATE 2: 黃燈奧客插隊態 (URGENT) --- */
    if (g_urgent_flag)
    {
      g_urgent_flag = 0;       // 清除旗標
      Process_Urgent_State();  // 進入插隊態 (綠燈滅、紅燈暫停、黃燈閃 3 下後自動恢復)
    }
    else
    {
      /* --- STATE 1: 常態呼吸漸變 (未進入插隊態時持續更新) --- */
      Update_Breathing_LED();
    }

    /* --- 背景任務: 外送持單態 (Ring Buffer 非同步解析) --- */
    Process_Delivery_Order();
  }
}

/**
  * @brief  STATE 2: 進入黃燈奧客插隊態 (URGENT)
  */
void Process_Urgent_State(void)
{
  // 1. 綠燈熄滅
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6, GPIO_PIN_RESET);

  UART_SendString("[狀態切換] 按下 PA10 -> 進入黃燈奧客插隊態！\r\n");

  // 2. 黃燈 (PC8) 閃爍 3 下
  for (int i = 0; i < 3; i++)
  {
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_SET);
    HAL_Delay(150);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, GPIO_PIN_RESET);
    HAL_Delay(150);
  }

  UART_SendString("[狀態切換] 閃完 3 下 -> 自動恢復常態態！\r\n");

  // 3. 自動恢復常態：綠燈重新點亮
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6, GPIO_PIN_SET);
}

/**
  * @brief  紅燈 (PC9) 呼吸燈漸變邏輯 (非阻塞式)
  */
void Update_Breathing_LED(void)
{
  static uint32_t last_update = 0;

  if (HAL_GetTick() - last_update >= 15)
  {
    last_update = HAL_GetTick();
    g_pulse += g_pulse_dir;

    if (g_pulse >= MAX_PWM_PULSE)
    {
      g_pulse = MAX_PWM_PULSE;
      g_pulse_dir = -g_pulse_dir;
    }
    else if (g_pulse <= 0)
    {
      g_pulse = 0;
      g_pulse_dir = -g_pulse_dir;
    }

    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, g_pulse);
  }
}

/**
  * @brief  背景外送持單態解析
  */
void Process_Delivery_Order(void)
{
  uint8_t byte;

  while (RingBuffer_Read(&byte))
  {
    if (byte == '\n' || byte == '\r')
    {
      if (cmd_idx > 0)
      {
        cmd_line[cmd_idx] = '\0';

        UART_SendString("[外送持單] 收到訂單: ");
        UART_SendString((char *)cmd_line);
        UART_SendString("\r\n");

        cmd_idx = 0;
      }
    }
    else
    {
      if (cmd_idx < (RING_BUFFER_SIZE - 1))
      {
        cmd_line[cmd_idx++] = byte;
      }
    }
  }
}

/* ---------------------------------------------------------------------------*/
/* 中斷向量與 Callbacks                                                         */
/* ---------------------------------------------------------------------------*/

/**
  * @brief  EXTI Line 15~10 中斷向量處理入口 (涵蓋 PA10 中斷)
  */
void EXTI15_10_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_10);
}

/**
  * @brief  GPIO EXTI 回調函數 (PA10 下降沿觸發)
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_10)
  {
    uint32_t current_tick = HAL_GetTick();

    // 200ms 防彈跳
    if ((current_tick - g_last_btn_tick) > DEBOUNCE_DELAY_MS)
    {
      g_last_btn_tick = current_tick;
      g_urgent_flag = 1; // 觸發進入 STATE 2
    }
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == LPUART1)
  {
    uint16_t next_head = (ring_head + 1) % RING_BUFFER_SIZE;

    if (next_head != ring_tail)
    {
      ring_buffer[ring_head] = rx_byte;
      ring_head = next_head;
    }

    HAL_UART_Receive_IT(&hlpuart1, &rx_byte, 1);
  }
}

/* ---------------------------------------------------------------------------*/
/* 輔助與外設初始化                                                           */
/* ---------------------------------------------------------------------------*/

uint8_t RingBuffer_Read(uint8_t *data)
{
  if (ring_head == ring_tail) return 0;
  *data = ring_buffer[ring_tail];
  ring_tail = (ring_tail + 1) % RING_BUFFER_SIZE;
  return 1;
}

void UART_SendString(const char *str)
{
  HAL_UART_Transmit(&hlpuart1, (uint8_t *)str, strlen(str), HAL_MAX_DELAY);
}

void SystemClock_Config(void) {}

static void MX_TIM3_Init(void)
{
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  __HAL_RCC_TIM3_CLK_ENABLE();

  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 170 - 1;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 1000 - 1;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  HAL_TIM_PWM_Init(&htim3);

  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig);

  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_4);
}

static void MX_LPUART1_UART_Init(void)
{
  hlpuart1.Instance = LPUART1;
  hlpuart1.Init.BaudRate = 115200;
  hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
  hlpuart1.Init.StopBits = UART_STOPBITS_1;
  hlpuart1.Init.Parity = UART_PARITY_NONE;
  hlpuart1.Init.Mode = UART_MODE_TX_RX;
  hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  HAL_UART_Init(&hlpuart1);
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* 1. 開啟 GPIOA 與 GPIOC 時脈 */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6 | GPIO_PIN_8, GPIO_PIN_RESET);

  /* 2. PC6 (綠燈) 與 PC8 (黃燈) 輸出 */
  GPIO_InitStruct.Pin = GPIO_PIN_6 | GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* 3. PC9 (紅燈 PWM / AF2) */
  GPIO_InitStruct.Pin = GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF2_TIM3;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* 4. 改用 PA10 作為按鈕輸入 -> 下降沿觸發 (GPIO_MODE_IT_FALLING) + 內部上拉 */
  GPIO_InitStruct.Pin = GPIO_PIN_10;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* 5. 致能 EXTI15_10 中斷 */
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}

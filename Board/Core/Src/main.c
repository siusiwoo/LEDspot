/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdlib.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* 프레임 바이트 */
#define STX           0x02
#define ETX           0x03

/* 명령 (CMD) */
#define LED_WRITE     0x01   /* PC -> MCU : 색 번호 */
#define LED_MASK      0x02   /* PC -> MCU : 비트 마스크 (bit0 초록, bit1 노랑, bit2 빨강) */
#define ACK           0x06   /* MCU -> PC : 받은 CMD */
#define NAK           0x15   /* MCU -> PC : 받은 CMD */
#define LED_RESP      0x82   /* MCU -> PC : 현재 색 번호 */
#define RAND_SEND     0x20   /* MCU -> PC : 자리마다 1바이트 (0~9) */
#define RAND_REQ      0x21   /* PC -> MCU : 자리마다 1바이트 (0~9) */
#define RAND_NEXT     0x22   /* PC -> MCU : 없음 (LEN 0), MCU는 RAND_SEND로 응답 */

/* 색 번호 */
#define COLOR_OFF     0x00
#define COLOR_GREEN   0x01
#define COLOR_YELLOW  0x02
#define COLOR_RED     0x03

#define RX_BUF_SIZE         32    /* 수신 버퍼 크기 */
#define MAX_DIGITS          4     /* 랜덤 숫자 최대 자리 수 */
#define BUTTON_DEBOUNCE_MS  200   /* 버튼 떨림 무시 시간 [미정, 제안 200ms] */
#define LED_OFF_GAP_MS      100   /* 색을 바꿀 때 꺼져 있는 시간 [미정, 제안 100ms, PC와 같게] */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

COM_InitTypeDef BspCOMInit;

/* USER CODE BEGIN PV */
uint8_t rx;                          /* UART 1바이트 수신 칸 */
uint8_t buf[RX_BUF_SIZE];            /* 패킷을 모으는 배열 */
uint8_t idx = 0;                     /* buf에 모은 개수 */
uint8_t collecting = 0;              /* STX를 본 뒤 모으는 중인지 */
uint8_t pkt[RX_BUF_SIZE];            /* 완성된 패킷 복사본 (main에서 처리) */
volatile uint8_t ready = 0;          /* 패킷 하나 완성 깃발 */
volatile uint8_t buttonPressed = 0;  /* USER 버튼 눌림 깃발 */
uint32_t lastButtonTick = 0;         /* 마지막 버튼 처리 시각 */
uint8_t randSeeded = 0;              /* rand() 시드 설정 여부 */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_ICACHE_Init(void);
/* USER CODE BEGIN PFP */
static uint8_t calc_crc(uint8_t *data, uint8_t len);
static void send_packet(uint8_t cmd, uint8_t *data, uint8_t len);
static void set_leds_mask(uint8_t mask);
static uint8_t color_to_mask(uint8_t color);
static void show_color(uint8_t color);
static uint8_t judge_color(const uint8_t *digits, uint8_t len);
static uint8_t make_random(uint8_t *digits);
static void send_random(void);
static void handle_packet(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* CRC-8 : 다항식 0x07, 초기값 0x00, CMD부터 DATA까지 (LED02와 같음) */
static uint8_t calc_crc(uint8_t *data, uint8_t len)
{
	uint8_t crc = 0;
	for (int i = 0; i < len; i++) {
		crc ^= data[i];
		for (int j = 0; j < 8; j++) {
			if (crc & 0x80) {
				crc = (uint8_t)((crc << 1) ^ 0x07);
			} else {
				crc = (uint8_t)(crc << 1);
			}
		}
	}
	return crc;
}

/* STX | CMD | LEN | DATA | CRC | ETX 를 조립해 한 번에 전송 (LED02와 같음) */
static void send_packet(uint8_t cmd, uint8_t *data, uint8_t len)
{
	uint8_t out[RX_BUF_SIZE];
	if (len > RX_BUF_SIZE - 5) {
		return;
	}
	out[0] = STX;
	out[1] = cmd;
	out[2] = len;
	for (int i = 0; i < len; i++) {
		out[3 + i] = data[i];
	}
	out[3 + len] = calc_crc(&out[1], 2 + len);
	out[4 + len] = ETX;
	HAL_UART_Transmit(&hcom_uart[COM1], out, 5 + len, 100);
}

/* LED 3개를 마스크로 켜고 끈다 (bit0 초록, bit1 노랑, bit2 빨강) */
static void set_leds_mask(uint8_t mask)
{
	if (mask & 0x01) BSP_LED_On(LED_GREEN);  else BSP_LED_Off(LED_GREEN);
	if (mask & 0x02) BSP_LED_On(LED_YELLOW); else BSP_LED_Off(LED_YELLOW);
	if (mask & 0x04) BSP_LED_On(LED_RED);    else BSP_LED_Off(LED_RED);
}

/* 색 번호(0 끄기, 1 초록, 2 노랑, 3 빨강)를 마스크로 바꾼다 */
static uint8_t color_to_mask(uint8_t color)
{
	if (color == COLOR_OFF || color > COLOR_RED) {
		return 0;
	}
	return (uint8_t)(1u << (color - 1));
}

/* 색을 바꿀 때 바로 넘어가지 않고, 잠깐 모두 끈 뒤 새 색을 켠다 */
static void show_color(uint8_t color)
{
	set_leds_mask(0);
	HAL_Delay(LED_OFF_GAP_MS);
	set_leds_mask(color_to_mask(color));
}

/* 색 판정 (PC와 같은 규칙): 0이면 초록, 0이 아니면 자리 수 짝수 노랑, 홀수 빨강 */
static uint8_t judge_color(const uint8_t *digits, uint8_t len)
{
	uint8_t allZero = 1;
	for (int i = 0; i < len; i++) {
		if (digits[i] != 0) {
			allZero = 0;
		}
	}
	if (allZero) {
		return COLOR_GREEN;
	}
	return (len % 2 == 0) ? COLOR_YELLOW : COLOR_RED;
}

/* 랜덤 숫자 생성: 자리 수 1~4, 첫 자리 1~9, 한 자리일 때만 0~9. 반환값 = 자리 수 */
static uint8_t make_random(uint8_t *digits)
{
	if (!randSeeded) {
		srand(HAL_GetTick());   /* 처음 버튼을 누른 시각으로 시드 설정 */
		randSeeded = 1;
	}
	uint8_t len = (uint8_t)(rand() % MAX_DIGITS + 1);
	if (len == 1) {
		digits[0] = (uint8_t)(rand() % 10);
	} else {
		digits[0] = (uint8_t)(rand() % 9 + 1);
		for (int i = 1; i < len; i++) {
			digits[i] = (uint8_t)(rand() % 10);
		}
	}
	return len;
}

/* 랜덤 숫자를 만들어 RAND_SEND로 보낸다 */
static void send_random(void)
{
	uint8_t digits[MAX_DIGITS];
	uint8_t len = make_random(digits);
	send_packet(RAND_SEND, digits, len);
}

/* 완성된 패킷 하나를 처리한다 */
static void handle_packet(void)
{
	uint8_t p[RX_BUF_SIZE];

	/* 인터럽트가 pkt를 덮어쓰지 않도록 잠깐 막고 복사 */
	__disable_irq();
	memcpy(p, pkt, sizeof(p));
	ready = 0;
	__enable_irq();

	uint8_t cmd = p[1];
	uint8_t len = p[2];
	uint8_t *data = &p[3];

	if (p[4 + len] != ETX) {
		return;                               /* 끝 바이트가 틀리면 버림 */
	}
	if (calc_crc(&p[1], 2 + len) != p[3 + len]) {
		send_packet(NAK, &cmd, 1);            /* CRC가 틀리면 NAK */
		return;
	}

	switch (cmd) {
	case LED_WRITE:                           /* A 방식: PC가 판정한 색으로 켬 */
		if (len == 1 && data[0] <= COLOR_RED) {
			send_packet(ACK, &cmd, 1);            /* ACK를 먼저 보내고 */
			show_color(data[0]);                  /* 꺼졌다가 새 색으로 켬 */
		} else {
			send_packet(NAK, &cmd, 1);
		}
		break;

	case LED_MASK:                            /* 카운트다운: 마스크대로 켬 */
		if (len == 1) {
			set_leds_mask(data[0] & 0x07);
			send_packet(ACK, &cmd, 1);
		} else {
			send_packet(NAK, &cmd, 1);
		}
		break;

	case RAND_NEXT:                           /* A 방식: 다음 숫자 요청 */
		send_random();
		break;

	case RAND_REQ: {                          /* B 방식: PC가 만든 숫자를 MCU가 판정 */
		uint8_t valid = (len >= 1 && len <= MAX_DIGITS);
		for (int i = 0; valid && i < len; i++) {
			if (data[i] > 9) {
				valid = 0;
			}
		}
		if (!valid) {
			send_packet(NAK, &cmd, 1);
			break;
		}
		send_packet(ACK, &cmd, 1);
		uint8_t color = judge_color(data, len);
		show_color(color);                    /* 꺼졌다가 새 색으로 켬 */
		send_packet(LED_RESP, &color, 1);
		break;
	}

	default:                                  /* PC가 보낸 ACK/NAK 등은 처리하지 않음 */
		break;
	}
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ICACHE_Init();
  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */

  /* Initialize leds */
  BSP_LED_Init(LED_GREEN);
  BSP_LED_Init(LED_YELLOW);
  BSP_LED_Init(LED_RED);

  /* Initialize USER push-button, will be used to trigger an interrupt each time it's pressed.*/
  BSP_PB_Init(BUTTON_USER, BUTTON_MODE_EXTI);

  /* Initialize COM1 port (115200, 8 bits (7-bit data + 1 stop bit), no parity */
  BspCOMInit.BaudRate   = 115200;
  BspCOMInit.WordLength = COM_WORDLENGTH_8B;
  BspCOMInit.StopBits   = COM_STOPBITS_1;
  BspCOMInit.Parity     = COM_PARITY_NONE;
  BspCOMInit.HwFlowCtl  = COM_HWCONTROL_NONE;
  if (BSP_COM_Init(COM1, &BspCOMInit) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  set_leds_mask(0);                                  /* 시작할 때 LED 모두 끔 */
  HAL_NVIC_SetPriority(USART3_IRQn, 0, 0);           /* COM1 = USART3 수신 인터럽트 */
  HAL_NVIC_EnableIRQ(USART3_IRQn);
  HAL_UART_Receive_IT(&hcom_uart[COM1], &rx, 1);
  while (1)
  {

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	/* PC가 보낸 패킷 처리 */
	if (ready) {
		handle_packet();
	}

	/* USER 버튼: A 방식 시작 (랜덤 숫자를 RAND_SEND로 보냄) */
	if (buttonPressed) {
		buttonPressed = 0;
		uint32_t now = HAL_GetTick();
		if (now - lastButtonTick >= BUTTON_DEBOUNCE_MS) {
			lastButtonTick = now;
			send_random();
		}
	}
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_CSI;
  RCC_OscInitStruct.CSIState = RCC_CSI_ON;
  RCC_OscInitStruct.CSICalibrationValue = RCC_CSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLL1_SOURCE_CSI;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 125;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1_VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1_VCORANGE_WIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_PCLK3;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the programming delay
  */
  __HAL_FLASH_SET_PROGRAM_DELAY(FLASH_PROGRAMMING_DELAY_2);
}

/**
  * @brief ICACHE Initialization Function
  * @param None
  * @retval None
  */
static void MX_ICACHE_Init(void)
{

  /* USER CODE BEGIN ICACHE_Init 0 */

  /* USER CODE END ICACHE_Init 0 */

  /* USER CODE BEGIN ICACHE_Init 1 */

  /* USER CODE END ICACHE_Init 1 */

  /** Enable instruction cache in 1-way (direct mapped cache)
  */
  if (HAL_ICACHE_ConfigAssociativityMode(ICACHE_1WAY) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_ICACHE_Enable() != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ICACHE_Init 2 */

  /* USER CODE END ICACHE_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pins : RMII_MDC_Pin RMII_RXD0_Pin RMII_RXD1_Pin */
  GPIO_InitStruct.Pin = RMII_MDC_Pin|RMII_RXD0_Pin|RMII_RXD1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : RMII_REF_CLK_Pin RMII_MDIO_Pin RMII_CRS_DV_Pin */
  GPIO_InitStruct.Pin = RMII_REF_CLK_Pin|RMII_MDIO_Pin|RMII_CRS_DV_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : VBUS_SENSE_Pin */
  GPIO_InitStruct.Pin = VBUS_SENSE_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(VBUS_SENSE_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : UCPD_CC1_Pin UCPD_CC2_Pin */
  GPIO_InitStruct.Pin = UCPD_CC1_Pin|UCPD_CC2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : RMII_TXD1_Pin */
  GPIO_InitStruct.Pin = RMII_TXD1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
  HAL_GPIO_Init(RMII_TXD1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : UCPD_FLT_Pin */
  GPIO_InitStruct.Pin = UCPD_FLT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(UCPD_FLT_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : USB_FS_N_Pin USB_FS_P_Pin */
  GPIO_InitStruct.Pin = USB_FS_N_Pin|USB_FS_P_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF10_USB;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : SWDIO_Pin T_JTDI_Pin */
  GPIO_InitStruct.Pin = SWDIO_Pin|T_JTDI_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF0_TRACE;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : RMII_TXT_EN_Pin RMI_TXD0_Pin */
  GPIO_InitStruct.Pin = RMII_TXT_EN_Pin|RMI_TXD0_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /*Configure GPIO pin : SWO_Pin */
  GPIO_InitStruct.Pin = SWO_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF0_TRACE;
  HAL_GPIO_Init(SWO_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : ARD_D1_TX_Pin ARD_D0_RX_Pin */
  GPIO_InitStruct.Pin = ARD_D1_TX_Pin|ARD_D0_RX_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF8_LPUART1;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/* UART 1바이트 수신 완료 콜백: STX를 본 뒤 모으고, LEN으로 길이를 계산해 자른다 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	(void)huart;
	if (collecting == 0 && rx == STX) {
		collecting = 1;
		idx = 0;
	}
	if (collecting == 1) {
		buf[idx++] = rx;
		if (idx >= 5 && idx == buf[2] + 5) {     /* STX, CMD, LEN, CRC, ETX + DATA */
			memcpy(pkt, buf, idx);
			collecting = 0;
			ready = 1;
		} else if (idx >= RX_BUF_SIZE) {
			collecting = 0;
			idx = 0;
		}
	}
	HAL_UART_Receive_IT(&hcom_uart[COM1], &rx, 1);
}

/* 수신 오류(오버런 등)가 나도 계속 받을 수 있게 다시 시작 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	(void)huart;
	collecting = 0;
	idx = 0;
	HAL_UART_Receive_IT(&hcom_uart[COM1], &rx, 1);
}

/* USER 버튼 눌림 (EXTI 인터럽트) */
void BSP_PB_Callback(Button_TypeDef Button)
{
	if (Button == BUTTON_USER) {
		buttonPressed = 1;
	}
}
/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};
  MPU_Attributes_InitTypeDef MPU_AttributesInit = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region 0 and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x08FFF000;
  MPU_InitStruct.LimitAddress = 0x08FFFFFF;
  MPU_InitStruct.AttributesIndex = MPU_ATTRIBUTES_NUMBER0;
  MPU_InitStruct.AccessPermission = MPU_REGION_ALL_RO;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Attribute 0 and the memory to be protected
  */
  MPU_AttributesInit.Number = MPU_ATTRIBUTES_NUMBER0;
  MPU_AttributesInit.Attributes = INNER_OUTER(MPU_NOT_CACHEABLE);

  HAL_MPU_ConfigMemoryAttributes(&MPU_AttributesInit);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @param None
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

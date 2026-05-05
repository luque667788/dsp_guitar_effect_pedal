/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2025 STMicroelectronics.
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
#include "adc.h"
#include "dac.h"
#include "dma.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "dsp.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define N 256     // Number of samples
#define halfN 128 // Half the number of samples
// the base latency from ADC to DAC is 256 samples at 48kHz = 5.3333ms
// Math: samples / Sample rate (samples/s) = time (s) -> a buffer needs to pass
// through ADC and DAC and it always goes like from second half buffer ADC to
// first half buffer DAC then first half buffer ADC to second half buffer DAC

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
// buffers for ADC and DAC data
// we are using 16-bit resolution (halfword)
// IMPORTANT: On STM32H7, DMA1 CANNOT access DTCM RAM (0x2000_0000).
// Buffers must be placed in D2 SRAM (0x3000_0000) or AXI SRAM (0x2400_0000).
__attribute__((section(".dma_buffer"))) uint16_t adcBuffer[N];
__attribute__((section(".dma_buffer"))) uint16_t dacBuffer[N];

// ---- DEBUG VARIABLES (volatile so GDB/optimizer can't hide them) ----
volatile uint32_t dbg_halfCpltCount = 0;  // how many times half-complete ISR fired
volatile uint32_t dbg_cpltCount = 0;      // how many times complete ISR fired
volatile uint16_t dbg_adcSnapshot[4];     // snapshot of first 4 ADC samples
volatile uint16_t dbg_adcMin = 0xFFFF;    // track min ADC value seen
volatile uint16_t dbg_adcMax = 0;         // track max ADC value seen
volatile uint16_t dbg_dacSnapshot[4];     // snapshot of first 4 DAC samples

// ---- RESET/FAULT DIAGNOSTICS ----
// init_progress: tracks how far we get before reset
//   1=HAL_Init done, 2=Clock done, 3=GPIO, 4=DMA, 5=ADC, 6=DAC, 7=TIM
//   8=DMA started, 9=in main loop
volatile uint32_t dbg_init_progress = 0;
volatile uint32_t dbg_reset_cause = 0;     // copy of RCC->RSR
volatile uint32_t dbg_fault_type = 0;      // 1=hard,2=mem,3=bus,4=usage
volatile uint32_t dbg_fault_CFSR = 0;      // Configurable Fault Status Register
volatile uint32_t dbg_fault_HFSR = 0;      // HardFault Status Register
volatile uint32_t dbg_fault_MMFAR = 0;     // MemManage Fault Address
volatile uint32_t dbg_fault_BFAR = 0;      // BusFault Address
volatile uint32_t dbg_error_handler_hit = 0; // set to 1 if Error_Handler called
// ---------------------------------

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void dsp_algorithm(uint16_t *input, uint16_t *output, int startindex,
                   int endindex);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
  // ===== NUCLEAR DEBUG-RESET CLEANUP =====
  // On STM32H7, a debug-reset (from ST-Link) resets the CPU core but NOT
  // the peripherals. DMA, ADC, DAC, timers can all be in mid-operation
  // with pending interrupts. Force-reset everything we use FIRST.
  
  // Reset DMA1
  __HAL_RCC_DMA1_CLK_ENABLE();
  __HAL_RCC_DMA1_FORCE_RESET();
  __HAL_RCC_DMA1_RELEASE_RESET();
  
  // Disable DMA interrupts that may be pending
  NVIC_DisableIRQ(DMA1_Stream0_IRQn);
  NVIC_DisableIRQ(DMA1_Stream1_IRQn);
  NVIC_DisableIRQ(DMAMUX1_OVR_IRQn);
  NVIC_ClearPendingIRQ(DMA1_Stream0_IRQn);
  NVIC_ClearPendingIRQ(DMA1_Stream1_IRQn);
  NVIC_ClearPendingIRQ(DMAMUX1_OVR_IRQn);

  // Reset ADC
  __HAL_RCC_ADC12_CLK_ENABLE();
  __HAL_RCC_ADC12_FORCE_RESET();
  __HAL_RCC_ADC12_RELEASE_RESET();

  // Reset DAC
  __HAL_RCC_DAC12_CLK_ENABLE();
  __HAL_RCC_DAC12_FORCE_RESET();
  __HAL_RCC_DAC12_RELEASE_RESET();

  // Reset TIM6
  __HAL_RCC_TIM6_CLK_ENABLE();
  __HAL_RCC_TIM6_FORCE_RESET();
  __HAL_RCC_TIM6_RELEASE_RESET();

  // Read and save the reset cause BEFORE HAL_Init clears it
  dbg_reset_cause = RCC->RSR;
  // Clear the reset flags so we can detect next reset
  __HAL_RCC_CLEAR_RESET_FLAGS();
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  dbg_init_progress = 1; // HAL_Init passed
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  dbg_init_progress = 2; // Clock config passed
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  dbg_init_progress = 3;
  MX_DMA_Init();
  dbg_init_progress = 4;
  MX_ADC1_Init();
  dbg_init_progress = 5;
  MX_DAC1_Init();
  dbg_init_progress = 6;
  MX_TIM6_Init();
  dbg_init_progress = 7;
  /* USER CODE BEGIN 2 */
  // start ADC and DAC in DMA mode and the timer

  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adcBuffer, N);
  HAL_DAC_Start_DMA(&hdac1, DAC_CHANNEL_1, (uint32_t *)dacBuffer, N,
                    DAC_ALIGN_12B_R);
  HAL_TIM_Base_Start(&htim6);
  dbg_init_progress = 8; // DMA started
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    dbg_init_progress = 9; // in main loop
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
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

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  * NOTE: Using HSI (internal 64MHz) instead of HSE (external crystal)
  *       because HSE is not available on this board/setup.
  * PLL: HSI(64MHz) / PLLM(8) = 8MHz VCI input
  *      8MHz * PLLN(120) = 960MHz VCO
  *      960MHz / PLLP(2) = 480MHz SYSCLK
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;              // HSI at 64 MHz
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;    // PLL fed by HSI
  RCC_OscInitStruct.PLL.PLLM = 8;                         // 64MHz / 8 = 8MHz
  RCC_OscInitStruct.PLL.PLLN = 120;                       // 8MHz * 120 = 960MHz VCO
  RCC_OscInitStruct.PLL.PLLP = 2;                         // 960 / 2 = 480MHz SYSCLK
  RCC_OscInitStruct.PLL.PLLQ = 2;                         // 960 / 2 = 480MHz
  RCC_OscInitStruct.PLL.PLLR = 2;                         // 960 / 2 = 480MHz
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;      // VCI range 8-16 MHz
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;      // Wide VCO (192-960 MHz)
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
// second half of the buffer is filled
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {

  dsp_algorithm(adcBuffer, dacBuffer, halfN, N);

}

// first half of the buffer is filled
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc) {
  dbg_halfCpltCount++;

  dsp_algorithm(adcBuffer, dacBuffer, 0, halfN);
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  dbg_error_handler_hit = 1;
  __disable_irq();
  while (1) {
    // SET A BREAKPOINT HERE to catch errors
    __NOP();
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
  /* User can add his own implementation to report the file name and line
     number, ex: printf("Wrong parameters value: file %s on line %d\r\n", file,
     line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

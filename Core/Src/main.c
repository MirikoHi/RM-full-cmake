/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *  2023 RobotPiolts
 *  机器人小队代码
 *  作者：zs
 *  邮箱：szhangszu@163.com
 *  QQ:   1273890169
 *
 *
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "can.h"
#include "dma.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "WS2812.h"
#include "can_protocal.h"
#include "Flag_State.h"
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
float motor_speed;
uint8_t ucSec;
extern float usTms;   // 毫秒时间
extern float ms_2500; // 2500毫秒
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BOARD_NUMBER 0
#define TIME_CNT_CLEAR 255

#define ON 1
#define OFF 0
#define BLUE 1
#define RED 0
#define COLOR (BLUE)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
float speed_rate;
uint8_t color;
uint8_t rand_set;
uint8_t rand_num_0 = 0; // 随机数0，确保不重复
uint8_t rand_num_1 = 1;
uint8_t rand_num_2 = 2;
uint8_t rand_num_3 = 3;
uint8_t rand_num_4 = 4;
uint8_t motor_control;
uint8_t s2_down_now;
uint8_t s2_down_last;
uint8_t s1_down_now;
uint8_t s1_down_last;
float a_x_1000;
float a_x_1;
float w_x_1000;
float w_x_1;
float b;
int32_t last_time = 0;
int32_t now_time = 0;
char speed1;
char speed2;
float sin_value;
char Manual;
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
// void rand_num(void)
//{
//	rand_num_0 = rand()%5;
//	rand_num_1 = rand()%5;
//	while(rand_num_1 == rand_num_0)
//	{
//		rand_num_1 = rand()%5;
//	}
//	rand_num_2 = rand()%5;
//	while(rand_num_2 == rand_num_0 || rand_num_2 == rand_num_1)
//	{
//		rand_num_2 = rand()%5;
//	}
//	rand_num_3 = rand()%5;
//	while(rand_num_3 == rand_num_0 || rand_num_3 == rand_num_1 || rand_num_3 == rand_num_2)
//	{
//		rand_num_3 = rand()%5;
//	}
//		rand_num_4 = rand()%5;
//	while(rand_num_4 == rand_num_0 || rand_num_4 == rand_num_1 || rand_num_4 == rand_num_2 || rand_num_4 == rand_num_3 )
//	{
//		rand_num_4 = rand()%5;
//	}
// }

void rand_num(void)
{
  int numbers[] = {0, 1, 2, 3, 4};

  // 洗牌算法
  for (int i = 4; i > 0; --i)
  {
    int j = rand() % (i + 1);
    // 交换元素
    int temp = numbers[i];
    numbers[i] = numbers[j];
    numbers[j] = temp;
  }

  // 赋值给全局变量
  rand_num_0 = numbers[0];
  rand_num_1 = numbers[1];
  rand_num_2 = numbers[2];
  rand_num_3 = numbers[3];
  rand_num_4 = numbers[4];
} // 随机数生成

// void rand_num(void) {
//     int numbers[] = {0, 1, 2, 3,};
//     srand(HAL_GetTick()); // 初始化随机种子
//
//     for (int i = 3; i > 0; --i) { // 注意循环次数改为3
//         int j = rand() % (i + 1);
//         // 交换元素
//         int temp = numbers[i];
//         numbers[i] = numbers[j];
//         numbers[j] = temp;
//     }

//    // 赋值给全局变量
//    rand_num_0 = numbers[0];
//    rand_num_1 = numbers[1];
//    rand_num_2 = numbers[2];
//    rand_num_3 = numbers[3];
//}//随机数生成（另一种方式）
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

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

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
  MX_DMA_Init();
  MX_CAN_Init();
  MX_TIM1_Init();
  /* USER CODE BEGIN 2 */
  CAN1_Init();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

  WS_CloseAll_R();
  /*关闭R*/
  WS_Write_R(0, 0, 255);

  /* 标志位初始化 */
  Board_Order_info_Init(Board_Order_info);
  Big_Fu_info_Init(Big_Fu_info);

  // uint64_t sleep_time = 0;

  speed_rate = 907;
  color = RED;

  /*初始化随机数生成*/
  rand_num(); // 调用随机数生成
  //	  rand_num_0 = 4;
  //    rand_num_1 = 3;
  //    rand_num_2 = 0;
  //    rand_num_3 = 1;
  //	  rand_num_4 = 2;

  while (1)
  {

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // 颜色设置
    // 颜色设置
    Board_Order_info[0].Set_Color = color;
    Board_Order_info[1].Set_Color = color;
    Board_Order_info[2].Set_Color = color;
    Board_Order_info[3].Set_Color = color;
    Board_Order_info[4].Set_Color = color;
    // 设置当前激活的板子工作状态
    Board_Order_info[rand_num_0].Board_Work = ON; // 只设置rand_num_0的Board_Work为ON，其他为OFF，用于顺序激活全板工作
    Board_Order_info[rand_num_1].Board_Work = OFF;
    Board_Order_info[rand_num_2].Board_Work = OFF;
    Board_Order_info[rand_num_3].Board_Work = OFF;
    Board_Order_info[rand_num_4].Board_Work = OFF;

    //	  Board_Order_info[rand_num_0].Board_Work  = OFF;        //只设置rand_num_0的Board_Work为ON，其他为OFF，用于顺序激活全板工作
    //		Board_Order_info[rand_num_1].Board_Work  = OFF;
    //	  Board_Order_info[rand_num_2].Board_Work  = OFF;
    //		Board_Order_info[rand_num_3].Board_Work  = OFF;
    //		Board_Order_info[rand_num_4].Board_Work  = OFF;

    //		Board_Order_info[rand_num_0].Board_Work  = ON;        //只设置rand_num_0的Board_Work为ON，其他为OFF，用于顺序激活全板工作
    //		Board_Order_info[rand_num_1].Board_Work  = ON;
    //	  Board_Order_info[rand_num_2].Board_Work  = ON;
    //		Board_Order_info[rand_num_3].Board_Work  = ON;
    //		Board_Order_info[rand_num_4].Board_Work  = ON;

    if (Big_Fu_info[rand_num_0].Single_Hit_State == OFF)
    {
      if ((ms_2500 >= 2000 && Big_Fu_info[rand_num_0].Single_Hit_State == OFF) || (Big_Fu_info[rand_num_1].Single_Hit_State || Big_Fu_info[rand_num_2].Single_Hit_State || Big_Fu_info[rand_num_3].Single_Hit_State || Big_Fu_info[rand_num_4].Single_Hit_State))
      {
        Board_Order_info[0].Board_Work = OFF;
        Board_Order_info[1].Board_Work = OFF;
        Board_Order_info[2].Board_Work = OFF;
        Board_Order_info[3].Board_Work = OFF;
        Board_Order_info[4].Board_Work = OFF;
        ms_2500 = 0;
        rand_num();
      }
    }
    if (Big_Fu_info[rand_num_0].Single_Hit_State == ON)
    {
      Board_Order_info[rand_num_1].Board_Work = ON;
    }
    if (Big_Fu_info[rand_num_1].Single_Hit_State == ON)
    {
      Board_Order_info[rand_num_2].Board_Work = ON;
    }
    if (Big_Fu_info[rand_num_2].Single_Hit_State == ON)
    {
      Board_Order_info[rand_num_3].Board_Work = ON;
    }
    if (Big_Fu_info[rand_num_3].Single_Hit_State == ON)
    {
      Board_Order_info[rand_num_4].Board_Work = ON;
    }
    if (Big_Fu_info[rand_num_4].Single_Hit_State == ON)
    {
      Board_Order_info[0].Board_Work = OFF;
      Board_Order_info[1].Board_Work = OFF;
      Board_Order_info[2].Board_Work = OFF;
      Board_Order_info[3].Board_Work = OFF;
      Board_Order_info[4].Board_Work = OFF;
      rand_num();
    }
    //	Board_Order_info[0].RainBow_Flag = 0;
    //	Board_Order_info[1].RainBow_Flag = 0;
    //	Board_Order_info[2].RainBow_Flag = 0;
    //	Board_Order_info[3].RainBow_Flag = 0;
    //	Board_Order_info[4].RainBow_Flag = 0;
    //
    //	Board_Order_info[0].Board_Work = OFF;
    //	Board_Order_info[1].Board_Work = OFF;
    //	Board_Order_info[2].Board_Work = OFF;
    //	Board_Order_info[3].Board_Work = OFF;
    //	Board_Order_info[4].Board_Work = OFF;

    Board_Info_Tx();
    HAL_Delay(20);
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

  /** Initializes the RCC Oscillators according to the specified parameters
   * in the RCC_OscInitTypeDef structure.
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSE;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
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

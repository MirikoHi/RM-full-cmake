/**
  ******************************************************************************
  * @file           : start.cpp
  * @brief          : Main program body
  ******************************************************************************
  * @author Miriko H    HFUT
  * @link https://github.com/MirikoHi/2026-Energy-Mechine-cmake/
  * 
  ******************************************************************************
*/
#include "start.h"
#include <algorithm>
#include <string.h>
int Rand(int i){
    return rand() % i;
}

#include "_can.h"
#define BOARD_READY 1

CanBox canbox;

// #include "Fu.h"

#include "dma.h"
#include "tim.h"
#include "gpio.h"
#include "rtc.h"

#define BLUE 1
#define RED 0

#define __DE_BUG

uint16_t ms_2500 = 0;

/** @brief 击打标志位 */
bool success[5] = {0};

void startup(void){
    canbox.CanInit();
    canbox.TxData[2] = BLUE; // color
    while(1){
        memset(success, 0, sizeof(success));
        memset(canbox.RxData[0], 0, sizeof(canbox.RxData[0]));
        memset(canbox.RxData[1], 0, sizeof(canbox.RxData[1]));
        memset(canbox.RxData[2], 0, sizeof(canbox.RxData[2]));
        memset(canbox.RxData[3], 0, sizeof(canbox.RxData[3]));
        memset(canbox.RxData[4], 0, sizeof(canbox.RxData[4]));

        /* 配置随机数 */
        RTC_TimeTypeDef time = {0, 0, 0};
        HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BCD);
        srand(time.Seconds + time.Minutes);

        /* 洗牌算法 */
        uint16_t board[5] = {0x101, 0x102, 0x103, 0x104, 0x105};
        std::random_shuffle(board, board + 5, Rand);

        /* RxData处理 */
        /*
        while(!(canbox.RxData[0][0] == BOARD_READY &&
            canbox.RxData[1][0] == BOARD_READY && 
            canbox.RxData[2][0] == BOARD_READY && 
            canbox.RxData[3][0] == BOARD_READY &&
            canbox.RxData[4][0] == BOARD_READY))
        {};*/

        HAL_Delay(200);
        for(int8_t i = 0; i < 5; i++){

            /* TxData处理 */
            canbox.TxData[0] = BOARD_READY; // controlState
            canbox.TxData[1] = 1; // hitEnable
            canbox.TxData[3] = 0; // twinkleState
            while(!canbox.Send(board[i],0x08)){};

            while(1){
                if(canbox.RxData[board[i] - 0x101][1] == 1){
                    success[i] = true;
                    ms_2500 = 0;
                    break; /* while(canbox.RxData[i][1] == 0) */
                }
                if(ms_2500 > 1850){ // 大约是2.5秒
                    success[i] = false;
                    ms_2500 = 0;
                    break; /* while(canbox.RxData[i][1] == 0) */
                }
                ms_2500++;
                HAL_Delay(1);
            }

            if(success[i] == false){
                canbox.TxData[1] = 0; // hitEnable
                for(int8_t j = 0; j < 5; j++){
                    while(!canbox.Send(board[j],0x08)){};
                }
                i = -1;
                HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BCD);
                srand(time.Seconds + time.Minutes);
                std::random_shuffle(board, board + 5, Rand);
            }

            if(success[0] == true &&
                success[1] == true &&
                success[2] == true &&
                success[3] == true &&
                success[4] == true)
            {
                canbox.TxData[3] = 1; // twinkleState
                for(int8_t j = 0; j < 5; j++){
                    while(!canbox.Send(board[j],0x08)){};
                }
                HAL_Delay(2000);
                canbox.TxData[2] = (canbox.TxData[2] == BLUE ? RED : BLUE); // change color
                break; /* for(int8_t i = 0; i < 5; i++) */
            }
        }
    }
}
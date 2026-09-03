/**
  ******************************************************************************
  * @file           : start.cpp
  * @brief          : 主程序入口：上电初始化、模式调度与双击切换模式
  *
  * 代码按职责拆分（逻辑不变）：
  * - board_ctrl.cpp    灯板公共控制：命令收发、点亮确认、清场、换色补发
  * - key_ctrl.cpp      PC9 按键检测：双击切模式 / 长按 2s 手动换色
  * - small_target.cpp  小符模式单次流程
  * - big_target.cpp    大符模式单次流程
  ******************************************************************************
*/
#include "start.h"
#include "_can.h"
#include "board_ctrl.h"
#include "small_target.h"
#include "big_target.h"
#include "rtc.h"
#include <stdlib.h>

/* 目标模式枚举 */
typedef enum
{
    TARGET_MODE_SMALL = 0,  // 小符模式
    TARGET_MODE_BIG   = 1   // 大符模式
} TargetMode_t;

/* 上电默认模式，按需求上电进入小符模式 */
#define BOOT_MODE TARGET_MODE_SMALL

/* 当前工作模式 */
static TargetMode_t current_mode = BOOT_MODE;

/**
 * @brief 切换大符/小符模式，切换前清理旧模式输出
 */
static void switch_target_mode(void)
{
    /* 先熄灭全部灯板，避免切换后残留 */
    all_small_boards_off();

    /* 双击切换模式后颜色恢复默认蓝色 */
    canbox.TxData[2] = BLUE;

    if (current_mode == TARGET_MODE_SMALL)
    {
        current_mode = TARGET_MODE_BIG;
    }
    else
    {
        current_mode = TARGET_MODE_SMALL;
    }
}

/**
 * @brief 主程序入口
 */
void startup(void)
{
    canbox.CanInit();
    canbox.TxData[2] = BLUE; // 上电默认颜色为蓝色

    /* 使用 RTC 时间和系统节拍初始化随机数种子 */
    RTC_TimeTypeDef time = {0, 0, 0};
    HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BCD);
    srand(time.Seconds + time.Minutes + (unsigned int)HAL_GetTick());

    while (1)
    {
        bool switch_requested = false;

        if (current_mode == TARGET_MODE_SMALL)
        {
            switch_requested = run_small_target_once();
        }
        else
        {
            switch_requested = run_big_target_once();
        }

        if (switch_requested)
        {
            switch_target_mode();
        }
    }
}

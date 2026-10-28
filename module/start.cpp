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
#include "key_ctrl.h"
#include "small_target.h"
#include "big_target.h"
#include "ext_ctrl.h"
#include "rtc.h"
#include <stdlib.h>

uint8_t small_t_status = 0;
uint8_t big_t_status = 0;

uint8_t small_co = 0;
uint8_t big_co = 0;

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

/* 大符是否处于“激活会话”中：收到启动命令后为 true，
   激活完成/复位/切回小符后回到 false（未激活待机） */
static bool big_activation_active = false;

/**
 * @brief 刷新对外运行状态/颜色标志（每次主循环开始时调用）
 */
static void update_outer_flags(void)
{
    /* 命令字颜色：BLUE=1, RED=0；对外标志约定蓝=1, 红=2 */
    uint8_t color = (canbox.TxData[2] == BLUE) ? 1U : 2U;

    if (current_mode == TARGET_MODE_SMALL)
    {
        small_t_status = 1;
        big_t_status = 0;
        small_co = color;
        big_co = 0;
    }
    else
    {
        small_t_status = 0;
        big_t_status = big_activation_active ? 2U : 1U;
        small_co = 0;
        big_co = color;
    }
}

/**
 * @brief 切换到指定大符/小符模式，切换前清理旧模式输出
 * @param target 目标模式：TARGET_MODE_SMALL 或 TARGET_MODE_BIG
 */
static void switch_target_mode(TargetMode_t target)
{
    /* 已经在目标模式：不做任何切换（避免清场/复位的副作用） */
    if (target == current_mode)
    {
        return;
    }

    /* 先熄灭全部灯板，避免切换后残留 */
    all_small_boards_off();

    /* 切换模式后颜色恢复默认蓝色 */
    canbox.TxData[2] = BLUE;

    /* 丢弃残留的启动/复位/模式请求，避免串扰到下一次进入大符 */
    ext_ctrl_clear_big_requests();

    if (target == TARGET_MODE_BIG)
    {
        current_mode = TARGET_MODE_BIG;
        reset_big_target_count(); // 重新进入大符模式时，连续成功轮数从 0 开始
        big_activation_active = false; // 刚切到大符：全灭未激活，等待启动命令
        ext_ctrl_set_big_state(EXT_STATE_BIG_INACTIVE); // 进入大符：开始持续上报“未激活”
    }
    else
    {
        current_mode = TARGET_MODE_SMALL;
        big_activation_active = false;
        ext_ctrl_set_big_state(EXT_STATE_NONE); // 切回小符：停止大符状态上报
    }
}

/**
 * @brief 大符未激活待机：全灭等待“启动 / 复位 / 切模式”
 *
 * 复位命令在函数内消化后继续等待；启动命令通过 start_requested 传出；
 * 双击或串口切模式命令返回 true，交由外层切回小符。
 *
 * @param start_requested 输出：是否收到启动请求（外接板 START 命令或按键单击）
 * @return true 需要切换回小符
 */
static bool wait_big_idle_or_start(bool *start_requested)
{
    *start_requested = false;

    /* 丢弃进入待机前残留的单击：小符模式或上一轮激活期间的单击不算启动 */
    clear_single_click_big_start();

    while (1)
    {
        /* 双击/串口切模式/复位都会让按键轮询返回 true，以便尽快退出等待 */
        if (poll_mode_switch_keys())
        {
            if (ext_ctrl_take_big_reset())
            {
                /* 复位：保持全灭未激活，继续等待启动命令 */
                ext_ctrl_set_big_state(EXT_STATE_BIG_INACTIVE);
                continue;
            }
            return true; // 双击或串口切模式：切回小符
        }

        /* 启动来源：外接板 START 命令，或大符待机期间的按键单击 */
        if (ext_ctrl_take_big_start() || take_single_click_big_start())
        {
            *start_requested = true;
            return false;
        }

        HAL_Delay(1);
    }
}

/**
 * @brief 主程序入口
 */
void startup(void)
{
    canbox.CanInit();
    ext_ctrl_init(); // 外接控制板串口（USART3）命令接收
    canbox.TxData[2] = BLUE; // 上电默认颜色为蓝色

    /* 使用 RTC 时间和系统节拍初始化随机数种子 */
    RTC_TimeTypeDef time = {0, 0, 0};
    HAL_RTC_GetTime(&hrtc, &time, RTC_FORMAT_BCD);
    srand(time.Seconds + time.Minutes + (unsigned int)HAL_GetTick());

    while (1)
    {
        bool switch_requested = false;

        update_outer_flags(); // 刷新对外状态/颜色标志

        if (current_mode == TARGET_MODE_SMALL)
        {
            switch_requested = run_small_target_once();
        }
        else
        {
            if (!big_activation_active)
            {
                /* 未激活待机：全灭，等待启动命令（复位命令在等待函数内消化） */
                bool start_requested = false;
                if (wait_big_idle_or_start(&start_requested))
                {
                    switch_requested = true; // 双击或串口切模式：切回小符
                }
                else if (start_requested)
                {
                    big_activation_active = true;
                    ext_ctrl_set_big_state(EXT_STATE_BIG_ACTIVATING); // 正在激活
                }
            }
            else
            {
                BigRunResult_t run_result = run_big_target_once();

                if (run_result == BIG_RUN_COMPLETED)
                {
                    /* 激活完成闪烁已正常结束（状态已回到未激活），等下一次启动 */
                    big_activation_active = false;
                }
                else if (run_result == BIG_RUN_ABORTED)
                {
                    big_activation_active = false;

                    if (ext_ctrl_take_big_reset())
                    {
                        /* 复位命令：立即全灭回到未激活，重新等待启动 */
                        all_small_boards_off();
                        reset_big_target_count();
                        ext_ctrl_set_big_state(EXT_STATE_BIG_INACTIVE);
                    }
                    else
                    {
                        switch_requested = true; // 双击或串口切模式：切回小符
                    }
                }
                /* BIG_RUN_CONTINUE：本轮未完成激活，保持“正在激活”继续下一轮 */
            }
        }

        if (switch_requested)
        {
            uint8_t ext_target = ext_ctrl_take_mode_target();
            TargetMode_t target;
            if (ext_target == EXT_MODE_SMALL)
            {
                target = TARGET_MODE_SMALL;
            }
            else if (ext_target == EXT_MODE_BIG)
            {
                target = TARGET_MODE_BIG;
            }
            else
            {
                /* EXT_MODE_TOGGLE：PC9 双击，取反切换 */
                target = (current_mode == TARGET_MODE_SMALL) ? TARGET_MODE_BIG : TARGET_MODE_SMALL;
            }
            switch_target_mode(target);
        }
    }
}
/**
  ******************************************************************************
  * @file           : start.cpp
  * @brief          : 主程序流程，负责小符模式与大符模式的切换和执行
  * @author Miriko H    HFUT
  * @link https://github.com/MirikoHi/2026-Energy-Mechine-cmake/
  *
  ******************************************************************************
*/
#include "start.h"
#include "_can.h"
#include "gpio.h"
#include "rtc.h"
#include <algorithm>
#include <string.h>
#include <stdlib.h>

/* 板级工作状态 */
#define BOARD_READY 1

/* 颜色定义 */
#define BLUE 1
#define RED 0

/* 双击模式切换相关时间参数，单位毫秒 */
#define MODE_SWITCH_DEBOUNCE_MS     20
#define MODE_SWITCH_DOUBLE_CLICK_MS 400

/* 模式切换按键按下时的有效电平，1 表示低电平有效（按下接地） */
#define MODE_SWITCH_ACTIVE_LOW 1

/* 上电后前一段时间不检测按键，防止开机瞬间误判为点击 */
#define MODE_SWITCH_STARTUP_LOCK_MS 1000

/* 灯板点亮时间，单位毫秒 */
#define TARGET_LIGHT_ON_MS 2500

/* 小符模式全部命中闪烁后，全部熄灭保持的时间，单位毫秒 */
#define SMALL_ROUND_OFF_MS 500

/* 大符模式首次命中后刷新时间延长值，单位毫秒 */
#define BIG_TARGET_HIT_EXTEND_MS 1000

/* 全灭后点亮下一批灯板前的等待时间：需大于灯板一个主循环周期(约 20ms+) */
#define BOARD_CLEAR_SETTLE_MS 100

/* 等待所有灯板反馈“在线且未命中”的兜底超时，防止某块板离线导致卡死 */
#define BOARD_CLEAR_CONFIRM_TIMEOUT_MS 500

/* 小符灯板 CAN 订单帧 ID 基址，0x101 或 0x301 根据实际硬件选择 */
#define SMALL_BOARD_ID_BASE 0x101

/* 目标模式枚举 */
typedef enum
{
    TARGET_MODE_SMALL = 0,  // 小符模式
    TARGET_MODE_BIG   = 1   // 大符模式
} TargetMode_t;

CanBox canbox;

/* 上电默认模式，按需求上电进入小符模式 */
#define BOOT_MODE TARGET_MODE_SMALL

/* 当前工作模式 */
static TargetMode_t current_mode = BOOT_MODE;

/* 小符模式各灯板命中标志 */
static bool success[5] = {0};

/* 单个模式切换按键的双击检测状态 */
typedef struct
{
    GPIO_TypeDef *port;         // 引脚所在 GPIO 端口
    uint16_t pin;               // 引脚编号
    bool debounced_state;       // 消抖后的稳定状态，true 为按下
    bool unstable;              // 是否处于抖动待确认状态
    uint32_t change_time;       // 最近一次状态变化的时间
    uint8_t click_count;        // 当前点击计数
    uint32_t first_click_time;  // 第一次点击的时间
} ModeSwitchKey_t;

/* 模式切换按键（PC9/K4），独立检测 */
static ModeSwitchKey_t mode_switch_keys[] =
{
    {MODE_SWITCH_PC9_GPIO_Port, MODE_SWITCH_PC9_Pin, false, false, 0, 0, 0},
};

#define MODE_SWITCH_KEY_NUM (sizeof(mode_switch_keys) / sizeof(mode_switch_keys[0]))

/* 随机数辅助函数 */
int Rand(int i)
{
    return rand() % i;
}

/**
 * @brief 读取单个模式切换按键是否处于按下状态
 * @param key 按键状态结构体指针
 * @return 按下返回 true，否则返回 false
 */
static bool mode_key_pressed(const ModeSwitchKey_t *key)
{
#if MODE_SWITCH_ACTIVE_LOW
    return HAL_GPIO_ReadPin(key->port, key->pin) == GPIO_PIN_RESET;
#else
    return HAL_GPIO_ReadPin(key->port, key->pin) == GPIO_PIN_SET;
#endif
}

/**
 * @brief 检测单个按键是否发生双击
 * @param key 按键状态结构体指针
 * @return 该按键发生一次有效双击返回 true，否则返回 false
 */
static bool detect_key_double_click(ModeSwitchKey_t *key)
{
    uint32_t now = HAL_GetTick();
    bool pressed = mode_key_pressed(key);

    if (pressed != key->debounced_state)
    {
        if (!key->unstable)
        {
            /* 第一次检测到差异，开始计时等待状态稳定 */
            key->unstable = true;
            key->change_time = now;
        }
        else if ((now - key->change_time) >= MODE_SWITCH_DEBOUNCE_MS)
        {
            /* 状态稳定保持足够长时间，确认一次边沿 */
            key->unstable = false;
            key->debounced_state = pressed;

            /* 只统计按下边沿 */
            if (pressed)
            {
                if (key->click_count == 0)
                {
                    key->click_count = 1;
                    key->first_click_time = now;
                }
                else if ((now - key->first_click_time) < MODE_SWITCH_DOUBLE_CLICK_MS)
                {
                    key->click_count = 0;
                    return true;
                }
                else
                {
                    /* 两次点击间隔过长，重新开始计数 */
                    key->click_count = 1;
                    key->first_click_time = now;
                }
            }
        }
    }
    else
    {
        /* 状态回到原值，说明之前的差异只是抖动 */
        key->unstable = false;
    }

    return false;
}

/**
 * @brief 检测模式切换按键是否发生双击
 * @return 发生一次有效双击返回 true，否则返回 false
 */
static bool detect_double_click(void)
{
    /* 开机保护：上电初期不统计任何点击 */
    if (HAL_GetTick() < MODE_SWITCH_STARTUP_LOCK_MS)
    {
        for (uint8_t i = 0; i < MODE_SWITCH_KEY_NUM; i++)
        {
            mode_switch_keys[i].click_count = 0;
        }
        return false;
    }

    for (uint8_t i = 0; i < MODE_SWITCH_KEY_NUM; i++)
    {
        if (detect_key_double_click(&mode_switch_keys[i]))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 延时等待，若期间检测到双击则提前返回
 * @param ms 需要等待的时间，单位毫秒
 * @return 期间检测到双击返回 true，正常延时结束返回 false
 */
static bool wait_ms_or_double_click(uint32_t ms)
{
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < ms)
    {
        if (detect_double_click())
        {
            return true;
        }
        HAL_Delay(1);
    }
    return false;
}

/**
 * @brief 向指定小符灯板发送点亮或熄灭命令
 * @param board_id 灯板 CAN 标准帧 ID
 * @param hit_enable 1 为点亮，0 为熄灭
 */
static void send_board_order(uint32_t board_id, uint8_t hit_enable)
{
    canbox.TxData[0] = BOARD_READY; // 控制状态
    canbox.TxData[1] = hit_enable;  // 点亮或熄灭
    canbox.TxData[3] = 0;           // 闪烁状态

    while (!canbox.Send(board_id, 0x08)) {}
}

/**
 * @brief 向指定小符灯板发送“点亮”命令
 *
 * 连续发送两帧点亮命令（冗余），避免单帧丢失导致该板整轮不亮。
 * 灯板收到重复点亮命令是幂等的：已处于点亮状态时会直接忽略。
 * @param board_id 灯板 CAN 标准帧 ID
 */
static void send_board_light(uint32_t board_id)
{
    send_board_order(board_id, 1);
    send_board_order(board_id, 1);
}

/**
 * @brief 熄灭全部小符灯板
 */
static void all_small_boards_off(void)
{
    /* 连续发送两轮熄灭命令，避免单帧丢失导致灯板残留击打状态 */
    for (uint8_t round = 0; round < 2; round++)
    {
        for (uint8_t i = 0; i < 5; i++)
        {
            send_board_order(SMALL_BOARD_ID_BASE + i, 0);
        }

        /* 等待熄灭命令全部发送完成，避免后续点亮帧抢在前面 */
        while (!__HAL_CAN_GET_FLAG(canbox.Can, CAN_FLAG_TME0) ||
               !__HAL_CAN_GET_FLAG(canbox.Can, CAN_FLAG_TME1) ||
               !__HAL_CAN_GET_FLAG(canbox.Can, CAN_FLAG_TME2)) {}
    }
}

/**
 * @brief 检查 5 块灯板是否都回报“在线且未命中”
 * @retval true 全部回报在线且未命中；false 仍有板子未回报或仍上报命中
 */
static bool all_boards_report_clear(void)
{
    for (uint8_t i = 0; i < 5; i++)
    {
        if (canbox.RxData[i][0] != BOARD_READY ||
            canbox.RxData[i][1] != 0)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 清场 + 双层保险：
 *        1) 熄灭命令发完后至少等待 BOARD_CLEAR_SETTLE_MS，
 *           保证每块灯板都读到并执行过“熄灭”；
 *        2) 再确认 5 块灯板反馈都已变成“在线且未命中”，
 *           避免板子带着上一轮的命中状态进入新一轮。
 * @retval true 等待期间检测到双击，调用方应交给外层切换模式
 */
static bool clear_all_boards_before_round(void)
{
    all_small_boards_off();

    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < BOARD_CLEAR_CONFIRM_TIMEOUT_MS)
    {
        if (detect_double_click())
        {
            return true;
        }
        if (((HAL_GetTick() - start) >= BOARD_CLEAR_SETTLE_MS) &&
            all_boards_report_clear())
        {
            break;
        }
        HAL_Delay(1);
    }
    return false;
}

/**
 * @brief 小符模式单次流程：一次只点亮一个灯板，等待命中或超时
 * @return 期间检测到双击返回 true，正常结束返回 false
 */
static bool run_small_target_once(void)
{
    /* 新一轮：熄灭全部灯板，等板子消化命令并确认全部未命中后再点亮 */
    if (clear_all_boards_before_round())
    {
        return true;
    }

    memset(success, 0, sizeof(success));
    memset(canbox.RxData, 0, sizeof(canbox.RxData));

    /* 洗牌，随机决定 5 个灯板的点亮顺序 */
    uint16_t board[5] = {SMALL_BOARD_ID_BASE, SMALL_BOARD_ID_BASE + 1,
                         SMALL_BOARD_ID_BASE + 2, SMALL_BOARD_ID_BASE + 3,
                         SMALL_BOARD_ID_BASE + 4};
    std::random_shuffle(board, board + 5, Rand);

    for (uint8_t i = 0; i < 5; i++)
    {
        /* 点亮当前灯板 */
        send_board_light(board[i]);

        /* 等待命中或超时（约 2.5s），期间检测双击 */
        uint32_t start = HAL_GetTick();
        while ((HAL_GetTick() - start) < TARGET_LIGHT_ON_MS)
        {
            if (detect_double_click())
            {
                return true;
            }

            if (canbox.RxData[board[i] - SMALL_BOARD_ID_BASE][1] == 1)
            {
                success[i] = true;
                break;
            }

            HAL_Delay(1);
        }

        /* 超时未命中，熄灭全部灯板并重新开始一轮 */
        if (!success[i])
        {
            all_small_boards_off();
            return false;
        }
    }

    /* 五个灯板全部命中，闪烁庆祝并切换颜色 */
    canbox.TxData[3] = 1;
    for (uint8_t j = 0; j < 5; j++)
    {
        while (!canbox.Send(board[j], 0x08)) {}
    }

    if (wait_ms_or_double_click(2000))
    {
        return true;
    }

    canbox.TxData[2] = (canbox.TxData[2] == BLUE ? RED : BLUE);

    /* 全部熄灭并保持一段时间，再进入下一轮 */
    all_small_boards_off();
    if (wait_ms_or_double_click(SMALL_ROUND_OFF_MS))
    {
        return true;
    }

    return false;
}

/**
 * @brief 大符模式单次流程：一次随机点亮两个灯板
 * @return 期间检测到双击返回 true，正常结束返回 false
 */
static bool run_big_target_once(void)
{
    uint8_t first = Rand(5);
    uint8_t second = Rand(5);
    while (second == first)
    {
        second = Rand(5);
    }

    uint32_t id1 = SMALL_BOARD_ID_BASE + first;
    uint32_t id2 = SMALL_BOARD_ID_BASE + second;
    uint8_t idx1 = first;
    uint8_t idx2 = second;

    /* 先熄灭上一轮可能残留的灯板，并确认清场完成后再点亮 */
    if (clear_all_boards_before_round())
    {
        return true;
    }

    /* 清空上一周期的命中反馈，避免残留数据影响新周期 */
    memset(canbox.RxData, 0, sizeof(canbox.RxData));

    /* 随机点亮两个灯板 */
    send_board_light(id1);
    send_board_light(id2);

    /* 基础刷新时间为 2.5s，首次命中后延长 1s 至 3.5s */
    uint32_t deadline = HAL_GetTick() + TARGET_LIGHT_ON_MS;
    bool hit1 = false;
    bool hit2 = false;
    bool extended = false;

    while (1)
    {
        /* 双击切换模式 */
        if (detect_double_click())
        {
            return true;
        }

        /* 记录两颗灯板的命中反馈 */
        if (!hit1 && canbox.RxData[idx1][1] == 1)
        {
            hit1 = true;
        }
        if (!hit2 && canbox.RxData[idx2][1] == 1)
        {
            hit2 = true;
        }

        /* 两颗灯板全部命中，立即结束本轮 */
        if (hit1 && hit2)
        {
            break;
        }

        /* 首次命中后将刷新时间由 2.5s 延长到 3.5s，只延长一次 */
        if (!extended && (hit1 || hit2))
        {
            extended = true;
            deadline += BIG_TARGET_HIT_EXTEND_MS;
        }

        /* 刷新时间到仍未全部命中，视为超时结束本轮 */
        if ((int32_t)(HAL_GetTick() - deadline) >= 0)
        {
            break;
        }

        HAL_Delay(1);
    }

    /* 本轮结束：整组快速熄灭；下一周期入口的 clear_all_boards_before_round()
       还会再做“完整全灭 + 等待 100ms + 确认反馈全 0”，确认后才点亮下一批 */
    all_small_boards_off();

    return false;
}

/**
 * @brief 切换大符/小符模式，切换前清理旧模式输出
 */
static void switch_target_mode(void)
{
    /* 先熄灭全部灯板，避免切换后残留 */
    all_small_boards_off();

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
    canbox.TxData[2] = BLUE;

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

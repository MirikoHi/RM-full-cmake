/**
  ******************************************************************************
  * @file           : key_ctrl.cpp
  * @brief          : PC9 模式切换按键检测：双击切换模式 / 长按手动换色
  *
  * 按键经消抖后区分两种操作：
  * - 双击：两次确认的“按下”边沿间隔 < 400ms，poll_mode_switch_keys() 返回 true；
  * - 长按：按住满 2s 手动切换一次颜色（蓝<->红），并把新颜色补发给所有
  *   仍亮着的灯板（换色动作由 board_ctrl 负责补发）。
  * 击打完成闪烁庆祝期间通过 set_color_change_locked(true) 锁定换色。
  ******************************************************************************
*/
#include "key_ctrl.h"
#include "board_ctrl.h"
#include "_can.h"
#include "main.h"
#include "ext_ctrl.h"

/* 双击模式切换 / 长按换色相关时间参数，单位毫秒 */
#define MODE_SWITCH_DEBOUNCE_MS     20
#define MODE_SWITCH_DOUBLE_CLICK_MS 400
#define COLOR_SWITCH_LONG_PRESS_MS  2000

/* 模式切换按键按下时的有效电平，1 表示低电平有效（按下接地） */
#define MODE_SWITCH_ACTIVE_LOW 1

/* 上电后前一段时间不检测按键，防止开机瞬间误判为点击 */
#define MODE_SWITCH_STARTUP_LOCK_MS 1000

/* 单个模式切换按键的状态：消抖、双击检测与长按计时 */
typedef struct
{
    GPIO_TypeDef *port;         // 引脚所在 GPIO 端口
    uint16_t pin;               // 引脚编号
    bool debounced_state;       // 消抖后的稳定状态，true 为按下
    bool unstable;              // 是否处于抖动待确认状态
    uint32_t change_time;       // 最近一次状态变化的时间
    uint8_t click_count;        // 当前点击计数
    uint32_t first_click_time;  // 第一次点击的时间
    uint32_t press_time;        // 最近一次“按下”边沿的时间（长按计时起点）
    bool long_press_done;       // 本次按下是否已作为长按换色处理过（只触发一次）
} ModeSwitchKey_t;

/* 模式切换按键（PC9/K4），独立检测 */
static ModeSwitchKey_t mode_switch_keys[] =
{
    {MODE_SWITCH_PC9_GPIO_Port, MODE_SWITCH_PC9_Pin, false, false, 0, 0, 0, 0, false},
};

#define MODE_SWITCH_KEY_NUM (sizeof(mode_switch_keys) / sizeof(mode_switch_keys[0]))

/* 击打完成闪烁庆祝期间锁定手动换色：由小符流程 set_color_change_locked() 设置 */
static bool color_change_locked = false;

/**
 * @brief 锁定/解锁长按换色（闪烁庆祝期间不支持换色）
 * @param locked true 锁定，false 恢复
 */
void set_color_change_locked(bool locked)
{
    color_change_locked = locked;
}

/**
 * @brief 查询是否处于闪烁庆祝换色锁定状态
 * @return true 锁定中，false 可正常换色
 */
bool is_color_change_locked(void)
{
    return color_change_locked;
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
 * @brief 轮询单个模式切换按键：消抖、双击判定与长按换色
 *
 * PC9 按键承担两种操作：
 * - 双击：两次确认的“按下”边沿间隔小于 MODE_SWITCH_DOUBLE_CLICK_MS，
 *   返回 true，由外层切换模式；
 * - 长按：确认按下后持续按住达到 COLOR_SWITCH_LONG_PRESS_MS，
 *   在函数内部手动切换一次颜色（蓝<->红），并返回 false，不触发模式切换。
 *
 * 长按一旦生效，本次按下不再计作单击，避免松手后再点一次被误判为双击。
 * @param key 按键状态结构体指针
 * @return 该按键发生一次有效双击返回 true，否则返回 false
 */
static bool poll_mode_switch_key(ModeSwitchKey_t *key)
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

            /* 只统计按下边沿（松开不计数） */
            if (pressed)
            {
                key->press_time = now;        // 长按计时起点
                key->long_press_done = false; // 本次按下尚未被长按消费

                if (key->click_count == 0)
                {
                    key->click_count = 1;
                    key->first_click_time = now;
                }
                else if ((now - key->first_click_time) < MODE_SWITCH_DOUBLE_CLICK_MS)
                {
                    key->click_count = 0;
                    key->long_press_done = true; // 双击已成立，继续按住不再触发长按
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

        /* 已确认按下且保持足够久：手动切换一次颜色（一次长按只换一次） */
        if (key->debounced_state && !key->long_press_done &&
            (now - key->press_time) >= COLOR_SWITCH_LONG_PRESS_MS)
        {
            key->long_press_done = true;
            key->click_count = 0; // 本次长按不算单击，避免与下一次点击组成双击

            /* 闪烁庆祝期间不支持换色：长按被消费但不切换 */
            if (!color_change_locked)
            {
                canbox.TxData[2] = (canbox.TxData[2] == BLUE ? RED : BLUE); // 手动换色
                resend_light_to_lit_boards(); // 给每一块亮着的灯板补发带新颜色的点亮命令
            }
        }
    }

    return false;
}

/**
 * @brief 轮询模式切换按键：长按换色 / 双击切换模式
 * @return 检测到一次有效双击返回 true，否则返回 false（长按换色在内部完成）
 */
bool poll_mode_switch_keys(void)
{
    /* 开机保护：上电初期不统计任何点击，也不响应长按 */
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
        if (poll_mode_switch_key(&mode_switch_keys[i]))
        {
            return true;
        }
    }

    /* 外接控制板串口命令：切模式命令与双击等价；
       大符复位命令也会返回 true，使当前流程尽快退出（由上层区分处理） */
    if (ext_ctrl_process_uart())
    {
        return true;
    }

    return false;
}

/**
 * @brief 延时等待，期间轮询按键（长按可手动换色），若检测到双击则提前返回
 * @param ms 需要等待的时间，单位毫秒
 * @return 期间检测到双击返回 true，正常延时结束返回 false
 */
bool wait_ms_or_double_click(uint32_t ms)
{
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < ms)
    {
        if (poll_mode_switch_keys())
        {
            return true;
        }
        HAL_Delay(1);
    }
    return false;
}

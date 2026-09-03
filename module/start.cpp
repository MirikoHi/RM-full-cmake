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

/* 双击模式切换 / 长按换色相关时间参数，单位毫秒 */
#define MODE_SWITCH_DEBOUNCE_MS     20
#define MODE_SWITCH_DOUBLE_CLICK_MS 400
#define COLOR_SWITCH_LONG_PRESS_MS  2000

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

/* 等待所有灯板反馈确认清场（在线/未命中/已熄灭/已执行最新命令）的兜底超时，防止某块板离线导致卡死 */
#define BOARD_CLEAR_CONFIRM_TIMEOUT_MS 500

/* 清场确认期间，对仍未确认“已熄灭”的板子补发熄灭命令的最小间隔，单位毫秒 */
#define BOARD_CLEAR_RESEND_INTERVAL_MS 150

/* 点亮后等待灯板回报“已亮起/已命中”的单次超时，单位毫秒 */
#define BOARD_LIGHT_CONFIRM_MS 200

/* 点亮确认的最大尝试次数：每次“冗余两帧点亮 + 等待回报”算一次 */
#define BOARD_LIGHT_CONFIRM_ATTEMPTS 3

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

/* 各灯板最近一次逻辑命令的序号（命令帧字节4）。同一条逻辑命令的冗余帧共用同一序号，
   灯板会在反馈字节4中回显“最后真正执行过的命令序号”，供主板确认命令是否被应用 */
static uint8_t board_cmd_seq[5] = {0};

/* 灯板反馈协议状态：新板回显字节2(实际亮起)/字节4(命令序号)，旧板只有字节0/1 */
typedef enum
{
    BOARD_PROTO_UNKNOWN = 0, // 尚未判定：先按旧协议保守处理，避免干等确认
    BOARD_PROTO_NEW,         // 新协议：能回显亮起状态与命令序号
    BOARD_PROTO_OLD          // 旧协议：只回报“在线”和“命中”
} BoardProto_t;

/* 每块灯板的协议判定结果 */
static uint8_t board_proto[5] = {BOARD_PROTO_UNKNOWN};

/* 当前处于“点亮/常亮”状态的灯板掩码：长按换色后需要向这些板补发点亮命令 */
static bool lit_board_mask[5] = {0};

/* 击打完成闪烁庆祝期间锁定手动换色：庆祝闪烁不支持换色 */
static bool color_change_locked = false;

/* 前向声明：点亮命令封装在下方定义，按键轮询（长按换色补发）需要提前调用 */
static void send_board_light(uint32_t board_id);
static void resend_light_to_lit_boards(void);

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
static bool poll_mode_switch_keys(void)
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
    return false;
}

/**
 * @brief 延时等待，期间轮询按键（长按可手动换色），若检测到双击则提前返回
 * @param ms 需要等待的时间，单位毫秒
 * @return 期间检测到双击返回 true，正常延时结束返回 false
 */
static bool wait_ms_or_double_click(uint32_t ms)
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

/**
 * @brief 向指定小符灯板发送点亮或熄灭命令
 * @param board_id 灯板 CAN 标准帧 ID
 * @param hit_enable 1 为点亮，0 为熄灭
 * @param seq 命令序号，同一条逻辑命令的冗余帧必须使用同一序号
 */
static void send_board_order(uint32_t board_id, uint8_t hit_enable, uint8_t seq)
{
    canbox.TxData[0] = BOARD_READY; // 控制状态
    canbox.TxData[1] = hit_enable;  // 点亮或熄灭
    canbox.TxData[3] = 0;           // 闪烁状态
    canbox.TxData[4] = seq;         // 命令序号，供灯板执行后回显

    while (!canbox.Send(board_id, 0x08)) {}
}

/**
 * @brief 向指定小符灯板发送“点亮”命令
 *
 * 连续发送两帧点亮命令（冗余），避免单帧丢失导致该板整轮不亮。
 * 灯板已亮时收到同色点亮命令保持现状；收到不同颜色的点亮命令会按
 * 新颜色重绘（灯板固件支持重复点亮改色）。
 * @param board_id 灯板 CAN 标准帧 ID
 */
static void send_board_light(uint32_t board_id)
{
    lit_board_mask[board_id - SMALL_BOARD_ID_BASE] = true; // 该板当前处于点亮状态

    /* 一次逻辑“点亮”只递增一次序号，冗余两帧共用，灯板回显的序号唯一 */
    uint8_t seq = ++board_cmd_seq[board_id - SMALL_BOARD_ID_BASE];
    send_board_order(board_id, 1, seq); // 点亮帧 1
    send_board_order(board_id, 1, seq); // 点亮帧 2（冗余）
}

/**
 * @brief 手动换色后，向每一块当前亮着的灯板补发带新颜色的点亮命令
 *
 * 灯板固件支持“已点亮时收到不同颜色的点亮命令就按新颜色重绘”，
 * 因此主控长按换色后只需把当前 TxData[2]（新颜色）补发给所有
 * 仍亮着的灯板，亮灯即可即时变色；熄灭中的板无需处理。
 */
static void resend_light_to_lit_boards(void)
{
    for (uint8_t i = 0; i < 5; i++)
    {
        if (lit_board_mask[i])
        {
            send_board_light(SMALL_BOARD_ID_BASE + i);
        }
    }
}

/**
 * @brief 判断灯板是否支持新协议（能回显字节2/字节4），并维护判定状态
 *
 * 新灯板执行过带序号的命令后，会在反馈字节4回显 lastSeq，点亮后还会在
 * 字节2回显 lightState=1；旧灯板只回显字节0(在线)和字节1(命中)，字节2/4
 * 恒为 0。因此：
 * - 只要出现过字节2/字节4非零，就升级判为新板；
 * - 已收到“在线”反馈、主板也发过带序号命令，却始终没有新字段回显 → 判为旧板；
 * - 尚无反馈/尚未发过命令的未知板，先按旧板保守处理，防止干等确认拖慢流程。
 * 判定只影响“能否严格确认”，不影响点亮/熄灭命令本身（命令前几字节两版通用）。
 * @param idx 灯板索引 0~4
 * @retval true 新协议板，可要求字节2/4 的严格确认
 */
static bool board_uses_new_proto(uint8_t idx)
{
    uint8_t *rx = canbox.RxData[idx];

    if (rx[2] != 0 || rx[4] != 0) // 出现过新字段：无论此前判定如何都升级
    {
        board_proto[idx] = BOARD_PROTO_NEW;
        return true;
    }
    if (board_proto[idx] == BOARD_PROTO_NEW)
    {
        return true; // 已判定为新板
    }
    if (board_proto[idx] == BOARD_PROTO_OLD)
    {
        return false; // 已判定为旧板
    }
    if (rx[0] == BOARD_READY && board_cmd_seq[idx] != 0)
    {
        board_proto[idx] = BOARD_PROTO_OLD; // 在线却从不回显新字段 → 旧板
        return false;
    }
    return false; // 还没收到反馈：先按旧板保守放行
}

/**
 * @brief 查询灯板反馈是否已确认“点亮/命中”
 * @param board_id 灯板 CAN 标准帧 ID
 * @retval true 反馈显示已亮起或已命中
 */
static bool board_reports_lit(uint32_t board_id)
{
    uint8_t idx = (uint8_t)(board_id - SMALL_BOARD_ID_BASE);
    return canbox.RxData[idx][2] == 1 || // 已回报亮起
           canbox.RxData[idx][1] == 1;   // 或已回报命中
}

/**
 * @brief 点亮灯板并等待其回报“已亮起/已命中”，未确认则自动按新序号重发
 *
 * 灯板点亮后约在下一个 20ms 主循环回报 lightState=1；
 * 若在 BOARD_LIGHT_CONFIRM_MS 内始终看不到“已亮起”，
 * 说明点亮帧可能丢失或灯板异常，本函数会重新点亮（最多
 * BOARD_LIGHT_CONFIRM_ATTEMPTS 次）。若重试后仍未确认，
 * 由调用方后续的命中等待/超时逻辑兜底，不会无限重发。
 * @param board_id 灯板 CAN 标准帧 ID
 * @retval true 等待/重发期间检测到双击，调用方应交由外层切换模式
 */
static bool light_board_and_confirm(uint32_t board_id)
{
    uint8_t idx = (uint8_t)(board_id - SMALL_BOARD_ID_BASE);

    /* 旧板/未知板无法回报“实际亮起”：冗余两帧发出后直接放行，
       由调用方后续的命中等待/超时逻辑兜底（等价于旧版行为） */
    if (!board_uses_new_proto(idx))
    {
        send_board_light(board_id); // 冗余两帧 + 新命令序号
        return false;
    }

    for (uint8_t attempt = 0; attempt < BOARD_LIGHT_CONFIRM_ATTEMPTS; attempt++)
    {
        send_board_light(board_id); // 冗余两帧 + 新命令序号

        uint32_t start = HAL_GetTick();
        while ((HAL_GetTick() - start) < BOARD_LIGHT_CONFIRM_MS)
        {
            if (poll_mode_switch_keys())
            {
                return true; // 双击：切换模式
            }
            if (board_reports_lit(board_id))
            {
                return false; // 确认成功
            }
            HAL_Delay(1);
        }
    }
    return false; // 多次重发仍未确认：交由命中等待/超时兜底
}

/**
 * @brief 点亮两块灯板并“并行”等待确认，未确认的板按新序号自动重发
 *
 * 大符模式要求两块灯板同时亮起，因此两块板的点亮命令必须在任何等待
 * 之前背靠背连续发出（两者实际间隔仅为 CAN 逐帧发送的亚毫秒级时间）。
 * 之后再并行等待各自回报“已亮起/已命中”：某一窗口内仍未确认的板，
 * 下一轮只对它重发，已确认的板不再打扰。
 * @param id1 第一块灯板 CAN 标准帧 ID
 * @param id2 第二块灯板 CAN 标准帧 ID
 * @retval true 等待/重发期间检测到双击，调用方应交由外层切换模式
 */
static bool light_two_boards_and_confirm(uint32_t id1, uint32_t id2)
{
    uint8_t idx1 = (uint8_t)(id1 - SMALL_BOARD_ID_BASE);
    uint8_t idx2 = (uint8_t)(id2 - SMALL_BOARD_ID_BASE);

    /* 只有新协议板才需要等“实际亮起”回报；旧板/未知板命令发出即视为完成 */
    bool wait1 = board_uses_new_proto(idx1);
    bool wait2 = board_uses_new_proto(idx2);
    bool lit1 = false;
    bool lit2 = false;

    for (uint8_t attempt = 0; attempt < BOARD_LIGHT_CONFIRM_ATTEMPTS; attempt++)
    {
        /* 关键：先给两块板都发点亮命令，再开始等待，保证两个灯同时亮起 */
        if (!lit1)
        {
            send_board_light(id1); // 冗余两帧 + 新命令序号
        }
        if (!lit2)
        {
            send_board_light(id2);
        }

        /* 旧板/未知板不支持回报亮起状态：本轮命令已发出，即视为放行（不再重发） */
        if (!wait1)
        {
            lit1 = true;
        }
        if (!wait2)
        {
            lit2 = true;
        }

        if (lit1 && lit2)
        {
            break; // 都不需要确认：两块命令已背靠背发出，立即放行
        }

        uint32_t start = HAL_GetTick();
        while ((HAL_GetTick() - start) < BOARD_LIGHT_CONFIRM_MS)
        {
            if (poll_mode_switch_keys())
            {
                return true; // 双击：切换模式
            }

            if (!lit1 && board_reports_lit(id1))
            {
                lit1 = true;
            }
            if (!lit2 && board_reports_lit(id2))
            {
                lit2 = true;
            }
            if (lit1 && lit2)
            {
                break;
            }
            HAL_Delay(1);
        }

        if (lit1 && lit2)
        {
            break;
        }
    }
    return false; // 多次重发仍未全部确认：交由命中等待/超时兜底
}

/**
 * @brief 熄灭全部小符灯板
 */
static void all_small_boards_off(void)
{
    /* 全部熄灭后没有仍亮着的灯板 */
    memset(lit_board_mask, 0, sizeof(lit_board_mask));

    /* 每块板的一次逻辑“熄灭”只分配一个序号，两轮发送是该命令的冗余拷贝 */
    uint8_t off_seq[5];
    for (uint8_t i = 0; i < 5; i++)
    {
        off_seq[i] = ++board_cmd_seq[i];
    }

    /* 连续发送两轮熄灭命令，避免单帧丢失导致灯板残留击打状态 */
    for (uint8_t round = 0; round < 2; round++)
    {
        for (uint8_t i = 0; i < 5; i++)
        {
            send_board_order(SMALL_BOARD_ID_BASE + i, 0, off_seq[i]);
        }

        /* 等待熄灭命令全部发送完成，避免后续点亮帧抢在前面 */
        while (!__HAL_CAN_GET_FLAG(canbox.Can, CAN_FLAG_TME0) ||
               !__HAL_CAN_GET_FLAG(canbox.Can, CAN_FLAG_TME1) ||
               !__HAL_CAN_GET_FLAG(canbox.Can, CAN_FLAG_TME2)) {}
    }
}

/**
 * @brief 检查 5 块灯板是否都确认清场
 *
 * 在线、未命中是两版协议共同的清场前提；新协议板额外要求“灯已熄灭且已执行
 * 最新熄灭命令”（字节2/4），旧协议板无法回显这些字段，在线且未命中即视为完成。
 * @retval true 全部确认清场；false 仍有板子未确认
 */
static bool all_boards_report_clear(void)
{
    for (uint8_t i = 0; i < 5; i++)
    {
        if (canbox.RxData[i][0] != BOARD_READY ||
            canbox.RxData[i][1] != 0)                  // 仍上报命中
        {
            return false;
        }

        /* 新协议板才要求严格确认；旧板/未知板到这里即视为清场完成 */
        if (board_uses_new_proto(i) &&
            (canbox.RxData[i][2] != 0 ||               // 灯仍亮着
             canbox.RxData[i][4] != board_cmd_seq[i])) // 最新熄灭命令还没被应用
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 清场 + 多层保险：
 *        1) 熄灭命令发完后至少等待 BOARD_CLEAR_SETTLE_MS，
 *           保证每块灯板都读到并执行过“熄灭”；
 *        2) 再确认 5 块灯板反馈都为“在线、未命中、已熄灭且已执行最新熄灭命令”；
 *        3) 确认期间对仍未清场的板子按反馈单独补发熄灭命令，
 *           用重发代替干等，提高偶发丢帧后的恢复率。
 * @retval true 等待期间检测到双击，调用方应交给外层切换模式
 */
static bool clear_all_boards_before_round(void)
{
    all_small_boards_off();

    uint32_t start = HAL_GetTick();
    uint32_t last_resend = start;
    while ((HAL_GetTick() - start) < BOARD_CLEAR_CONFIRM_TIMEOUT_MS)
    {
        if (poll_mode_switch_keys())
        {
            return true;
        }

        uint32_t now = HAL_GetTick();
        if (((now - start) >= BOARD_CLEAR_SETTLE_MS) && all_boards_report_clear())
        {
            break;
        }

        /* 消化时间过后仍未全部确认：对“支持新协议但反馈显示仍亮着/仍报命中/未执行
           最新命令”的板子单独补发一轮熄灭（同一命令序号），旧板不补发；
           每 BOARD_CLEAR_RESEND_INTERVAL_MS 至多一次 */
        if (((now - start) >= BOARD_CLEAR_SETTLE_MS) &&
            ((now - last_resend) >= BOARD_CLEAR_RESEND_INTERVAL_MS))
        {
            last_resend = now;
            for (uint8_t i = 0; i < 5; i++)
            {
                if (canbox.RxData[i][0] == BOARD_READY &&
                    board_uses_new_proto(i) &&
                    (canbox.RxData[i][1] != 0 ||
                     canbox.RxData[i][2] != 0 ||
                     canbox.RxData[i][4] != board_cmd_seq[i]))
                {
                    send_board_order(SMALL_BOARD_ID_BASE + i, 0, board_cmd_seq[i]);
                }
            }
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
        /* 点亮当前灯板，并确认它回报“已亮起/已命中”，未确认会自动重发 */
        if (light_board_and_confirm(board[i]))
        {
            return true; // 确认期间检测到双击
        }

        /* 等待命中或超时（约 2.5s），期间检测双击 */
        uint32_t start = HAL_GetTick();
        while ((HAL_GetTick() - start) < TARGET_LIGHT_ON_MS)
        {
            if (poll_mode_switch_keys())
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

    /* 五个灯板全部命中，闪烁庆祝（庆祝闪烁期间不支持手动换色） */
    color_change_locked = true;
    canbox.TxData[3] = 1;
    for (uint8_t j = 0; j < 5; j++)
    {
        canbox.TxData[4] = ++board_cmd_seq[board[j] - SMALL_BOARD_ID_BASE]; // 命令序号
        while (!canbox.Send(board[j], 0x08)) {}
    }

    bool switch_req = wait_ms_or_double_click(2000); // 闪烁 2s，期间可双击退出
    color_change_locked = false; // 庆祝结束，恢复手动换色
    if (switch_req)
    {
        return true;
    }

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

    /* 两块灯板的点亮命令先背靠背全部发出（间隔仅亚毫秒级），再并行等待
       各自回报“已亮起/已命中”，保证两灯同时亮起；未确认的板自动重发 */
    if (light_two_boards_and_confirm(id1, id2))
    {
        return true; // 确认期间检测到双击
    }

    /* 基础刷新时间为 2.5s，首次命中后延长 1s 至 3.5s */
    uint32_t deadline = HAL_GetTick() + TARGET_LIGHT_ON_MS;
    bool hit1 = false;
    bool hit2 = false;
    bool extended = false;

    while (1)
    {
        /* 双击切换模式（长按换色在轮询内部完成） */
        if (poll_mode_switch_keys())
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

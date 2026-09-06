/**
  ******************************************************************************
  * @file           : big_target.cpp
  * @brief          : 大符模式控制：每周期随机点亮两块灯板
  *
  * 两块灯板同时点亮，基础刷新时间 2.5s，首次命中后延长 1s 至 3.5s；
  * 两颗都命中立即结束，或超时结束，进入下一周期。
  * 灯板命令/确认/清场等公共逻辑见 board_ctrl.cpp。
  ******************************************************************************
*/
#include "big_target.h"
#include "board_ctrl.h"
#include "key_ctrl.h"
#include "ext_ctrl.h"
#include "_can.h"
#include <string.h>

/* 大符模式连续成功轮数（每轮至少一块灯板被击打即成功） */
static uint8_t big_success_rounds = 0;

/* 连续成功多少轮后触发整组闪烁庆祝 */
#define BIG_TARGET_SUCCESS_ROUNDS 5

/* 庆祝闪烁时间，与小符一致，单位毫秒 */
#define BIG_TARGET_CELEBRATE_MS 2000

/* 庆祝结束后全灭保持时间，与小符一致，单位毫秒 */
#define BIG_TARGET_ROUND_OFF_MS 500

/**
 * @brief 清零大符模式连续成功轮数（双击重新进入大符模式时由 start.cpp 调用）
 */
void reset_big_target_count(void)
{
    big_success_rounds = 0;
}

/**
 * @brief 大符模式单次流程：一次随机点亮两个灯板
 * @return BIG_RUN_CONTINUE 普通一轮结束，继续下一轮；
 *         BIG_RUN_COMPLETED 闪烁庆祝正常结束（激活完成）；
 *         BIG_RUN_ABORTED 期间双击/切模式/复位请求，中断当前流程
 */
BigRunResult_t run_big_target_once(void)
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
        return BIG_RUN_ABORTED;
    }

    /* 清空上一周期的命中反馈，避免残留数据影响新周期 */
    memset(canbox.RxData, 0, sizeof(canbox.RxData));

    /* 两块灯板的点亮命令先背靠背全部发出（间隔仅亚毫秒级），再并行等待
       各自回报“已亮起/已命中”，保证两灯同时亮起；未确认的板自动重发 */
    if (light_two_boards_and_confirm(id1, id2))
    {
        return BIG_RUN_ABORTED; // 确认期间检测到双击/切模式/复位
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
            return BIG_RUN_ABORTED;
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

    /* 判定本轮是否成功：只要本轮有一块灯板被击打即算单轮成功；
       一块都未命中而超时结束则视为失败，重新计数 */
    bool round_hit = (hit1 || hit2);
    if (round_hit)
    {
        big_success_rounds++;
    }
    else
    {
        big_success_rounds = 0;
    }

    if (big_success_rounds >= BIG_TARGET_SUCCESS_ROUNDS)
    {
        /* 连续 5 轮成功：整组 5 块灯板一起闪烁庆祝（照搬小符庆祝逻辑） */
        big_success_rounds = 0; // 庆祝后清零，下一组重新累计

        uint16_t boards[5] = {SMALL_BOARD_ID_BASE, SMALL_BOARD_ID_BASE + 1,
                              SMALL_BOARD_ID_BASE + 2, SMALL_BOARD_ID_BASE + 3,
                              SMALL_BOARD_ID_BASE + 4};
        set_color_change_locked(true); // 庆祝闪烁期间不支持手动换色
        ext_ctrl_set_big_state(EXT_STATE_BIG_ACTIVATE_DONE); // 连续 5 轮成功：大符激活完成
        send_celebration_twinkle(boards);

        bool switch_req = wait_ms_or_double_click(BIG_TARGET_CELEBRATE_MS); // 闪烁 2s，期间可双击退出
        set_color_change_locked(false); // 庆祝结束，恢复手动换色
        ext_ctrl_set_big_state(EXT_STATE_BIG_INACTIVE); // 闪烁结束：回到未激活
        if (switch_req)
        {
            return BIG_RUN_ABORTED;
        }

        /* 全部熄灭并保持一小段时间；结束后外层回到“未激活待机” */
        all_small_boards_off();
        if (wait_ms_or_double_click(BIG_TARGET_ROUND_OFF_MS))
        {
            return BIG_RUN_ABORTED;
        }
        return BIG_RUN_COMPLETED; // 激活完成，等待外层回到未激活待机
    }

    /* 未达到庆祝轮数：整组快速熄灭；下一周期入口的 clear_all_boards_before_round()
       还会再做“完整全灭 + 等待 100ms + 确认反馈全 0”，确认后才点亮下一批 */
    all_small_boards_off();
    return BIG_RUN_CONTINUE; // 未达激活完成，外层继续下一轮
}

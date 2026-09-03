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
#include "_can.h"
#include <string.h>

/**
 * @brief 大符模式单次流程：一次随机点亮两个灯板
 * @return 期间检测到双击返回 true，正常结束返回 false
 */
bool run_big_target_once(void)
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

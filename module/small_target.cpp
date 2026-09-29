/**
  ******************************************************************************
  * @file           : small_target.cpp
  * @brief          : 小符模式控制：一轮一块，全部命中后闪烁庆祝
  *
  * 5 块灯板洗牌排序后逐块点亮，等待被击打或超时；5 块全部命中后
  * 闪烁庆祝 2s（期间不支持换色），全灭 500ms 后进入下一轮。
  * 灯板命令/确认/清场等公共逻辑见 board_ctrl.cpp。
  ******************************************************************************
*/
#include "small_target.h"
#include "board_ctrl.h"
#include "key_ctrl.h"
#include "_can.h"
#include <algorithm>
#include <string.h>

/* 小符模式各灯板命中标志 */
static bool success[5] = {0};

/* 小符模式全部命中闪烁后，全部熄灭保持的时间，单位毫秒 */
#define SMALL_ROUND_OFF_MS 500

/**
 * @brief 小符模式单次流程：一次只点亮一个灯板，等待命中或超时
 * @return 期间检测到双击返回 true，正常结束返回 false
 */
bool run_small_target_once(void)
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
    set_color_change_locked(true);
    send_celebration_twinkle(board, 0);

    bool switch_req = wait_ms_or_double_click(2000); // 闪烁 2s，期间可双击退出
    set_color_change_locked(false); // 庆祝结束，恢复手动换色
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

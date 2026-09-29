/**
  ******************************************************************************
  * @file           : ext_ctrl.cpp
  * @brief          : 外接控制板串口命令（USART3）：切换模式 / 切换颜色
  *
  * 帧格式（4 字节）：
  *   AA 55 CMD CHK
  *   CHK = (0xAA + 0x55 + CMD) & 0xFF
  * 命令：
  *   0x01 切换模式（小符 <-> 大符，等效 PC9 双击）
  *   0x02 切换颜色（蓝 <-> 红，等效 PC9 长按换色一次）
  *   0x03 启动大符激活（仅大符未激活待机时生效）
  *   0x04 复位大符（立即全灭回到未激活，重新等待启动）
  * 校验通过后主板回 ACK：AA 55 (CMD|0x80) CHK
  *
  * 大符模式期间主板额外持续上报状态帧（沿用同格式，外接板无需回 ACK）：
  *   0x11 大符未激活
  *   0x12 大符正在激活（击打中）
  *   0x13 大符激活完成（闪烁庆祝中）
  * 默认每 EXT_STATUS_PERIOD_MS（100ms）重发一次，状态切换时立即补发一帧；
  * 杜邦线直连抗干扰靠“周期重发 + 8 位校验”，丢帧最多 100ms 后自动补上。
  *
  * 接收用中断逐字节存入环形缓冲，主循环约每 1ms 轮询解析一次，
  * 避免在中断里做耗时处理；115200 波特率下 1ms 约 11 字节，缓冲取 16 足够。
  ******************************************************************************
*/
#include "ext_ctrl.h"
#include "usart.h"
#include "_can.h"
#include "board_ctrl.h"
#include "key_ctrl.h"

/* 帧固定长度与校验字节位置 */
#define EXT_FRAME_LEN 4
#define EXT_FRAME_CHK_IDX 3

/* 大符状态持续上报周期，单位毫秒 */
#define EXT_STATUS_PERIOD_MS 100

/* 当前需上报的大符状态（EXT_STATE_NONE 表示停止上报）与上次发送时刻 */
static uint8_t ext_status_cmd = EXT_STATE_NONE;
static uint32_t ext_status_last = 0;

/* 未处理的大符启动/复位请求：仅大符模式下置位，由主流程取走 */
static bool ext_big_start_pending = false;
static bool ext_big_reset_pending = false;

/* 固定长度接收帧：AA 55 CMD CHK */
static volatile uint8_t ext_rx_frame[EXT_FRAME_LEN];
static volatile uint8_t ext_rx_index = 0;
static volatile bool ext_switch_pending = false;
static uint8_t ext_rx_tmp;

/**
 * @brief 回一帧 ACK，供外接控制板确认命令已收到
 */
static void ext_send_ack(uint8_t cmd)
{
    uint8_t frame[EXT_FRAME_LEN] = {EXT_FRAME_H1, EXT_FRAME_H2,
                                    (uint8_t)(cmd | 0x80), 0};
    frame[EXT_FRAME_CHK_IDX] = (uint8_t)(frame[0] + frame[1] + frame[2]);
    HAL_UART_Transmit(&huart3, frame, EXT_FRAME_LEN, 50);
}

/**
 * @brief 发送一帧当前大符状态（主控主动上报，外接板无需回 ACK）
 */
static void ext_send_status(void)
{
    uint8_t frame[EXT_FRAME_LEN] = {EXT_FRAME_H1, EXT_FRAME_H2,
                                    ext_status_cmd, 0};
    frame[EXT_FRAME_CHK_IDX] = (uint8_t)(frame[0] + frame[1] + frame[2]);
    HAL_UART_Transmit(&huart3, frame, EXT_FRAME_LEN, 50);
}

/**
 * @brief 执行“切换颜色”：蓝 <-> 红，并给仍亮着的灯板补发新颜色
 *
 * 闪烁庆祝期间与长按换色行为一致：命令被消费但不实际改色。
 */
static void ext_apply_color_toggle(void)
{
    if (is_color_change_locked())
    {
        return;
    }

    canbox.TxData[2] = (canbox.TxData[2] == BLUE) ? RED : BLUE;
    resend_light_to_lit_boards();
}

/**
 * @brief 处理一条校验通过的完整命令
 * @return true 表示“切换模式”，调用方按双击切模式处理
 */
static bool ext_handle_command(uint8_t cmd)
{
    switch (cmd)
    {
        case EXT_CMD_TOGGLE_MODE:
            ext_send_ack(cmd);
            return true;

        case EXT_CMD_TOGGLE_COLOR:
            ext_send_ack(cmd);
            ext_apply_color_toggle();
            break;

        case EXT_CMD_START_BIG:
            ext_send_ack(cmd);
            if (ext_status_cmd == EXT_STATE_BIG_INACTIVE) // 仅大符未激活待机时启动才生效
            {
                ext_big_start_pending = true;
            }
            break;

        case EXT_CMD_RESET_BIG:
            ext_send_ack(cmd);
            if (ext_status_cmd != EXT_STATE_NONE) // 仅大符模式下的复位命令有效
            {
                ext_big_reset_pending = true;
            }
            break;

        default:
            break; // 未知命令：不回 ACK
    }
    return false;
}

/**
 * @brief 按固定帧格式接收一个字节，收满后校验并处理命令
 */
static void ext_feed_byte(uint8_t b)
{
    if (ext_rx_index == 0U)
    {
        if (b == EXT_FRAME_H1) ext_rx_frame[ext_rx_index++] = b;
        return;
    }
    if (ext_rx_index == 1U)
    {
        if (b == EXT_FRAME_H2)
        {
            ext_rx_frame[ext_rx_index++] = b;
        }
        else
        {
            ext_rx_index = (b == EXT_FRAME_H1) ? 1U : 0U;
            if (ext_rx_index != 0U) ext_rx_frame[0] = b;
        }
        return;
    }

    ext_rx_frame[ext_rx_index++] = b;
    if (ext_rx_index < EXT_FRAME_LEN) return;

    uint8_t cmd = ext_rx_frame[2];
    uint8_t expect = (uint8_t)(ext_rx_frame[0] + ext_rx_frame[1] + cmd);
    if (ext_rx_frame[3] == expect && ext_handle_command(cmd))
    {
        ext_switch_pending = true;
    }
    ext_rx_index = 0U;
}

/**
 * @brief 轮询处理已收到的串口命令（每 1ms 调用一次即可）
 */
bool ext_ctrl_process_uart(void)
{
    bool switch_requested = ext_switch_pending;
    ext_switch_pending = false;

    /* 大符模式期间持续上报：本函数由按键轮询约每 1ms 调用一次，
       在这里按固定周期重发当前状态，切换状态后首次轮询立即补发 */
    if (ext_status_cmd != EXT_STATE_NONE &&
        (uint32_t)(HAL_GetTick() - ext_status_last) >= EXT_STATUS_PERIOD_MS)
    {
        ext_status_last = HAL_GetTick();
        ext_send_status();
    }
    /* 大符模式下收到复位命令也要让当前流程尽快退出，由上层区分“切模式/复位” */
    return switch_requested || ext_big_reset_pending;
}

/**
 * @brief 设置/停止大符状态持续上报
 *
 * @param state EXT_STATE_BIG_INACTIVE / EXT_STATE_BIG_ACTIVATING /
 *              EXT_STATE_BIG_ACTIVATE_DONE / EXT_STATE_NONE（停止）
 */
void ext_ctrl_set_big_state(uint8_t state)
{
    ext_status_cmd = state;
    ext_status_last = 0; // 置 0 保证下一轮轮询立即发送新状态
}

/**
 * @brief 取走大符“启动”请求（收到 EXT_CMD_START_BIG 后置位）
 */
bool ext_ctrl_take_big_start(void)
{
    bool request = ext_big_start_pending;
    ext_big_start_pending = false;
    return request;
}

/**
 * @brief 取走大符“复位”请求（收到 EXT_CMD_RESET_BIG 后置位）
 */
bool ext_ctrl_take_big_reset(void)
{
    bool request = ext_big_reset_pending;
    ext_big_reset_pending = false;
    return request;
}

/**
 * @brief 清空未处理的大符启动/复位请求（切换模式时丢弃残留请求）
 */
void ext_ctrl_clear_big_requests(void)
{
    ext_big_start_pending = false;
    ext_big_reset_pending = false;
}

/**
 * @brief 初始化外接控制板串口（USART3）：启动中断接收
 */
void ext_ctrl_init(void)
{
    ext_rx_index = 0;
    ext_switch_pending = false;
    ext_status_cmd = EXT_STATE_NONE; // 上电默认不主动上报（默认小符模式）
    ext_status_last = 0;
    ext_big_start_pending = false;
    ext_big_reset_pending = false;
    HAL_UART_Receive_IT(&huart3, &ext_rx_tmp, 1);
}

/* 接收完成回调：逐字节写入固定长度帧并重新挂接接收 */
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        ext_feed_byte(ext_rx_tmp);
        HAL_UART_Receive_IT(&huart3, &ext_rx_tmp, 1);
    }
}

/* 串口错误回调：溢出(ORE)等错误会终止接收，重新挂接 */
extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        HAL_UART_Receive_IT(&huart3, &ext_rx_tmp, 1);
    }
}

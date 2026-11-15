/**
  ******************************************************************************
  * @file           : ext_ctrl.cpp
  * @brief          : 外接控制板串口命令（USART3）：切换模式 / 切换颜色
  *
  * 帧格式（4 字节）：AA 55 TYPE VALUE
  * 命令：
  *   01 01 大符；01 06 小符
  *   02 02 红色；02 05 蓝色
  *   03 02 启动大符激活（仅大符未激活待机时生效）
  *   04 03 复位大符（立即全灭回到未激活，重新等待启动）
  * 主板 ACK：AA 55 (TYPE|0x80) CHK，CHK 沿用原实现
  *
  * 大符模式期间主板额外持续上报状态帧（AA 55 STATE CHK，外接板无需回 ACK）：
  *   0x11 大符未激活
  *   0x12 大符正在激活（击打中）
  *   0x13 大符激活完成（闪烁庆祝中）
  * 默认每 EXT_STATUS_PERIOD_MS（100ms）重发一次，状态切换时立即补发一帧；
  * 状态帧沿用“周期重发 + 8 位校验”，丢帧最多 100ms 后自动补上。
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

/* 帧固定长度及 ACK/状态帧的校验字节位置 */
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
static uint8_t ext_mode_target = EXT_MODE_TOGGLE;

/* 固定长度接收帧：AA 55 TYPE VALUE */
static volatile uint8_t ext_rx_frame[EXT_FRAME_LEN];
static volatile uint8_t ext_rx_index = 0;
static volatile bool ext_switch_pending = false;
static volatile uint8_t ext_rx_tmp;

static volatile uint8_t ext_ack_pending_cmd = 0;

/* 调试用：最近一帧的命令类型和参数，ext_last_rx_ok 表示是否为已定义组合 */
volatile uint8_t ext_last_rx_cmd = 0;
volatile uint8_t ext_last_rx_arg = 0;
volatile uint8_t ext_last_rx_ok = 0;

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
static void ext_apply_color(uint8_t color)
{
    if (is_color_change_locked())
    {
        return;
    }

    canbox.TxData[2] = color;
    resend_light_to_lit_boards();
}

/**
 * @brief 处理一对已定义的命令类型和参数
 * @return true 表示“切换模式”，调用方按双击切模式处理
 */
static bool ext_handle_command(uint8_t type, uint8_t arg)
{
    switch (type)
    {
        case EXT_CMD_MODE:
            if (arg != EXT_ARG_BIG && arg != EXT_ARG_SMALL)
            {
                break;
            }
            ext_last_rx_ok = 1;
            ext_ack_pending_cmd = type;
            ext_mode_target = (arg == EXT_ARG_BIG) ? EXT_MODE_BIG : EXT_MODE_SMALL;
            return true;

        case EXT_CMD_COLOR:
            if (arg == EXT_ARG_RED)
            {
                ext_last_rx_ok = 1;
                ext_ack_pending_cmd = type;
                ext_apply_color(RED);
            }
            else if (arg == EXT_ARG_BLUE)
            {
                ext_last_rx_ok = 1;
                ext_ack_pending_cmd = type;
                ext_apply_color(BLUE);
            }
            break;

        case EXT_CMD_START_BIG:
            if (arg != EXT_ARG_START_BIG)
            {
                break;
            }
            ext_last_rx_ok = 1;
            ext_ack_pending_cmd = type;
            if (ext_status_cmd == EXT_STATE_BIG_INACTIVE) // 仅大符未激活待机时启动才生效
            {
                ext_big_start_pending = true;
            }
            break;

        case EXT_CMD_RESET_BIG:
            if (arg != EXT_ARG_RESET_BIG)
            {
                break;
            }
            ext_last_rx_ok = 1;
            ext_ack_pending_cmd = type;
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
 * @brief 按固定帧格式接收一个字节，收满后匹配并处理命令
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

    uint8_t type = ext_rx_frame[2];
    uint8_t arg = ext_rx_frame[3];

    /* 调试：记录最近一帧的命令类型、参数及是否为已定义组合 */
    ext_last_rx_cmd = type;
    ext_last_rx_arg = arg;
    ext_last_rx_ok = 0;

    if (ext_handle_command(type, arg))
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

    /* 在主循环中发送 ACK，避免在接收中断回调里调用阻塞式 HAL_UART_Transmit */
    if (ext_ack_pending_cmd != 0U)
    {
        uint8_t ack_cmd = ext_ack_pending_cmd;
        ext_ack_pending_cmd = 0U;
        ext_send_ack(ack_cmd);
    }

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

uint8_t ext_ctrl_take_mode_target(void)
{
    uint8_t target = ext_mode_target;
    ext_mode_target = EXT_MODE_TOGGLE;
    return target;
}

/**
 * @brief 清空未处理的大符启动/复位请求（切换模式时丢弃残留请求）
 */
void ext_ctrl_clear_big_requests(void)
{
    ext_big_start_pending = false;
    ext_big_reset_pending = false;
    ext_mode_target = EXT_MODE_TOGGLE;
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
    ext_mode_target = EXT_MODE_TOGGLE;
    HAL_UART_Receive_IT(&huart3, (uint8_t *)&ext_rx_tmp, 1);
}

/* 重新挂接串口接收，并清除可能残留的溢出(ORE)等错误标志。
   ORE 标志需“读 SR + 读 DR”才能清除，否则后续每次中断都走错误分支卡死。 */
static void ext_restart_uart_rx(void)
{
    __HAL_UART_CLEAR_OREFLAG(&huart3);
    huart3.ErrorCode = HAL_UART_ERROR_NONE;

    if (HAL_UART_Receive_IT(&huart3, (uint8_t *)&ext_rx_tmp, 1) != HAL_OK)
    {
        /* 仍失败则强制复位接收状态后再试一次 */
        huart3.RxState = HAL_UART_STATE_READY;
        HAL_UART_Receive_IT(&huart3, (uint8_t *)&ext_rx_tmp, 1);
    }
}

/* 接收完成回调：逐字节写入固定长度帧并重新挂接接收 */
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        ext_feed_byte(ext_rx_tmp);
        ext_restart_uart_rx();
    }
}

/* 串口错误回调：溢出(ORE)等错误会终止接收，清除错误标志后重新挂接 */
extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        ext_restart_uart_rx();
    }
}

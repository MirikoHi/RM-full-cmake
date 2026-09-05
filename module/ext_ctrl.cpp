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
  * 校验通过后主板回 ACK：AA 55 (CMD|0x80) CHK
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

/* 接收环形缓冲大小 */
#define EXT_RX_RING_SIZE 16

/* 接收环形缓冲（中断写入 / 主循环读取） */
static volatile uint8_t ext_rx_ring[EXT_RX_RING_SIZE];
static volatile uint8_t ext_rx_head = 0;
static volatile uint8_t ext_rx_tail = 0;
static uint8_t ext_rx_tmp;

/* 帧解析状态机 */
typedef enum
{
    EXT_PARSE_WAIT_H1 = 0, // 等待帧头1 (0xAA)
    EXT_PARSE_WAIT_H2,     // 等待帧头2 (0x55)
    EXT_PARSE_WAIT_CMD,    // 等待命令字
    EXT_PARSE_WAIT_CHK     // 等待校验字节
} ExtParseState_t;

static ExtParseState_t ext_parse_state = EXT_PARSE_WAIT_H1;
static uint8_t ext_parse_cmd = 0;

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

        default:
            break; // 未知命令：不回 ACK
    }
    return false;
}

/**
 * @brief 状态机喂入一个字节，返回本字节是否使一条“切模式”命令完整生效
 */
static bool ext_feed_byte(uint8_t b)
{
    switch (ext_parse_state)
    {
        case EXT_PARSE_WAIT_H1:
            if (b == EXT_FRAME_H1)
            {
                ext_parse_state = EXT_PARSE_WAIT_H2;
            }
            break;

        case EXT_PARSE_WAIT_H2:
            if (b == EXT_FRAME_H2)
            {
                ext_parse_state = EXT_PARSE_WAIT_CMD;
            }
            else if (b == EXT_FRAME_H1)
            {
                ext_parse_state = EXT_PARSE_WAIT_H2; // 帧头连续出现时按新帧处理
            }
            else
            {
                ext_parse_state = EXT_PARSE_WAIT_H1;
            }
            break;

        case EXT_PARSE_WAIT_CMD:
            ext_parse_cmd = b;
            ext_parse_state = EXT_PARSE_WAIT_CHK;
            break;

        case EXT_PARSE_WAIT_CHK:
        {
            uint8_t expect = (uint8_t)(EXT_FRAME_H1 + EXT_FRAME_H2 + ext_parse_cmd);
            ext_parse_state = EXT_PARSE_WAIT_H1; // 无论对错都回到找帧头
            if (b == expect)
            {
                return ext_handle_command(ext_parse_cmd);
            }
            break;
        }

        default:
            ext_parse_state = EXT_PARSE_WAIT_H1;
            break;
    }
    return false;
}

/**
 * @brief 轮询处理已收到的串口命令（每 1ms 调用一次即可）
 */
bool ext_ctrl_process_uart(void)
{
    bool switch_requested = false;

    while (ext_rx_head != ext_rx_tail)
    {
        uint8_t b = ext_rx_ring[ext_rx_tail];
        ext_rx_tail = (uint8_t)((ext_rx_tail + 1) % EXT_RX_RING_SIZE);

        if (ext_feed_byte(b))
        {
            switch_requested = true;
        }
    }
    return switch_requested;
}

/**
 * @brief 初始化外接控制板串口（USART3）：启动中断接收
 */
void ext_ctrl_init(void)
{
    ext_parse_state = EXT_PARSE_WAIT_H1;
    ext_rx_head = 0;
    ext_rx_tail = 0;
    HAL_UART_Receive_IT(&huart3, &ext_rx_tmp, 1);
}

/* 接收完成回调：中断上下文，只入环形缓冲并重新挂接接收 */
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3)
    {
        uint8_t next = (uint8_t)((ext_rx_head + 1) % EXT_RX_RING_SIZE);
        if (next != ext_rx_tail) // 缓冲未满才存入
        {
            ext_rx_ring[ext_rx_head] = ext_rx_tmp;
            ext_rx_head = next;
        }
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

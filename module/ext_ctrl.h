#ifndef __EXT_CTRL_H__
#define __EXT_CTRL_H__

#include <stdint.h>
#include <stdbool.h>

/* 外接控制板串口命令帧：AA 55 CMD CHK
   CHK = (0xAA + 0x55 + CMD) & 0xFF，校验通过后主板回 ACK：AA 55 (CMD|0x80) CHK */
#define EXT_FRAME_H1 0xAA
#define EXT_FRAME_H2 0x55

/* 命令定义 */
#define EXT_CMD_TOGGLE_MODE  0x01 /* 切换模式：小符 <-> 大符，等效 PC9 双击 */
#define EXT_CMD_TOGGLE_COLOR 0x02 /* 切换颜色：蓝 <-> 红，等效 PC9 长按换色一次 */

/* 主控 -> 外接板大符状态帧（沿用 AA 55 STATE CHK，外接板无需回 ACK） */
#define EXT_STATE_NONE             0x00 /* 停止上报（不在大符模式） */
#define EXT_STATE_BIG_INACTIVE     0x11 /* 大符未激活 */
#define EXT_STATE_BIG_ACTIVE_FLASH 0x12 /* 大符已激活（连续 5 轮成功，闪烁庆祝中） */

/**
 * @brief 初始化外接控制板串口（USART3）：启动中断接收
 */
void ext_ctrl_init(void);

/**
 * @brief 轮询处理已收到的串口命令（每 1ms 调用一次即可）
 *
 * 收到“切换模式”命令返回 true，调用方应与“双击切模式”同样处理；
 * “切换颜色”命令在本函数内直接执行，不通过返回值表达。
 *
 * @return true 本次轮询收到并校验通过一条“切换模式”命令
 */
bool ext_ctrl_process_uart(void);

/**
 * @brief 设置/停止大符状态持续上报
 *
 * 设置后，ext_ctrl_process_uart() 每次轮询会按 EXT_STATUS_PERIOD_MS
 * 周期重发当前状态帧，状态切换时立即补发一帧。
 *
 * @param state 要上报的状态：EXT_STATE_BIG_INACTIVE /
 *              EXT_STATE_BIG_ACTIVE_FLASH；EXT_STATE_NONE 停止上报
 */
void ext_ctrl_set_big_state(uint8_t state);

#endif /* __EXT_CTRL_H__ */

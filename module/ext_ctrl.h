#ifndef __EXT_CTRL_H__
#define __EXT_CTRL_H__

#include <stdint.h>
#include <stdbool.h>

/* 外接控制板串口命令帧：AA 55 CMD CHK
   CHK = (0xAA + 0x55 + CMD) & 0xFF，校验通过后主板回 ACK：AA 55 (CMD|0x80) CHK */
#define EXT_FRAME_H1 0xAA
#define EXT_FRAME_H2 0x55

/* 命令定义 */
#define EXT_CMD_SET_BIG      0x01 /* 设置为大符模式 */
#define EXT_CMD_SET_SMALL    0x06 /* 设置为小符模式 */
#define EXT_CMD_SET_RED      0x02 /* 设置为红色 */
#define EXT_CMD_SET_BLUE     0x05 /* 设置为蓝色 */
#define EXT_CMD_START_BIG    0x03 /* 启动大符激活：未激活待机时收到才生效 */
#define EXT_CMD_RESET_BIG    0x04 /* 复位大符：立即全灭回到未激活，重新等待启动 */

/* 主控 -> 外接板大符状态帧（沿用 AA 55 STATE CHK，外接板无需回 ACK） */
#define EXT_STATE_NONE              0x00 /* 停止上报（不在大符模式） */
#define EXT_STATE_BIG_INACTIVE      0x11 /* 大符未激活（全灭待启动） */
#define EXT_STATE_BIG_ACTIVATING    0x12 /* 大符正在激活（按轮点亮击打中） */
#define EXT_STATE_BIG_ACTIVATE_DONE 0x13 /* 大符激活完成（连续 5 轮成功，闪烁庆祝中） */

#define EXT_MODE_SMALL   0
#define EXT_MODE_BIG     1
#define EXT_MODE_TOGGLE  0xFF /* 无外接直接指令，走 PC9 双击取反逻辑 */

/* 调试用：最近一次收到的命令字节（不筛选，校验失败也记录，新的覆盖旧的） */
extern volatile uint8_t ext_last_rx_cmd;
extern volatile uint8_t ext_last_rx_chk;    /* 收到的校验字节 */
extern volatile uint8_t ext_last_rx_expect; /* 计算出的期望校验字节 */
extern volatile uint8_t ext_last_rx_ok;     /* 1=校验通过，0=校验失败 */

/**
 * @brief 初始化外接控制板串口（USART3）：启动中断接收
 */
void ext_ctrl_init(void);

/**
 * @brief 轮询处理已收到的串口命令（每 1ms 调用一次即可）
 *
 * 收到“切换模式”命令返回 true，调用方应与“双击切模式”同样处理；
 * 大符模式下收到“复位”命令同样返回 true（用于中断当前流程，由上层区分处理）；
 * “切换颜色”命令在本函数内直接执行，不通过返回值表达。
 *
 * @return true 本次轮询收到并校验通过一条“切换模式”命令
 */
bool ext_ctrl_process_uart(void);

/**
 * @brief 取走大符“启动”请求（收到 EXT_CMD_START_BIG 后置位）
 * @return true 表示有待处理的启动请求，取走后清空
 */
bool ext_ctrl_take_big_start(void);

/**
 * @brief 取走大符“复位”请求（收到 EXT_CMD_RESET_BIG 后置位）
 * @return true 表示有待处理的复位请求，取走后清空
 */
bool ext_ctrl_take_big_reset(void);

/**
 * @brief 清空未处理的大符启动/复位请求（切换模式时丢弃残留请求）
 */
void ext_ctrl_clear_big_requests(void);

/**
 * @brief 设置/停止大符状态持续上报
 *
 * 设置后，ext_ctrl_process_uart() 每次轮询会按 EXT_STATUS_PERIOD_MS
 * 周期重发当前状态帧，状态切换时立即补发一帧。
 *
 * @param state 要上报的状态：EXT_STATE_BIG_INACTIVE /
 *              EXT_STATE_BIG_ACTIVATING / EXT_STATE_BIG_ACTIVATE_DONE；
 *              EXT_STATE_NONE 停止上报
 */
void ext_ctrl_set_big_state(uint8_t state);

/**
 * @brief 取走外接板请求的目标模式（收到 SET_SMALL / SET_BIG 后置位）
 * @return EXT_MODE_SMALL / EXT_MODE_BIG 表示直接切到该模式；
 *         EXT_MODE_TOGGLE 表示没有外接直接指令（即 PC9 双击，由调用方取反切换）
 */
uint8_t ext_ctrl_take_mode_target(void);

#endif /* __EXT_CTRL_H__ */
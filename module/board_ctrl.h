#ifndef __BOARD_CTRL_H__
#define __BOARD_CTRL_H__

#include <stdint.h>
#include <stdbool.h>

/* 板级工作状态 */
#define BOARD_READY 1

/* 颜色定义 */
#define BLUE 1
#define RED 0

/* 灯板点亮时间，单位毫秒 */
#define TARGET_LIGHT_ON_MS 2500

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

/**
 * @brief 随机数辅助函数
 * @param i 随机范围上限
 * @return 返回 0 ~ i-1 的随机数（洗牌/选板共用）
 */
int Rand(int i);

/**
 * @brief 手动换色后，向每一块当前亮着的灯板补发带新颜色的点亮命令
 */
void resend_light_to_lit_boards(void);

/**
 * @brief 五块灯板全部命中后的庆祝闪烁：向全部灯板发送 twinkle=1 命令
 * @param board_ids 灯板 CAN 标准帧 ID 数组（长度 5，通常为当轮洗牌顺序）
 * @param mode 命令帧字节5：0=小符（灯板闪完复位），1=大符（灯板闪完不复位）
 */
void send_celebration_twinkle(const uint16_t *board_ids, uint8_t mode);

/**
 * @brief 熄灭全部小符灯板
 */
void all_small_boards_off(void);

/**
 * @brief 清场 + 多层保险（含补发与兜底超时）
 * @retval true 等待期间检测到双击，调用方应交给外层切换模式
 */
bool clear_all_boards_before_round(void);

/**
 * @brief 点亮单块灯板并等待“已亮起/已命中”回报，未确认按新序号有界重发
 * @retval true 等待/重发期间检测到双击，调用方应交由外层切换模式
 */
bool light_board_and_confirm(uint32_t board_id);

/**
 * @brief 大符每轮开始：给全部 5 块灯板发“本轮进度”命令（mode=1, hitEnable=0, step=轮号）
 *
 * 未抽中的灯板据此显示整体激活进度；上一轮被抽中的灯板也借此清掉命中锁存，
 * 下一帧点亮命令到达时才会重新画“待击打”。
 * @param step 本轮轮号，0 起（= 本轮开始前已完成的连续成功轮数）
 */
void send_big_round_progress(uint8_t step);

/**
 * @brief 同时点亮两块灯板并并行等待确认（大符模式），未确认按新序号有界重发
 * @param step 本轮轮号，随命令帧字节7下发，供灯板显示进度
 * @retval true 等待/重发期间检测到双击，调用方应交由外层切换模式
 */
bool light_two_boards_and_confirm(uint32_t id1, uint32_t id2, uint8_t step);

#endif /* __BOARD_CTRL_H__ */

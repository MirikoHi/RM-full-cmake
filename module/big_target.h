#ifndef __BIG_TARGET_H__
#define __BIG_TARGET_H__

#include <stdbool.h>

/* 大符模式单轮流程结果 */
typedef enum
{
    BIG_RUN_CONTINUE = 0, // 本轮正常结束，未达到激活完成，继续下一轮
    BIG_RUN_COMPLETED,    // 连续 5 轮成功，闪烁庆祝正常结束（大符激活完成）
    BIG_RUN_ABORTED       // 期间双击/串口切模式/复位请求，中断当前流程
} BigRunResult_t;

/**
 * @brief 大符模式单次流程：一次随机点亮两个灯板
 * @return BIG_RUN_ABORTED 期间检测到双击/切模式/复位；
 *         BIG_RUN_COMPLETED 本轮为闪烁庆祝且已正常结束；
 *         BIG_RUN_CONTINUE 普通一轮结束，继续下一轮
 */
BigRunResult_t run_big_target_once(void);

/**
 * @brief 清零大符模式连续成功轮数（双击重新进入大符模式时由 start.cpp 调用）
 */
void reset_big_target_count(void);

#endif /* __BIG_TARGET_H__ */

#ifndef __BIG_TARGET_H__
#define __BIG_TARGET_H__

#include <stdbool.h>

/**
 * @brief 大符模式单次流程：一次随机点亮两个灯板
 * @return 期间检测到双击返回 true，正常结束返回 false
 */
bool run_big_target_once(void);

/**
 * @brief 清零大符模式连续成功轮数（双击重新进入大符模式时由 start.cpp 调用）
 */
void reset_big_target_count(void);

#endif /* __BIG_TARGET_H__ */

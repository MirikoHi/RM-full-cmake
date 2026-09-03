#ifndef __SMALL_TARGET_H__
#define __SMALL_TARGET_H__

#include <stdbool.h>

/**
 * @brief 小符模式单次流程：一次只点亮一个灯板，等待命中或超时
 * @return 期间检测到双击返回 true，正常结束返回 false
 */
bool run_small_target_once(void);

#endif /* __SMALL_TARGET_H__ */

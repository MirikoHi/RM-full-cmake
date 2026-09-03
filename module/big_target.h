#ifndef __BIG_TARGET_H__
#define __BIG_TARGET_H__

#include <stdbool.h>

/**
 * @brief 大符模式单次流程：一次随机点亮两个灯板
 * @return 期间检测到双击返回 true，正常结束返回 false
 */
bool run_big_target_once(void);

#endif /* __BIG_TARGET_H__ */

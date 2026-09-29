#ifndef __START_H__
#define __START_H__

#include "main.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief while循环入口
 */
void startup(void);

/* 对外运行状态标志（start.cpp / big_target.cpp 中维护）
   small_t_status: 1 = 小符流程运行中, 0 = 小符未运行
   big_t_status:   0 = 大符未运行, 1 = 大符运行中且未激活（待机）,
                   2 = 大符正在激活, 3 = 大符激活完成（闪烁庆祝中） */
extern uint8_t small_t_status;
extern uint8_t big_t_status;

/* 对外颜色标志（start.cpp 中维护）
   small_co: 小符模式下 0 = 未开启, 1 = 蓝色, 2 = 红色
   big_co:   大符模式下 0 = 未开启, 1 = 蓝色, 2 = 红色 */
extern uint8_t small_co;
extern uint8_t big_co;

#ifdef __cplusplus
}
#endif

#endif

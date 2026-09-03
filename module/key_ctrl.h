#ifndef __KEY_CTRL_H__
#define __KEY_CTRL_H__

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 轮询模式切换按键：长按换色 / 双击切换模式
 *
 * PC9 按键承担两种操作：
 * - 双击：两次确认的“按下”边沿间隔小于 400ms，返回 true，由外层切换模式；
 * - 长按：按住满 2s 手动切换一次颜色（蓝<->红），内部完成换色并返回 false。
 * 闪烁庆祝期间（set_color_change_locked(true)）长按会被消费但不换色。
 *
 * @return 检测到一次有效双击返回 true，否则返回 false
 */
bool poll_mode_switch_keys(void);

/**
 * @brief 延时等待，期间轮询按键（长按可手动换色），若检测到双击则提前返回
 * @param ms 需要等待的时间，单位毫秒
 * @return 期间检测到双击返回 true，正常延时结束返回 false
 */
bool wait_ms_or_double_click(uint32_t ms);

/**
 * @brief 锁定/解锁长按换色
 *
 * 击打完成闪烁庆祝期间置 true：长按只被消费、不切换颜色。
 * @param locked true 锁定（不支持换色），false 恢复
 */
void set_color_change_locked(bool locked);

#endif /* __KEY_CTRL_H__ */

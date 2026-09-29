/**
  ******************************************************************************
  * @file           : board_ctrl.cpp
  * @brief          : 小符灯板公共控制：命令收发、点亮确认、清场与换色补发
  *
  * 从小符/大符两条流程中抽出的共享部分：5 块灯板的 CAN 命令封装、命令序号、
  * 新/旧协议判定、点亮确认、整组清场以及长按换色后的补发。
  ******************************************************************************
*/
#include "board_ctrl.h"
#include "key_ctrl.h"
#include "_can.h"
#include <string.h>
#include <stdlib.h>

/* 各灯板最近一次逻辑命令的序号（命令帧字节4）。同一条逻辑命令的冗余帧共用同一序号，
   灯板会在反馈字节4中回显“最后真正执行过的命令序号”，供主板确认命令是否被应用 */
static uint8_t board_cmd_seq[5] = {0};

/* 灯板反馈协议状态：新板回显字节2(实际亮起)/字节4(命令序号)，旧板只有字节0/1 */
typedef enum
{
    BOARD_PROTO_UNKNOWN = 0, // 尚未判定：先按旧协议保守处理，避免干等确认
    BOARD_PROTO_NEW,         // 新协议：能回显亮起状态与命令序号
    BOARD_PROTO_OLD          // 旧协议：只回报“在线”和“命中”
} BoardProto_t;

/* 每块灯板的协议判定结果 */
static uint8_t board_proto[5] = {BOARD_PROTO_UNKNOWN};

/* 当前处于“点亮/常亮”状态的灯板掩码：长按换色后需要向这些板补发点亮命令 */
static bool lit_board_mask[5] = {0};

/* 命令帧字节5(模式)/字节7(轮号)的当前取值：小符与熄灭固定 0/0，
   大符流程发送前切到 mode=1 + 本轮轮号 */
static uint8_t order_mode = 0;
static uint8_t order_step = 0;

/**
 * @brief 设置后续命令帧的模式与轮号字段（字节5/字节7）
 */
static void set_order_context(uint8_t mode, uint8_t step)
{
    order_mode = mode;
    order_step = step;
}

/* 随机数辅助函数 */
int Rand(int i)
{
    return rand() % i;
}

/**
 * @brief 向指定小符灯板发送点亮或熄灭命令
 * @param board_id 灯板 CAN 标准帧 ID
 * @param hit_enable 1 为点亮，0 为熄灭
 * @param seq 命令序号，同一条逻辑命令的冗余帧必须使用同一序号
 */
static void send_board_order(uint32_t board_id, uint8_t hit_enable, uint8_t seq)
{
    canbox.TxData[0] = BOARD_READY; // 控制状态
    canbox.TxData[1] = hit_enable;  // 点亮或熄灭
    canbox.TxData[3] = 0;           // 闪烁状态
    canbox.TxData[4] = seq;         // 命令序号，供灯板执行后回显
    canbox.TxData[5] = order_mode;  // 0=小符/熄灭，1=大符
    canbox.TxData[7] = order_step;  // 大符：本轮轮号（小符恒 0）

    while (!canbox.Send(board_id, 0x08)) {}
}

/**
 * @brief 向指定小符灯板发送“点亮”命令
 *
 * 连续发送两帧点亮命令（冗余），避免单帧丢失导致该板整轮不亮。
 * 灯板已亮时收到同色点亮命令保持现状；收到不同颜色的点亮命令会按
 * 新颜色重绘（灯板固件支持重复点亮改色）。
 * @param board_id 灯板 CAN 标准帧 ID
 */
static void send_board_light(uint32_t board_id)
{
    lit_board_mask[board_id - SMALL_BOARD_ID_BASE] = true; // 该板当前处于点亮状态

    /* 一次逻辑“点亮”只递增一次序号，冗余两帧共用，灯板回显的序号唯一 */
    uint8_t seq = ++board_cmd_seq[board_id - SMALL_BOARD_ID_BASE];
    send_board_order(board_id, 1, seq); // 点亮帧 1
    send_board_order(board_id, 1, seq); // 点亮帧 2（冗余）
}

/**
 * @brief 手动换色后，把新颜色补发给每一块灯板
 *
 * 亮着的板补发点亮命令（灯板已亮时收到不同颜色会按新颜色重绘）；
 * 没点亮的板也要收到一条命令，好让常亮的 R 标跟着换色。
 */
void resend_light_to_lit_boards(void)
{
    if (order_mode == 1)
    {
        /* 大符：先给全部 5 块重发本轮进度，未抽中的板据此换色 */
        send_big_round_progress(order_step);
    }
    else
    {
        /* 小符：没点亮的板补一条熄灭命令，只为刷新常亮的 R 标 */
        for (uint8_t i = 0; i < 5; i++)
        {
            if (!lit_board_mask[i])
            {
                send_board_order(SMALL_BOARD_ID_BASE + i, 0, ++board_cmd_seq[i]);
            }
        }
    }

    /* 亮着的板补发点亮命令（大符下就是被抽中的两块） */
    for (uint8_t i = 0; i < 5; i++)
    {
        if (lit_board_mask[i])
        {
            send_board_light(SMALL_BOARD_ID_BASE + i);
        }
    }
}

/**
 * @brief 五块灯板全部命中后的庆祝闪烁
 *
 * 向全部 5 块灯板发送 twinkle=1 命令，每块板使用各自递增的命令序号；
 * 颜色沿用 TxData[2]（闪烁庆祝期间由上层锁定，不支持换色）。
 * @param board_ids 灯板 CAN 标准帧 ID 数组（长度 5）
 */
void send_celebration_twinkle(const uint16_t *board_ids, uint8_t mode)
{
    set_order_context(mode, 0);
    canbox.TxData[3] = 1; // 开启闪烁
    for (uint8_t j = 0; j < 5; j++)
    {
        canbox.TxData[4] = ++board_cmd_seq[board_ids[j] - SMALL_BOARD_ID_BASE]; // 命令序号
        while (!canbox.Send(board_ids[j], 0x08)) {}
    }
}

/**
 * @brief 大符每轮开始：给全部 5 块灯板发“本轮进度”命令（mode=1, hitEnable=0, step=轮号）
 */
void send_big_round_progress(uint8_t step)
{
    set_order_context(1, step);

    /* 每块板一条逻辑命令，各分配一个命令序号 */
    for (uint8_t i = 0; i < 5; i++)
    {
        send_board_order(SMALL_BOARD_ID_BASE + i, 0, ++board_cmd_seq[i]);
    }
}

/**
 * @brief 判断灯板是否支持新协议（能回显字节2/字节4），并维护判定状态
 *
 * 新灯板执行过带序号的命令后，会在反馈字节4回显 lastSeq，点亮后还会在
 * 字节2回显 lightState=1；旧灯板只回显字节0(在线)和字节1(命中)，字节2/4
 * 恒为 0。因此：
 * - 只要出现过字节2/字节4非零，就升级判为新板；
 * - 已收到“在线”反馈、主板也发过带序号命令，却始终没有新字段回显 → 判为旧板；
 * - 尚无反馈/尚未发过命令的未知板，先按旧板保守处理，防止干等确认拖慢流程。
 * 判定只影响“能否严格确认”，不影响点亮/熄灭命令本身（命令前几字节两版通用）。
 * @param idx 灯板索引 0~4
 * @retval true 新协议板，可要求字节2/4 的严格确认
 */
static bool board_uses_new_proto(uint8_t idx)
{
    uint8_t *rx = canbox.RxData[idx];

    if (rx[2] != 0 || rx[4] != 0) // 出现过新字段：无论此前判定如何都升级
    {
        board_proto[idx] = BOARD_PROTO_NEW;
        return true;
    }
    if (board_proto[idx] == BOARD_PROTO_NEW)
    {
        return true; // 已判定为新板
    }
    if (board_proto[idx] == BOARD_PROTO_OLD)
    {
        return false; // 已判定为旧板
    }
    if (rx[0] == BOARD_READY && board_cmd_seq[idx] != 0)
    {
        board_proto[idx] = BOARD_PROTO_OLD; // 在线却从不回显新字段 → 旧板
        return false;
    }
    return false; // 还没收到反馈：先按旧板保守放行
}

/**
 * @brief 查询灯板反馈是否已确认“点亮/命中”
 * @param board_id 灯板 CAN 标准帧 ID
 * @retval true 反馈显示已亮起或已命中
 */
static bool board_reports_lit(uint32_t board_id)
{
    uint8_t idx = (uint8_t)(board_id - SMALL_BOARD_ID_BASE);
    return canbox.RxData[idx][2] == 1 || // 已回报亮起
           canbox.RxData[idx][1] == 1;   // 或已回报命中
}

/**
 * @brief 点亮单块灯板并等待其回报“已亮起/已命中”，未确认则自动按新序号重发
 *
 * 灯板点亮后约在下一个 20ms 主循环回报 lightState=1；
 * 若在 BOARD_LIGHT_CONFIRM_MS 内始终看不到“已亮起”，
 * 说明点亮帧可能丢失或灯板异常，本函数会重新点亮（最多
 * BOARD_LIGHT_CONFIRM_ATTEMPTS 次）。若重试后仍未确认，
 * 由调用方后续的命中等待/超时逻辑兜底，不会无限重发。
 * @param board_id 灯板 CAN 标准帧 ID
 * @retval true 等待/重发期间检测到双击，调用方应交由外层切换模式
 */
bool light_board_and_confirm(uint32_t board_id)
{
    uint8_t idx = (uint8_t)(board_id - SMALL_BOARD_ID_BASE);

    set_order_context(0, 0); // 小符命令

    /* 旧板/未知板无法回报“实际亮起”：冗余两帧发出后直接放行，
       由调用方后续的命中等待/超时逻辑兜底（等价于旧版行为） */
    if (!board_uses_new_proto(idx))
    {
        send_board_light(board_id); // 冗余两帧 + 新命令序号
        return false;
    }

    for (uint8_t attempt = 0; attempt < BOARD_LIGHT_CONFIRM_ATTEMPTS; attempt++)
    {
        send_board_light(board_id); // 冗余两帧 + 新命令序号

        uint32_t start = HAL_GetTick();
        while ((HAL_GetTick() - start) < BOARD_LIGHT_CONFIRM_MS)
        {
            if (poll_mode_switch_keys())
            {
                return true; // 双击：切换模式
            }
            if (board_reports_lit(board_id))
            {
                return false; // 确认成功
            }
            HAL_Delay(1);
        }
    }
    return false; // 多次重发仍未确认：交由命中等待/超时兜底
}

/**
 * @brief 点亮两块灯板并“并行”等待确认，未确认的板按新序号自动重发
 *
 * 大符模式要求两块灯板同时亮起，因此两块板的点亮命令必须在任何等待
 * 之前背靠背连续发出（两者实际间隔仅为 CAN 逐帧发送的亚毫秒级时间）。
 * 之后再并行等待各自回报“已亮起/已命中”：某一窗口内仍未确认的板，
 * 下一轮只对它重发，已确认的板不再打扰。
 * @param id1 第一块灯板 CAN 标准帧 ID
 * @param id2 第二块灯板 CAN 标准帧 ID
 * @retval true 等待/重发期间检测到双击，调用方应交由外层切换模式
 */
bool light_two_boards_and_confirm(uint32_t id1, uint32_t id2, uint8_t step)
{
    uint8_t idx1 = (uint8_t)(id1 - SMALL_BOARD_ID_BASE);
    uint8_t idx2 = (uint8_t)(id2 - SMALL_BOARD_ID_BASE);

    set_order_context(1, step); // 大符命令：带本轮轮号

    /* 只有新协议板才需要等“实际亮起”回报；旧板/未知板命令发出即视为完成 */
    bool wait1 = board_uses_new_proto(idx1);
    bool wait2 = board_uses_new_proto(idx2);
    bool lit1 = false;
    bool lit2 = false;

    for (uint8_t attempt = 0; attempt < BOARD_LIGHT_CONFIRM_ATTEMPTS; attempt++)
    {
        /* 关键：先给两块板都发点亮命令，再开始等待，保证两个灯同时亮起 */
        if (!lit1)
        {
            send_board_light(id1); // 冗余两帧 + 新命令序号
        }
        if (!lit2)
        {
            send_board_light(id2);
        }

        /* 旧板/未知板不支持回报亮起状态：本轮命令已发出，即视为放行（不再重发） */
        if (!wait1)
        {
            lit1 = true;
        }
        if (!wait2)
        {
            lit2 = true;
        }

        if (lit1 && lit2)
        {
            break; // 都不需要确认：两块命令已背靠背发出，立即放行
        }

        uint32_t start = HAL_GetTick();
        while ((HAL_GetTick() - start) < BOARD_LIGHT_CONFIRM_MS)
        {
            if (poll_mode_switch_keys())
            {
                return true; // 双击：切换模式
            }

            if (!lit1 && board_reports_lit(id1))
            {
                lit1 = true;
            }
            if (!lit2 && board_reports_lit(id2))
            {
                lit2 = true;
            }
            if (lit1 && lit2)
            {
                break;
            }
            HAL_Delay(1);
        }

        if (lit1 && lit2)
        {
            break;
        }
    }
    return false; // 多次重发仍未全部确认：交由命中等待/超时兜底
}

/**
 * @brief 熄灭全部小符灯板
 */
void all_small_boards_off(void)
{
    set_order_context(0, 0); // 熄灭命令按小符下发：灯板据此真正熄灯复位

    /* 全部熄灭后没有仍亮着的灯板 */
    memset(lit_board_mask, 0, sizeof(lit_board_mask));

    /* 每块板的一次逻辑“熄灭”只分配一个序号，两轮发送是该命令的冗余拷贝 */
    uint8_t off_seq[5];
    for (uint8_t i = 0; i < 5; i++)
    {
        off_seq[i] = ++board_cmd_seq[i];
    }

    /* 连续发送两轮熄灭命令，避免单帧丢失导致灯板残留击打状态 */
    for (uint8_t round = 0; round < 2; round++)
    {
        for (uint8_t i = 0; i < 5; i++)
        {
            send_board_order(SMALL_BOARD_ID_BASE + i, 0, off_seq[i]);
        }

        /* 等待熄灭命令全部发送完成，避免后续点亮帧抢在前面 */
        while (!__HAL_CAN_GET_FLAG(canbox.Can, CAN_FLAG_TME0) ||
               !__HAL_CAN_GET_FLAG(canbox.Can, CAN_FLAG_TME1) ||
               !__HAL_CAN_GET_FLAG(canbox.Can, CAN_FLAG_TME2)) {}
    }
}

/**
 * @brief 检查 5 块灯板是否都确认清场
 *
 * 在线、未命中是两版协议共同的清场前提；新协议板额外要求“灯已熄灭且已执行
 * 最新熄灭命令”（字节2/4），旧协议板无法回显这些字段，在线且未命中即视为完成。
 * @retval true 全部确认清场；false 仍有板子未确认
 */
static bool all_boards_report_clear(void)
{
    for (uint8_t i = 0; i < 5; i++)
    {
        if (canbox.RxData[i][0] != BOARD_READY ||
            canbox.RxData[i][1] != 0)                  // 仍上报命中
        {
            return false;
        }

        /* 新协议板才要求严格确认；旧板/未知板到这里即视为清场完成 */
        if (board_uses_new_proto(i) &&
            (canbox.RxData[i][2] != 0 ||               // 灯仍亮着
             canbox.RxData[i][4] != board_cmd_seq[i])) // 最新熄灭命令还没被应用
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief 清场 + 多层保险：
 *        1) 熄灭命令发完后至少等待 BOARD_CLEAR_SETTLE_MS，
 *           保证每块灯板都读到并执行过“熄灭”；
 *        2) 再确认 5 块灯板反馈都为“在线、未命中、已熄灭且已执行最新熄灭命令”；
 *        3) 确认期间对仍未清场的板子按反馈单独补发熄灭命令，
 *           用重发代替干等，提高偶发丢帧后的恢复率。
 * @retval true 等待期间检测到双击，调用方应交给外层切换模式
 */
bool clear_all_boards_before_round(void)
{
    all_small_boards_off();

    uint32_t start = HAL_GetTick();
    uint32_t last_resend = start;
    while ((HAL_GetTick() - start) < BOARD_CLEAR_CONFIRM_TIMEOUT_MS)
    {
        if (poll_mode_switch_keys())
        {
            return true;
        }

        uint32_t now = HAL_GetTick();
        if (((now - start) >= BOARD_CLEAR_SETTLE_MS) && all_boards_report_clear())
        {
            break;
        }

        /* 消化时间过后仍未全部确认：对“支持新协议但反馈显示仍亮着/仍报命中/未执行
           最新命令”的板子单独补发一轮熄灭（同一命令序号），旧板不补发；
           每 BOARD_CLEAR_RESEND_INTERVAL_MS 至多一次 */
        if (((now - start) >= BOARD_CLEAR_SETTLE_MS) &&
            ((now - last_resend) >= BOARD_CLEAR_RESEND_INTERVAL_MS))
        {
            last_resend = now;
            for (uint8_t i = 0; i < 5; i++)
            {
                if (canbox.RxData[i][0] == BOARD_READY &&
                    board_uses_new_proto(i) &&
                    (canbox.RxData[i][1] != 0 ||
                     canbox.RxData[i][2] != 0 ||
                     canbox.RxData[i][4] != board_cmd_seq[i]))
                {
                    send_board_order(SMALL_BOARD_ID_BASE + i, 0, board_cmd_seq[i]);
                }
            }
        }
        HAL_Delay(1);
    }
    return false;
}

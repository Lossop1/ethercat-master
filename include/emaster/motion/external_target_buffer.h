#ifndef EMASTER_MOTION_EXTERNAL_TARGET_BUFFER_H
#define EMASTER_MOTION_EXTERNAL_TARGET_BUFFER_H

/*
 * 外部目标缓冲区：**一个汇点，两条入口**。
 *
 * 入口一是命令口的文本动词 set_external_target（低频手工路径，只填位置与它那根轴的
 * 配置模式），入口二是设点通道的二进制记录（外部控制器按自己的频率送）。两条入口写
 * 同一份结构、同一把锁，周期回调只从这一处读——否则就得回答"回调到底该信谁"，而那个
 * 问题没有正确答案。
 *
 * 语义是**覆盖式最新值**：后到的整条记录覆盖先到的，没有队列、没有积压。所以主站只
 * 关心"最新那条有多旧"（last_update_ns），不关心客户端多久发一次——协议频率会变，
 * 不能写死。
 *
 * 单独成头，是因为它只依赖 pthread 与 stdint：设点通道要写它，而设点通道不该为此
 * 去拉总线层那一大串头文件。
 */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* 能带的轴数上限。协议、缓冲区、设点记录三处同值。 */
#define EMASTER_EXTERNAL_TARGET_MAX_AXES 16U

typedef struct
{
    pthread_mutex_t mutex;
    /* 目标位置（原始计数）。今天进输出路径的只有这一个数组。 */
    int32_t positions[EMASTER_EXTERNAL_TARGET_MAX_AXES];
    /*
     * 以下五组由设点通道写入，文本动词不碰它们——文本 verb 的那根轴写自己配置的模式，
     * 其余保持上次的值。
     *
     * dq / tau / kp / kd 今天不进输出路径：驱动器在 CSP 下自己闭合位置环，MIT 是不是
     * 主站给 kp、kd 还没有 LZ 文档佐证。先原样存下，等出口口径定了再接。
     */
    int8_t  modes[EMASTER_EXTERNAL_TARGET_MAX_AXES];
    int32_t velocities[EMASTER_EXTERNAL_TARGET_MAX_AXES];
    int32_t torques[EMASTER_EXTERNAL_TARGET_MAX_AXES];
    int32_t kp[EMASTER_EXTERNAL_TARGET_MAX_AXES];
    int32_t kd[EMASTER_EXTERNAL_TARGET_MAX_AXES];
    /* 最新一条记录携带的客户端序号与客户端时标，原样存下，供丢包与年龄判定。 */
    uint64_t sequence;
    uint64_t client_time_ns;
    /* 主站自己的单调时钟，写入本条的时刻。超时判定用的是它，不是客户端的时标。 */
    uint64_t last_update_ns;
    size_t   axis_count;
    int      available;
} emaster_external_target_buffer_t;

#endif /* EMASTER_MOTION_EXTERNAL_TARGET_BUFFER_H */

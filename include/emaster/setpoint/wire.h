#ifndef EMASTER_SETPOINT_WIRE_H
#define EMASTER_SETPOINT_WIRE_H

/*
 * 设点通道的线格式：定长小端二进制，一条记录一次发送。
 *
 * 与观测通道（src/observation/wire.c）同一套做法与同一套理由：显式逐字节编解码，
 * 不 memcpy 结构体；头里带 version 与 record_bytes，解释不了就整体拒。这两点是
 * "旧客户端静默读出错位数字"的唯一防线，而错位的目标值比重连危险得多。
 *
 * 与观测通道的差别在两处，都是被语义逼出来的：
 *   - 方向相反。观测是主站往外吐，设点是外部控制器往里送。
 *   - 记录里带 seq 与 client_time_ns，由客户端填。丢包、重放、目标有多旧因此成为
 *     可判的事实，而不是靠"客户端大概多少赫兹"去猜——协议频率会变，不能写死。
 *
 * 单位与观测通道同口径：位置、速度、力矩都是**原始计数**，换算由上层做；
 * kp / kd 主站不解释，原样存下。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 线格式版本。头**字段**的任何增删改都必须 +1。 */
#define EMASTER_SETPOINT_WIRE_VERSION 1U

/* 记录魔数。防止把观测帧或命令口的文本当成设点记录解。 */
#define EMASTER_SETPOINT_WIRE_MAGIC_0 ((uint8_t)'E')
#define EMASTER_SETPOINT_WIRE_MAGIC_1 ((uint8_t)'S')

/* 单条记录能带的轴数上限。与外部目标缓冲区同值，两条入口写的是同一个汇点。 */
#define EMASTER_SETPOINT_WIRE_MAX_AXES 16U

/* 头与每条轴条目的字节数。 */
#define EMASTER_SETPOINT_WIRE_HEADER_BYTES 24U
#define EMASTER_SETPOINT_WIRE_AXIS_BYTES 24U

/* 轴数取上限时一条记录的字节数。接收缓冲按它 +1 定长，见 server.h。 */
#define EMASTER_SETPOINT_WIRE_MAX_RECORD_BYTES                          \
    (EMASTER_SETPOINT_WIRE_HEADER_BYTES +                               \
     (EMASTER_SETPOINT_WIRE_MAX_AXES * EMASTER_SETPOINT_WIRE_AXIS_BYTES))

/*
 * 一条记录被拒的原因。四条都落在"这条记录按当前部署解释不通"上，因此与记录格式放在
 * 一起：前两条由编解码判，后两条由服务端按部署核对。
 *
 * 取值进报告。文本由 emaster_setpoint_reject_text() 给，只写一份——服务端的 stderr
 * 与报告里的那一列必须是同一句话，否则同一件事会有两种说法。
 */
typedef enum
{
    EMASTER_SETPOINT_REJECT_NONE = 0,
    /* 收到的字节数正好是接收缓冲容量：SOCK_SEQPACKET 截断过，长度不可信。 */
    EMASTER_SETPOINT_REJECT_TRUNCATED,
    /* 魔数 / 版本 / record_bytes 与轴数不自洽：不是这个协议的客户端，或版本对不上。 */
    EMASTER_SETPOINT_REJECT_BAD_FORMAT,
    /* 记录里的轴数与本部署不符：整条拒，不做部分采纳。 */
    EMASTER_SETPOINT_REJECT_AXIS_COUNT,
    /* 某轴的 mode 与部署配置不符：同一条记录在两种模式下语义完全不同。 */
    EMASTER_SETPOINT_REJECT_MODE_MISMATCH
} emaster_setpoint_reject_t;

/* 拒收原因的可读文本。取值非法时返回一句明确的"认不出"，不返回 NULL。 */
const char *emaster_setpoint_reject_text(emaster_setpoint_reject_t reason);

/* 一条记录里的一个轴。 */
typedef struct
{
    /* 该轴的运行模式（8=CSP、9=CSV、10=CST），主站按配置逐轴核对。 */
    int8_t  mode;
    /* 保留位，主站原样存下、不解释。留给客户端自己标状态用。 */
    uint8_t flags;
    /* 目标位置（原始计数）。今天进输出路径的就是这一个。 */
    int32_t q;
    /* 以下四个今天不进输出路径：解码、校验、存进缓冲，等 LZ 文档确认 MIT 口径。 */
    int32_t dq;
    int32_t tau;
    int32_t kp;
    int32_t kd;
} emaster_setpoint_axis_t;

/* 一条解码后的记录。字段布局按对齐排过，与线上顺序无关（线上顺序见 wire.c）。 */
typedef struct
{
    uint8_t  version;
    uint16_t record_bytes;
    uint16_t axis_count;
    uint64_t sequence;
    uint64_t client_time_ns;
    emaster_setpoint_axis_t axes[EMASTER_SETPOINT_WIRE_MAX_AXES];
} emaster_setpoint_record_t;

/* 给定轴数时一条记录的字节数（含头）。超过上限时按上限算。 */
size_t emaster_setpoint_wire_record_bytes(uint16_t axis_count);

/*
 * 解一条记录。任何一处不自洽都返回 false 且不写 *record——不做部分信任：
 * 魔数、版本、record_bytes 与 axis_count 是否自洽、轴数是否在 1..上限内、
 * 实际收到的字节数是否够。
 */
bool emaster_setpoint_wire_decode(const uint8_t *buffer,
                                  size_t length,
                                  emaster_setpoint_record_t *record);

/* 编一条记录（供测试往返与将来写 C 客户端用）。容量不足返回 false 且不写 *written。 */
bool emaster_setpoint_wire_encode(const emaster_setpoint_record_t *record,
                                  uint8_t *buffer,
                                  size_t capacity,
                                  size_t *written);

#endif /* EMASTER_SETPOINT_WIRE_H */

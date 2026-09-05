#ifndef EMASTER_BUS_SOEM_SESSION_MAILBOX_H
#define EMASTER_BUS_SOEM_SESSION_MAILBOX_H

#include "soem/soem.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * 单请求邮箱：周期所有者发布请求并取回结果，后台线程使用 SOEM SDO API 等待。
 * release/acquire 将请求字段和返回数据一起发布，周期路径不等待线程锁。
 * SOEM 的周期邮箱队列负责实际协议处理，禁止另行构造 CoE 字节流。
 */
typedef struct {
    ecx_contextt *context;
    pthread_t thread;
    atomic_int state;
    atomic_bool stopping;
    bool started;
    uint32_t poll_ns;
    uint16_t slave;
    uint16_t index;
    uint8_t subindex;
    int8_t value;
    bool succeeded;
} emaster_session_mailbox_t;

bool emaster_session_mailbox_start(emaster_session_mailbox_t *mailbox, ecx_contextt *context,
                                   uint32_t cycle_ns);
bool emaster_session_mailbox_request_i8(emaster_session_mailbox_t *mailbox, uint16_t slave,
                                        uint16_t index, uint8_t subindex);
bool emaster_session_mailbox_take_i8(emaster_session_mailbox_t *mailbox, uint16_t *slave,
                                     uint16_t *index, uint8_t *subindex, int8_t *value,
                                     bool *succeeded);
/* 只由周期所有者调用；每次最多处理一个邮箱事务，不同步等待 SDO 完成。 */
void emaster_session_mailbox_service(emaster_session_mailbox_t *mailbox);
/* 周期退出后先等后台事务结束，再恢复直接邮箱模式，最后才能关闭 SOEM。 */
void emaster_session_mailbox_stop(emaster_session_mailbox_t *mailbox);

#endif

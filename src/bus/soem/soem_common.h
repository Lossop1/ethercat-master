#ifndef EMASTER_SOEM_COMMON_H
#define EMASTER_SOEM_COMMON_H

#include "emaster/protocol/pdo_layout.h"

#include "soem/soem.h"

#include <stdbool.h>
#include <stdint.h>

/* SOEM 访问只在 src/bus/soem 内部共享；该头文件不属于公共主站接口。 */
typedef struct
{
    ecx_contextt *context;
    uint16_t slave;
} emaster_soem_sdo_reader_context_t;

/* 请求全体从站恢复 INIT，并返回状态确认结果。 */
bool emaster_soem_restore_init(ecx_contextt *context);

/* 在打开 SOEM 原始套接字前检查物理接口是否已检测到链路。 */
bool emaster_soem_interface_carrier(const char *interface_name);

/* 等待 SOEM 发现后的全体从站进入 PRE-OP。 */
bool emaster_soem_wait_preop(ecx_contextt *context);

/* 以精确字节宽度读取 CoE 数值对象，自动处理 EtherCAT 字节序。 */
bool emaster_soem_read_u8(void *user_data, uint16_t index, uint8_t subindex,
                          uint8_t *value);
bool emaster_soem_read_u16(void *user_data, uint16_t index, uint8_t subindex,
                           uint16_t *value);
bool emaster_soem_read_u32(void *user_data, uint16_t index, uint8_t subindex,
                           uint32_t *value);

/* 发现指定从站当前生效的完整 PDO 布局；调用者负责析构结果。 */
bool emaster_soem_discover_pdo_layout(ecx_contextt *context, uint16_t slave,
                                      emaster_pdo_layout_t *layout);

#endif

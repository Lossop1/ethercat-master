#ifndef EMASTER_BUS_SOEM_SESSION_OBSERVER_H
#define EMASTER_BUS_SOEM_SESSION_OBSERVER_H

#include "cia_process_image.h"
#include "emaster/bus/control_session.h"
#include "soem_common.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * 观察器只读取会话事实并写入报告，不决定状态转换、运动目标或失败后的安全动作。
 * 所有 SDO 都通过统一 SOEM 访问器执行，因此不会绕过运行审计。
 */
void emaster_session_observer_read_drive(
    emaster_soem_sdo_reader_context_t *reader,
    emaster_drive_diagnostic_t *diagnostic);
void emaster_session_observer_read_sync(
    emaster_soem_sdo_reader_context_t *reader,
    uint16_t index,
    emaster_sync_diagnostic_t *diagnostic);
void emaster_session_observer_read_position_scale(
    emaster_soem_sdo_reader_context_t *reader,
    emaster_position_scale_t *scale);
bool emaster_session_observer_read_configured_sdo(
    emaster_soem_sdo_reader_context_t *reader,
    const emaster_sdo_read_config_t *command);

/* 读取 ESC 寄存器并把原始字节、解码值和成功状态写入同一运行审计。 */
bool emaster_session_observer_read_dc_register(
    ecx_contextt *context,
    uint16_t slave,
    uint16_t address,
    void *value,
    uint16_t size,
    emaster_run_audit_t *audit,
    emaster_audit_phase_t phase,
    uint64_t exchange);

/* 有限运动会话在周期开始前一次性准备全部观察记录空间。 */
bool emaster_session_observer_prepare_audit(
    const emaster_session_plan_t *plan,
    const emaster_cia_process_image_t *runtime,
    uint64_t transition_cycles,
    emaster_run_audit_t *audit);

#endif

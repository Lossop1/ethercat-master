/* 回合测试：扁平文件 -> runtime apply -> dump，键空间与输入一致，可直接比对。 */
#include "emaster/config/config_reader.h"
#include "emaster/config/generated_runtime.h"
#include "emaster/config/runtime_config.h"

#include <stdio.h>

bool emaster_generated_runtime_apply(const emaster_config_reader_t *reader);

int main(int argc, char **argv)
{
    emaster_config_reader_t reader;
    size_t oi, mi, fi, wi, ri, ai;

    if (argc != 2) { fprintf(stderr, "用法: %s <扁平配置>\n", argv[0]); return 2; }
    if (!emaster_config_reader_open(&reader, argv[1])) { fprintf(stderr, "打不开\n"); return 2; }
    if (!emaster_generated_runtime_apply(&reader)) { fprintf(stderr, "apply 失败\n"); return 1; }

    printf("operation.count\t%zu\n", emaster_operation_profile_count());
    for (oi = 0U; oi < emaster_operation_profile_count(); ++oi)
    {
        const emaster_operation_profile_t *o = emaster_operation_profile_at(oi);
        printf("operation.%zu.id\t%s\n", oi, o->profile_id);
        printf("operation.%zu.approval\t%d\n", oi, (int)o->approval);
        printf("operation.%zu.device_profile_id\t%s\n", oi, o->device_profile_id);
        printf("operation.%zu.pdo_set_id\t%s\n", oi, o->pdo_set_id);
        printf("operation.%zu.strategy\t%d\n", oi, (int)o->sync_strategy);
        printf("operation.%zu.assign_present\t%s\n", oi, o->has_assign_activate ? "true" : "false");
        printf("operation.%zu.assign_activate\t0x%08X\n", oi, o->assign_activate);
        printf("operation.%zu.cycle_present\t%s\n", oi, o->has_cycle_ns ? "true" : "false");
        printf("operation.%zu.cycle_ns\t%u\n", oi, o->cycle_ns);
        printf("operation.%zu.phase_present\t%s\n", oi, o->has_process_data_phase_ns ? "true" : "false");
        printf("operation.%zu.process_data_phase_ns\t%u\n", oi, o->process_data_phase_ns);
        printf("operation.%zu.startup_present\t%s\n", oi, o->has_dc_startup_cycles ? "true" : "false");
        printf("operation.%zu.dc_startup_cycles\t%u\n", oi, o->dc_startup_cycles);
        printf("operation.%zu.shift_present\t%s\n", oi, o->has_sync0_shift_ns ? "true" : "false");
        printf("operation.%zu.shift_ns\t%d\n", oi, (int)o->sync0_shift_ns);
        printf("operation.%zu.sm2_present\t%s\n", oi, o->has_sm2_sync_type ? "true" : "false");
        printf("operation.%zu.sm2_type\t0x%04X\n", oi, (unsigned)o->sm2_sync_type);
        printf("operation.%zu.sm3_present\t%s\n", oi, o->has_sm3_sync_type ? "true" : "false");
        printf("operation.%zu.sm3_type\t0x%04X\n", oi, (unsigned)o->sm3_sync_type);
        printf("operation.%zu.selected_mode_id_present\t%s\n", oi, o->selected_mode_id != NULL ? "true" : "false");
        if (o->selected_mode_id != NULL) { printf("operation.%zu.selected_mode_id\t%s\n", oi, o->selected_mode_id); }
        printf("operation.%zu.modes.count\t%zu\n", oi, o->mode_count);
        for (mi = 0U; mi < o->mode_count; ++mi)
        {
            const emaster_operation_mode_t *m = &o->modes[mi];
            printf("operation.%zu.modes.%zu.id\t%s\n", oi, mi, m->mode_id);
            printf("operation.%zu.modes.%zu.value\t%d\n", oi, mi, (int)m->value);
            printf("operation.%zu.modes.%zu.display\t%d\n", oi, mi, (int)m->mode_display_policy);
            printf("operation.%zu.modes.%zu.rx_fields.count\t%zu\n", oi, mi, m->required_rx_field_count);
            printf("operation.%zu.modes.%zu.tx_fields.count\t%zu\n", oi, mi, m->required_tx_field_count);
            for (fi = 0U; fi < m->required_rx_field_count; ++fi) { printf("operation.%zu.modes.%zu.rx_fields.%zu\t%s\n", oi, mi, fi, m->required_rx_fields[fi]); }
            for (fi = 0U; fi < m->required_tx_field_count; ++fi) { printf("operation.%zu.modes.%zu.tx_fields.%zu\t%s\n", oi, mi, fi, m->required_tx_fields[fi]); }
            printf("operation.%zu.modes.%zu.sdo_writes.count\t%zu\n", oi, mi, m->safeop_to_op_sdo_write_count);
            for (wi = 0U; wi < m->safeop_to_op_sdo_write_count; ++wi)
            {
                printf("operation.%zu.modes.%zu.sdo_writes.%zu.index\t0x%04X\n", oi, mi, wi, (unsigned)m->safeop_to_op_sdo_writes[wi].index);
                printf("operation.%zu.modes.%zu.sdo_writes.%zu.subindex\t%u\n", oi, mi, wi, (unsigned)m->safeop_to_op_sdo_writes[wi].subindex);
                printf("operation.%zu.modes.%zu.sdo_writes.%zu.type\t%d\n", oi, mi, wi, (int)m->safeop_to_op_sdo_writes[wi].type);
                printf("operation.%zu.modes.%zu.sdo_writes.%zu.value\t%lld\n", oi, mi, wi, (long long)m->safeop_to_op_sdo_writes[wi].value);
            }
            printf("operation.%zu.modes.%zu.sdo_reads.count\t%zu\n", oi, mi, m->final_sdo_read_count);
            for (ri = 0U; ri < m->final_sdo_read_count; ++ri)
            {
                printf("operation.%zu.modes.%zu.sdo_reads.%zu.name\t%s\n", oi, mi, ri, m->final_sdo_reads[ri].name);
                printf("operation.%zu.modes.%zu.sdo_reads.%zu.index\t0x%04X\n", oi, mi, ri, (unsigned)m->final_sdo_reads[ri].index);
                printf("operation.%zu.modes.%zu.sdo_reads.%zu.subindex\t%u\n", oi, mi, ri, (unsigned)m->final_sdo_reads[ri].subindex);
                printf("operation.%zu.modes.%zu.sdo_reads.%zu.type\t%d\n", oi, mi, ri, (int)m->final_sdo_reads[ri].type);
            }
        }
    }
    printf("motion.count\t%zu\n", emaster_motion_profile_count());
    for (mi = 0U; mi < emaster_motion_profile_count(); ++mi)
    {
        const emaster_motion_profile_t *p = emaster_motion_profile_at(mi);
        printf("motion.%zu.id\t%s\n", mi, p->motion_profile_id);
        printf("motion.%zu.approval\t%d\n", mi, (int)p->approval);
        printf("motion.%zu.required_mode_id\t%s\n", mi, p->required_mode_id);
        printf("motion.%zu.trajectory\t%d\n", mi, (int)p->trajectory);
        printf("motion.%zu.coordinate\t%d\n", mi, (int)p->coordinate);
        printf("motion.%zu.duration_ms\t%u\n", mi, p->duration_ms);
        printf("motion.%zu.settle_ms\t%u\n", mi, p->settle_ms);
        printf("motion.%zu.axes.count\t%zu\n", mi, p->axis_count);
        for (ai = 0U; ai < p->axis_count; ++ai)
        {
            const emaster_motion_axis_config_t *a = &p->axes[ai];
            printf("motion.%zu.axes.%zu.axis_id\t%s\n", mi, ai, a->axis_id);
            printf("motion.%zu.axes.%zu.relative_angle\t%d\n", mi, ai, (int)a->relative_angle_millidegrees);
            printf("motion.%zu.axes.%zu.target_velocity\t%d\n", mi, ai, (int)a->target_velocity_millidegrees_per_second);
            printf("motion.%zu.axes.%zu.acceleration\t%u\n", mi, ai, a->acceleration_millidegrees_per_second2);
            printf("motion.%zu.axes.%zu.deceleration\t%u\n", mi, ai, a->deceleration_millidegrees_per_second2);
            printf("motion.%zu.axes.%zu.following_error\t%u\n", mi, ai, a->max_following_error_millidegrees);
            printf("motion.%zu.axes.%zu.max_step\t%u\n", mi, ai, a->max_step_millidegrees);
            printf("motion.%zu.axes.%zu.velocity_error\t%u\n", mi, ai, a->max_velocity_error_millidegrees_per_second);
            printf("motion.%zu.axes.%zu.encoder_increments\t%u\n", mi, ai, a->expected_encoder_increments);
            printf("motion.%zu.axes.%zu.encoder_motor_revolutions\t%u\n", mi, ai, a->expected_encoder_motor_revolutions);
            printf("motion.%zu.axes.%zu.gear_motor_revolutions\t%u\n", mi, ai, a->expected_gear_motor_revolutions);
            printf("motion.%zu.axes.%zu.gear_shaft_revolutions\t%u\n", mi, ai, a->expected_gear_shaft_revolutions);
        }
    }
    emaster_config_reader_close(&reader);
    return 0;
}

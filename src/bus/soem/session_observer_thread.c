#include "session_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * P4.3: SDO 慢速观测线程
 *
 * 架构：独立非 RT 线程，50ms 轮询周期
 * 功能：对每个从站依次读取：
 *   - 6078h: 电流实际值 (int16)
 *   - 603Fh: CiA402 错误码 (uint16) —— P2.6
 *   - 6079h: 母线电压 (uint32, mV)
 *   - 200Bh:01h: MOSFET 温度 (int16, 0.1°C)
 *   - 200Bh:02h: 电机温度 (int16, 0.1°C)
 * 数据存储：写入 session->axes[].sdo_* 和 drive_diagnostic
 * 线程安全：使用 observer_mutex 保护共享数据
 */

static void *observer_thread_func(void *arg)
{
    emaster_soem_session_t *session = (emaster_soem_session_t *)arg;
    struct timespec interval = {0, 50000000}; /* 50ms */

    fprintf(stderr, "[P4.3] SDO 观测线程已启动\n");
    fprintf(stderr, "[P4.3] session=%p\n", (void*)session);
    fprintf(stderr, "[P4.3] observer_running=%d\n", session->observer_running);

    if (session->plan == NULL)
    {
        fprintf(stderr, "[P4.3] 错误：session->plan 为 NULL，线程退出\n");
        return NULL;
    }

    fprintf(stderr, "[P4.3] axis_count=%zu\n", session->plan->axis_count);
    fflush(stderr);

    while (session->observer_running)
    {
        fprintf(stderr, "[P4.3] 循环迭代开始\n");
        fflush(stderr);

        for (size_t axis = 0U; axis < session->plan->axis_count; ++axis)
        {
            int16_t current_6078h = 0;
            uint16_t error_code_603f = 0;
            uint32_t voltage_6079h = 0;
            int16_t mosfet_temp_200b01h = 0;
            int16_t motor_temp_200b02h = 0;
            int wkc_current = 0;
            int wkc_error = 0;
            int wkc_voltage = 0;
            int wkc_mosfet_temp = 0;
            int wkc_motor_temp = 0;
            uint16_t slave_position = (uint16_t)(axis + 1U);

            /* 读取 6078h: 电流实际值 */
            int size_current = (int)sizeof(current_6078h);
            wkc_current = ecx_SDOread(&session->context, slave_position, 0x6078U, 0x00U,
                             FALSE, &size_current, &current_6078h,
                             EC_TIMEOUTRXM);

            /* P2.6: 读取 603Fh: CiA402 错误码 */
            int size_error = (int)sizeof(error_code_603f);
            wkc_error = ecx_SDOread(&session->context, slave_position, 0x603FU, 0x00U,
                             FALSE, &size_error, &error_code_603f,
                             EC_TIMEOUTRXM);

            /* 读取 6079h: 母线电压 (mV) */
            int size_voltage = (int)sizeof(voltage_6079h);
            wkc_voltage = ecx_SDOread(&session->context, slave_position, 0x6079U, 0x00U,
                             FALSE, &size_voltage, &voltage_6079h,
                             EC_TIMEOUTRXM);

            /* 读取 200Bh:01h: MOSFET 温度 (0.1°C) */
            int size_mosfet_temp = (int)sizeof(mosfet_temp_200b01h);
            wkc_mosfet_temp = ecx_SDOread(&session->context, slave_position, 0x200BU, 0x01U,
                             FALSE, &size_mosfet_temp, &mosfet_temp_200b01h,
                             EC_TIMEOUTRXM);

            /* 读取 200Bh:02h: 电机温度 (0.1°C) */
            int size_motor_temp = (int)sizeof(motor_temp_200b02h);
            wkc_motor_temp = ecx_SDOread(&session->context, slave_position, 0x200BU, 0x02U,
                             FALSE, &size_motor_temp, &motor_temp_200b02h,
                             EC_TIMEOUTRXM);

            /* 前3次读取始终打印以验证通道工作 */
            if (session->axes[axis].sdo_read_count < 3U)
            {
                fprintf(stderr, "[P4.3] 轴%zu 第%lu次读取:\n"
                       "  6078h(电流) wkc=%d val=%d\n"
                       "  603Fh(错误) wkc=%d val=0x%04X\n"
                       "  6079h(电压) wkc=%d val=%u mV\n"
                       "  200Bh:01h(MOSFET温度) wkc=%d val=%d (%.1f°C)\n"
                       "  200Bh:02h(电机温度) wkc=%d val=%d (%.1f°C)\n",
                       axis, (unsigned long)(session->axes[axis].sdo_read_count + 1U),
                       wkc_current, current_6078h,
                       wkc_error, error_code_603f,
                       wkc_voltage, voltage_6079h,
                       wkc_mosfet_temp, mosfet_temp_200b01h, mosfet_temp_200b01h / 10.0,
                       wkc_motor_temp, motor_temp_200b02h, motor_temp_200b02h / 10.0);
            }

            /* 加锁写入结果 */
            pthread_mutex_lock(&session->observer_mutex);
            if (wkc_current > 0)
            {
                session->axes[axis].sdo_current_read = true;
                session->axes[axis].sdo_current_6078h = current_6078h;
            }
            else
            {
                session->axes[axis].sdo_current_read = false;
            }

            if (wkc_voltage > 0)
            {
                session->axes[axis].sdo_voltage_read = true;
                session->axes[axis].sdo_voltage_6079h = voltage_6079h;
            }
            else
            {
                session->axes[axis].sdo_voltage_read = false;
            }

            if (wkc_mosfet_temp > 0)
            {
                session->axes[axis].sdo_mosfet_temp_read = true;
                session->axes[axis].sdo_mosfet_temp_200b01h = mosfet_temp_200b01h;
            }
            else
            {
                session->axes[axis].sdo_mosfet_temp_read = false;
            }

            if (wkc_motor_temp > 0)
            {
                session->axes[axis].sdo_motor_temp_read = true;
                session->axes[axis].sdo_motor_temp_200b02h = motor_temp_200b02h;
            }
            else
            {
                session->axes[axis].sdo_motor_temp_read = false;
            }

            /* P2.6: 更新运行期 603F 错误码 */
            if (wkc_error > 0)
            {
                session->axes[axis].drive_diagnostic.cia402_error_code = error_code_603f;
                /* 如果错误码非零，打印警告 */
                if (error_code_603f != 0U && session->axes[axis].sdo_read_count < 3U)
                {
                    fprintf(stderr, "[P2.6] 警告：轴%zu 检测到 CiA402 错误码 0x%04X\n",
                           axis, error_code_603f);
                }
            }

            session->axes[axis].sdo_read_count++;

            /* 每20次读取打印一次（约1秒间隔） */
            if (session->axes[axis].sdo_read_count % 20U == 0U)
            {
                fprintf(stderr, "[P4.3] 轴%zu: 电流=%d, 错误=0x%04X, 电压=%umV, MOSFET=%.1f°C, 电机=%.1f°C (读取次数=%lu)\n",
                       axis, current_6078h, error_code_603f, voltage_6079h,
                       mosfet_temp_200b01h / 10.0, motor_temp_200b02h / 10.0,
                       (unsigned long)session->axes[axis].sdo_read_count);
            }
            pthread_mutex_unlock(&session->observer_mutex);
        }

        nanosleep(&interval, NULL);
    }

    fprintf(stderr, "[P4.3] SDO 观测线程已退出\n");
    return NULL;
}

bool emaster_soem_session_start_observer(emaster_soem_session_t *session)
{
    if (pthread_mutex_init(&session->observer_mutex, NULL) != 0)
    {
        return false;
    }

    session->observer_running = true;

    if (pthread_create(&session->observer_thread, NULL, observer_thread_func, session) != 0)
    {
        session->observer_running = false;
        pthread_mutex_destroy(&session->observer_mutex);
        return false;
    }

    session->observer_thread_created = true;
    return true;
}

void emaster_soem_session_stop_observer(emaster_soem_session_t *session)
{
    if (!session->observer_thread_created)
    {
        return;
    }

    session->observer_running = false;
    pthread_join(session->observer_thread, NULL);
    pthread_mutex_destroy(&session->observer_mutex);
    session->observer_thread_created = false;
}

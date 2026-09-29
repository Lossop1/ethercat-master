/*
 * 总线扫描：只做一次 ecx_config_init，然后逐个打印从站在环上的身份与状态。
 *
 * 存在的理由：主站那些工具都要求"先有一份声明了轴数与身份的部署档"，于是最前面的
 * 那个问题——线接好了没有、到底扫到几台——反而没有工具能回答。接错一根线时主站报的
 * 是"身份或 PDO 映射与部署配置不一致"，而"环上到底有几台、不一致的是哪一台"它不说。
 *
 * 本工具不读部署档、不发过程数据、不进 OP：扫描完立刻 ecx_close。对台架是只读的，
 * 可以在每次改完接线之后先跑一次。
 *
 * 用法:
 *   emaster-scan <接口>        例: emaster-scan enp49s0
 *   不带参数时只列出可用的网络适配器。
 *
 * 输出每行一台，k=v 形式，直接与部署档的 identity 三件套核对。
 */

#define _POSIX_C_SOURCE 200809L

/* 这个 fork 的伞头文件是 soem/soem.h，没有上游那个 soem/ethercat.h；
 * 而且它只提供 context 版 API（ecx_*），没有 ec_init／ec_slave 那一套。 */
#include "soem/soem.h"

#include <stdio.h>

int main(int argc, char *argv[])
{
    ecx_contextt context;

    if (argc < 2)
    {
        ec_adaptert *adapter = ec_find_adapters();

        fprintf(stderr, "用法: %s <接口>\n\n可用的网络适配器:\n", argv[0]);
        for (ec_adaptert *entry = adapter; entry != NULL; entry = entry->next)
        {
            fprintf(stderr, "  %s\t%s\n", entry->name, entry->desc);
        }
        ec_free_adapters(adapter);
        return 1;
    }

    printf("interface=%s\n", argv[1]);
    if (!ecx_init(&context, argv[1]))
    {
        fprintf(stderr,
                "错误：ecx_init 失败，接口 %s 打不开（网卡名对不对？网卡 up 了吗？）\n",
                argv[1]);
        return 1;
    }

    const int slave_count = ecx_config_init(&context);
    if (slave_count <= 0)
    {
        fprintf(stderr, "错误：一台从站都没扫到（slavecount=%d）\n", context.slavecount);
        ecx_close(&context);
        return 1;
    }

    printf("slaves=%d\n", slave_count);
    for (int index = 1; index <= context.slavecount; ++index)
    {
        const ec_slavet *slave = &context.slavelist[index];

        /*
         * state 是这一拍读回来的 AL 状态：1=INIT、2=PRE-OP、4=SAFE-OP、8=OP。
         * 刚 ecx_config_init 完的正常值是 2；出现别的值说明那一台有自己的问题，
         * 而主站启动日志里"AL state="那一栏印的就是它。
         * vendor/product/revision 三件套是身份核对用的，与部署档里的 identity 对得上
         * 才说明"装的是它"。
         */
        printf("slave=%d name=%s vendor=0x%08X product=0x%08X revision=0x%08X "
               "state=%u al_status=0x%04X dc=%d coe=%d mbx_rl=%u\n",
               index, slave->name, (unsigned)slave->eep_man, (unsigned)slave->eep_id,
               (unsigned)slave->eep_rev, (unsigned)slave->state,
               (unsigned)slave->ALstatuscode, slave->hasdc != 0U ? 1 : 0,
               (slave->mbx_proto & ECT_MBXPROT_COE) != 0U ? 1 : 0,
               (unsigned)slave->mbx_rl);
    }

    ecx_close(&context);
    return 0;
}

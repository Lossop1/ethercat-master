#include <stdio.h>
#include <string.h>
#include "soem/ethercat.h"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <interface>\n", argv[0]);
        return 1;
    }

    ec_adaptert *adapter = NULL;
    printf("扫描网络适配器...\n");
    adapter = ec_find_adapters();
    while (adapter != NULL) {
        printf("  - %s / %s\n", adapter->name, adapter->desc);
        adapter = adapter->next;
    }
    ec_free_adapters(adapter);

    char IOmap[4096];
    int expectedWKC;
    boolean needlf;
    volatile int wkc;
    boolean inOP;
    uint8 currentgroup = 0;

    printf("\n初始化 EtherCAT 主站 (接口: %s)...\n", argv[1]);
    if (ec_init(argv[1]))
    {
        printf("✓ ec_init 成功\n");

        printf("扫描从站...\n");
        if (ec_config_init(FALSE) > 0)
        {
            printf("✓ 检测到 %d 个从站\n", ec_slavecount);

            for (int i = 1; i <= ec_slavecount; i++)
            {
                printf("\n从站 %d:\n", i);
                printf("  名称: %s\n", ec_slave[i].name);
                printf("  Vendor ID: 0x%08x\n", ec_slave[i].eep_man);
                printf("  Product Code: 0x%08x\n", ec_slave[i].eep_id);
                printf("  Revision: 0x%08x\n", ec_slave[i].eep_rev);
                printf("  状态: 0x%04x\n", ec_slave[i].state);
                printf("  AL Status Code: 0x%04x\n", ec_slave[i].ALstatuscode);
                printf("  Has DC: %s\n", ec_slave[i].hasdc ? "是" : "否");
            }
        }
        else
        {
            printf("✗ 未检测到从站 (ec_slavecount = %d)\n", ec_slavecount);
        }

        ec_close();
    }
    else
    {
        printf("✗ ec_init 失败\n");
    }

    return 0;
}

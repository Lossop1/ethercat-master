#!/bin/bash
# 最小 SOEM 测试脚本

cat > /tmp/test_soem.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 直接声明 SOEM 函数（避免头文件路径问题）
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef int boolean;

typedef struct {
    char *name;
    char *desc;
    void *next;
} ec_adaptert;

extern int ec_init(const char *ifname);
extern int ec_config_init(uint8 usetable);
extern void ec_close(void);
extern int ec_slavecount;
extern ec_adaptert *ec_find_adapters(void);
extern void ec_free_adapters(ec_adaptert *adapter);

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <interface>\n", argv[0]);
        return 1;
    }

    printf("初始化 EtherCAT (接口: %s)...\n", argv[1]);
    if (ec_init(argv[1])) {
        printf("✓ ec_init 成功\n");

        printf("扫描从站...\n");
        int wkc = ec_config_init(0);
        printf("ec_config_init 返回: %d\n", wkc);
        printf("ec_slavecount: %d\n", ec_slavecount);

        if (ec_slavecount > 0) {
            printf("✓ 检测到 %d 个从站\n", ec_slavecount);
        } else {
            printf("✗ 未检测到从站\n");
        }

        ec_close();
    } else {
        printf("✗ ec_init 失败\n");
        return 1;
    }

    return 0;
}
EOF

gcc -o /tmp/test_soem /tmp/test_soem.c -L$HOME/ethercat-master/build/external/SOEM -lsoem -lpthread
if [ $? -eq 0 ]; then
    echo "编译成功，运行测试..."
    sudo /tmp/test_soem enp49s0
else
    echo "编译失败"
    exit 1
fi

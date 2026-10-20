/*
 * 运行时配置的加载入口。
 *
 * 加载什么
 *     --config-dir 指向的目录下四个扁平文件：
 *       topologies.flat   拓扑
 *       catalog.flat      设备目录（从站事实）
 *       runtime.flat      运行方案与运动方案
 *       deployments.flat  部署
 *     顺序不能颠倒：部署按 ID 引用前三者，必须最后加载。
 *
 * 生命周期
 *     **读取器常驻，直到进程结束都不释放。** 填进结构体的每个字符串都指向读取器
 *     那块缓冲，关掉读取器就等于把配置里所有字符串变成悬垂指针。这不是使用习惯，
 *     是接口契约——第一版测试里提前 close，表现是"引用了不存在的 ID"，而真正的
 *     原因是被引用的内存已经没了。
 */

#ifndef EMASTER_CONFIG_RUNTIME_CONFIG_LOAD_H
#define EMASTER_CONFIG_RUNTIME_CONFIG_LOAD_H

#include <stdbool.h>
#include <stdint.h>

/* 从目录加载全部运行时配置。成功返回 true；失败时把具体是哪个文件、哪一步失败
 * 打到 stderr。重复调用只有第一次真正加载。 */
bool emaster_runtime_config_load(const char *directory);

/*
 * 本次成功加载的四个文件的**内容指纹**（FNV-1a 64 位）。
 *
 * 用途只有一个：让一份报告能说明"它是哪份配置跑出来的"。现场比对时不必再靠人记
 * 版本号；四个文件里改一个字节，指纹就变。按下表顺序把文件名与内容依次喂进哈希，
 * 所以"把内容从一个文件挪到另一个文件"也会改变结果。
 *
 *   topologies.flat → catalog.flat → runtime.flat → deployments.flat
 *
 * 它不是权限校验，也不是防篡改：能改配置文件的人也能改这份代码。它只做身份标识。
 *
 * 返回 false 表示没有成功加载过配置（指纹无意义）。不能用 0 兼任这个含义 ——
 * 0 是一个合法的哈希值，把"没加载"读成"某个指纹"正是本仓库反复踩过的坑。
 */
bool emaster_runtime_config_fingerprint(uint64_t *fingerprint);

#endif /* EMASTER_CONFIG_RUNTIME_CONFIG_LOAD_H */

#ifndef EMASTER_THREAD_SCHED_H
#define EMASTER_THREAD_SCHED_H

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/*
 * 让当前线程退出实时调度：**除了控制周期线程，主站里没有第二个线程该是实时的。**
 *
 * 为什么非要显式做一次。主线程在进 OP 之前把自己升成 SCHED_FIFO、绑上核
 * （tools/master/main.c 的 install_rt_primitives），而 Linux 上 pthread_create
 * 新建的线程**默认继承创建者的调度策略与优先级**——glibc 的默认属性是
 * PTHREAD_INHERIT_SCHED，亲和掩码同理继承。于是观测线程、命令监听线程、观测服务
 * 线程全都跟着变成了 SCHED_FIFO、掩码 {11}，没有一个例外；而它们各自的设计声明
 * 都是"非实时"（见 session_observer_thread.c 顶部「架构：独立非 RT 线程」、
 * observation/server.c 顶部「观测通道一旦能阻塞，它就多了一条把非实时负载传导到
 * [周期线程]」的路）。**代码与它自己的设计说明不一致，方向是错的那一边。**
 *
 * 坐实它的是内核而不是自述：2026-09-28 从台架报告里读 /proc 的实况
 * （thread_schedstat 的 Cpus_allowed_list 与 /proc/self/task/<tid>/sched），
 * 9月16 的四轴报告与 9月28 的五轴报告逐字一致——三个线程 policy 全是 1
 * （SCHED_FIFO）、priority 全是 19（内核存法是 99 − 80，见 control_session.h:633）、
 * cpus_allowed 全是 "11"。不是偶发，是结构性继承。
 *
 * 为什么这不能忍。同一个核上、**同一个优先级**的 SCHED_FIFO 线程之间不抢占：
 * 周期线程不会因为是周期线程就先跑，只能等正在跑的那个让出来。任何一个管理线程
 * 走进一段不阻塞的执行（解析一条命令、写一行日志、拷一段数组），周期线程就整段
 * 被挡住。这就是「管理面一活动、截止期就错过」的机制，报告里那个 wait_ns 字段
 * 量的正是它（session_shutdown.c:474 的注释：「三个同优先级 SCHED_FIFO 线程、
 * 亲和掩码同为 11……任何一个长时间占核都会把周期线程整段挡住」）。
 * 降到 SCHED_OTHER 之后周期线程永远抢得过它们，这条路就断了。
 *
 * 亲和掩码**故意不动**：留在这个核上有好处——周期线程只占这个核不到 1%，
 * 管理线程正好吃剩下的空档，还省掉跨核迁移与共享缓冲的缓存失效。要挡的是"抢"，
 * 不是"同核"。所以这个函数只改调度策略，不碰掩码。
 *
 * 用法：每个非周期线程在自己的入口函数里第一件事调一次，参数是给日志看的名字。
 * 失败**不中止**：线程照常干活，只是没摆脱竞争，比整个会话起不来轻得多；这一行
 * 会说明它没摆脱。要判断"到底摆脱没有"，别信这一行，看报告里 thread_schedstat
 * 的 policy——那是内核的实况。
 *
 * 新增线程时别忘了调它：scripts/checks/master_thread_sched_check.py 盯着这件事。
 */
static inline bool emaster_thread_leave_realtime(const char *who)
{
    struct sched_param param;

    memset(&param, 0, sizeof(param));
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &param) == 0)
    {
        return true;
    }

    fprintf(stderr, "[RT] %s 未能退出实时调度：%s（它仍会与周期线程抢核）\n",
            who != NULL ? who : "线程", strerror(errno));
    return false;
}

#endif /* EMASTER_THREAD_SCHED_H */

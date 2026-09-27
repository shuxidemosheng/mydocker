/*
 * spin.c —— CPU 压力测试：验证 cgroup v2 cpu.max 的限流
 *
 * 用法: spin [秒数，默认3]
 * 单线程死循环 N 秒，报告"每秒迭代次数"。迭代速度与可用 CPU 时间成正比：
 * 不限流约等于机器主频水平；--cpu 30 时应降至 ~30%（CFS 配额节流）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 3;

    for (int s = 0; s < secs; s++) {
        volatile unsigned long x = 0;   /* volatile 防止编译器把空循环优化掉 */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        do {
            for (int i = 0; i < 1000000; i++) x += i;
            clock_gettime(CLOCK_MONOTONIC, &t1);
        } while (t1.tv_sec - t0.tv_sec < 1);
        printf("second %d: %lu M-iterations\n", s + 1, x / 1000000);
    }
    return 0;
}

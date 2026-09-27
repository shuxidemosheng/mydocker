/*
 * memhog.c —— 内存压力测试：验证 cgroup v2 memory.max 的 OOM kill
 *
 * 用法: memhog [步进MB，默认10]
 * 每步分配 10MB 并逐页写满（memset 触发缺页，物理内存才真正被占用），
 * 直到被 OOM killer 杀死或分配失败。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    size_t step = 10 * 1024 * 1024;     /* 每步 10MB */
    if (argc > 1) step = (size_t)atoi(argv[1]) * 1024 * 1024;

    size_t total = 0;
    for (;;) {
        char *p = malloc(step);
        if (!p) { printf("malloc failed at %zu MB\n", total >> 20); return 0; }
        memset(p, 1, step);             /* 逐页写，缺页中断后内核才分配物理页 */
        total += step;
        printf("resident ~%zu MB\n", total >> 20);
        fflush(stdout);
    }
}

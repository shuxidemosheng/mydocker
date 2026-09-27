/*
 * main.c —— mydocker 命令行入口（阶段 5 完整子命令分发）
 *
 *   mydocker run    [--user] [--hostname H] [--rootfs DIR] [--overlay]
 *                   [--memory 100m] [--cpu 50] [--net]     创建并运行容器
 *   mydocker ps                                             列出运行中的容器
 *   mydocker exec <pid>                                     进入容器（等价 nsenter）
 *   mydocker rm <pid>                                       终止并清理容器
 */
#define _GNU_SOURCE                     /* CLONE_NEWxxx 常量是 GNU 扩展 */
#include "mydocker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

static void usage(void)
{
    fprintf(stderr,
        "用法: mydocker <子命令> [参数]\n"
        "  run    [--user] [--hostname 名字] [--rootfs 目录] [--overlay]\n"
        "         [--memory 100m] [--cpu 50] [--net]\n"
        "  ps     列出运行中的容器\n"
        "  exec   <pid>          进入容器（setns 到各 Namespace）\n"
        "  rm     <pid>          终止并清理容器\n");
}

/* "100m" -> 字节数；支持无后缀 / k / m / g（不区分大小写） */
static long parse_size(const char *s)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || v <= 0) return -1;
    switch (*end | 0x20) {                  /* 统一转小写 */
    case 'k': v *= 1024L;       end++; break;
    case 'm': v *= 1024L * 1024; end++; break;
    case 'g': v *= 1024L * 1024 * 1024; end++; break;
    }
    return *end == '\0' ? v : -1;
}

/* run 子命令：解析选项 -> 组装 Namespace flags -> container_run */
static int cmd_run(int argc, char **argv)
{
    struct container_opts opts = {0};

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--user") == 0) {
            opts.use_user_ns = 1;
        } else if (strcmp(argv[i], "--hostname") == 0) {
            if (i + 1 >= argc) { usage(); return 2; }
            opts.hostname = argv[++i];
        } else if (strcmp(argv[i], "--rootfs") == 0) {
            if (i + 1 >= argc) { usage(); return 2; }
            opts.rootfs = argv[++i];
        } else if (strcmp(argv[i], "--overlay") == 0) {
            opts.use_overlay = 1;
        } else if (strcmp(argv[i], "--memory") == 0) {
            if (i + 1 >= argc) { usage(); return 2; }
            opts.memory_bytes = parse_size(argv[++i]);
            if (opts.memory_bytes <= 0) {
                fprintf(stderr, "mydocker: 非法的 --memory 值\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--cpu") == 0) {
            if (i + 1 >= argc) { usage(); return 2; }
            opts.cpu_percent = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--net") == 0) {
            opts.use_net = 1;
        } else {
            usage();
            return 2;
        }
    }

    opts.ns_flags = CLONE_NEWNS          /* 挂载表隔离 */
                  | CLONE_NEWPID         /* 进程号隔离 */
                  | CLONE_NEWUTS         /* 主机名隔离 */
                  | (opts.use_net ? CLONE_NEWNET : 0)   /* 网络栈隔离 */
                  | (opts.use_user_ns ? CLONE_NEWUSER : 0);

    if (opts.use_overlay && !opts.rootfs) {
        fprintf(stderr, "mydocker: --overlay 需要配合 --rootfs 使用\n");
        return 2;
    }
    if (opts.use_net && opts.use_user_ns) {
        fprintf(stderr, "mydocker: --net 与 --user 互斥（网桥属于宿主网络栈）\n");
        return 2;
    }
    return container_run(&opts);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }

    if (strcmp(argv[1], "run") == 0)      return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0)       return cmd_ps();
    if (strcmp(argv[1], "exec") == 0)     return cmd_exec(argc, argv);
    if (strcmp(argv[1], "rm") == 0)       return cmd_rm(argc, argv);
    usage();
    return 2;
}

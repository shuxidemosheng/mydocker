/*
 * main.c —— mydocker 的命令行入口
 *
 * 阶段 1 的用法：
 *   mydocker run [--user] [--hostname 名字]
 *
 *   --user      借助 User Namespace 免 root 运行（把宿主机 uid 映射成容器内 root）
 *   --hostname  指定容器内看到的主机名
 *
 * 默认（不带 --user）直接创建 PID/UTS/Mount Namespace，需要 sudo。
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
        "用法: mydocker run [--user] [--hostname 名字] [--rootfs 目录]\n"
        "                   [--memory 100m] [--cpu 50] [--overlay]\n"
        "  --user       免 root 模式（User Namespace + uid 映射）\n"
        "  --hostname   设置容器内主机名（默认不变）\n"
        "  --rootfs     容器根目录（scripts/build-rootfs.sh 生成）\n"
        "  --overlay    rootfs 只读 + overlayfs 可写层\n"
        "  --memory     内存限额，如 100m / 1g（cgroup v2 memory.max）\n"
        "  --cpu        CPU 百分比 1-100（cgroup v2 cpu.max）\n");
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

int main(int argc, char **argv)
{
    struct container_opts opts = {0};

    /* 简单的手写参数解析：阶段 5 会换成语义更完整的版本。
       注意循环里要先跳过子命令 "run" 本身，它不是选项。 */
    if (argc < 2 || strcmp(argv[1], "run") != 0) { usage(); return 2; }
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
        } else {
            usage();
            return 2;
        }
    }

    /* 组装 Namespace flags：阶段 1 一次性创建三种最基础的隔离。
       CLONE_NEWUSER 必须在 flags 里，uid/gid 映射才挂得上。 */
    opts.ns_flags = CLONE_NEWNS          /* 挂载表隔离 */
                  | CLONE_NEWPID         /* 进程号隔离 */
                  | CLONE_NEWUTS         /* 主机名隔离 */
                  | (opts.use_user_ns ? CLONE_NEWUSER : 0);

    if (opts.use_overlay && !opts.rootfs) {
        fprintf(stderr, "mydocker: --overlay 需要配合 --rootfs 使用\n");
        return 2;
    }

    return container_run(&opts);
}

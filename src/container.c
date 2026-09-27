/*
 * container.c —— 阶段 1 的核心：用 clone() 创建"容器进程"
 *
 * 原理速览：
 *   fork() 创建的子进程和父进程共享同一个"世界"（同一套 PID、同一张挂载表、
 *   同一个主机名……）。clone() 比 fork 多一个 flags 参数，flags 里带上
 *   CLONE_NEWUTS / CLONE_NEWPID / CLONE_NEWNS 等常量时，内核会在创建子进程的
 *   同时把它放进全新的 Namespace 里 —— 子进程从此"看不见"旧世界，
 *   这就是容器隔离的本质。
 *
 *   User Namespace 特殊一点：它给普通用户一个"当 root"的机会 ——
 *   在新 User Namespace 里把宿主机上的普通 uid（比如 1000）映射成 0，
 *   于是子进程在这个 Namespace 内拥有全部 capability（包括 CAP_SYS_ADMIN），
 *   有权再创建 UTS/PID/Mount 等其它 Namespace。Docker 的 rootless 模式同理。
 *   代价：uid/gid 映射必须由父进程在 clone 之后、子进程干活之前写好，
 *   所以需要一根管道做父子同步。
 *
 * 阶段 1 范围：容器内固定启动 /bin/bash（execv 硬编码路径 + 固定参数数组，
 *   无任何外部输入参与进程映像选择）。支持任意外部命令是阶段 5 的目标。
 */
#define _GNU_SOURCE                     /* clone() 是 GNU 扩展，必须先定义这个宏 */
#include "mydocker.h"

#include <errno.h>
#include <fcntl.h>                      /* open() */
#include <sched.h>                      /* clone(), CLONE_NEWxxx */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>                  /* mount(), MS_NOSUID 等 */
#include <sys/wait.h>                   /* waitpid() */
#include <unistd.h>                     /* read(), write(), execv(), sethostname() */

/* 子进程独立栈：clone 不像 fork 那样自动复制栈，需要调用方提供。
   1 MiB 对一个马上 exec 的进程绰绰有余（exec 后整个栈会被新程序重建）。 */
#define STACK_SIZE (1024 * 1024)

/* 小工具：把一段文本写进 /proc 或 /sys 下的伪文件。
   返回 0 成功，-1 失败（errno 保留）。 */
static int write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY);      /* 这些伪文件只支持写 */
    if (fd < 0) {
        fprintf(stderr, "mydocker: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t len = strlen(content);
    if (write(fd, content, len) != (ssize_t)len) {
        fprintf(stderr, "mydocker: write %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/* ---------------- 子进程入口 ---------------- */

static int child_main(void *arg)
{
    struct container_opts *o = arg;
    char done;

    /* 若启用了 User Namespace，此刻我们还没有身份映射：
       父进程正在往 /proc/<我们的pid>/uid_map 写映射，读管道等它写完。
       （读不到数据会阻塞，正好当"等一下"用。） */
    if (o->ns_flags & CLONE_NEWUSER) {
        if (read(o->sync_pipe[0], &done, 1) != 1) {
            perror("mydocker: child read sync");
            return 1;
        }
    }
    close(o->sync_pipe[0]);

    /* 在新 UTS Namespace 里改主机名 —— 只改本 Namespace 的视角，
       宿主机的主机名不受影响。 */
    if (o->hostname && sethostname(o->hostname, strlen(o->hostname)) < 0)
        perror("mydocker: sethostname");

    /* 重挂 /proc：/proc 是内核按"调用者所在 PID Namespace"动态生成的，
       但必须重新挂载一次它才会刷新成新 Namespace 的内容。
       注意三个安全位：内核要求在带 User Namespace 的环境挂 proc
       必须显式加 nosuid/nodev/noexec，否则返回 EPERM。 */
    if (o->ns_flags & CLONE_NEWPID) {
        if (mount("proc", "/proc", "proc",
                  MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
            perror("mydocker: mount /proc");
    }

    /* 替换进程映像：固定 /bin/bash + 固定参数（无外部输入参与）。
       成功后本行不再返回；失败才落到 perror。 */
    char *const shell_argv[] = { "bash", NULL };
    execv("/bin/bash", shell_argv);
    fprintf(stderr, "mydocker: exec /bin/bash: %s\n", strerror(errno));
    return 127;                         /* shell 惯例：127 = 命令找不到 */
}

/* ---------------- 父进程逻辑 ---------------- */

int container_run(struct container_opts *o)
{
    /* 1. 准备子进程栈 */
    char *stack = malloc(STACK_SIZE);
    if (!stack) { perror("mydocker: malloc"); return -1; }

    /* 2. 建同步管道（仅 User Namespace 模式会真正用到） */
    if (pipe(o->sync_pipe) < 0) { perror("mydocker: pipe"); return -1; }

    /* 3. clone 出"容器进程"。
          SIGCHLD：子进程结束时给父进程发信号，waitpid 才能正常收尸；
          其余 flags 原样透传。注意栈向下生长，传栈顶而不是栈底。 */
    int flags = o->ns_flags | SIGCHLD;
    pid_t pid = clone(child_main, stack + STACK_SIZE, flags, o);
    if (pid < 0) {
        perror("mydocker: clone");
        return -1;
    }
    /* 父进程不再需要读端 */
    close(o->sync_pipe[0]);

    /* 4. User Namespace 模式：写 uid/gid 映射，把"外面的我"映射成"里面的 root"。
          映射文件每行格式： <Namespace内的id> <外面的id> <映射数量>
          "0 1000 1" = 内部的 0号(root) 就是外面的 1000号。
          写 gid_map 前必须先向 setgroups 写 "deny"（内核规定，防止绕过组权限）。 */
    if (o->ns_flags & CLONE_NEWUSER) {
        char path[64], map[64];
        snprintf(map, sizeof(map), "0 %d 1\n", geteuid());

        snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
        if (write_file(path, "deny\n") < 0) goto map_fail;

        snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
        if (write_file(path, map) < 0) goto map_fail;

        snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
        snprintf(map, sizeof(map), "0 %d 1\n", getegid());
        if (write_file(path, map) < 0) goto map_fail;

map_fail:
        /* 不管成功与否都放行子进程：失败时子进程稍后自己会因权限不足报错，
           退出码能如实反映问题 */
        if (write(o->sync_pipe[1], "g", 1) != 1)
            perror("mydocker: pipe write");
    }
    close(o->sync_pipe[1]);

    /* 5. 等容器进程退出并回收（否则它变僵尸进程挂在进程表里） */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("mydocker: waitpid");
        return -1;
    }
    free(stack);
    if (WIFEXITED(status))  return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status); /* shell 惯例 */
    return -1;
}

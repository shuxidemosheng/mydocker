/*
 * cli.c —— 阶段 5：exec / ps / rm 三个管理子命令
 *
 * exec 的原理等价于 nsenter：打开目标进程的 /proc/<pid>/ns/* 文件，
 * 逐个 setns() 把自己"搬进"目标的各个 Namespace，最后 chroot 进目标
 * 的根文件系统（/proc/<pid>/root），再 exec 用户命令。
 */
#define _GNU_SOURCE                     /* setns() 是 GNU 扩展 */
#include "mydocker.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>                      /* setns() */
#include <signal.h>                     /* kill(), SIGKILL */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------- mydocker ps：列出运行中的容器 ---------- */

/* 状态目录与 container_run 里的约定一致：root 用 /run/mydocker，
   普通用户用 ~/.mydocker */
static void state_dir(char *buf, size_t len)
{
    if (geteuid() == 0)
        snprintf(buf, len, "/run/mydocker");
    else
        snprintf(buf, len, "%s/.mydocker",
                 getenv("HOME") ? getenv("HOME") : "/tmp");
}

int cmd_ps(void)
{
    char rundir[256], path[512];
    state_dir(rundir, sizeof(rundir));

    printf("%-8s %-12s %s\n", "PID", "HOSTNAME", "CMD");
    DIR *d = opendir(rundir);
    if (!d) return 0;                       /* 没有状态目录 = 没有容器在跑 */

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;

        snprintf(path, sizeof(path), "%s/%s", rundir, e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char info[512];
        ssize_t n = read(fd, info, sizeof(info) - 1);
        close(fd);
        if (n <= 0) continue;
        info[n] = 0;

        /* 解析 hostname= 行 */
        char host[64] = "-";
        for (char *line = strtok(info, "\n"); line; line = strtok(NULL, "\n"))
            if (sscanf(line, "hostname=%63s", host) == 1) break;

        /* 读该 pid 的 cmdline（NUL 分隔 -> 空格展示） */
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            unlink(rundir);                 /* 容器已死，清掉残留状态文件 */
            snprintf(path, sizeof(path), "%s/%s", rundir, e->d_name);
            unlink(path);
            continue;
        }
        n = read(fd, info, sizeof(info) - 1);
        close(fd);
        if (n <= 0) continue;
        for (ssize_t i = 0; i < n; i++)
            if (info[i] == 0) info[i] = ' ';
        info[n] = 0;
        printf("%-8s %-12s %s\n", e->d_name, host, info);
    }
    closedir(d);
    return 0;
}

/* ---------- mydocker exec <pid>：进入容器 ---------- */

int cmd_exec(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: mydocker exec <pid> [命令，默认 sh]\n");
        return 2;
    }
    pid_t pid = (pid_t)atoi(argv[2]);
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    if (access(path, F_OK) < 0) {
        fprintf(stderr, "mydocker: 容器 %d 不存在\n", pid);
        return 1;
    }

    /* 1. 目标容器根：/proc/<pid>/root 是"透过宿主视角看到的容器根目录" */
    snprintf(path, sizeof(path), "/proc/%d/root", pid);
    int rootfd = open(path, O_RDONLY | O_DIRECTORY);
    if (rootfd < 0) { perror("mydocker: open container root"); return 1; }

    /* 2. 依次 setns：uts(主机名) / net(网络) / pid(进程号) / mnt(挂载表)。
          setns(fd, 0) 表示自动匹配 fd 对应的 namespace 类型。 */
    static const char *nss[] = { "uts", "net", "pid", "mnt" };
    for (size_t i = 0; i < sizeof(nss) / sizeof(nss[0]); i++) {
        char nspath[96];
        snprintf(nspath, sizeof(nspath), "/proc/%d/ns/%s", pid, nss[i]);
        int fd = open(nspath, O_RDONLY);
        if (fd < 0) { perror("mydocker: open ns"); return 1; }
        if (setns(fd, 0) < 0) {
            fprintf(stderr, "mydocker: setns %s: %s\n",
                    nss[i], strerror(errno));
            return 1;
        }
        close(fd);
    }

    /* 3. 切根：setns(mnt) 不会自动搬 root/cwd，需要显式跳进目标根。
          fchdir 到目标根目录后 chroot(".") 把它变成自己的 / 。 */
    if (fchdir(rootfd) < 0 || chroot(".") < 0) {
        perror("mydocker: chroot container root");
        return 1;
    }
    close(rootfd);

    /* 4. 在容器里执行命令（固定 sh，与 run 的阶段 1 取舍一致） */
    char *const shell_argv[] = { "sh", NULL };
    execv("/bin/sh", shell_argv);
    fprintf(stderr, "mydocker: exec /bin/sh: %s\n", strerror(errno));
    return 127;
}

/* ---------- mydocker rm <pid>：终止容器 ---------- */

int cmd_rm(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: mydocker rm <pid>\n");
        return 2;
    }
    pid_t pid = (pid_t)atoi(argv[2]);
    if (kill(pid, SIGKILL) < 0) {
        fprintf(stderr, "mydocker: kill %d: %s\n", pid, strerror(errno));
        return 1;
    }
    /* cgroup 组由容器父进程在 waitpid 后清理；父进程若已不在，兜底删一次 */
    char cgdir[128], rundir[256], statefile[512];
    snprintf(cgdir, sizeof(cgdir), "/sys/fs/cgroup/mydocker/mydocker-%d", pid);
    rmdir(cgdir);                           /* 失败不阻塞 */
    state_dir(rundir, sizeof(rundir));
    snprintf(statefile, sizeof(statefile), "%s/%d", rundir, pid);
    unlink(statefile);
    printf("container %d removed\n", pid);
    return 0;
}

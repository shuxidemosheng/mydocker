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
#include "netlink.h"

#ifdef NET_DEBUG
#define DBG(...) do { fprintf(stderr, "[dbg %d] ", getpid()); \
                      fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
#else
#define DBG(...) do {} while (0)
#endif

#include <errno.h>
#include <fcntl.h>                      /* open() */
#include <limits.h>                     /* PATH_MAX */
#include <net/if.h>                     /* IFNAMSIZ, if_nametoindex() */
#include <signal.h>                     /* kill(), SIGKILL */
#include <sched.h>                      /* clone(), CLONE_NEWxxx */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>                  /* mount(), MS_NOSUID 等 */
#include <sys/stat.h>                   /* mkdir(), mknod(), S_IFCHR */
#include <sys/syscall.h>                /* SYS_pivot_root */
#include <sys/sysmacros.h>              /* makedev() */
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

/* ---------------- 阶段 2：切换容器根文件系统 ---------------- */

/*
 * overlayfs 分层：镜像（lower，只读）+ 容器写时复制层（upper）。
 *   - lowerdir：我们的 rootfs，容器内无论怎么改都不会碰它；
 *   - upperdir：所有修改落在这里（新建文件、改写文件）；
 *   - workdir ：overlayfs 的内部工作目录，必须与 upper 同文件系统；
 *   - merged  ：三者叠加后的"合并视图"，pivot_root 的目标就是它。
 * 目录名直接派生自 rootfs 路径：rootfs-upper / -work / -merged。
 */
static int mount_overlay(const char *rootfs, char *merged, size_t mlen)
{
    char upper[PATH_MAX], work[PATH_MAX], mopts[PATH_MAX * 3];

    snprintf(upper, sizeof(upper), "%s-upper", rootfs);
    snprintf(work,  sizeof(work),  "%s-work",  rootfs);
    snprintf(merged,mlen,          "%s-merged", rootfs);

    const char *dirs[] = { upper, work, merged };
    for (size_t i = 0; i < 3; i++)
        if (mkdir(dirs[i], 0755) < 0 && errno != EEXIST) {
            fprintf(stderr, "mydocker: mkdir %s: %s\n",
                    dirs[i], strerror(errno));
            return -1;
        }

    snprintf(mopts, sizeof(mopts),
             "lowerdir=%s,upperdir=%s,workdir=%s", rootfs, upper, work);
    if (mount("overlay", merged, "overlay", 0, mopts) < 0) {
        fprintf(stderr, "mydocker: mount overlay: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * pivot_root 的最小可用流程（glibc 没有包装函数，走 syscall）：
 *   1) bind mount 把 rootfs"钉"成挂载点（pivot_root 要求 new_root 必须是挂载点）；
 *   2) 把整个 / 的传播属性从 shared 降为 slave（systemd 默认把 / 设为 shared，
 *      不降级 pivot_root 会返回 EBUSY —— 这是最常见的第一个坑）；
 *   3) chdir 进 rootfs，mkdir 一个目录用来"安置"旧根；
 *   4) pivot_root(".", ".mydocker-old")：新根 = 当前目录，旧根整体挪到 .mydocker-old；
 *   5) chdir("/") 后把旧根 MNT_DETACH 卸掉 —— 宿主机文件系统从此不可见。
 * 最后挂上容器必备的三个虚拟文件系统 /proc /sys /dev。
 */
static int setup_rootfs(const char *rootfs)
{
    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) < 0) {
        perror("mydocker: bind rootfs");
        return -1;
    }
    if (mount(NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) < 0)
        perror("mydocker: demote / to slave");   /* 非致命，但失败时 pivot 多半 EBUSY */

    if (chdir(rootfs) < 0) {
        perror("mydocker: chdir rootfs");
        return -1;
    }
    if (mkdir(".mydocker-old", 0700) < 0 && errno != EEXIST) {
        perror("mydocker: mkdir old-root");
        return -1;
    }
    if (syscall(SYS_pivot_root, ".", ".mydocker-old") < 0) {
        perror("mydocker: pivot_root");
        return -1;
    }
    if (chdir("/") < 0) {
        perror("mydocker: chdir /");
        return -1;
    }
    if (umount2(".mydocker-old", MNT_DETACH) < 0)
        perror("mydocker: umount old root");

    /* /proc：新 PID Namespace 的进程视图（带 userns 时内核强制三个安全位） */
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
        perror("mydocker: mount /proc");

    /* /sys：内核规定 sysfs 不允许在非初始 User Namespace 里挂载，
       rootless 模式下这里注定失败 —— 打印警告继续跑（同 podman 的取舍） */
    if (mount("sysfs", "/sys", "sysfs",
              MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RDONLY, NULL) < 0)
        fprintf(stderr, "mydocker: mount /sys skipped (%s)\n", strerror(errno));

    /* /dev：容器内 tmpfs + 手工 mknod 几个基础设备节点。
       mknod 在 userns 里允许，但打开这些节点可能被设备 cgroup 拦 —— 同样只警告。 */
    if (mount("tmpfs", "/dev", "tmpfs",
              MS_NOSUID | MS_NOEXEC, "mode=755,size=64m") < 0) {
        perror("mydocker: mount /dev");
    } else {
        static const struct { const char *path; int major, minor; char type; } devs[] = {
            { "/dev/null",  1, 3, 'c' }, { "/dev/zero", 1, 5, 'c' },
            { "/dev/random",1, 8, 'c' }, { "/dev/urandom", 1, 9, 'c' },
        };
        for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
            mode_t m = devs[i].type == 'c' ? S_IFCHR : S_IFBLK;
            if (mknod(devs[i].path, m | 0666,
                      makedev(devs[i].major, devs[i].minor)) < 0)
                fprintf(stderr, "mydocker: mknod %s: %s\n",
                        devs[i].path, strerror(errno));
        }
        mkdir("/dev/pts", 0755);
        mkdir("/dev/shm", 0777);
    }
    return 0;
}

/* ---------------- 阶段 3：cgroup v2 资源限制 ---------------- */

/*
 * cgroup v2 是"统一层级"：一颗进程树，每个目录一个组，控制器按需开启。
 * 与 v1（每个子系统一棵独立树，memory/cpu 各挂各的）最大的不同。
 *
 * 本函数在父进程里执行，做四件事：
 *   1) 定位基础组：root 下用 /sys/fs/cgroup/mydocker；
 *      普通用户走 systemd 委派目录 user@<uid>.service（rootless 路径）。
 *   2) 在基础组的 subtree_control 里开启 memory/cpu/pids 控制器
 *      （v2 规则：控制器必须在"父链"上逐级开启，子组里才会出现对应接口文件）。
 *   3) 建容器专属组 mydocker-<pid>，写 memory.max / cpu.max。
 *      cpu.max 两列: "配额 周期"，单位微秒；50% => "50000 100000"（每 100ms 用 50ms）。
 *   4) 把容器进程 pid 写进组的 cgroup.procs —— 从此它受这组限额约束。
 * 组路径通过 cgdir 带回，waitpid 后由调用方清理（删目录）。
 */
static int cgroup_apply(const struct container_opts *o, pid_t pid,
                        char *cgdir, size_t cglen)
{
    char base[PATH_MAX], path[PATH_MAX], buf[256];

    if (geteuid() == 0) {
        snprintf(base, sizeof(base), "/sys/fs/cgroup/mydocker");
    } else {
        /* rootless：systemd 把 user@<uid>.service 子树委派给普通用户，
           我们只能在这棵子树里活动，越界一律 EPERM */
        snprintf(base, sizeof(base),
                 "/sys/fs/cgroup/user.slice/user-%d.slice/user@%d.service",
                 geteuid(), geteuid());
    }
    if (mkdir(base, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "mydocker: cgroup mkdir %s: %s\n",
                base, strerror(errno));
        return -1;
    }

    /* 开启控制器：只认 cpu/memory/pids 三个，其余忽略 */
    snprintf(path, sizeof(path), "%s/cgroup.controllers", base);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "mydocker: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) { perror("mydocker: read controllers"); return -1; }
    buf[n] = '\0';

    snprintf(path, sizeof(path), "%s/cgroup.subtree_control", base);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "mydocker: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    for (char *tok = strtok(buf, " \n"); tok; tok = strtok(NULL, " \n")) {
        if (strcmp(tok, "cpu") && strcmp(tok, "memory") && strcmp(tok, "pids"))
            continue;
        char enable[32];
        snprintf(enable, sizeof(enable), "+%s", tok);
        if (write(fd, enable, strlen(enable)) < 0) {
            /* 已开启的控制器重复写 +xxx 会报 EINVAL？不，v2 幂等，可忽略失败 */
            fprintf(stderr, "mydocker: enable +%.16s: %s\n",
                    tok, strerror(errno));
        }
    }
    close(fd);

    /* 建容器专属组并写限额 */
    snprintf(cgdir, cglen, "%s/mydocker-%d", base, pid);
    if (mkdir(cgdir, 0755) < 0) {
        fprintf(stderr, "mydocker: cgroup mkdir %s: %s\n",
                cgdir, strerror(errno));
        return -1;
    }
    if (o->memory_bytes > 0) {
        snprintf(path, sizeof(path), "%s/memory.max", cgdir);
        snprintf(buf, sizeof(buf), "%ld\n", o->memory_bytes);
        if (write_file(path, buf) < 0) return -1;
        /* swap.max=0：禁用换页兜底。否则内核会持续把匿名页换出到 swap，
           容器实际能吃掉 max+swap 的内存，OOM 大大推迟（Docker 默认也允许
           swap；这里为了"到额即 OOM"的清晰语义选择禁用） */
        snprintf(path, sizeof(path), "%s/memory.swap.max", cgdir);
        if (write_file(path, "0\n") < 0) return -1;
    }
    if (o->cpu_percent > 0) {
        if (o->cpu_percent < 1 || o->cpu_percent > 100) {
            fprintf(stderr, "mydocker: --cpu 取值 1-100\n");
            return -1;
        }
        snprintf(path, sizeof(path), "%s/cpu.max", cgdir);
        /* 配额 = 百分比 × 1000us，周期固定 100000us(100ms) */
        snprintf(buf, sizeof(buf), "%ld 100000\n", o->cpu_percent * 1000);
        if (write_file(path, buf) < 0) return -1;
    }

    /* 把容器进程塞进组：从此整棵子树受本组限额 */
    snprintf(path, sizeof(path), "%s/cgroup.procs", cgdir);
    snprintf(buf, sizeof(buf), "%d", pid);
    if (write_file(path, buf) < 0) return -1;
    return 0;
}

/* ---------------- 子进程入口 ---------------- */

static int child_main(void *arg)
{
    struct container_opts *o = arg;

    /* 若启用了 User Namespace 或 cgroup 限额，父进程还需要做收尾工作
       （写 uid/gid 映射、把我们的 pid 写进 cgroup 组），
       读管道等它发"可以了"的信号，再继续干活。 */
    /* 等父进程放行：veth 名字在 clone 前已写入结构体（clone 时随地址空间
       一起复制，子进程天然可见），管道只传一个字节的"可以了"信号。 */
    {
        char done;
        if (read(o->sync_pipe[0], &done, 1) != 1) {
            perror("mydocker: child read sync");
            return 1;
        }
    }
    close(o->sync_pipe[0]);
    DBG("child: handshake done");

    /* 在新 UTS Namespace 里改主机名 —— 只改本 Namespace 的视角，
       宿主机的主机名不受影响。 */
    if (o->hostname && sethostname(o->hostname, strlen(o->hostname)) < 0)
        perror("mydocker: sethostname");

    /* 切换根文件系统（阶段 2）。--overlay 时先叠 overlayfs，pivot 进合并视图；
       pivot 成功后 /proc 已在 setup_rootfs 内挂好；
       未指定 rootfs 时保持阶段 1 行为：仅重挂 /proc。 */
    if (o->rootfs) {
        char merged[PATH_MAX];
        const char *target = o->rootfs;
        if (o->use_overlay) {
            if (mount_overlay(o->rootfs, merged, sizeof(merged)) < 0)
                return 1;
            target = merged;
        }
        if (setup_rootfs(target) < 0) return 1;
        DBG("child: rootfs done");
    } else if (o->ns_flags & CLONE_NEWPID) {
        if (mount("proc", "/proc", "proc",
                  MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
            perror("mydocker: mount /proc");
    }

    /* 6.5 容器侧网络自配（阶段 4）：此刻已在自己的 netns 里，
           veth 容器端（父进程起名 vc<pid>）已被父进程迁入本 netns，
           重命名为 eth0 后配地址与默认路由（网关 = 网桥 IP 172.20.0.1） */
    if (o->use_net && o->veth_peer[0]) {
        DBG("child: net config start");
        if (nl_link_up("lo") < 0 ||
            nl_link_rename(o->veth_peer, "eth0") < 0 ||
            nl_addr_add("eth0", "172.20.0.2", 24) < 0 ||
            nl_link_up("eth0") < 0 ||
            nl_route_add_default("172.20.0.1", "eth0") < 0)
            return 1;
        DBG("child: net config done");
    } else {
        DBG("child: net config SKIPPED, veth_peer=%s", o->veth_peer);
    }

    /* 替换进程映像：固定 /bin/sh + 固定参数（无外部输入参与）。
       busybox rootfs 里 /bin/bash 不存在（busybox 无 bash applet），
       /bin/sh 是 POSIX 标准 shell，同时兼容完整发行版 rootfs。
       成功后本行不再返回；失败才落到 perror。 */
    char *const shell_argv[] = { "sh", NULL };
    execv("/bin/sh", shell_argv);
    fprintf(stderr, "mydocker: exec /bin/sh: %s\n", strerror(errno));
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

    /* 2.5 预生成容器端 veth 名字：必须在 clone 之前写入结构体，
           否则子进程的地址空间副本里看不到（fork 语义）。 */
    snprintf(o->veth_peer, sizeof(o->veth_peer), "vc%d", getpid());

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

    /* 3.5 写容器状态文件（阶段 5）：/run/mydocker/<pid>（root）或
           ~/.mydocker/<pid>（普通用户）。ps/exec/rm 据此发现容器。 */
    char rundir[PATH_MAX], statefile[PATH_MAX];
    if (geteuid() == 0)
        snprintf(rundir, sizeof(rundir), "/run/mydocker");
    else
        snprintf(rundir, sizeof(rundir), "%s/.mydocker",
                 getenv("HOME") ? getenv("HOME") : "/tmp");
    mkdir(rundir, 0755);
    snprintf(statefile, sizeof(statefile), "%s/%d", rundir, pid);
    int sfd = open(statefile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sfd >= 0) {
        char info[512];
        snprintf(info, sizeof(info),
                 "pid=%d\nhostname=%s\nveth=%s\nrootfs=%s\n",
                 pid, o->hostname ? o->hostname : "-", o->veth_peer,
                 o->rootfs ? o->rootfs : "-");
        ssize_t ignore = write(sfd, info, strlen(info));
        (void)ignore;
        close(sfd);
    }

    /* 4. User Namespace 模式：写 uid/gid 映射，把"外面的我"映射成"里面的 root"。
          映射文件每行格式： <Namespace内的id> <外面的id> <映射数量>
          "0 1000 1" = 内部的 0号(root) 就是外面的 1000号。
          写 gid_map 前必须先向 setgroups 写 "deny"（内核规定，防止绕过组权限）。 */
    char cgdir[PATH_MAX] = "";
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
        ; /* 失败也继续放行：子进程稍后自己会因权限不足报错 */
    }

    /* 5. cgroup 限额（阶段 3）：建组 -> 写限额 -> 把子进程 pid 塞进去 */
    if ((o->memory_bytes > 0 || o->cpu_percent > 0) &&
        cgroup_apply(o, pid, cgdir, sizeof(cgdir)) < 0) {
        close(o->sync_pipe[1]);
        kill(pid, SIGKILL);             /* 限额没设成就别让它裸奔 */
        waitpid(pid, NULL, 0);
        free(stack);
        return -1;
    }

    /* 6. 容器网络（阶段 4）：
          宿主侧 —— bridge(mydocker0) + veth pair，把宿主端挂上桥、容器端塞进
          子进程的 netns；子进程放行后自配 lo/eth0/地址/默认路由。
          宿主侧这些操作动的是"真实"网络栈，必须 root（--user 模式不支持 --net）。 */
    if (o->use_net) {
        if (geteuid() != 0) {
            fprintf(stderr, "mydocker: --net 需要以 root 运行（bridge 属于宿主网络栈）\n");
            close(o->sync_pipe[1]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            return -1;
        }
        if (if_nametoindex("mydocker0") == 0) {
            /* 网桥幂等创建：不存在才建，多个容器共享同一座桥 */
            if (nl_link_create_bridge("mydocker0") == 0) {
                nl_addr_add("mydocker0", "172.20.0.1", 24);
            }
        }
        nl_link_up("mydocker0");
        char veth_host[IFNAMSIZ];
        snprintf(veth_host, sizeof(veth_host), "vh%d", pid);
        DBG("net: create veth done");
        if (nl_link_create_veth(veth_host, o->veth_peer) == 0) {
            DBG("net: set_master");
            nl_link_set_master(veth_host, "mydocker0");
            nl_link_up(veth_host);
            DBG("net: set_ns");
            nl_link_set_ns(o->veth_peer, pid);  /* 一端进入容器 netns */
            /* 开转发（容器出网 NAT 的前提）；失败只警告 */
            if (write_file("/proc/sys/net/ipv4/ip_forward", "1\n") < 0)
                fprintf(stderr, "mydocker: ip_forward 未开启，容器将无法出网\n");
        } else {
            close(o->sync_pipe[1]);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            return -1;
        }
    }

    /* 7. 握手放行：映射/限额/网络都布置完毕，子进程可以开始跑了 */
    DBG("net: write g");
    if (write(o->sync_pipe[1], "g", 1) != 1)
        perror("mydocker: pipe write");  /* 子进程若已出错退出，写管道会 EPIPE */
    close(o->sync_pipe[1]);

    /* 7. 等容器进程退出并回收（否则它变僵尸进程挂在进程表里） */
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("mydocker: waitpid");
        return -1;
    }
    free(stack);

    /* 8. 清理本次容器的 cgroup 组与状态文件（组内已无进程，rmdir 即删） */
    if (cgdir[0]) rmdir(cgdir);
    unlink(statefile);

    if (WIFEXITED(status))  return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status); /* shell 惯例 */
    return -1;
}

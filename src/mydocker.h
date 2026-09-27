/*
 * mydocker.h —— 公共头文件
 *
 * 一个"容器"在本项目里的定义：
 *   一个普通 Linux 进程 + 若干 Namespace（隔离视图）+ 后续阶段的 cgroups/rootfs/网络。
 * 本文件只声明阶段 1 涉及的"创建隔离进程"接口。
 */
#ifndef MYDOCKER_H
#define MYDOCKER_H

/* 一次 "mydocker run" 的全部参数 */
struct container_opts {
    int         ns_flags;      /* 要创建的 Namespace 组合（CLONE_NEWxxx 按位或），
                                  不含 SIGCHLD，SIGCHLD 由 container_run 内部补上 */
    int         use_user_ns;   /* 1 = 额外套一层 User Namespace（免 root 运行用） */
    const char *hostname;      /* 容器内主机名；NULL 表示不改 */
    const char *rootfs;        /* 容器根目录（阶段 2）；NULL = 不切换根 */
    int         use_overlay;   /* 1 = 在 rootfs 上叠 overlayfs（lower=rootfs 只读，
                                  upper/work/merged 自动生成在 rootfs 旁边） */
    long        memory_bytes;  /* 阶段 3：memory.max 字节数；0 = 不限制 */
    long        cpu_percent;   /* 阶段 3：cpu.max 限额百分比（1-100）；0 = 不限制 */
    int         use_net;       /* 阶段 4：接入 mydocker0 网桥（veth + NAT 前置） */
    char        veth_peer[16]; /* 容器侧 veth 名（父进程生成，子进程 rename 为 eth0）*/
    int         sync_pipe[2];  /* 父 -> 子 的同步管道：
                                  子进程要等父进程写好 uid/gid 映射才能继续干活，
                                  管道就是最简单的"等一下"手段 */
};

/* 创建隔离进程并在其中启动 /bin/sh（阶段 1 起固定行为），
   返回值同 waitpid 语义（正常退出为 sh 的退出码） */
int container_run(struct container_opts *opts);

/* 阶段 5 管理子命令（cli.c） */
int cmd_ps(void);
int cmd_exec(int argc, char **argv);
int cmd_rm(int argc, char **argv);

#endif /* MYDOCKER_H */

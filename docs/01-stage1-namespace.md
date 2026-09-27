# 阶段 1：clone + Namespace 隔离进程

日期：2026-09-27

## 实现要点

1. `clone(child_main, stack_top, flags | SIGCHLD, opts)` —— 与 fork 的三点区别：
   - flags 可指定同时创建多种 Namespace；
   - 必须调用方提供栈（向下生长，传 `stack + STACK_SIZE`）；
   - 子进程从指定入口函数开始，而不是从调用点返回。
2. User Namespace + uid/gid 映射（免 root 模式）：
   - 映射 `"0 1000 1"` 写入 `/proc/<pid>/uid_map`；
   - 写 `gid_map` 前必须先向 `/proc/<pid>/setgroups` 写 `deny`；
   - **父子同步**：映射写完前子进程没有合法身份视图，用管道阻塞子进程；
   - 踩坑：`groups=` 显示一排 65534(nogroup)，因为 setgroups 被 deny —— 符合预期。
3. PID Namespace 生效后必须**重挂 /proc** 才能看到新空间的进程视图；
   内核要求带 userns 时挂 proc 必须加 `MS_NOSUID|MS_NODEV|MS_NOEXEC`，否则 EPERM。
4. 调试教训：嵌套 shell（Git Bash → wsl bash -c "..."）里 `\$(...)` 会被逐层展开，
   一次"实测失败"其实是外层 shell 的变量展开假象。容器内自检要用单引号原样传入。

## 实测记录

```
$ printf 'id; grep Cap /proc/self/status; cat /proc/sys/kernel/hostname' \
    | ./mydocker run --user --hostname container
uid=0(root) ...                    # 容器内是"root"（仅限本 userns 视图）
CapEff: 000001ffffffffff           # userns 内满配 capability
container                          # UTS 隔离生效
$ ps -o pid,comm                   # 容器内
    PID COMMAND
      1 bash                        # 容器进程是 PID 1
      2 ps
```

## 遗留

- 默认模式（不带 --user，直接创建其余 Namespace）需要 root，待 sudo 免密配置后补测。
- 阶段 5 将把"固定 /bin/bash"改为支持任意外部命令。

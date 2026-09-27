# 阶段 0：环境验证笔记

日期：2026-09-27

## 环境

| 项 | 值 |
|---|---|
| 宿主 | Windows 11 + WSL2 (Ubuntu 26.04 LTS) |
| 内核 | 6.18.33.2-microsoft-standard-WSL2 |
| cgroup | **v2**（`stat -fc %T /sys/fs/cgroup` → cgroup2fs） |
| overlayfs | 可用（/proc/filesystems 含 overlay） |
| 编译器 | gcc/g++ 15.2.0 |

## Namespace 支持验证

`ls /proc/self/ns/` 显示内核支持全部 8 种：
cgroup, ipc, mnt, net, pid, pid_for_children, time, time_for_children, user, uts

## 无 root 实测记录

1. `unshare --user --mount bash -c 'mount -t tmpfs tmpfs /tmp'` → 成功，
   挂载只影响新 Mount Namespace 内的视图（对应教程 M0 实验）。
2. `unshare --uts bash -c hostname` → `Operation not permitted`。
   结论：创建 UTS/PID/Net 等 Namespace 需要 root，或先有映射过的 User Namespace
   （在 User Namespace 里进程获得 CAP_SYS_ADMIN，即可创建其余 Namespace）。
   这就是 Docker 无 root 运行的原理，也是本项目 `run --user` 模式的依据。

## 对后续实现的决定

- 阶段 1 代码设计双模式：`--user`（创建 User Namespace + uid_map 映射，免 root 可跑）
  与默认模式（直接创建其余 Namespace，需 root）。
- cgroup 部分按 v2 API 实现（教程是 v1），差异记录在阶段 3 笔记。

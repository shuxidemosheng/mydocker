# 阶段 5：CLI 整合（ps / exec / rm）

日期：2026-09-28

## 实现要点

1. **容器状态落盘**：父进程在 clone 后把元信息写进
   `/run/mydocker/<pid>`（root）或 `~/.mydocker/<pid>`（普通用户）：
   pid / hostname / veth 名 / rootfs 路径；waitpid 后 unlink。
   这是 ps/exec/rm 的"注册表"。真实 Docker 用的是 containerd 的元数据存储。
2. **exec ≈ nsenter**：
   - 逐个 `setns()` 打开的 `/proc/<pid>/ns/{uts,net,pid,mnt}`；
   - `setns(mnt)` 不会搬动 root/cwd —— 必须显式 `fchdir(/proc/<pid>/root)`
     + `chroot(".")` 才真正落进容器根；
   - exec 前打开目标 root 的 fd 要在 setns 之前（路径以宿主视角解析）。
3. **rm**：SIGKILL 容器进程 → 兜底删 cgroup 组和状态文件。
   veth 无需手动删：容器 netns 销毁时 veth 对随之销毁（内核行为）。

## 实测记录（verify-stage5.sh）

```
PID      HOSTNAME     CMD
1117     web          sh
--- exec 1117 ---
web                                  # UTS 隔离
inet 172.20.0.2/24 scope global eth0 # 网络隔离
PID   USER     COMMAND
    1  0        sh                  # PID 隔离：sh 是容器内的 1 号进程
    2  0        /bin/sleep 20
```

## 遗留（复试可讲的改进方向）

- run 支持任意外部命令（当前固定 /bin/sh）；
- 容器生命周期状态机（running/stopped）与日志；
- 后台容器（-d）与容器退出码回传；
- rootless 网络方案（slirp4netns / pasta）。

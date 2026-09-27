# 阶段 3：cgroup v2 资源限制

日期：2026-09-28

## 实现要点

1. **v2 统一层级**：一颗进程树，控制器沿"父链"逐级在 `cgroup.subtree_control`
   开启（`+cpu` `+memory` `+pids`），子组里才出现对应接口文件。
   v1 则是每个子系统一棵独立树 —— 教程多为 v1，本项目按 v2。
2. **rootless 与 root 双路径**：
   - root：`/sys/fs/cgroup/mydocker/<pid>/`；
   - 普通用户：systemd 委派子树 `user.slice/user-<uid>.slice/user@<uid>.service/`
     （只允许在委派范围内 mkdir/写）。
3. **cpu.max 两列**："配额 周期"（微秒）。30% => `30000 100000`：每 100ms 周期
   允许跑 30ms，超了就 throttle（挂起），下个周期恢复。
4. **memory.max 的 swap 陷阱（本次实测最大的发现）**：
   限额 50MB 的容器冲到了 2GB 才 OOM —— 因为 WSL2 有 swap，memcg 到额后
   内核不断把匿名页 swap out，`memory.current` 始终不超 50MB，直到 swap 耗尽。
   补 `memory.swap.max = 0` 后立刻在 40~50MB 处 OOM。
   （Docker 默认允许 swap = memory 的 2 倍，语义选择不同而已。）
5. 父子同步管道升级为无条件握手：父进程在放行前完成 uid 映射 + cgroup 布置。

## 实测记录

CPU（--cpu 30）：
```
cpu.max:        30000 100000
nr_periods:     20
nr_throttled:   20          # 每个周期都被节流
throttled_usec: 1339505     # 2 秒墙钟里被挂起 1.34s => 占空比 ~33% ✓
```
（spin 计数的绝对值在 WSL2 上不可比：无节流基线本身在 2.5e8~9.2e8 间漂移，
以 cpu.stat 为准。）

内存（--memory 50m + memhog）：
```
resident ~40 MB
Killed                                    # memhog 被 SIGKILL
dmesg: oom-kill:constraint=CONSTRAINT_MEMCG,
       oom_memcg=/mydocker/mydocker-1063, task=memhog
```
sh 存活并继续执行后续命令 —— cgroup OOM 只杀超限进程，符合容器语义。

## 调试坑

- busybox rootfs 无 glibc 动态链接器，动态编译的测试程序 execve 报 ENOENT
  （"not found" 但 ls 明明可见）→ 测试程序一律 `-static`。
- Makefile 不是目标的依赖，改编译参数不会触发重建 → rm 后重编。
- 容器内看不到自己的 cgroup：我们把 sysfs 挂在 /sys，盖住了宿主 cgroup2
  挂载点（真实 Docker 会单独只读挂 cgroup2 进容器）。

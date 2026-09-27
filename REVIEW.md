# mydocker 复试复习总纲

> **阅读对象**：考研初试结束后的你。距离上次触碰本项目已过去约一年。
> **目标**：3 天内恢复到能在复试现场讲清"从零写 Docker"每个细节的状态。
>
> 复习顺序：本文 → 跑通复验清单 → 按需读 `docs/0x-*.md` 六篇阶段笔记 → 通读源码 → 背问答。

---

## 一、项目一句话定位

**用 C 从零实现一个容器运行时**：clone + 六种 Namespace 隔离、pivot_root + overlayfs
分层根文件系统、cgroup v2 资源限额、rtnetlink 原生实现的 bridge + veth 网络、
ps/exec/rm 管理命令。≈ 真实 Docker 的核心机制骨架（不含镜像分发与安全加固）。

## 二、代码地图（哪里有什么）

| 文件 | 内容 | 核心函数 |
|---|---|---|
| `src/main.c` | CLI 分发：run/ps/exec/rm，选项解析 | `cmd_run` `parse_size` |
| `src/container.c` | **项目心脏**：clone、namespace、uid 映射、pivot_root、overlay、cgroup、veth、父子握手 | `child_main` `container_run` `setup_rootfs` `mount_overlay` `cgroup_apply` |
| `src/netlink.c` | 纯 rtnetlink 封装（不 shell out 到 ip 命令） | `nl_link_create_veth` `nl_addr_add` `nl_route_add_default` 等 |
| `src/cli.c` | exec(setns)/ps/rm | `cmd_exec` `cmd_ps` `cmd_rm` |
| `scripts/build-rootfs.sh` | 免 root 构建 busybox rootfs | — |
| `scripts/verify-stage5.sh` | 一键验收：run→ps→exec→rm | — |
| `test/memhog.c` `test/spin.c` | 内存/CPU 压测（验证 cgroup，**必须 -static**） | — |
| `docs/00~05-*.md` | 六篇阶段笔记：原理 + 踩坑 + 实测数据 | — |

git 历史：5 个 commit 按阶段提交（`de05333`→`a9dcd4e`），tag `v0.1-stage5`。

## 三、知识地图：项目知识点 ↔ 复试科目

| 项目内容 | 对应 408/复试考点 |
|---|---|
| clone/fork、waitpid 收尸、僵尸进程 | OS 进程管理 |
| PID/Mount/UTS/Net Namespace | OS：进程与资源隔离 |
| User Namespace + uid_map | OS：权限与 capability |
| pivot_root、挂载传播 shared/slave | OS 文件系统、mount 语义 |
| overlayfs 写时复制、镜像分层 | OS 文件系统 + 存储设计 |
| cgroup v2：memory.max / cpu.max / CFS 节流 | OS：内存管理、CPU 调度 |
| memcg OOM（oom_score、swap 陷阱） | OS：虚拟内存、页面置换 |
| veth pair / bridge / 路由 / NAT MASQUERADE | 计算机网络：二层交换、三层路由、NAT |
| rtnetlink 消息格式 | 网络：协议设计、TLV 编码 |
| ENOENT 之谜（动态链接器缺失） | OS：装载与链接（对照 408"程序装入"） |

## 四、三天复习路线

### 第 1 天：恢复手感（1~2 小时）
1. 读 README 架构图 + 本文；
2. 跑"五分钟复验清单"（下节），确认项目在当前机器能跑；
3. 通读 `docs/00~05` 六篇笔记（每篇 10 分钟），重点看"实测记录"和"坑"。

### 第 2 天：读透代码（2~3 小时）
按此顺序读，每读完讲一遍给自己听：
1. `container.c:child_main` —— 容器子进程完整生命周期（隔离→换根→网络→exec）；
2. `container.c:container_run` —— 父进程编排（映射→cgroup→veth→放行→收尸）；
3. `container.c:setup_rootfs` —— pivot_root 五步流程 + overlay 挂载；
4. `container.c:cgroup_apply` —— v2 层级与控制器开启；
5. `netlink.c` —— 挑 `nl_link_create_veth`（嵌套属性组装）精读；
6. `cli.c:cmd_exec` —— setns 顺序与 chroot 时机。

### 第 3 天：准备现场表达（1~2 小时）
1. 背第六节的"预设问答"（每个问题 30 秒版本答案要点）；
2. 准备 3 分钟项目自述（下一节有提纲）；
3. 演练"五分钟复验清单"，确保现场能边讲边跑。

## 五、3 分钟项目自述提纲（背熟）

> 这个项目是我用 C 从零实现的容器运行时，覆盖了 Docker 的三大核心机制。
> 第一是隔离：用 clone 系统调用同时创建 Mount/PID/UTS/Net Namespace，
> 另外实现了 User Namespace 的 uid 映射，让普通用户也能建容器——这是
> Docker rootless 模式的原理。第二是文件系统：pivot_root 切换根目录，
> 用 overlayfs 做到镜像层只读、容器可写，写时复制落在 upper 层——我实测
> 验证过容器内写文件不会污染镜像层。第三是资源与网络：cgroup v2 的
> memory.max 加 cpu.max，实测 50MB 限额精准 OOM、30% 配额精确节流；
> 网络用 rtnetlink 手工组消息包创建 bridge 和 veth pair，配 NAT 后容器可以出网，
> 容器内起了 HTTP 服务宿主能访问。开发中我踩过两个印象最深的坑：
> 一是 clone 不带 CLONE_VM 时子进程是地址空间副本，父进程 clone 后写的
> 结构体字段子进程看不见；二是 WSL2 的 swap 让 memcg 到额后不 OOM 而是换页，
> 补 memory.swap.max=0 才实现"到额即杀"。这个项目让我把操作系统的进程、
> 内存、文件系统知识和网络的下层实现真正串了起来。

## 六、预设问答（复试老师最可能追问的 18 问）

### 总体与虚拟化
1. **容器和虚拟机的区别？**
   容器共享宿主内核，隔离靠 Namespace/cgroup（进程级）；VM 用 hypervisor
   虚拟化硬件，每个 VM 有独立内核。容器启动快、开销小，但隔离强度弱于 VM
   （内核攻击面共享）。真实 Docker 还靠 seccomp、capability drop、AppArmor 补强。
2. **Namespace 有哪几种，分别隔离什么？**
   Mount(挂载表)、PID(进程号)、Net(网络栈)、UTS(主机名)、IPC(信号量/共享内存)、
   User(uid/gid 映射)、Cgroup(cgroup 视图)、Time(时钟，5.6+)。本项目用了前四种
   （+可选 User）。
3. **cgroup v1 和 v2 的区别？**（你实现的是 v2，教材多为 v1——差异化亮点）
   v1 每个子系统一棵树，可分别挂载；v2 统一层级，控制器要在父链逐级
   `+xxx` 写进 subtree_control，子组才有接口文件。

### 进程与 Namespace（阶段 1）
4. **clone/fork 的区别？**
   clone 多了 flags（选择共享/新建哪些资源）并要求调用方提供子进程栈
   （向下生长，传栈顶）；fork 是 clone 的特例。
5. **为什么容器里要重挂 /proc？**
   /proc 的内容由内核按"读取者所在的 PID Namespace"动态生成，但已挂载的
   实例不会自动刷新，必须重挂一次才看到新空间的进程视图。带 userns 时内核
   强制加 MS_NOSUID|NODEV|NOEXEC。
6. **rootless 容器的原理？**
   新 User Namespace 里写 uid_map（如 `0 1000 1`），把宿主普通用户映射成
   容器内 root；进程在该 userns 内获得全部 capability（含 CAP_SYS_ADMIN），
   从而有权创建其它 Namespace。写 gid_map 前必须先向 setgroups 写 deny。
7. **PID 1 有什么特殊性？**
   负责收养孤儿进程、对未注册 handler 的信号免疫。真实容器用 tini/init 做 1 号。
8. **clone 后父进程写的结构体字段子进程为什么看不到？**（你踩过的坑）
   clone 不带 CLONE_VM，子进程是 clone 时刻地址空间的副本（写时复制），
   之后两边各自独立——这正是 fork 语义。父进程后续要传的数据只能走
   管道/共享内存/文件。

### 文件系统（阶段 2）
9. **chroot 和 pivot_root 的区别？**
   chroot 只改当前进程的根目录视图，老的挂载仍可达（有逃逸风险）；
   pivot_root 把整个挂载树的根原子性地换掉，旧根整体挪到指定目录再卸载，
   隔离更彻底。pivot_root 要求 new_root 必须是挂载点（先 bind mount 自绑定）。
10. **为什么 pivot_root 报 EBUSY？**
    根挂载的传播属性是 shared 时，内核要求先降级为 slave
    （`mount(NULL, "/", NULL, MS_SLAVE|MS_REC)`）——systemd 默认把 / 设为 shared。
11. **overlayfs 各目录的作用？**
    lowerdir=只读镜像层；upperdir=写时复制层，所有修改落这里；workdir=
    overlay 内部工作目录（必须与 upper 同文件系统）；merged=合并视图。
    删除 lower 文件会在 upper 产生 whiteout 字符设备。
12. **为什么 /sys 挂载失败、/dev/null 的 mknod 失败？**（rootless 边界，实测）
    sysfs 只允许初始 User Namespace 挂载；mknod 需要初始 userns 的
    CAP_MKNOD。rootless 运行时（podman）由宿主侧 helper 预先准备好这些。
13. **动态链接的程序在容器里报 "not found" 但文件明明在，为什么？**（实测坑）
    execve 返回 ENOENT 的另一种情况：ELF 的 PT_INTERP 指定的动态链接器
    （/lib64/ld-linux-x86-64.so.2）在 rootfs 里不存在。busybox-static 是静态
    链接所以能跑。对应 408"程序的链接与装入"。

### 资源限制（阶段 3）
14. **cpu.max 两列是什么意思？**
    "配额 周期"（微秒）。`30000 100000` = 每 100ms 周期允许跑 30ms，
    超了就被 CFS 停住等下个周期（throttle），宏观上就是 30% 占空比。
    我用 cpu.stat 的 nr_throttled/throttled_usec 验证过。
15. **设了 memory.max=50M，进程为什么可能吃到 2GB 才 OOM？**（实测坑）
    有 swap 时，memcg 到额后内核持续把匿名页换出，memory.current 始终
    不超限，直到 swap 也耗尽才 OOM。补 memory.swap.max=0 实现"到额即杀"。
    （Docker 默认允许 swap，是语义取舍不同。）
16. **OOM 时内核杀谁？**
    memcg OOM 在组内选 oom_score（≈ badness，主要看 RSS）最高的进程发
    SIGKILL；dmesg 可见 `constraint=CONSTRAINT_MEMCG`。组外进程不受影响
    ——我实测容器里 sh 存活、只有 memhog 被杀。

### 网络（阶段 4）
17. **画一下容器网络的数据路径？**
    容器 eth0(veth容器端) ── veth pair ── vh<pid>(宿主端，挂在 mydocker0 网桥上)
    → 容器发包：route 查默认路由 → eth0 → veth → 网桥 → 宿主协议栈 →
    POSTROUTING 链 MASQUERADE（源 IP 换成宿主出口 IP）→ 外网。
    回包按 NAT 连接跟踪表反向还原。
18. **为什么用 rtnetlink 而不是 system("ip ...")？**
    ip 命令本质也是往 NETLINK_ROUTE 发消息；自己组包无外部依赖、不拼
    shell 命令串（无注入面）、错误可精确归因。消息 = [nlmsghdr][ifinfomsg]
    [rtattr...]，属性 4 字节对齐，veth 一次消息造两端（嵌套属性
    LINKINFO/INFO_DATA/PEER）。netfilter(NAT) 无法用 rtnetlink 配置，
    所以那一条 iptables 保留。

### exec 与管理（阶段 5）
19. **docker exec 的原理？**
    nsenter：对 /proc/<pid>/ns/{uts,net,pid,mnt} 逐个 setns。注意
    setns(mnt) 不会搬动 root/cwd，要 fchdir(/proc/<pid>/root) + chroot(".")
    才真正落进容器根。我的 `cmd_exec` 就是这个流程。
20. **容器退出后 veth 为什么不用手动删？**
    veth 对的任一端所在 netns 销毁时，整对 veth 被内核自动销毁。

## 七、与真实 Docker 的差距（复试"诚实边界"话术）

| 简化点 | 真实 Docker |
|---|---|
| busybox rootfs 手工搭 | OCI 镜像 + 内容寻址存储 + 镜像仓库分发 |
| 固定 exec /bin/sh | 任意 entrypoint/参数、-d 后台、退出码回传 |
| cgroup 组手动建删 | systemd cgroup driver / 委派管理 |
| iptables 一条 MASQUERADE | 完整的 docker0 + DNAT 端口映射链 |
| 无安全加固 | seccomp profile、capability 白名单、AppArmor |
| 手写 runc 骨架 | containerd → containerd-shim → runc(OCI runtime spec) |

建议话术："我做的是 runc 的教育版骨架，覆盖 namespace/cgroup/overlayfs/
网络四块核心机制；镜像分发、安全加固这些工程化部分我清楚边界在哪。"

## 八、五分钟复验清单（复试现场可演示）

```bash
# 前提：WSL2 Ubuntu（或任何 cgroup v2 的 Linux），gcc，sudo
cd ~/mydocker && make && make test
./scripts/build-rootfs.sh                    # 若 rootfs 缺失

# 1) 隔离 + overlay（免 root 都能跑）
printf 'hostname; ls /; echo hi > /x; cat /x; exit\n' \
  | ./mydocker run --user --hostname c1 --rootfs "$PWD/rootfs" --overlay
ls rootfs/x 2>&1        # → 不存在：写入在 upper 层，镜像未被污染
ls rootfs-upper/x       # → hi

# 2) 资源限额（root）
printf '/bin/memhog 10; exit\n' | sudo ./mydocker run --rootfs "$PWD/rootfs" \
  --overlay --memory 50m      # → ~40MB 处 Killed（memcg OOM）

# 3) 网络 + 服务（root；若出网还需 README 里的那条 NAT 规则）
sudo ./scripts/verify-stage5.sh   # run→ps→exec→rm 全链路
# 容器内服务演示：
# /bin/busybox httpd -p 80 -h /www 后，宿主 curl http://172.20.0.2/
```

从零恢复环境（新机器/WSL 重装后）：
1. WSL2 + Ubuntu 22.04+（内核 ≥5.8，cgroup v2 注意与笔记差异）；
2. `sudo apt install gcc make iptables`；解包本项目；
3. `./scripts/build-rootfs.sh && make`；
4. sudo 免密（或逐步手动 sudo）：`sudo visudo -f /etc/sudoers.d/<你>`；
5. 跑第八节清单。

## 九、档案信息

- 归档：`D:\AIcoding\mydocker-backup-2026-09-28.tar.gz`（代码+文档+git 历史）
- 开发环境：Windows 11 + WSL2 Ubuntu 26.04，内核 6.18，cgroup v2，gcc 15
- 六篇阶段笔记：`docs/00-stage0-env.md`（环境验证）
  `01-namespace` `02-rootfs` `03-cgroup` `04-net` `05-cli`

# mydocker

从零用 C 实现一个简易容器运行时，学习 Docker 的底层原理。
参考《自己动手写 Docker》与 lixd/mydocker 博客系列的思路，代码独立实现。

## 环境

- WSL2 Ubuntu 26.04 / 内核 6.18 / cgroup **v2**（教程多为 v1，本项目按 v2 实现）

## 当前进度

- [x] 阶段 0：环境验证（[笔记](docs/00-stage0-env.md)）
- [x] 阶段 1：clone + Namespace 隔离进程（[笔记](docs/01-stage1-namespace.md)）
- [x] 阶段 2：rootfs + pivot_root + overlayfs（[笔记](docs/02-stage2-rootfs.md)）
- [x] 阶段 3：cgroup v2 资源限制（[笔记](docs/03-stage3-cgroup.md)）
- [x] 阶段 4：veth + bridge 容器网络（[笔记](docs/04-stage4-net.md)）
- [x] 阶段 5：CLI 整合（[笔记](docs/05-stage5-cli.md)）

## 架构总览

```
mydocker run --rootfs rootfs --overlay --net --memory 100m --cpu 30
 └─ 父进程（宿主侧）
     ├─ clone(SIGCHLD|NEWNS|NEWPID|NEWUTS|NEWNET)   ← 阶段 1
     ├─ 写 uid/gid 映射（--user 时）                 ← 阶段 1
     ├─ cgroup v2 组：memory.max / cpu.max           ← 阶段 3
     ├─ bridge mydocker0 + veth pair + 迁移到容器     ← 阶段 4
     └─ 管道写 "g" 放行，waitpid 收尸
        子进程（容器内）
     ├─ 重挂 /proc、pivot_root 换根                  ← 阶段 2
     ├─ overlayfs：镜像层只读 + upper 可写层          ← 阶段 2
     ├─ lo up / rename eth0 / 地址 / 默认路由         ← 阶段 4
     └─ execv("/bin/sh")
```

## 用法

```bash
make            # 编译
make test       # 编译压测程序（memhog / spin）

# 免 root 最小体验（User Namespace）
./mydocker run --user --hostname container

# 完整容器（需 root；先 ./scripts/build-rootfs.sh）
sudo ./mydocker run --rootfs "$PWD/rootfs" --overlay \
     --hostname web --memory 100m --cpu 30 --net

# 管理子命令
sudo ./mydocker ps
sudo ./mydocker exec <pid>
sudo ./mydocker rm <pid>

# 阶段 5 验收脚本
sudo ./scripts/verify-stage5.sh
```

容器出网需一次性配 NAT（宿主执行）：

```bash
sudo iptables -t nat -C POSTROUTING -s 172.20.0.0/24 ! -o mydocker0 -j MASQUERADE \
  || sudo iptables -t nat -A POSTROUTING -s 172.20.0.0/24 ! -o mydocker0 -j MASQUERADE
```

# 阶段 2：rootfs + pivot_root + overlayfs

日期：2026-09-27

## 实现要点

1. **pivot_root 前置条件**（缺一即 EBUSY/EPERM）：
   - new_root 必须是挂载点 → 先对 rootfs 做 `MS_BIND` 自绑定；
   - 当前根的传播属性若是 shared 会 EBUSY → 先 `mount(NULL, "/", NULL, MS_SLAVE|MS_REC)`
     降级（systemd 默认把 `/` 设为 shared，这是最常踩的坑）。
2. pivot_root 本体：`syscall(SYS_pivot_root, ".", ".mydocker-old")` ——
   glibc 没有包装函数；新根是 chdir 进去的 `.`，旧根整体挪到 `.mydocker-old`
   后用 `umount2(..., MNT_DETACH)` 摘掉，宿主文件系统从此不可见。
3. **overlayfs 三层**：lowerdir=rootfs（只读镜像）、upperdir（写时复制层）、
   workdir（overlay 内部工作目录，必须与 upper 同文件系统）、merged（合并视图）。
   容器的一切写入都落在 upper，镜像层保持不变 —— 这就是 Docker 镜像不可变 +
   容器可写的内容寻址基础（真正的 Docker 还会在 upper 之上再做 snapshotter 管理）。
4. **rootless 的边界**（实测确认）：
   - sysfs 拒绝在非初始 userns 挂载 → `/sys` 挂载跳过（podman 同样取舍）；
   - mknod 返回 EPERM → 基础设备节点在 rootless 下无法创建，
     需要 root 模式补齐（runc 的做法也是由 root 完成 /dev 初始化）。

## 实测记录

```
$ printf 'echo overlay-test > /overlay-written; cat /overlay-written' \
    | ./mydocker run --user --rootfs ~/mydocker/rootfs --overlay
overlay-test                          # 容器内写文件、读回正常
$ ls rootfs/overlay-written           # 宿主侧看镜像层
ls: cannot access ...: No such file   # 镜像层未被污染 ✓
$ ls rootfs-upper/
overlay-written                       # 写入落在可写层 ✓
```

容器内 `ls /` 只有 rootfs 内容、`/home` 为空、`hostname=container` ——
pivot_root + UTS + PID 隔离同时生效。

## 坑

- busybox 没有 bash applet，rootfs 里 `/bin/bash` 符号链接会报 "applet not found"，
  容器内固定 shell 改为 `/bin/sh`。
- 多层嵌套 shell 调试时变量被外层展开造成的假象（见阶段 1 笔记）。

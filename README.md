# mydocker

从零用 C 实现一个简易容器运行时，学习 Docker 的底层原理。
参考《自己动手写 Docker》与 lixd/mydocker 博客系列的思路，代码独立实现。

## 环境

- WSL2 Ubuntu 26.04 / 内核 6.18 / cgroup **v2**（教程多为 v1，本项目按 v2 实现）

## 当前进度

- [x] 阶段 0：环境验证（[笔记](docs/00-stage0-env.md)）
- [x] 阶段 1：clone + Namespace 隔离进程（[笔记](docs/01-stage1-namespace.md)）
- [ ] 阶段 2：rootfs + pivot_root + overlayfs
- [ ] 阶段 3：cgroup v2 资源限制
- [ ] 阶段 4：veth + bridge 容器网络
- [ ] 阶段 5：CLI 整合

## 用法（随阶段更新）

```bash
make            # 编译
sudo ./mydocker run bash        # 阶段 1：进入隔离 shell
./mydocker run --user bash      # 免 root 模式（借助 User Namespace）
```

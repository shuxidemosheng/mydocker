# 阶段 4：veth + bridge 容器网络（纯 rtnetlink 实现）

日期：2026-09-28

## 架构

```
宿主 netns                              容器 netns（CLONE_NEWNET）
┌─────────────────────┐                ┌──────────────────┐
│  eth3 ... docker0   │                │   lo             │
│  mydocker0(172.20.0.1/24, bridge)    │   eth0(172.20.0.2/24)│
│    └─ vh<pid> ── veth pair ──────     │     ↑ rename+addr │
│  iptables MASQUERADE (出网 NAT)      │   default via .0.1│
└─────────────────────┘                └──────────────────┘
```

## 实现要点

1. **为什么用 rtnetlink 而不是 shell 出 `ip` 命令**：ip 命令本质也是往
   NETLINK_ROUTE 发消息；自己组消息包省掉中间人、无外部依赖、错误可精确归因。
   消息格式 `[nlmsghdr][ifinfomsg/rtmsg][rtattr...]`，属性 4 字节对齐。
2. **veth 一次消息造两端**：`IFLA_LINKINFO{INFO_KIND "veth", INFO_DATA{PEER{...}}}`，
   PEER 里内嵌完整 ifinfomsg + IFLA_IFNAME。
3. **跨 netns 迁移**：`IFLA_NET_NS_PID` 把 veth 一端塞进目标进程的 netns。
   容器端随后重命名为 eth0（复用名字 eth0 会与宿主冲突，先叫 vc<pid>）。
4. **NAT**：`iptables -t nat -A POSTROUTING -s 172.20.0.0/24 ! -o mydocker0
   -j MASQUERADE`（netfilter 无法用 rtnetlink 配置，这条保留为脚本/手动步骤）+
   开 `/proc/sys/net/ipv4/ip_forward`。
5. **网桥幂等创建**：`if_nametoindex("mydocker0")==0` 才创建，多容器共享一桥。

## 两个有普遍价值的调试记录

1. **clone 后写结构体字段子进程看不见**（fork 语义）：veth 名字原本 clone 后
   由父进程写进共享的 opts —— 但 clone 不带 CLONE_VM 时子进程是地址空间副本。
   表现为子进程拿到的字段为空 → 静默跳过网络配置。修复：名字在 clone 前生成
   （vc<父pid>，唯一且 ≤15 字节 IFNAMSIZ 限制）。
2. **两阶段管道握手竞态**：先写名字再写 'g' 两次 write，子进程第一次 read
   可能一次吞掉两个消息 → 第二个 read 撞 EOF。管道是字节流不保消息边界。
   修复：协议减到只有一个信号。

## 实测记录

- 容器 → 网桥：ping 172.20.0.1，0% loss ✓
- 宿主 → 容器：ping 172.20.0.2，0% loss ✓
- 容器 → 外网：NAT 后 ping 223.5.5.5 有回包 ✓（busybox ping 的丢包率显示
  是它自己的整数下溢 bug，RTT 数据真实）
- 容器内 busybox httpd -p 80，宿主 `curl http://172.20.0.2/` 返回页面 ✓

## 已知边界

- `--net` 必须 root（操作宿主真实网络栈）；--user 模式 netns 里没有宿主桥的
  管理权，rootless 网络要做 slirp4netns/pasta 那套用户态网络，超出本项目范围。

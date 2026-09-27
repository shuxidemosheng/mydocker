/*
 * netlink.h —— rtnetlink 薄封装（阶段 4 容器网络用）
 *
 * 为什么用 rtnetlink 而不是 system("ip ...")：
 *   1) ip 命令本质也是往 NETLINK_ROUTE socket 发消息，我们只是省掉中间人；
 *   2) 容器工具链不应拼接 shell 命令串（安全 + 无外部依赖）；
 *   3) 每个 helper 的失败可以精确归因。
 */
#ifndef MYDOCKER_NETLINK_H
#define MYDOCKER_NETLINK_H

#include <sys/types.h>

/* 创建一个名为 name 的软件交换机（bridge） */
int nl_link_create_bridge(const char *name);

/* 创建一对 veth 虚拟网线（在宿主 netns 里成对出现） */
int nl_link_create_veth(const char *host_end, const char *peer_end);

/* 把接口 ifname 迁移到 pid 所在的 Network Namespace */
int nl_link_set_ns(const char *ifname, pid_t target);

/* 把接口挂到 bridge 上（IFLA_MASTER） */
int nl_link_set_master(const char *ifname, const char *bridge);

/* 拉起接口（IFF_UP） */
int nl_link_up(const char *ifname);

/* 在当前 netns 里重命名接口 */
int nl_link_rename(const char *ifname, const char *newname);

/* 给接口加 IPv4 地址/前缀（如 172.20.0.2/24） */
int nl_addr_add(const char *ifname, const char *ip, int plen);

/* 加默认路由：via gw dev oif */
int nl_route_add_default(const char *gw, const char *oif);

#endif

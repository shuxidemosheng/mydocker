/*
 * netlink.c —— rtnetlink 实现
 *
 * rtnetlink 是内核暴露"链路/地址/路由"管理能力的 netlink 子系统
 * （NETLINK_ROUTE）。消息格式：[nlmsghdr][协议头][一串 rtattr 属性]。
 * 所有操作走同一个"发消息 -> 等 ACK/NLMSG_ERROR 回执"的 nl_talk 流程。
 *
 * 注意属性对齐规则：rtattr 与 nlmsghdr 都要 4 字节对齐（RTA_ALIGN/NLMSG_ALIGN）。
 */
#include "netlink.h"

#include <errno.h>
#include <net/if.h>                     /* IFNAMSIZ, if_nametoindex() */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_link.h>              /* IFLA_* */
#include <linux/rtnetlink.h>            /* RTM_*, ifinfomsg, rtmsg */
#include <linux/veth.h>                 /* VETH_INFO_PEER */
#include <arpa/inet.h>                  /* inet_pton() */

#define NL_BUF_SIZE 8192

/* 往消息尾追加一个 rtattr 属性；返回属性指针，空间不足返回 NULL */
static struct rtattr *rta_push(struct nlmsghdr *n, int type,
                               const void *data, size_t len)
{
    size_t pad = RTA_LENGTH(len);
    if (NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(pad) > NL_BUF_SIZE) return NULL;
    struct rtattr *a = (struct rtattr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
    a->rta_type = type;
    a->rta_len = (unsigned short)pad;
    if (len) memcpy(RTA_DATA(a), data, len);
    n->nlmsg_len = (unsigned int)(NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(pad));
    return a;
}

/* 嵌套属性（属性里再套属性）：先 push 一个空属性记下偏移，填完回填总长 */
static size_t rta_nested_begin(struct nlmsghdr *n, int type)
{
    size_t off = NLMSG_ALIGN(n->nlmsg_len);
    rta_push(n, type, NULL, 0);
    return off;
}
static void rta_nested_end(struct nlmsghdr *n, size_t off)
{
    struct rtattr *a = (struct rtattr *)((char *)n + off);
    a->rta_type |= NLA_F_NESTED;
    a->rta_len = (unsigned short)(n->nlmsg_len - off);
}

/* 发送并等回执：内核对带 NLM_F_ACK 的请求回 error=0 的 NLMSG_ERROR 表示成功 */
static int nl_talk(struct nlmsghdr *n)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) { perror("mydocker: netlink socket"); return -1; }

    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    if (sendto(fd, n, n->nlmsg_len, 0,
               (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("mydocker: netlink send");
        close(fd);
        return -1;
    }

    char buf[NL_BUF_SIZE];
    int rc = 0;
    for (;;) {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        if (len < 0) { perror("mydocker: netlink recv"); rc = -1; break; }
        struct nlmsghdr *h = (struct nlmsghdr *)buf;
        for (; NLMSG_OK(h, (unsigned)len); h = NLMSG_NEXT(h, len)) {
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h);
                if (e->error) {
                    fprintf(stderr, "mydocker: netlink op %u: %s\n",
                            n->nlmsg_type, strerror(-e->error));
                    rc = -1;
                }
                goto out;                    /* error==0 即 ACK */
            }
            if (h->nlmsg_type == NLMSG_DONE) goto out;
        }
    }
out:
    close(fd);
    return rc;
}

/* —— 为避免抽象过度，直接逐个实现，消息构造一目了然 —— */

static int link_modify(unsigned type, unsigned flags, int ifindex,
                       unsigned set_flags, const char *name,
                       int master_ifindex, pid_t *to_ns)
{
    char buf[NL_BUF_SIZE] = {0};
    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    n->nlmsg_type = type;
    n->nlmsg_flags = NLM_F_REQUEST | flags;
    n->nlmsg_seq = 1;

    struct ifinfomsg *ifi = NLMSG_DATA(n);
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_index = ifindex;
    if (set_flags) {                       /* 要改 flags（如 IFF_UP）时，
                                              change 位标记哪些位生效 */
        ifi->ifi_flags = set_flags;
        ifi->ifi_change = set_flags;
    }
    n->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));

    if (name)   rta_push(n, IFLA_IFNAME, name, strlen(name) + 1);
    if (master_ifindex) rta_push(n, IFLA_MASTER, &master_ifindex, 4);
    if (to_ns)  rta_push(n, IFLA_NET_NS_PID, to_ns, sizeof(pid_t));

    return nl_talk(n);
}

static int idx(const char *ifname)
{
    unsigned i = if_nametoindex(ifname);
    if (i == 0) {
        fprintf(stderr, "mydocker: interface %s: %s\n", ifname, strerror(errno));
        return -1;
    }
    return (int)i;
}

int nl_link_create_bridge(const char *name)
{
    char buf[NL_BUF_SIZE] = {0};
    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    n->nlmsg_type = RTM_NEWLINK;
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    n->nlmsg_seq = 1;

    struct ifinfomsg *ifi = NLMSG_DATA(n);
    ifi->ifi_family = AF_UNSPEC;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));

    rta_push(n, IFLA_IFNAME, name, strlen(name) + 1);
    /* LINKINFO{ INFO_KIND "bridge" }：告诉内核这是 bridge 类型虚接口 */
    size_t li = rta_nested_begin(n, IFLA_LINKINFO);
    rta_push(n, IFLA_INFO_KIND, "bridge", 6);
    rta_nested_end(n, li);

    return nl_talk(n);
}

int nl_link_create_veth(const char *host_end, const char *peer_end)
{
    char buf[NL_BUF_SIZE] = {0};
    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    n->nlmsg_type = RTM_NEWLINK;
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    n->nlmsg_seq = 1;

    struct ifinfomsg *ifi = NLMSG_DATA(n);
    ifi->ifi_family = AF_UNSPEC;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(*ifi));

    rta_push(n, IFLA_IFNAME, host_end, strlen(host_end) + 1);
    /* LINKINFO{ INFO_KIND "veth", INFO_DATA{ PEER{ ifinfomsg + IFLA_IFNAME } } }
       PEER 里内嵌一个完整的 ifinfomsg —— veth 创建是"一次消息造两端" */
    size_t li = rta_nested_begin(n, IFLA_LINKINFO);
    rta_push(n, IFLA_INFO_KIND, "veth", 4);
    size_t id = rta_nested_begin(n, IFLA_INFO_DATA);
    size_t pp = rta_nested_begin(n, VETH_INFO_PEER);
    struct ifinfomsg *peer = (struct ifinfomsg *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
    /* 先给 PEER 预留 ifinfomsg 头的空间 */
    n->nlmsg_len = (unsigned int)(NLMSG_ALIGN(n->nlmsg_len) + NLMSG_ALIGN(sizeof(*peer)));
    memset(peer, 0, sizeof(*peer));
    peer->ifi_family = AF_UNSPEC;
    rta_push(n, IFLA_IFNAME, peer_end, strlen(peer_end) + 1);
    rta_nested_end(n, pp);
    rta_nested_end(n, id);
    rta_nested_end(n, li);

    return nl_talk(n);
}

int nl_link_set_ns(const char *ifname, pid_t target)
{
    int i = idx(ifname);
    if (i < 0) return -1;
    return link_modify(RTM_NEWLINK, NLM_F_ACK, i, 0, NULL, 0, &target);
}

int nl_link_set_master(const char *ifname, const char *bridge)
{
    int i = idx(ifname), b = idx(bridge);
    if (i < 0 || b < 0) return -1;
    return link_modify(RTM_NEWLINK, NLM_F_ACK, i, 0, NULL, b, NULL);
}

int nl_link_up(const char *ifname)
{
    int i = if_nametoindex(ifname);
    if (i == 0) {                          /* 容器内新 ns 里查不到是常见情况，报清楚 */
        fprintf(stderr, "mydocker: link %s not found: %s\n",
                ifname, strerror(errno));
        return -1;
    }
    return link_modify(RTM_NEWLINK, NLM_F_ACK, i, IFF_UP, NULL, 0, NULL);
}

int nl_link_rename(const char *ifname, const char *newname)
{
    int i = idx(ifname);
    if (i < 0) return -1;
    return link_modify(RTM_NEWLINK, NLM_F_ACK, i, 0, newname, 0, NULL);
}

int nl_addr_add(const char *ifname, const char *ip, int plen)
{
    int i = idx(ifname);
    if (i < 0) return -1;

    uint32_t addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        fprintf(stderr, "mydocker: bad ip %s\n", ip);
        return -1;
    }

    char buf[NL_BUF_SIZE] = {0};
    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    n->nlmsg_type = RTM_NEWADDR;
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    n->nlmsg_seq = 1;

    struct ifaddrmsg *ifa = NLMSG_DATA(n);
    ifa->ifa_family = AF_INET;
    ifa->ifa_prefixlen = (unsigned char)plen;
    ifa->ifa_index = (unsigned)i;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(*ifa));

    rta_push(n, IFA_LOCAL,  &addr, 4);     /* LOCAL=接口地址 */
    rta_push(n, IFA_ADDRESS, &addr, 4);    /* 点对点场景 ADDRESS=对端，这里同值 */

    return nl_talk(n);
}

int nl_route_add_default(const char *gw, const char *oif)
{
    uint32_t g;
    if (inet_pton(AF_INET, gw, &g) != 1) {
        fprintf(stderr, "mydocker: bad gw %s\n", gw);
        return -1;
    }
    int oi = idx(oif);
    if (oi < 0) return -1;

    char buf[NL_BUF_SIZE] = {0};
    struct nlmsghdr *n = (struct nlmsghdr *)buf;
    n->nlmsg_type = RTM_NEWROUTE;
    n->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL | NLM_F_ACK;
    n->nlmsg_seq = 1;

    struct rtmsg *rt = NLMSG_DATA(n);
    rt->rtm_family = AF_INET;
    rt->rtm_dst_len = 0;                   /* 目的 0.0.0.0/0 = 默认路由 */
    rt->rtm_table = RT_TABLE_MAIN;
    rt->rtm_protocol = RTPROT_BOOT;
    rt->rtm_scope = RT_SCOPE_UNIVERSE;     /* 有网关的路由 scope 必须是 UNIVERSE */
    rt->rtm_type = RTN_UNICAST;
    n->nlmsg_len = NLMSG_LENGTH(sizeof(*rt));

    rta_push(n, RTA_GATEWAY, &g, 4);
    rta_push(n, RTA_OIF, &oi, 4);

    return nl_talk(n);
}

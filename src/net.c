#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <termios.h>
#include <netdb.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/wait.h>

#include "hyper.h"
#include "util.h"
#include "parse.h"
#include "../config.h"

#define UEVENT_BUFFER_SIZE 512

void hyper_set_be32(uint8_t *buf, uint32_t val)
{
	buf[0] = val >> 24;
	buf[1] = val >> 16;
	buf[2] = val >> 8;
	buf[3] = val;
}

uint32_t hyper_get_be32(uint8_t *buf)
{
	return buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
}

void hyper_set_be64(uint8_t *buf, uint64_t val)
{
	hyper_set_be32(buf, val >> 32);
	hyper_set_be32(buf + 4, val);
}

uint64_t hyper_get_be64(uint8_t *buf)
{
	uint64_t v;

	v = (uint64_t) hyper_get_be32(buf) << 32;
	v |= hyper_get_be32(buf + 4);
	return v;
}

int hyper_send_data(int fd, uint8_t *data, uint32_t len)
{
	int length = 0, size;

	while (length < len) {
		size = write(fd, data + length, len - length);
		if (size <= 0) {
			if (errno == EINTR)
				continue;
			/* EAGAIN means unblock and the peer of virtio-ports is disappear */
			if (errno == EAGAIN)
				return 0;

			perror("send hyper data failed");
			return -1;
		}
#if WITH_VBOX
		tcdrain(fd);
#endif
		length += size;
	}

	return 0;
}

int hyper_send_msg(int fd, uint32_t type, uint32_t len,
		 uint8_t *message)
{
	uint8_t buf[8];

	fprintf(stdout, "hyper send type %d, len %d\n", type, len);

	hyper_set_be32(buf, type);
	hyper_set_be32(buf + 4, len + 8);

	if (hyper_send_data(fd, buf, 8) < 0)
		return -1;

	if (message && hyper_send_data(fd, message, len) < 0)
		return -1;

	return 0;
}

int hyper_send_type(int fd, uint32_t type)
{
	return hyper_send_msg(fd, type, 0, NULL);
}

int hyper_get_type(int fd, uint32_t *type)
{
	int len = 0, size;
	uint8_t buf[8];

	while (len < 8) {
		size = read(fd, buf + len, 8 - len);
		if (size <= 0) {
			if (errno == EINTR)
				continue;
			perror("wait for ack failed");
			return -1;
		}
		len += size;
	}

	*type = hyper_get_be32(buf);
	return 0;
}

int hyper_send_msg_block(int fd, uint32_t type, uint32_t len, uint8_t *data)
{
	int ret, flags;

	flags = hyper_setfd_block(fd);
	if (flags < 0) {
		fprintf(stderr, "%s fail to set fd block\n", __func__);
		return -1;
	}

	ret = hyper_send_msg(fd, type, len, data);

	if (fcntl(fd, F_SETFL, flags) < 0) {
		perror("restore fd flag failed");
		return -1;
	}

	return ret;
}

static int get_addr_ipv4(uint8_t *ap, const char *cp)
{
	int i;

	for (i = 0; i < 4; i++) {
		unsigned long n;
		char *endp;

		n = strtoul(cp, &endp, 0);
		if (n > 255)
			return -1;      /* bogus network value */

		if (endp == cp) /* no digits */
			return -1;
		ap[i] = n;

		if (*endp == '\0')
			break;

		if (i == 3 || *endp != '.')
			return -1;      /* extra characters */

		cp = endp + 1;
	}

	return 1;
}

static int addattr_l(struct nlmsghdr *n, int maxlen, int type, void *data, int alen)
{
	int len = RTA_LENGTH(alen);
	struct rtattr *rta;

	if (NLMSG_ALIGN(n->nlmsg_len) + len > maxlen)
		return -1;
	rta = (struct rtattr *)(((char *)n) + NLMSG_ALIGN(n->nlmsg_len));
	rta->rta_type = type;
	rta->rta_len = len;
	memcpy(RTA_DATA(rta), data, alen);
	n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + len;
	return 0;
}


static int free_uevent(struct uevent *ue){

	if(! ue){ return -1; }

	free_if_set(ue->action);
	free_if_set(ue->modalias);
	free_if_set(ue->driver);
	free_if_set(ue->devpath);
	free_if_set(ue->interface);

	return 0;
}

static int parse_uevent(const char *msg, struct uevent *ue, int len)
{
        if( !(msg && ue) ) {
                return -1;
        }

        if( len < 0 ) {
                return -1;
        }

	struct udev_label{
		const char* label;
		char** var;
	} labels [] = {
		{ "ACTION=", &(ue->action)},
		{ "MODALIAS=", &(ue->modalias)},
		{ "DRIVER=", &(ue->driver)},
		{ "DEVPATH=", &(ue->devpath)},
		{ "INTERFACE=", &(ue->interface)},
		{ NULL }
	};

        while (*msg) {

		for (struct udev_label* l=labels;  l && l->label ; l++) {
			if (!*l->var && !strncmp(msg, l->label, strlen(l->label))) {
				msg += strlen(l->label);
				*l->var= strdup(msg);
			}
		}
                /* advance to after the next \0 */
                while (*msg++);
        }

	return 0;
}



static int wait_for_nic(const char *nic_name)
{
	struct sockaddr_nl nls;
	char msg[UEVENT_BUFFER_SIZE];
	int fd = -1;
	int ret = -1;
	char path[PATH_MAX];
	struct uevent ue = {0};

	if (! (nic_name && nic_name[0])) {
		return 0;
	}

	sprintf(path, "/sys/class/net/%s/ifindex", nic_name);

	if ( access( path, F_OK ) != -1 ) {
		fprintf(stdout, "nic %s already exists\n", nic_name);
		return 0;
	}

	memset(&nls, 0, sizeof(struct sockaddr_nl));
	nls.nl_family = AF_NETLINK;
	nls.nl_pid = getpid();
	nls.nl_groups = -1;

	fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (fd==-1){
		perror("Open socket failed\n");
		return -1;
	}


	if (bind(fd, (void *)&nls, sizeof(struct sockaddr_nl))) {
		perror("Bind failed\n");
		goto out;
	}

	if ( access( path, F_OK ) != -1 ) {
		ret = 0;
		fprintf(stdout, "nic %s detected \n", nic_name);
		goto out;
	}

	while (1){
		int len = recv(fd, msg, sizeof(msg), 0);
		if (parse_uevent(msg, &ue, len) < 0) {
			goto out;
		}

		if(! (ue.action && ue.interface)) {
			free_uevent(&ue);
			continue;
		}

		if (strcmp(ue.interface, nic_name) == 0 ){
			fprintf(stdout, "nic %s detected\n", nic_name);
			free_uevent(&ue);
			ret = 0;
			goto out;
		}
		free_uevent(&ue);

	}
out:
	close(fd);
	return ret;
}

static int hyper_get_ifindex(char *nic)
{
	int fd, ifindex = -1;
	char path[512], buf[8];
	ssize_t bytes_read;

	fprintf(stdout, "net device %s\n", nic);
	sprintf(path, "/sys/class/net/%s/ifindex", nic);
	fprintf(stdout, "net device sys path is %s\n", path);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("can not open file");
		return -1;
	}

	memset(buf, 0, sizeof(buf));
	bytes_read = read(fd, buf, sizeof(buf) - 1);
	if (bytes_read <= 0) {
		perror("can read open file");
		goto out;
	}

	ifindex = atoi(buf);
	fprintf(stdout, "get ifindex %d\n", ifindex);
out:
	close(fd);
	return ifindex;
}

static int netlink_open(struct rtnl_handle *rth)
{
	memset(rth, 0, sizeof(*rth));

	rth->fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (rth->fd < 0) {
		perror("cannot open netlink socket");
		return -1;
	}

	rth->local.nl_family = AF_NETLINK;
	rth->local.nl_groups = 0;

	if (bind(rth->fd, (struct sockaddr *)&rth->local, sizeof(rth->local)) < 0) {
		perror("cannot bind netlink socket");
		goto out;
	}

	rth->seq = 0;
	return 0;
out:
	close(rth->fd);
	return -1;
}

static void netlink_close(struct rtnl_handle *rth)
{
	if (rth->fd > 0)
		close(rth->fd);
	rth->fd = -1;
}

static int rtnl_talk(struct rtnl_handle *rtnl,
		     struct nlmsghdr *n, pid_t peer,
		     unsigned groups, struct nlmsghdr *answer)
{
	int status;
	struct sockaddr_nl nladdr;
	struct iovec iov = { (void *)n, n->nlmsg_len };
	struct msghdr msg = { (void *)&nladdr, sizeof(nladdr), &iov, 1, NULL, 0, 0 };

	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;
	nladdr.nl_pid = peer;
	nladdr.nl_groups = groups;
	n->nlmsg_seq = ++rtnl->seq;
	if (answer == NULL)
		n->nlmsg_flags |= NLM_F_ACK;

	status = sendmsg(rtnl->fd, &msg, 0);
	if (status < 0)
		return -1;

	return 0;
}

static int hyper_up_nic(struct rtnl_handle *rth, int ifindex)
{
	struct {
		struct nlmsghdr n;
		struct ifinfomsg i;
		char buf[1024];
	} req;

	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.n.nlmsg_flags = NLM_F_REQUEST;
	req.n.nlmsg_type = RTM_NEWLINK;
	req.i.ifi_family = AF_UNSPEC;
	req.i.ifi_change |= IFF_UP;
	req.i.ifi_flags |= IFF_UP;
	req.i.ifi_index = ifindex;

	if (rtnl_talk(rth, &req.n, 0, 0, NULL) < 0)
		return -1;

	return 0;
}

static int hyper_remove_nic(char *device)
{
	char path[256], real[128];
	int fd;
	ssize_t size;

	sprintf(path, "/sys/class/net/%s", device);

	size = readlink(path, real, 128);
	if (size < 0 || size > 127) {
		perror("fail to read link directory");
		return -1;
	}

	real[size] = '\0';
	sprintf(path, "/sys/%s/../../../remove", real + 5);

	fprintf(stdout, "get net sys path %s\n", path);

	fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror("open file failed");
		return -1;
	}

	if (write(fd, "1\n", 2) < 0) {
		perror("write 1 to file failed");
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}

static int hyper_down_nic(struct rtnl_handle *rth, int ifindex)
{
	struct {
		struct nlmsghdr n;
		struct ifinfomsg i;
		char buf[1024];
	} req;

	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.n.nlmsg_flags = NLM_F_REQUEST;
	req.n.nlmsg_type = RTM_NEWLINK;
	req.i.ifi_family = AF_UNSPEC;
	req.i.ifi_change |= IFF_UP;
	req.i.ifi_flags &= ~IFF_UP;
	req.i.ifi_index = ifindex;

	if (rtnl_talk(rth, &req.n, 0, 0, NULL) < 0)
		return -1;

	return 0;
}

static int mask2bits(uint32_t netmask)
{
	unsigned bits = 0;
	uint32_t mask = ntohl(netmask);
	uint32_t host = ~mask;

	/* a valid netmask must be 2^n - 1 */
	if ((host & (host + 1)) != 0)
		return -1;

	for (; mask; mask <<= 1)
		++bits;

	return bits;
}

static int get_netmask(unsigned *val, const char *addr)
{
	char *ptr;
	unsigned long res;
	uint32_t data;
	int b;

	res = strtoul(addr, &ptr, 0);

	if (!ptr || ptr == addr || *ptr)
		goto get_addr;

	if (res == ULONG_MAX && errno == ERANGE)
		goto get_addr;

	if (res > UINT_MAX)
		goto get_addr;

	*val = res;
	return 0;

get_addr:
	if (get_addr_ipv4((uint8_t *)&data, addr) <= 0)
		return -1;

	b = mask2bits(data);
	if (b < 0)
		return -1;

	*val = b;
	return 0;
}

static int hyper_setup_route(struct rtnl_handle *rth,
			   struct hyper_route *rt)
{
	uint32_t data;
	struct {
		struct nlmsghdr n;
		struct rtmsg r;
		char buf[1024];
	} req;

	if (!rt->dst) {
		fprintf(stderr, "route dest is null\n");
		return -1;
	}

	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	req.n.nlmsg_flags = NLM_F_CREATE | NLM_F_EXCL | NLM_F_REQUEST;
	req.n.nlmsg_type = RTM_NEWROUTE;

	req.r.rtm_family = AF_INET;
	req.r.rtm_table = RT_TABLE_MAIN;
	req.r.rtm_scope = RT_SCOPE_UNIVERSE;
	req.r.rtm_type = RTN_UNICAST;
	req.r.rtm_protocol = RTPROT_BOOT;
	req.r.rtm_dst_len = 0;

	if (rt->gw) {
		if (get_addr_ipv4((uint8_t *)&data, rt->gw) <= 0) {
			fprintf(stderr, "get gw failed\n");
			return -1;
		}

		if (addattr_l(&req.n, sizeof(req), RTA_GATEWAY, &data, 4)) {
			fprintf(stderr, "setup gateway attr failed\n");
			return -1;
		}
	}

	if (rt->device) {
		int ifindex = hyper_get_ifindex(rt->device);
		if (ifindex < 0) {
			fprintf(stderr, "failed to get the ifindix of %s\n", rt->device);
			return -1;
		}

		if (addattr_l(&req.n, sizeof(req), RTA_OIF, &ifindex, 4)) {
			fprintf(stderr, "setup oif attr failed\n");
			return -1;
		}
	}

	if (strcmp(rt->dst, "default") && strcmp(rt->dst, "any") && strcmp(rt->dst, "all")) {
		unsigned mask;
		char *slash = strchr(rt->dst, '/');

		req.r.rtm_dst_len = 32;

		if (slash)
			*slash = 0;

		if (get_addr_ipv4((uint8_t *)&data, rt->dst) <= 0) {
			fprintf(stderr, "get dst failed\n");
			return -1;
		}

		if (addattr_l(&req.n, sizeof(req), RTA_DST, &data, 4)) {
			fprintf(stderr, "setup gateway attr failed\n");
			return -1;
		}

		if (slash) {
			if (get_netmask(&mask, slash + 1) < 0) {
				fprintf(stderr, "get netmask failed\n");
				return -1;
			}
			req.r.rtm_dst_len = mask;
			*slash = '/';
		}
	}

	if (rtnl_talk(rth, &req.n, 0, 0, NULL) < 0) {
		fprintf(stderr, "rtnl talk failed\n");
		return -1;
	}

	return 0;
}

static int hyper_cleanup_route(struct rtnl_handle *rth, struct hyper_route *rt)
{
	uint32_t data;
	struct {
		struct nlmsghdr n;
		struct rtmsg r;
		char buf[1024];
	} req;

	if (!rt->dst) {
		fprintf(stderr, "route dest is null\n");
		return -1;
	}

	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST;
	req.n.nlmsg_type = RTM_DELROUTE;

	req.r.rtm_family = AF_INET;
	req.r.rtm_table = RT_TABLE_MAIN;
	req.r.rtm_scope = RT_SCOPE_UNIVERSE;
	req.r.rtm_type = RTN_UNICAST;
	req.r.rtm_protocol = RTPROT_BOOT;
	req.r.rtm_dst_len = 0;

	if (rt->gw) {
		if (get_addr_ipv4((uint8_t *)&data, rt->gw) <= 0) {
			fprintf(stderr, "get gw failed\n");
			return -1;
		}

		if (addattr_l(&req.n, sizeof(req), RTA_GATEWAY, &data, 4)) {
			fprintf(stderr, "setup gateway attr failed\n");
			return -1;
		}
	}

	if (rt->device) {
		int ifindex = hyper_get_ifindex(rt->device);
		if (ifindex < 0) {
			fprintf(stderr, "failed to get the ifindix of %s\n", rt->device);
			return -1;
		}

		if (addattr_l(&req.n, sizeof(req), RTA_OIF, &ifindex, 4)) {
			fprintf(stderr, "setup oif attr failed\n");
			return -1;
		}
	}

	if (strcmp(rt->dst, "default") && strcmp(rt->dst, "any") && strcmp(rt->dst, "all")) {
		unsigned mask;
		char *slash = strchr(rt->dst, '/');

		req.r.rtm_dst_len = 32;

		if (slash)
			*slash = 0;

		if (get_addr_ipv4((uint8_t *)&data, rt->dst) <= 0) {
			fprintf(stderr, "get dst failed\n");
			return -1;
		}

		if (addattr_l(&req.n, sizeof(req), RTA_DST, &data, 4)) {
			fprintf(stderr, "setup gateway attr failed\n");
			return -1;
		}

		if (slash) {
			if (get_netmask(&mask, slash + 1) < 0) {
				fprintf(stderr, "get netmask failed\n");
				return -1;
			}
			req.r.rtm_dst_len = mask;
			*slash = '/';
		}
	}

	if (rtnl_talk(rth, &req.n, 0, 0, NULL) < 0) {
		fprintf(stderr, "rtnl talk failed\n");
		return -1;
	}

	return 0;
}

static int hyper_set_interface_attr(struct rtnl_handle *rth,
				int ifindex,
				void *data,
				int len,
				int type)
{
	struct {
		struct nlmsghdr n;
		struct ifinfomsg i;
		char buf[1024];
	} req;

	if (!rth || ifindex < 0)
		return -1;

	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	req.n.nlmsg_flags = NLM_F_REQUEST;
	req.n.nlmsg_type = RTM_SETLINK;

	req.i.ifi_family = AF_UNSPEC;
	req.i.ifi_change = 0xFFFFFFFF;
	req.i.ifi_index = ifindex;

	if (addattr_l(&req.n, sizeof(req), type,
			data,
			len)) {
                fprintf(stderr, "setup attr failed\n");
                return -1;
        }

	if (rtnl_talk(rth, &req.n, 0, 0, NULL) < 0){
		perror("rtnl_talk failed");
		return -1;
	}

	return 0;
}

static int hyper_set_interface_name(struct rtnl_handle *rth,
				int ifindex,
				char *new_device_name)
{
	if ( !rth || ifindex < 0 || !new_device_name) {
		return -1;
	}
	return hyper_set_interface_attr(rth, ifindex, 
				new_device_name,
				strlen(new_device_name)+1,
				IFLA_IFNAME);
}

static int hyper_set_interface_mtu(struct rtnl_handle *rth,
				int ifindex,
				unsigned int mtu)
{
	if (!rth || ifindex < 0) {
		return -1;
	}
	return hyper_set_interface_attr(rth, ifindex, &mtu,
				sizeof(mtu),
				IFLA_MTU);
}

/*!
 * Check hardware address related to the provided network interface name
 * matches the expected hardware address. It expects EUI-48 MAC addresses.
 * The socket has already been opened by the caller, preventing from
 * successive open/close in case of a loop using the same socket.
 *
 * \param fd socket opened by the caller.
 * \param mac_addr hardware address (EUI-48) to match with \p net_iface one.
 * \param net_iface network interface name.
 *
 * \note In case the function succeeds, it returns 0. In case the function
 * fails, it returns -1.
 */
static int hyper_check_net_iface_match_mac_addr(int fd,
						const char *mac_addr,
						const char *net_iface)
{
	struct ifreq ifr;
	char *tmp_mac_addr = NULL;
	int ret = -1;

	if (fd < 0) {
		fprintf(stderr, "invalid socket %d\n", fd);
		goto err;
	}

	if (!mac_addr || !*mac_addr) {
		fprintf(stderr, "invalid mac_addr\n");
		goto err;
	}

	if (!net_iface || !*net_iface) {
		fprintf(stderr, "invalid net_iface\n");
		goto err;
	}

	if (!strncpy(ifr.ifr_name, net_iface, strlen(net_iface) + 1)) {
		fprintf(stderr, "strncpy failed to copy interface"
			" name: %s\n", strerror(errno));
		goto err;
	}

	if (ioctl(fd, SIOCGIFHWADDR, &ifr) == -1) {
		fprintf(stderr, "ioctl SIOCGIFHWADDR failed: %s\n",
			strerror(errno));
		goto err;
	}

	if (asprintf(&tmp_mac_addr,
		     "%02x:%02x:%02x:%02x:%02x:%02x",
		     (uint8_t)ifr.ifr_hwaddr.sa_data[0],
		     (uint8_t)ifr.ifr_hwaddr.sa_data[1],
		     (uint8_t)ifr.ifr_hwaddr.sa_data[2],
		     (uint8_t)ifr.ifr_hwaddr.sa_data[3],
		     (uint8_t)ifr.ifr_hwaddr.sa_data[4],
		     (uint8_t)ifr.ifr_hwaddr.sa_data[5]) == -1) {
		fprintf(stderr, "failed to allocate mac_addr string\n");
		goto err;
	}

	if (!tmp_mac_addr || !*tmp_mac_addr) {
		fprintf(stderr, "invalid tmp_mac_addr\n");
		goto err;
	}

	if (strcasecmp(mac_addr, tmp_mac_addr)) {
		goto err;
	}

	ret = 0;

err:
	free(tmp_mac_addr);
	return ret;
} 

/*!
 * Check hardware address related to the provided network interface name
 * matches the expected hardware address. It expects EUI-48 MAC addresses.
 *
 * \param device network interface name.
 * \param mac_addr hardware address (EUI-48) to match with \p device one.
 *
 * \note In case the function succeeds, it returns 0. In case the function
 * fails, it returns -1.
 */
static int hyper_check_device_match_mac_addr(const char *mac_addr,
					     const char *device)
{
	int sock = -1;
	int ret = -1;

	if (!mac_addr || !*mac_addr) {
		fprintf(stderr, "invalid mac_addr\n");
		goto err;
	}

	if (!device || !*device) {
		fprintf(stderr, "invalid device\n");
		goto err;
	}

	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock == -1) {
		fprintf(stderr, "failed to get socket handle: %s\n",
			strerror(errno));
		goto err;
	}

	if (hyper_check_net_iface_match_mac_addr(sock, mac_addr, device)) {
		fprintf(stderr, "device mac address found does not match"
			" with the expected one %s\n", mac_addr);
		goto err;
	}

	ret = 0;

err:
	if (sock >= 0)
		close(sock);
	return ret;
}

/*!
 * Find network interface list, and retrieve the name of the one
 * matching the EUI-48 hardware address provided.
 *
 * \param mac_addr hardware address (EUI-48) to match.
 * \param[out] device name of the network interface matching \p mac_addr.
 *
 * \note In case the function succeeds, it returns 0 and \p device is
 * filled with the right network interface name. In case the function
 * fails, it returns -1 and \p device will point to \c NULL.
 */
static int hyper_get_iface_name_from_mac_addr(const char *mac_addr,
					      char **device)
{
	struct ifaddrs *ifaddr, *ifa;
	int sock = -1;
	int ret = -1;

	if (!mac_addr || !*mac_addr) {
		fprintf(stderr, "invalid mac_addr\n");
		goto err;
	}

	if (!device) {
		fprintf(stderr, "device pointer is NULL\n");
		goto err;
	}

	if (*device) {
		free(*device);
		*device = NULL;
	}

	if (getifaddrs(&ifaddr) == -1) {
		fprintf(stderr, "failed to get interface list: %s\n",
			strerror(errno));
		goto err;
	}

	sock = socket(PF_INET, SOCK_DGRAM, 0);
	if (sock == -1) {
		fprintf(stderr, "failed to get socket handle: %s\n",
			strerror(errno));
		goto err1;
	}

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL) {
			continue;
		}

		if (!hyper_check_net_iface_match_mac_addr(sock,
							  mac_addr,
							  ifa->ifa_name)) {
			*device = strdup((const char*) ifa->ifa_name);
			if (*device == NULL) {
				fprintf(stderr, "strdup failed\n");
				goto err1;
			}

			break;
		}

	}

	if (*device == NULL) {
		fprintf(stderr, "failed to find MAC address %s\n", mac_addr);
		goto err1;
	}

	ret = 0;

err1:
	freeifaddrs(ifaddr);
err:
	if (sock >= 0)
		close(sock);

	return ret;
}

static int hyper_setup_interface(struct rtnl_handle *rth,
			       struct hyper_interface *iface)
{
	uint8_t data[4];
	unsigned mask;
	struct {
		struct nlmsghdr n;
		struct ifaddrmsg ifa;
		char buf[256];
	} req;
	int ifindex;
	struct hyper_ipaddress *ip;

	if ((!iface->device && !iface->mac_addr) ||
	    list_empty(&iface->ipaddresses)) {
		fprintf(stderr, "interface information incorrect\n");
		return -1;
	}

	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL;
	req.n.nlmsg_type = RTM_NEWADDR;
	req.ifa.ifa_family = AF_INET;

	/* Check and wait for the interface to appear in /sys first if the 
	 * predictable name is provided. A process like systemd-udevd may be
	 * in the middle of changing the interface name to the provided
	 * predictable name.
	 * This needs to happen before any operations on the interface to
	 * reliably depend on its name.
	 */
	if (wait_for_nic(iface->device) < 0) {
		fprintf(stderr, "failed to wait for  %s\n", iface->device);
		return -1;
	}

	if (iface->device && iface->mac_addr &&
	    hyper_check_device_match_mac_addr(iface->mac_addr,
					      iface->device)) {
		fprintf(stderr, "failed to match device %s and mac_addr %s\n",
			iface->device, iface->mac_addr);
		return -1;
	} else if (!iface->device &&
	    hyper_get_iface_name_from_mac_addr(iface->mac_addr,
					       &iface->device)) {
		fprintf(stderr, "failed to get interface name from MAC"
			" address %s\n", iface->mac_addr);
		return -1;
	}

	ifindex = hyper_get_ifindex(iface->device);
	if (ifindex < 0) {
		fprintf(stderr, "failed to get the ifindix of %s\n", iface->device);
		return -1;
	}

	req.ifa.ifa_index = ifindex;
	req.ifa.ifa_scope = 0;

	list_for_each_entry(ip, &iface->ipaddresses, list) {
		if (get_addr_ipv4((uint8_t *)&data, ip->addr) <= 0) {
			fprintf(stderr, "get addr failed\n");
			return -1;
		}

		if (addattr_l(&req.n, sizeof(req), IFA_LOCAL, &data, 4)) {
			fprintf(stderr, "setup attr failed\n");
			return -1;
		}

		if (get_netmask(&mask, ip->mask) < 0) {
			fprintf(stderr, "get netamsk failed\n");
			return -1;
		}

		req.ifa.ifa_prefixlen = mask;
		fprintf(stdout, "interface get netamsk %d %s\n", req.ifa.ifa_prefixlen, ip->mask);
		if (rtnl_talk(rth, &req.n, 0, 0, NULL) < 0) {
			perror("rtnl_talk failed");
			return -1;
		}
	}

	if (iface->new_device_name && strcmp(iface->new_device_name, iface->device)) {
		fprintf(stdout, "Setting interface name to %s\n", iface->new_device_name);
		if (hyper_set_interface_name(rth, ifindex, iface->new_device_name) < 0) {
			fprintf(stderr, "Set interface name failed for interface %s\n",
					iface->device);
			return -1;
		}
		free(iface->device);
		iface->device = strdup(iface->new_device_name);
	}

	if (iface->mtu > 0) {
		fprintf(stdout, "Setting interface MTU to %d\n", iface->mtu);
		if (hyper_set_interface_mtu(rth, ifindex, iface->mtu) < 0) {
			fprintf(stderr, "set mtu failed for interface %s\n", 
					iface->device);
			return -1;
		}
	}

	if (hyper_up_nic(rth, ifindex) < 0) {
		fprintf(stderr, "up device %d failed\n", ifindex);
		return -1;
	}

	return 0;
}

static int hyper_cleanup_interface(struct rtnl_handle *rth,
				 struct hyper_interface *iface)
{
	uint8_t data[4];
	unsigned mask;
	struct {
		struct nlmsghdr n;
		struct ifaddrmsg ifa;
		char buf[256];
	} req;
	int ifindex;
	struct hyper_ipaddress *ip;

	if (!iface->device || list_empty(&iface->ipaddresses)) {
		fprintf(stderr, "interface information incorrect\n");
		return -1;
	}

	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
	req.n.nlmsg_flags = NLM_F_REQUEST;
	req.n.nlmsg_type = RTM_DELADDR;
	req.ifa.ifa_family = AF_INET;

	ifindex = hyper_get_ifindex(iface->device);
	if (ifindex < 0) {
		fprintf(stderr, "failed to get the ifindix of %s\n", iface->device);
		return -1;
	}

	req.ifa.ifa_index = ifindex;
	req.ifa.ifa_scope = 0;

	list_for_each_entry(ip, &iface->ipaddresses, list) {
		if (get_addr_ipv4((uint8_t *)&data, ip->addr) <= 0) {
			fprintf(stderr, "get addr failed\n");
			return -1;
		}

		if (addattr_l(&req.n, sizeof(req), IFA_LOCAL, &data, 4)) {
			fprintf(stderr, "setup attr failed\n");
			return -1;
		}

		if (get_netmask(&mask, ip->mask) < 0) {
			fprintf(stderr, "get netamsk failed\n");
			return -1;
		}

		req.ifa.ifa_prefixlen = mask;
		fprintf(stdout, "interface get netamsk %d %s\n", req.ifa.ifa_prefixlen, ip->mask);
		if (rtnl_talk(rth, &req.n, 0, 0, NULL) < 0) {
			perror("rtnl_talk failed");
			return -1;
		}
	}

	/* Don't down&remove lo device */
	if (strcmp(iface->device, "lo") == 0) {
		return 0;
	}

	if (hyper_down_nic(rth, ifindex) < 0) {
		fprintf(stderr, "up device %d failed\n", ifindex);
		return -1;
	}

	if (hyper_remove_nic(iface->device) < 0) {
		fprintf(stderr, "remove device %s failed\n", iface->device);
		return -1;
	}

	return 0;
}

int hyper_rescan(void)
{
	int fd = open("/sys/bus/pci/rescan", O_WRONLY);

	if (fd < 0) {
		perror("can not open rescan file");
		return -1;
	}

	if (write(fd, "1\n", 2) < 0) {
		perror("can not open rescan file");
		close(fd);
		return -1;
	}
	fprintf(stdout, "finish rescan\n");
	close(fd);
	return 0;
}

int hyper_setup_network(struct hyper_pod *pod)
{
	int i, ret = 0;
	struct hyper_interface *iface;
	struct hyper_route *rt;
	struct rtnl_handle rth;

	if (netlink_open(&rth) < 0)
		return -1;

	for (i = 0; i < pod->i_num; i++) {
		iface = &pod->iface[i];

		ret = hyper_setup_interface(&rth, iface);
		if (ret < 0) {
			fprintf(stderr, "link up device %s failed\n", iface->device);
			goto out;
		}
	}

	ret = hyper_up_nic(&rth, 1);
	if (ret < 0) {
		fprintf(stderr, "link up lo device failed\n");
		goto out;
	}

	for (i = 0; i < pod->r_num; i++) {
		rt = &pod->rt[i];

		ret = hyper_setup_route(&rth, rt);
		if (ret < 0) {
			fprintf(stderr, "setup route failed\n");
			goto out;
		}
	}

out:
	netlink_close(&rth);
	return ret;
}

void hyper_cleanup_network(struct hyper_pod *pod)
{
	int i;
	struct rtnl_handle rth;
	struct hyper_interface *iface;
	struct hyper_route *rt;

	if (netlink_open(&rth) < 0) {
		fprintf(stdout, "open netlink failed\n");
		return;
	}

	for (i = 0; i < pod->r_num; i++) {
		rt = &pod->rt[i];

		if (hyper_cleanup_route(&rth, rt) < 0)
			fprintf(stderr, "cleanup route failed\n");

		free(rt->dst);
		free(rt->gw);
		free(rt->device);
	}

	free(pod->rt);
	pod->rt = NULL;
	pod->r_num = 0;

	for (i = 0; i < pod->i_num; i++) {
		iface = &pod->iface[i];

		if (hyper_cleanup_interface(&rth, iface) < 0)
			fprintf(stderr, "link down device %s failed\n", iface->device);

		hyper_free_interface(iface);
	}

	free(pod->iface);
	pod->iface = NULL;
	pod->i_num = 0;
	netlink_close(&rth);
}

int hyper_cmd_setup_interface(char *json, int length)
{
	int ret = -1;
	struct hyper_interface *iface;
	struct rtnl_handle rth;

	if (hyper_rescan() < 0)
		return -1;

	if (netlink_open(&rth) < 0)
		return -1;


	iface = hyper_parse_setup_interface(json, length);
	if (iface == NULL) {
		fprintf(stderr, "parse interface failed\n");
		goto out;
	}
	ret = hyper_setup_interface(&rth, iface);
	if (ret < 0) {
		fprintf(stderr, "link up device %s failed\n", iface->device);
		goto out1;
	}
	ret = 0;
out1:
	hyper_free_interface(iface);
	free(iface);
out:
	netlink_close(&rth);
	return ret;
}

int hyper_cmd_setup_route(char *json, int length) {
	struct hyper_route *rts = NULL;
	int i, ret = -1;
	uint32_t r_num;
	struct rtnl_handle rth;

	if (netlink_open(&rth) < 0)
		return -1;

	if (hyper_parse_setup_routes(&rts, &r_num, json, length) < 0) {
		fprintf(stderr, "parse route failed\n");
		goto out;
	}

	for (i = 0; i < r_num; i++) {
		ret = hyper_setup_route(&rth, &rts[i]);
		if (ret < 0) {
			fprintf(stderr, "setup route failed\n");
			goto out;
		}
	}

	ret = 0;
out:
	netlink_close(&rth);
	free(rts);
	return ret;
}

int hyper_setup_dns(struct hyper_pod *pod)
{
	int i, fd, ret = -1;
	char buf[28];

	if (pod->dns == NULL)
		return 0;

	fd = open("/tmp/hyper/resolv.conf", O_CREAT| O_TRUNC| O_WRONLY, 0644);

	if (fd < 0) {
		perror("create /tmp/resolv.conf failed");
		return -1;
	}

	for (i = 0; i < pod->d_num; i++) {
		int size = snprintf(buf, sizeof(buf), "nameserver %s\n", pod->dns[i]);
		int len = 0, l;

		if (size < 0) {
			fprintf(stderr, "sprintf resolv.conf entry failed\n");
			goto out;
		}

		while (len < size) {
			l = write(fd, buf + len, size - len);
			if (l < 0) {
				perror("fail to write resolv.conf");
				goto out;
			}
			len += l;
		}
	}

	ret = 0;
out:
	close(fd);
	return ret;
}

void hyper_cleanup_dns(struct hyper_pod *pod)
{
	int fd, i;

	if (pod->dns == NULL)
		return;

	for (i = 0; i < pod->d_num; i++) {
		free(pod->dns[i]);
	}

	free(pod->dns);
	pod->dns = NULL;
	pod->d_num = 0;

	fd = open("/tmp/hyper/resolv.conf", O_WRONLY| O_TRUNC);
	if (fd < 0) {
		perror("open /tmp/hyper/resolv.conf failed");
		return;
	}

	close(fd);
}

int hyper_setup_iptables(struct hyper_pod *pod) {
	int pid, status;
	int pipefd[2] = {-1, -1};
	int pipe_parent_err[2] = {-1, -1};
	int ret = -1;
	size_t size, offset = 0;
	int pipe_size = 0;
	const char *cmd = "iptables-restore";

	if (! pod) {
		return -1;
	}

	if (! pod->iptable_rules) {
		return 0;
	}

	if (! *(pod->iptable_rules)) {
		free(pod->iptable_rules);
		return 0;
	}

	if (pipe(pipefd) == -1) {
		fprintf(stderr, "Error while creating pipe for iptables: %s\n",
				strerror(errno));
		goto err;
	}

	if (pipe2(pipe_parent_err, O_CLOEXEC) == -1) {
		fprintf(stderr, "Error while creating parent error pipe for "
				"iptables: %s\n",
				strerror(errno));
		goto err;
	}

	pid = fork();
	if ( pid == -1) {
		fprintf(stderr, "Failed to spawn child: %s\n", strerror(errno));
		goto err;
	}

	if (pid == 0) {
		char c;
		close_if_set(pipefd[1]);
		close_if_set(pipe_parent_err[1]);

		if (read(pipe_parent_err[0], &c, sizeof(c)) != 0) {
			fprintf(stderr, "Parent setup failed for command %s\n",
					cmd);
		}

		/* The rules are sent to the stdin of iptables-restore
		 */
		if (dup2(pipefd[0], STDIN_FILENO) == -1) {
			fprintf(stderr, "Dup call failed : %s\n",
					strerror(errno));
			goto err_child;
		}

		fprintf(stdout, "execing %s\n", cmd);
		if (execlp(cmd, cmd, "-v", NULL) == -1) {
			fprintf(stderr, "Exec call for %s failed :%s\n",
					cmd, strerror(errno));
		}

err_child:
		close_if_set(pipefd[0]);
		close_if_set(pipe_parent_err[0]);
		exit(EXIT_FAILURE);
	}

	//parent
	close_if_set(pipefd[0]);
	close_if_set(pipe_parent_err[0]);

	size = strlen(pod->iptable_rules);

	pipe_size = fcntl(pipefd[1], F_GETPIPE_SZ);
	if (pipe_size <= size) {
		if (fcntl(pipefd[1], F_SETPIPE_SZ, size+1) < 0) {
			fprintf(stderr, "failed to change pipe size: %s",
				strerror(errno));
			goto err_parent;
		}
	}

	while (offset < size) {
		ret = write(pipefd[1], pod->iptable_rules+offset, size-offset);
		if (ret < 0 && ret != EINTR) {
			break;
		}
		offset += ret;
	}

err_parent:
	if (offset < size) {
		fprintf(stderr, "Pipe Write err : %s\n", strerror(errno));
		if (write(pipe_parent_err[1], "E", 1) == -1) {
			fprintf(stderr, "Error writing to parent err pipe: "
				"%s\n", strerror(errno));
		}
	}

	close_if_set(pipe_parent_err[1]);
	close_if_set(pipefd[1]);

	if (waitpid(pid, &status, 0) <= 0) {
		fprintf(stderr, "Error waiting for child for %s: %s",
			 cmd, strerror(errno));
		goto err;
	}

	if (WIFEXITED(status)) {
		ret = WEXITSTATUS(status);
		fprintf(stdout, "Command %s exited normally, "
				"status %" PRIu8 "\n", 
				cmd, ret);
		if (ret == 0) {
			free(pod->iptable_rules);
			return 0;
		}
	}

	fprintf(stdout, "Command %s exit unexpectedly, "
			"status %" PRIu8 "\n",
			cmd, status);

err:
	close_if_set(pipefd[0]);
	close_if_set(pipefd[1]);
	close_if_set(pipe_parent_err[0]);
	close_if_set(pipe_parent_err[1]);
	free(pod->iptable_rules);
	return -1;
}

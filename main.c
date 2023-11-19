/*
 *
 * Copyright 2023 Kenichi Yasukata
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <syscall.h>
#include <sys/epoll.h>

#include <arpa/inet.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_bus_pci.h>

/* workaround to avoid conflicts between dpdk and lwip definitions */
#undef IP_DF
#undef IP_MF
#undef IP_RF
#undef IP_OFFMASK

#include <lwip/opt.h>
#include <lwip/init.h>
#include <lwip/pbuf.h>
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <lwip/tcpip.h>
#include <lwip/tcp.h>
#include <lwip/timeouts.h>
#include <lwip/prot/tcp.h>

#include <netif/ethernet.h>

typedef long (*syscall_fn_t)(long, long, long, long, long, long, long);

static syscall_fn_t next_sys_call = NULL;

#define MAX_PKT_BURST (32)
#define NUM_SLOT (256)

#define MEMPOOL_CACHE_SIZE (256)

#define PACKET_BUF_SIZE (1518)

static struct rte_mempool *pktmbuf_pool = NULL;
static int tx_idx = 0;
static struct rte_mbuf *tx_mbufs[MAX_PKT_BURST] = { 0 };

static void tx_flush(void)
{
	int xmit = tx_idx, xmitted = 0;
	while (xmitted != xmit)
		xmitted += rte_eth_tx_burst(0 /* port id */, 0 /* queue id */, &tx_mbufs[xmitted], xmit - xmitted);
	tx_idx = 0;
}

static err_t low_level_output(struct netif *netif __attribute__((unused)), struct pbuf *p)
{
	char buf[PACKET_BUF_SIZE];
	void *bufptr, *largebuf = NULL;
	if (sizeof(buf) < p->tot_len) {
		largebuf = (char *) malloc(p->tot_len);
		assert(largebuf);
		bufptr = largebuf;
	} else
		bufptr = buf;

	pbuf_copy_partial(p, bufptr, p->tot_len, 0);

	assert((tx_mbufs[tx_idx] = rte_pktmbuf_alloc(pktmbuf_pool)) != NULL);
	assert(p->tot_len <= RTE_MBUF_DEFAULT_BUF_SIZE);
	rte_memcpy(rte_pktmbuf_mtod(tx_mbufs[tx_idx], void *), bufptr, p->tot_len);
	rte_pktmbuf_pkt_len(tx_mbufs[tx_idx]) = rte_pktmbuf_data_len(tx_mbufs[tx_idx]) = p->tot_len;
	if (++tx_idx == MAX_PKT_BURST)
		tx_flush();

	if (largebuf)
		free(largebuf);
	return ERR_OK;
}

#define MAX_ACCEPT_FD (512)
#define MAX_RXPBUF (512)
#define MAX_FD (1024)

struct lwip_fd {
	char used;
	char close_posted;
	unsigned short num_accept_fd;
	int accept_fd[MAX_ACCEPT_FD];
	size_t tmp_pbuf_off;
	unsigned short num_rxpbuf;
	struct pbuf *rxpbuf[MAX_RXPBUF];
	struct tcp_pcb *tpcb;
	int epfd;
};

struct lwip_fd lfd[MAX_FD] = { 0 };

#define MAX_EPOLL_FD (512)

struct epoll_fd {
	char used;
	int num_fd;
	int fd[MAX_EPOLL_FD];
};

struct epoll_fd efd[MAX_FD] = { 0 };

static struct netif _netif = { 0 };

static int close_post_cnt = 0;
static int close_post_queue[MAX_FD] = { 0 };

static void dpdk_poll(void)
{
	struct rte_mbuf *rx_mbufs[MAX_PKT_BURST];
	unsigned short i, nb_rx = rte_eth_rx_burst(0 /* port id */, 0 /* queue id */, rx_mbufs, MAX_PKT_BURST);
	for (i = 0; i < nb_rx; i++) {
		{
			struct pbuf *p;
			assert((p = pbuf_alloc(PBUF_RAW, rte_pktmbuf_pkt_len(rx_mbufs[i]), PBUF_POOL)) != NULL);
			pbuf_take(p, rte_pktmbuf_mtod(rx_mbufs[i], void *), rte_pktmbuf_pkt_len(rx_mbufs[i]));
			p->len = p->tot_len = rte_pktmbuf_pkt_len(rx_mbufs[i]);
			assert(_netif.input(p, &_netif) == ERR_OK);
		}
		rte_pktmbuf_free(rx_mbufs[i]);
	}
	tx_flush();
	sys_check_timeouts();
}

static int lwip_syscall_close(int fd);

static void tcp_destroy_handeler(u8_t id __attribute__((unused)), void *data)
{
	int fd = (int) ((uintptr_t) data);
	{
		unsigned short i;
		for (i = 0; i < lfd[fd].num_rxpbuf; i++)
			pbuf_free(lfd[fd].rxpbuf[i]);
	}
	{
		unsigned short i;
		for (i = 0; i < lfd[fd].num_accept_fd; i++)
			lwip_syscall_close(lfd[fd].accept_fd[i]);
	}
	memset(&lfd[fd], 0, sizeof(lfd[fd]));
	asm volatile ("" ::: "memory");
	close(fd);
}

static const struct tcp_ext_arg_callbacks tcp_ext_arg_cbs =  {
	.destroy = tcp_destroy_handeler,
};

static void tcp_destroy_handeler_dummy(u8_t id __attribute__((unused)),
				       void *data __attribute__((unused)))
{

}

static const struct tcp_ext_arg_callbacks tcp_ext_arg_cbs_dummy =  {
	.destroy = tcp_destroy_handeler_dummy,
};

static err_t tcp_recv_handler(void *arg, struct tcp_pcb *tpcb,
			      struct pbuf *p, err_t err)
{
	if (err != ERR_OK)
		return err;
	if (!p) {
		tcp_close(tpcb);
		return ERR_OK;
	}
	lfd[(int)((uintptr_t) arg)].rxpbuf[lfd[(int)((uintptr_t) arg)].num_rxpbuf++] = p;
	return ERR_OK;
}

static err_t accept_handler(void *arg, struct tcp_pcb *tpcb, err_t err)
{
	if (err != ERR_OK)
		return err;

	{
		int newfd;
		assert((newfd = open("/dev/null", O_RDONLY)) != -1);
		lfd[newfd].used = 1;
		lfd[newfd].tpcb = tpcb;
		tcp_arg(tpcb, (void *)((uintptr_t) newfd));
		lfd[(int)((uintptr_t) arg)].accept_fd[lfd[(int)((uintptr_t) arg)].num_accept_fd++] = newfd;
	}
	tcp_recv(tpcb, tcp_recv_handler);
	tcp_setprio(tpcb, TCP_PRIO_MAX);

	tpcb->so_options |= SOF_KEEPALIVE;
	tpcb->keep_intvl = (60 * 1000);
	tpcb->keep_idle = (60 * 1000);
	tpcb->keep_cnt = 1;

	return err;
}

static err_t if_init(struct netif *netif)
{
	{
		struct rte_ether_addr ports_eth_addr;
		assert(rte_eth_macaddr_get(0 /* port id */, &ports_eth_addr) >= 0);
		memcpy(netif->hwaddr, ports_eth_addr.addr_bytes, 6);
	}
	assert(rte_eth_dev_get_mtu(0 /* port id */, &netif->mtu) >= 0); assert(netif->mtu <= PACKET_BUF_SIZE);
	netif->output = etharp_output;
	netif->linkoutput = low_level_output;
	netif->hwaddr_len = 6;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
	return ERR_OK;
}

static ssize_t lwip_syscall_read(int fd, char *buf, size_t count)
{
	if (!lfd[fd].num_rxpbuf) {
		dpdk_poll();
		if (!lfd[fd].num_rxpbuf)
			return -EAGAIN;
	}
	{
		unsigned short i; size_t c;
		for (i = 0, c = 0; i < lfd[fd].num_rxpbuf && c < count; i++) {
			struct pbuf *p = lfd[fd].rxpbuf[i];
			size_t l = ((count - c) < (p->tot_len - lfd[fd].tmp_pbuf_off) ? (count - c) : (p->tot_len - lfd[fd].tmp_pbuf_off));
			pbuf_copy_partial(p, &buf[c], l, lfd[fd].tmp_pbuf_off);
			c += l;
			if (p->tot_len != l) {
				assert(c == count);
				lfd[fd].tmp_pbuf_off = l;
			} else {
				tcp_recved(lfd[fd].tpcb, p->tot_len);
				pbuf_free(p);
			}
		}
		memmove(&lfd[fd].rxpbuf[0],
				&lfd[fd].rxpbuf[i - (lfd[fd].tmp_pbuf_off ? 1 : 0)],
				(i - (lfd[fd].tmp_pbuf_off ? 1 : 0)) * sizeof(struct pbuf *));
		lfd[fd].num_rxpbuf -= (i - (lfd[fd].tmp_pbuf_off ? 1 : 0));
		return c;
	}
}

static ssize_t lwip_syscall_write(int fd, const char *buf, size_t count)
{

	assert(tcp_sndbuf(lfd[fd].tpcb) >= count);
	assert(tcp_write(lfd[fd].tpcb, buf, count, TCP_WRITE_FLAG_COPY) == ERR_OK);
	assert(tcp_output(lfd[fd].tpcb) == ERR_OK);
	return count;
}

static int lwip_syscall_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);

static int lwip_syscall_close(int fd)
{
	if (!lfd[fd].close_posted) {
		assert(!lwip_syscall_epoll_ctl(lfd[fd].epfd, EPOLL_CTL_DEL, fd, NULL));
		close_post_queue[close_post_cnt++] = fd;
		lfd[fd].close_posted = 1;
	}
	return 0;
}

static int lwip_syscall_socket(int domain, int type, int protocol)
{
	if (domain == AF_INET
			&& type == SOCK_STREAM
			&& (protocol == 0 || protocol == IPPROTO_TCP)) {
		int fd;
		assert((fd = open("/dev/null", O_RDONLY)) != -1);
		lfd[fd].used = 1;
		assert((lfd[fd].tpcb = tcp_new()) != NULL);
		tcp_arg(lfd[fd].tpcb, (void *)((uintptr_t) fd));
		tcp_ext_arg_set_callbacks(lfd[fd].tpcb, 0, &tcp_ext_arg_cbs);
		tcp_ext_arg_set(lfd[fd].tpcb, 0, (void *) ((uintptr_t) fd));
		return fd;
	} else
		return next_sys_call(__NR_socket, domain, type, protocol, 0, 0, 0);
}

static int lwip_syscall_accept(int sockfd, struct sockaddr *addr __attribute__((unused)), socklen_t *addrlen __attribute__((unused)))
{
	if (!lfd[sockfd].num_accept_fd) {
		dpdk_poll();
		if (!lfd[sockfd].num_accept_fd)
			return -EAGAIN;
	}
	return lfd[sockfd].accept_fd[--lfd[sockfd].num_accept_fd];
}

static int lwip_syscall_bind(int sockfd, const struct sockaddr *addr __attribute__((unused)), socklen_t addrlen __attribute__((unused)))
{
	assert(tcp_bind(lfd[sockfd].tpcb,
			(const ip_addr_t *) &((const struct sockaddr_in *) addr)->sin_addr.s_addr,
			ntohs(((const struct sockaddr_in *) addr)->sin_port)) == ERR_OK);
	return 0;
}

static int lwip_syscall_listen(int sockfd, int backlog __attribute__((unused)))
{
	tcp_ext_arg_set_callbacks(lfd[sockfd].tpcb, 0, &tcp_ext_arg_cbs_dummy);
	assert((lfd[sockfd].tpcb = tcp_listen(lfd[sockfd].tpcb)) != NULL);
	tcp_arg(lfd[sockfd].tpcb, (void *)((uintptr_t) sockfd));
	tcp_ext_arg_set_callbacks(lfd[sockfd].tpcb, 0, &tcp_ext_arg_cbs);
	tcp_accept(lfd[sockfd].tpcb, accept_handler);
	return 0;
}

static int lwip_syscall_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event __attribute__((unused)))
{
	switch (op) {
	case EPOLL_CTL_ADD:
		efd[epfd].fd[efd[epfd].num_fd++] = fd;
		lfd[fd].epfd = epfd;
		break;
	case EPOLL_CTL_DEL:
		{
			int i;
			for (i = 0; i < efd[epfd].num_fd; i++) {
				if (efd[epfd].fd[i] == fd) {
					efd[epfd].fd[i] = efd[epfd].fd[--efd[epfd].num_fd];
					lfd[fd].epfd = 0;
					break;
				}
			}
		}
		break;
	default:
		assert(0);
		break;
	}
	return 0;
}

static int lwip_syscall_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
	int e = 0;
	{
		struct timespec start;
		assert(!clock_gettime(CLOCK_REALTIME, &start));
		while (1) {
			{
				int i;
				for (i = 0; i < efd[epfd].num_fd && !e && e < maxevents; i++) {
					if (lfd[efd[epfd].fd[i]].num_accept_fd || lfd[efd[epfd].fd[i]].num_rxpbuf) {
						struct epoll_event *ev = &events[e++];
						ev->data.fd = efd[epfd].fd[i];
						ev->events = EPOLLIN;
					}
				}
				if (e)
					break;
			}
			if (timeout >= 0) {
				struct timespec now;
				assert(!clock_gettime(CLOCK_REALTIME, &now));
				if (((unsigned long) timeout * 1000) <
						((now.tv_sec * 1000000000UL + now.tv_nsec) - 
						 (start.tv_sec * 1000000000UL + start.tv_nsec))) {
					break;
				}
			}
			dpdk_poll();
		}
	}
	return e;
}

static long lwip_syscall(long a1, long a2, long a3,
			 long a4,
			 long a5 __attribute__((unused)),
			 long a6 __attribute__((unused)),
			 long a7 __attribute__((unused)))
{
	long ret = 0;
	switch (a1) {
		case __NR_read: // 0
			ret = lwip_syscall_read((int) a2, (void *) a3, (size_t) a4);
			break;
		case __NR_write: // 1
			ret = lwip_syscall_write((int) a2, (const void *) a3, (size_t) a4);
			break;
		case __NR_close: // 3
			ret = lwip_syscall_close((int) a2);
			break;
		case __NR_ioctl: // 16
			ret = 0;
			break;
		case __NR_socket: // 41
			ret = lwip_syscall_socket((int) a2, (int) a3, (int) a4);
			break;
		case __NR_accept: // 43
		case __NR_accept4: // 288
			ret = lwip_syscall_accept((int) a2, (struct sockaddr *) a3, (socklen_t *) a4);
			break;
		case __NR_bind: // 49
			ret = lwip_syscall_bind((int) a2, (const struct sockaddr *) a3, (socklen_t) a4);
			break;
		case __NR_listen: // 50
			ret = lwip_syscall_listen((int) a2, (int) a3);
			break;
		case __NR_setsockopt: // 54
			ret = 0;
			break;
		case __NR_getsockopt: // 55
			ret = 0;
			break;
		case __NR_fcntl: // 72
			ret = 0;
			break;
		default:
			printf("unhandled %lu\n", a1);
			assert(0);
			break;
	}
	return ret;
}

static int lwip_syscall_epoll_close(int fd)
{
	memset(&efd[fd], 0, sizeof(efd[fd]));
	asm volatile ("" ::: "memory");
	close(fd);
	return 0;
}

static int lwip_syscall_epoll_create(int size __attribute__((unused)))
{
	int fd;
	assert((fd = open("/dev/null", O_RDONLY)) != -1);
	efd[fd].used = 1;
	return fd;
}

static long epoll_syscall(long a1, long a2, long a3,
			  long a4, long a5,
			  long a6 __attribute__((unused)),
			  long a7 __attribute__((unused)))
{
	long ret = 0;
	switch (a1) {
		case __NR_close: // 3
			ret = lwip_syscall_epoll_close((int) a2);
			break;
		case __NR_fcntl: // 72
			ret = 0;
			break;
		case __NR_epoll_create: // 213
			ret = lwip_syscall_epoll_create((int) a2);
			break;
		case __NR_epoll_ctl_old: // 214
			ret = lwip_syscall_epoll_ctl((int) a2, (int) a3, (int) a4, (struct epoll_event *) a5);
			break;
		case __NR_epoll_wait_old: // 215
			ret = lwip_syscall_epoll_wait((int) a2, (struct epoll_event *) a3, (int) a4, (int) a5);
			break;
		case __NR_epoll_wait: // 232
			ret = lwip_syscall_epoll_wait((int) a2, (struct epoll_event *) a3, (int) a4, (int) a5);
			break;
		case __NR_epoll_ctl: // 233
			ret = lwip_syscall_epoll_ctl((int) a2, (int) a3, (int) a4, (struct epoll_event *) a5);
			break;
		case __NR_epoll_create1: // 291
			ret = lwip_syscall_epoll_create((int) a2);
			break;
		default:
			printf("unhandled %lu\n", a1);
			assert(0);
			break;
	}
	return ret;
}

static long hook_function(long a1, long a2, long a3,
			  long a4, long a5, long a6,
			  long a7)
{
	switch (a1) {
		case __NR_socket: // 41
			return lwip_syscall(a1, a2, a3, a4, a5, a6, a7);
		case __NR_close: // 3
		case __NR_fcntl: // 72
			if (lfd[a2].used)
				return lwip_syscall(a1, a2, a3, a4, a5, a6, a7);
			if (efd[a2].used)
				return epoll_syscall(a1, a2, a3, a4, a5, a6, a7);
			return next_sys_call(a1, a2, a3, a4, a5, a6, a7);
		case __NR_read: // 0
		case __NR_write: // 1
		case __NR_ioctl: // 16
		case __NR_accept: // 43
		case __NR_bind: // 49
		case __NR_listen: // 50
		case __NR_setsockopt: // 54
		case __NR_getsockopt: // 55
		case __NR_accept4: //288
			if (lfd[a2].used)
				return lwip_syscall(a1, a2, a3, a4, a5, a6, a7);
			return next_sys_call(a1, a2, a3, a4, a5, a6, a7);
		case __NR_epoll_create: // 213
		case __NR_epoll_ctl_old: // 214
		case __NR_epoll_wait_old: // 215
		case __NR_epoll_wait: // 232
		case __NR_epoll_ctl: // 233
		case __NR_epoll_create1: // 291
			return epoll_syscall(a1, a2, a3, a4, a5, a6, a7);
		default:
			return next_sys_call(a1, a2, a3, a4, a5, a6, a7);
	}
}

int __hook_init(long placeholder __attribute__((unused)),
		void *sys_call_hook_ptr)
{
	if (!getenv("NET_ADDR"))
		return -1;
	if (!getenv("NET_MASK"))
		return -1;
	if (!getenv("NET_GATE"))
		return -1;
	if (!getenv("DPDK_ARGS"))
		return -1;

	/* setting up dpdk */
	{
		{
			int argc = 0;
			char **argv = NULL;
			char *argstr;
			assert((argstr = strdup(getenv("DPDK_ARGS"))) != NULL);
			{
				size_t l = strlen(argstr);
				int argvlen = 8;
				assert((argv = realloc(argv, sizeof(*argv) * argvlen)) != NULL);
				argv[argc++] = "app";
				{
					bool prev_space = true;
					{
						size_t i;
						for (i = 0; i < l; i++) {
							if (prev_space) {
								if (argstr[i] != ' ') {
									if (argvlen < argc + 2) {
										argvlen += 16;
										assert((argv = realloc(argv, sizeof(*argv) * argvlen)) != NULL);
									}
									argv[argc++] = &argstr[i];
									prev_space = false;
								} else
									argstr[i] = '\0';
							} else if (argstr[i] == ' ') {
								argstr[i] = '\0';
								prev_space = true;
							}
						}
					}
					argv[argc] = NULL;
				}
			}
			assert(rte_eal_init(argc, argv) >= 0);
			free(argv);
			free(argstr);
		}

		{
			uint16_t nb_rxd = NUM_SLOT;
			uint16_t nb_txd = NUM_SLOT;

			assert(rte_eth_dev_count_avail() == 1);

			assert((pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool",
							RTE_MAX(1 /* nb_ports */ * (nb_rxd + nb_txd + MAX_PKT_BURST + 1 * MEMPOOL_CACHE_SIZE), 8192),
							MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
							rte_socket_id())) != NULL);

			{
				struct rte_eth_dev_info dev_info;
				struct rte_eth_conf local_port_conf = { 0 };

				assert(rte_eth_dev_info_get(0 /* port id */, &dev_info) >= 0);

				assert(rte_eth_dev_configure(0 /* port id */, 1 /* num queues */, 1 /* num queues */, &local_port_conf) >= 0);

				assert(rte_eth_dev_adjust_nb_rx_tx_desc(0 /* port id */, &nb_rxd, &nb_txd) >= 0);

				assert(rte_eth_rx_queue_setup(0 /* port id */, 0 /* queue */, nb_rxd,
							rte_eth_dev_socket_id(0 /* port id */),
							&dev_info.default_rxconf,
							pktmbuf_pool) >= 0);

				assert(rte_eth_tx_queue_setup(0 /* port id */, 0 /* queue */, nb_txd,
							rte_eth_dev_socket_id(0 /* port id */),
							&dev_info.default_txconf) >= 0);

				assert(rte_eth_dev_start(0 /* port id */) >= 0);
				assert(rte_eth_promiscuous_enable(0 /* port id */) >= 0);
			}
		}
	}

	/* setting up lwip */
	{
		lwip_init();
		{
			ip4_addr_t _addr, _mask, _gate;
			inet_pton(AF_INET, getenv("NET_ADDR"), &_addr);
			inet_pton(AF_INET, getenv("NET_MASK"), &_mask);
			inet_pton(AF_INET, getenv("NET_GATE"), &_gate);
			assert(netif_add(&_netif, &_addr, &_mask, &_gate, NULL, if_init, ethernet_input) != NULL);
		}
		netif_set_default(&_netif);
		netif_set_link_up(&_netif);
		netif_set_up(&_netif);
	}

	next_sys_call = *((syscall_fn_t *) sys_call_hook_ptr);
	*((syscall_fn_t *) sys_call_hook_ptr) = hook_function;

	return 0;
}

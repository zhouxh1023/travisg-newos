/*
** Copyright 2001, Travis Geiselbrecht. All rights reserved.
** Distributed under the terms of the NewOS License.
*/
#ifndef _IF_H
#define _IF_H

#include <types.h>
#include <kernel/cbuf.h>
#include <kernel/queue.h>
#include <kernel/lock.h>
#include <kernel/net/net.h>
#include <sys/defines.h>

typedef struct ifaddr {
	struct ifaddr *next;
	struct ifnet *if_owner;
	netaddr addr;
	netaddr netmask;
	netaddr broadcast;
} ifaddr;

enum {
	IF_TYPE_NULL = 0,
	IF_TYPE_LOOPBACK,
	IF_TYPE_ETHERNET
};

typedef int if_id;

typedef struct ifnet {
	struct ifnet *next;
	if_id id;
	char path[SYS_MAX_PATH_LEN];
	int type;
	int fd;
	thread_id rx_thread;
	thread_id tx_thread;
	ifaddr *addr_list;
	ifaddr *link_addr;
	sem_id tx_queue_sem;
	mutex tx_queue_lock;
	fixed_queue tx_queue;
	uint8 tx_buf[2048];
	uint8 rx_buf[2048];
} ifnet;

int if_init(void);
ifnet *if_id_to_ifnet(if_id id);
ifnet *if_register_interface(const char *path, int type);
void if_bind_address(ifnet *i, ifaddr *addr);
void if_bind_link_address(ifnet *i, ifaddr *addr);
int if_boot_interface(ifnet *i);
int if_output(cbuf *b, ifnet *i);

#endif

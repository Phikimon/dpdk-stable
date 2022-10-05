/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2022 Microsoft Corporation
 */

#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <ethdev_driver.h>
#include <ethdev_pci.h>
#include <rte_kvargs.h>
#include <rte_eal_paging.h>

#include <infiniband/verbs.h>
#include <infiniband/manadv.h>

#include <assert.h>

#include "mana.h"

/* Shared memory between primary/secondary processes, per driver */
/* Data to track primary/secondary usage */
struct mana_shared_data *mana_shared_data;
static struct mana_shared_data mana_local_data;

/* The memory region for the above data */
static const struct rte_memzone *mana_shared_mz;
static const char *MZ_MANA_SHARED_DATA = "mana_shared_data";

/* Spinlock for mana_shared_data */
static rte_spinlock_t mana_shared_data_lock = RTE_SPINLOCK_INITIALIZER;

/* Allocate a buffer on the stack and fill it with a printf format string. */
#define MANA_MKSTR(name, ...) \
	int mkstr_size_##name = snprintf(NULL, 0, "" __VA_ARGS__); \
	char name[mkstr_size_##name + 1]; \
	\
	memset(name, 0, mkstr_size_##name + 1); \
	snprintf(name, sizeof(name), "" __VA_ARGS__)

int mana_logtype_driver;
int mana_logtype_init;

/*
 * Callback from rdma-core to allocate a buffer for a queue.
 */
void *
mana_alloc_verbs_buf(size_t size, void *data)
{
	void *ret;
	size_t alignment = rte_mem_page_size();
	int socket = (int)(uintptr_t)data;

	DRV_LOG(DEBUG, "size=%zu socket=%d", size, socket);

	if (alignment == (size_t)-1) {
		DRV_LOG(ERR, "Failed to get mem page size");
		rte_errno = ENOMEM;
		return NULL;
	}

	ret = rte_zmalloc_socket("mana_verb_buf", size, alignment, socket);
	if (!ret && size)
		rte_errno = ENOMEM;
	return ret;
}

void
mana_free_verbs_buf(void *ptr, void *data __rte_unused)
{
	rte_free(ptr);
}

static int
mana_dev_configure(struct rte_eth_dev *dev)
{
	struct mana_priv *priv = dev->data->dev_private;
	struct rte_eth_conf *dev_conf = &dev->data->dev_conf;

	if (dev_conf->rxmode.mq_mode & RTE_ETH_MQ_RX_RSS_FLAG)
		dev_conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_RSS_HASH;

	if (dev->data->nb_rx_queues != dev->data->nb_tx_queues) {
		DRV_LOG(ERR, "Only support equal number of rx/tx queues");
		return -EINVAL;
	}

	if (!rte_is_power_of_2(dev->data->nb_rx_queues)) {
		DRV_LOG(ERR, "number of TX/RX queues must be power of 2");
		return -EINVAL;
	}

	priv->num_queues = dev->data->nb_rx_queues;

	manadv_set_context_attr(priv->ib_ctx, MANADV_CTX_ATTR_BUF_ALLOCATORS,
				(void *)((uintptr_t)&(struct manadv_ctx_allocators){
					.alloc = &mana_alloc_verbs_buf,
					.free = &mana_free_verbs_buf,
					.data = 0,
				}));

	return 0;
}

static int mana_intr_uninstall(struct mana_priv *priv);

static int
mana_dev_start(struct rte_eth_dev *dev)
{
	int ret;
	struct mana_priv *priv = dev->data->dev_private;

	rte_spinlock_init(&priv->mr_btree_lock);
	ret = mana_mr_btree_init(&priv->mr_btree, MANA_MR_BTREE_CACHE_N,
				 dev->device->numa_node);
	if (ret) {
		DRV_LOG(ERR, "Failed to init device MR btree %d", ret);
		return ret;
	}

	ret = mana_start_tx_queues(dev);
	if (ret) {
		DRV_LOG(ERR, "failed to start tx queues %d", ret);
		goto failed_tx;
	}

	ret = mana_start_rx_queues(dev);
	if (ret) {
		DRV_LOG(ERR, "failed to start rx queues %d", ret);
		goto failed_rx;
	}

	rte_wmb();

	dev->tx_pkt_burst = mana_tx_burst;
	dev->rx_pkt_burst = mana_rx_burst;

	DRV_LOG(INFO, "TX/RX queues have started");

	/* Enable datapath for secondary processes */
	mana_mp_req_on_rxtx(dev, MANA_MP_REQ_START_RXTX);

	return 0;

failed_rx:
	mana_stop_tx_queues(dev);

failed_tx:
	mana_mr_btree_free(&priv->mr_btree);

	return ret;
}

static int
mana_dev_stop(struct rte_eth_dev *dev __rte_unused)
{
	int ret;

	dev->tx_pkt_burst = mana_tx_burst_removed;
	dev->rx_pkt_burst = mana_rx_burst_removed;

	/* Stop datapath on secondary processes */
	mana_mp_req_on_rxtx(dev, MANA_MP_REQ_STOP_RXTX);

	rte_wmb();

	ret = mana_stop_tx_queues(dev);
	if (ret) {
		DRV_LOG(ERR, "failed to stop tx queues");
		return ret;
	}

	ret = mana_stop_rx_queues(dev);
	if (ret) {
		DRV_LOG(ERR, "failed to stop tx queues");
		return ret;
	}

	return 0;
}

static int
mana_dev_close(struct rte_eth_dev *dev)
{
	struct mana_priv *priv = dev->data->dev_private;
	int ret;

	mana_remove_all_mr(priv);

	ret = mana_intr_uninstall(priv);
	if (ret)
		return ret;

	ret = ibv_close_device(priv->ib_ctx);
	if (ret) {
		ret = errno;
		return ret;
	}

	return 0;
}

static int
mana_dev_info_get(struct rte_eth_dev *dev,
		  struct rte_eth_dev_info *dev_info)
{
	struct mana_priv *priv = dev->data->dev_private;

	dev_info->max_mtu = RTE_ETHER_MTU;

	/* RX params */
	dev_info->min_rx_bufsize = MIN_RX_BUF_SIZE;
	dev_info->max_rx_pktlen = MAX_FRAME_SIZE;

	dev_info->max_rx_queues = priv->max_rx_queues;
	dev_info->max_tx_queues = priv->max_tx_queues;

	dev_info->max_mac_addrs = MANA_MAX_MAC_ADDR;
	dev_info->max_hash_mac_addrs = 0;

	dev_info->max_vfs = 1;

	/* Offload params */
	dev_info->rx_offload_capa = MANA_DEV_RX_OFFLOAD_SUPPORT;

	dev_info->tx_offload_capa = MANA_DEV_TX_OFFLOAD_SUPPORT;

	/* RSS */
	dev_info->reta_size = INDIRECTION_TABLE_NUM_ELEMENTS;
	dev_info->hash_key_size = TOEPLITZ_HASH_KEY_SIZE_IN_BYTES;
	dev_info->flow_type_rss_offloads = MANA_ETH_RSS_SUPPORT;

	/* Thresholds */
	dev_info->default_rxconf = (struct rte_eth_rxconf){
		.rx_thresh = {
			.pthresh = 8,
			.hthresh = 8,
			.wthresh = 0,
		},
		.rx_free_thresh = 32,
		/* If no descriptors available, pkts are dropped by default */
		.rx_drop_en = 1,
	};

	dev_info->default_txconf = (struct rte_eth_txconf){
		.tx_thresh = {
			.pthresh = 32,
			.hthresh = 0,
			.wthresh = 0,
		},
		.tx_rs_thresh = 32,
		.tx_free_thresh = 32,
	};

	/* Buffer limits */
	dev_info->rx_desc_lim.nb_min = MIN_BUFFERS_PER_QUEUE;
	dev_info->rx_desc_lim.nb_max = priv->max_rx_desc;
	dev_info->rx_desc_lim.nb_align = MIN_BUFFERS_PER_QUEUE;
	dev_info->rx_desc_lim.nb_seg_max = priv->max_recv_sge;
	dev_info->rx_desc_lim.nb_mtu_seg_max = priv->max_recv_sge;

	dev_info->tx_desc_lim.nb_min = MIN_BUFFERS_PER_QUEUE;
	dev_info->tx_desc_lim.nb_max = priv->max_tx_desc;
	dev_info->tx_desc_lim.nb_align = MIN_BUFFERS_PER_QUEUE;
	dev_info->tx_desc_lim.nb_seg_max = priv->max_send_sge;
	dev_info->rx_desc_lim.nb_mtu_seg_max = priv->max_recv_sge;

	/* Speed */
	dev_info->speed_capa = RTE_ETH_LINK_SPEED_100G;

	/* RX params */
	dev_info->default_rxportconf.burst_size = 1;
	dev_info->default_rxportconf.ring_size = MAX_RECEIVE_BUFFERS_PER_QUEUE;
	dev_info->default_rxportconf.nb_queues = 1;

	/* TX params */
	dev_info->default_txportconf.burst_size = 1;
	dev_info->default_txportconf.ring_size = MAX_SEND_BUFFERS_PER_QUEUE;
	dev_info->default_txportconf.nb_queues = 1;

	return 0;
}

static void
mana_dev_tx_queue_info(struct rte_eth_dev *dev, uint16_t queue_id,
		       struct rte_eth_txq_info *qinfo)
{
	struct mana_txq *txq = dev->data->tx_queues[queue_id];

	qinfo->conf.offloads = dev->data->dev_conf.txmode.offloads;
	qinfo->nb_desc = txq->num_desc;
}

static void
mana_dev_rx_queue_info(struct rte_eth_dev *dev, uint16_t queue_id,
		       struct rte_eth_rxq_info *qinfo)
{
	struct mana_rxq *rxq = dev->data->rx_queues[queue_id];

	qinfo->mp = rxq->mp;
	qinfo->nb_desc = rxq->num_desc;
	qinfo->conf.offloads = dev->data->dev_conf.rxmode.offloads;
}

static const uint32_t *
mana_supported_ptypes(struct rte_eth_dev *dev __rte_unused)
{
	static const uint32_t ptypes[] = {
		RTE_PTYPE_L2_ETHER,
		RTE_PTYPE_L3_IPV4_EXT_UNKNOWN,
		RTE_PTYPE_L3_IPV6_EXT_UNKNOWN,
		RTE_PTYPE_L4_FRAG,
		RTE_PTYPE_L4_TCP,
		RTE_PTYPE_L4_UDP,
		RTE_PTYPE_UNKNOWN
	};

	return ptypes;
}

static int
mana_rss_hash_update(struct rte_eth_dev *dev,
		     struct rte_eth_rss_conf *rss_conf)
{
	struct mana_priv *priv = dev->data->dev_private;

	/* Currently can only update RSS hash when device is stopped */
	if (dev->data->dev_started) {
		DRV_LOG(ERR, "Can't update RSS after device has started");
		return -ENODEV;
	}

	if (rss_conf->rss_hf & ~MANA_ETH_RSS_SUPPORT) {
		DRV_LOG(ERR, "Port %u invalid RSS HF 0x%" PRIx64,
			dev->data->port_id, rss_conf->rss_hf);
		return -EINVAL;
	}

	if (rss_conf->rss_key && rss_conf->rss_key_len) {
		if (rss_conf->rss_key_len != TOEPLITZ_HASH_KEY_SIZE_IN_BYTES) {
			DRV_LOG(ERR, "Port %u key len must be %u long",
				dev->data->port_id,
				TOEPLITZ_HASH_KEY_SIZE_IN_BYTES);
			return -EINVAL;
		}

		priv->rss_conf.rss_key_len = rss_conf->rss_key_len;
		priv->rss_conf.rss_key =
			rte_zmalloc("mana_rss", rss_conf->rss_key_len,
				    RTE_CACHE_LINE_SIZE);
		if (!priv->rss_conf.rss_key)
			return -ENOMEM;
		memcpy(priv->rss_conf.rss_key, rss_conf->rss_key,
		       rss_conf->rss_key_len);
	}
	priv->rss_conf.rss_hf = rss_conf->rss_hf;

	return 0;
}

static int
mana_rss_hash_conf_get(struct rte_eth_dev *dev,
		       struct rte_eth_rss_conf *rss_conf)
{
	struct mana_priv *priv = dev->data->dev_private;

	if (!rss_conf)
		return -EINVAL;

	if (rss_conf->rss_key &&
	    rss_conf->rss_key_len >= priv->rss_conf.rss_key_len) {
		memcpy(rss_conf->rss_key, priv->rss_conf.rss_key,
		       priv->rss_conf.rss_key_len);
	}

	rss_conf->rss_key_len = priv->rss_conf.rss_key_len;
	rss_conf->rss_hf = priv->rss_conf.rss_hf;

	return 0;
}

static int
mana_dev_tx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_idx,
			uint16_t nb_desc, unsigned int socket_id,
			const struct rte_eth_txconf *tx_conf __rte_unused)

{
	struct mana_priv *priv = dev->data->dev_private;
	struct mana_txq *txq;
	int ret;

	txq = rte_zmalloc_socket("mana_txq", sizeof(*txq), 0, socket_id);
	if (!txq) {
		DRV_LOG(ERR, "failed to allocate txq");
		return -ENOMEM;
	}

	txq->socket = socket_id;

	txq->desc_ring = rte_malloc_socket("mana_tx_desc_ring",
					   sizeof(struct mana_txq_desc) *
						nb_desc,
					   RTE_CACHE_LINE_SIZE, socket_id);
	if (!txq->desc_ring) {
		DRV_LOG(ERR, "failed to allocate txq desc_ring");
		ret = -ENOMEM;
		goto fail;
	}

	ret = mana_mr_btree_init(&txq->mr_btree,
				 MANA_MR_BTREE_PER_QUEUE_N, socket_id);
	if (ret) {
		DRV_LOG(ERR, "Failed to init TXQ MR btree");
		goto fail;
	}

	DRV_LOG(DEBUG, "idx %u nb_desc %u socket %u txq->desc_ring %p",
		queue_idx, nb_desc, socket_id, txq->desc_ring);

	txq->desc_ring_head = 0;
	txq->desc_ring_tail = 0;
	txq->priv = priv;
	txq->num_desc = nb_desc;
	dev->data->tx_queues[queue_idx] = txq;

	return 0;

fail:
	rte_free(txq->desc_ring);
	rte_free(txq);
	return ret;
}

static void
mana_dev_tx_queue_release(struct rte_eth_dev *dev, uint16_t qid)
{
	struct mana_txq *txq = dev->data->tx_queues[qid];

	mana_mr_btree_free(&txq->mr_btree);

	rte_free(txq->desc_ring);
	rte_free(txq);
}

static int
mana_dev_rx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_idx,
			uint16_t nb_desc, unsigned int socket_id,
			const struct rte_eth_rxconf *rx_conf __rte_unused,
			struct rte_mempool *mp)
{
	struct mana_priv *priv = dev->data->dev_private;
	struct mana_rxq *rxq;
	int ret;

	rxq = rte_zmalloc_socket("mana_rxq", sizeof(*rxq), 0, socket_id);
	if (!rxq) {
		DRV_LOG(ERR, "failed to allocate rxq");
		return -ENOMEM;
	}

	DRV_LOG(DEBUG, "idx %u nb_desc %u socket %u",
		queue_idx, nb_desc, socket_id);

	rxq->socket = socket_id;

	rxq->desc_ring = rte_zmalloc_socket("mana_rx_mbuf_ring",
					    sizeof(struct mana_rxq_desc) *
						nb_desc,
					    RTE_CACHE_LINE_SIZE, socket_id);

	if (!rxq->desc_ring) {
		DRV_LOG(ERR, "failed to allocate rxq desc_ring");
		ret = -ENOMEM;
		goto fail;
	}

	rxq->desc_ring_head = 0;
	rxq->desc_ring_tail = 0;

	ret = mana_mr_btree_init(&rxq->mr_btree,
				 MANA_MR_BTREE_PER_QUEUE_N, socket_id);
	if (ret) {
		DRV_LOG(ERR, "Failed to init RXQ MR btree");
		goto fail;
	}

	rxq->priv = priv;
	rxq->num_desc = nb_desc;
	rxq->mp = mp;
	dev->data->rx_queues[queue_idx] = rxq;

	return 0;

fail:
	rte_free(rxq->desc_ring);
	rte_free(rxq);
	return ret;
}

static void
mana_dev_rx_queue_release(struct rte_eth_dev *dev, uint16_t qid)
{
	struct mana_rxq *rxq = dev->data->rx_queues[qid];

	mana_mr_btree_free(&rxq->mr_btree);

	rte_free(rxq->desc_ring);
	rte_free(rxq);
}

static int
mana_dev_link_update(struct rte_eth_dev *dev,
		     int wait_to_complete __rte_unused)
{
	struct rte_eth_link link;

	/* MANA has no concept of carrier state, always reporting UP */
	link = (struct rte_eth_link) {
		.link_duplex = RTE_ETH_LINK_FULL_DUPLEX,
		.link_autoneg = RTE_ETH_LINK_SPEED_FIXED,
		.link_speed = RTE_ETH_SPEED_NUM_100G,
		.link_status = RTE_ETH_LINK_UP,
	};

	return rte_eth_linkstatus_set(dev, &link);
}

static const struct eth_dev_ops mana_dev_ops = {
	.dev_configure		= mana_dev_configure,
	.dev_start		= mana_dev_start,
	.dev_stop		= mana_dev_stop,
	.dev_close		= mana_dev_close,
	.dev_infos_get		= mana_dev_info_get,
	.txq_info_get		= mana_dev_tx_queue_info,
	.rxq_info_get		= mana_dev_rx_queue_info,
	.dev_supported_ptypes_get = mana_supported_ptypes,
	.rss_hash_update	= mana_rss_hash_update,
	.rss_hash_conf_get	= mana_rss_hash_conf_get,
	.tx_queue_setup		= mana_dev_tx_queue_setup,
	.tx_queue_release	= mana_dev_tx_queue_release,
	.rx_queue_setup		= mana_dev_rx_queue_setup,
	.rx_queue_release	= mana_dev_rx_queue_release,
	.link_update		= mana_dev_link_update,
};

static const struct eth_dev_ops mana_dev_secondary_ops = {
	.dev_infos_get = mana_dev_info_get,
};

uint16_t
mana_rx_burst_removed(void *dpdk_rxq __rte_unused,
		      struct rte_mbuf **pkts __rte_unused,
		      uint16_t pkts_n __rte_unused)
{
	rte_mb();
	return 0;
}

uint16_t
mana_tx_burst_removed(void *dpdk_rxq __rte_unused,
		      struct rte_mbuf **pkts __rte_unused,
		      uint16_t pkts_n __rte_unused)
{
	rte_mb();
	return 0;
}

#define ETH_MANA_MAC_ARG "mac"
static const char * const mana_init_args[] = {
	ETH_MANA_MAC_ARG,
	NULL,
};

/* Support of parsing up to 8 mac address from EAL command line */
#define MAX_NUM_ADDRESS 8
struct mana_conf {
	struct rte_ether_addr mac_array[MAX_NUM_ADDRESS];
	unsigned int index;
};

static int
mana_arg_parse_callback(const char *key, const char *val, void *private)
{
	struct mana_conf *conf = (struct mana_conf *)private;
	int ret;

	DRV_LOG(INFO, "key=%s value=%s index=%d", key, val, conf->index);

	if (conf->index >= MAX_NUM_ADDRESS) {
		DRV_LOG(ERR, "Exceeding max MAC address");
		return 1;
	}

	ret = rte_ether_unformat_addr(val, &conf->mac_array[conf->index]);
	if (ret) {
		DRV_LOG(ERR, "Invalid MAC address %s", val);
		return ret;
	}

	conf->index++;

	return 0;
}

static int
mana_parse_args(struct rte_devargs *devargs, struct mana_conf *conf)
{
	struct rte_kvargs *kvlist;
	unsigned int arg_count;
	int ret = 0;

	kvlist = rte_kvargs_parse(devargs->drv_str, mana_init_args);
	if (!kvlist) {
		DRV_LOG(ERR, "failed to parse kvargs args=%s", devargs->drv_str);
		return -EINVAL;
	}

	arg_count = rte_kvargs_count(kvlist, mana_init_args[0]);
	if (arg_count > MAX_NUM_ADDRESS) {
		ret = -EINVAL;
		goto free_kvlist;
	}
	ret = rte_kvargs_process(kvlist, mana_init_args[0],
				 mana_arg_parse_callback, conf);
	if (ret) {
		DRV_LOG(ERR, "error parsing args");
		goto free_kvlist;
	}

free_kvlist:
	rte_kvargs_free(kvlist);
	return ret;
}

static int
get_port_mac(struct ibv_device *device, unsigned int port,
	     struct rte_ether_addr *addr)
{
	FILE *file;
	int ret = 0;
	DIR *dir;
	struct dirent *dent;
	unsigned int dev_port;
	char mac[20];

	MANA_MKSTR(path, "%s/device/net", device->ibdev_path);

	dir = opendir(path);
	if (!dir)
		return -ENOENT;

	while ((dent = readdir(dir))) {
		char *name = dent->d_name;

		MANA_MKSTR(port_path, "%s/%s/dev_port", path, name);

		/* Ignore . and .. */
		if ((name[0] == '.') &&
		    ((name[1] == '\0') ||
		     ((name[1] == '.') && (name[2] == '\0'))))
			continue;

		file = fopen(port_path, "r");
		if (!file)
			continue;

		ret = fscanf(file, "%u", &dev_port);
		fclose(file);

		if (ret != 1)
			continue;

		/* Ethernet ports start at 0, IB port start at 1 */
		if (dev_port == port - 1) {
			MANA_MKSTR(address_path, "%s/%s/address", path, name);

			file = fopen(address_path, "r");
			if (!file)
				continue;

			ret = fscanf(file, "%s", mac);
			fclose(file);

			if (ret < 0)
				break;

			ret = rte_ether_unformat_addr(mac, addr);
			if (ret)
				DRV_LOG(ERR, "unrecognized mac addr %s", mac);
			break;
		}
	}

	closedir(dir);
	return ret;
}

static int
mana_ibv_device_to_pci_addr(const struct ibv_device *device,
			    struct rte_pci_addr *pci_addr)
{
	FILE *file;
	char *line = NULL;
	size_t len = 0;

	MANA_MKSTR(path, "%s/device/uevent", device->ibdev_path);

	file = fopen(path, "r");
	if (!file)
		return -errno;

	while (getline(&line, &len, file) != -1) {
		/* Extract information. */
		if (sscanf(line,
			   "PCI_SLOT_NAME="
			   "%" SCNx32 ":%" SCNx8 ":%" SCNx8 ".%" SCNx8 "\n",
			   &pci_addr->domain,
			   &pci_addr->bus,
			   &pci_addr->devid,
			   &pci_addr->function) == 4) {
			break;
		}
	}

	free(line);
	fclose(file);
	return 0;
}

/*
 * Interrupt handler from IB layer to notify this device is being removed.
 */
static void
mana_intr_handler(void *arg)
{
	struct mana_priv *priv = arg;
	struct ibv_context *ctx = priv->ib_ctx;
	struct ibv_async_event event;

	/* Read and ack all messages from IB device */
	while (true) {
		if (ibv_get_async_event(ctx, &event))
			break;

		if (event.event_type == IBV_EVENT_DEVICE_FATAL) {
			struct rte_eth_dev *dev;

			dev = &rte_eth_devices[priv->port_id];
			if (dev->data->dev_conf.intr_conf.rmv)
				rte_eth_dev_callback_process(dev,
					RTE_ETH_EVENT_INTR_RMV, NULL);
		}

		ibv_ack_async_event(&event);
	}
}

static int
mana_intr_uninstall(struct mana_priv *priv)
{
	int ret;

	ret = rte_intr_callback_unregister(priv->intr_handle,
					   mana_intr_handler, priv);
	if (ret <= 0) {
		DRV_LOG(ERR, "Failed to unregister intr callback ret %d", ret);
		return ret;
	}

	rte_intr_instance_free(priv->intr_handle);

	return 0;
}

static int
mana_intr_install(struct mana_priv *priv)
{
	int ret, flags;
	struct ibv_context *ctx = priv->ib_ctx;

	priv->intr_handle = rte_intr_instance_alloc(RTE_INTR_INSTANCE_F_SHARED);
	if (!priv->intr_handle) {
		DRV_LOG(ERR, "Failed to allocate intr_handle");
		rte_errno = ENOMEM;
		return -ENOMEM;
	}

	rte_intr_fd_set(priv->intr_handle, -1);

	flags = fcntl(ctx->async_fd, F_GETFL);
	ret = fcntl(ctx->async_fd, F_SETFL, flags | O_NONBLOCK);
	if (ret) {
		DRV_LOG(ERR, "Failed to change async_fd to NONBLOCK");
		goto free_intr;
	}

	rte_intr_fd_set(priv->intr_handle, ctx->async_fd);
	rte_intr_type_set(priv->intr_handle, RTE_INTR_HANDLE_EXT);

	ret = rte_intr_callback_register(priv->intr_handle,
					 mana_intr_handler, priv);
	if (ret) {
		DRV_LOG(ERR, "Failed to register intr callback");
		rte_intr_fd_set(priv->intr_handle, -1);
		goto restore_fd;
	}

	return 0;

restore_fd:
	fcntl(ctx->async_fd, F_SETFL, flags);

free_intr:
	rte_intr_instance_free(priv->intr_handle);
	priv->intr_handle = NULL;

	return ret;
}

static int
mana_proc_priv_init(struct rte_eth_dev *dev)
{
	struct mana_process_priv *priv;

	priv = rte_zmalloc_socket("mana_proc_priv",
				  sizeof(struct mana_process_priv),
				  RTE_CACHE_LINE_SIZE,
				  dev->device->numa_node);
	if (!priv)
		return -ENOMEM;

	dev->process_private = priv;
	return 0;
}

/*
 * Map the doorbell page for the secondary process through IB device handle.
 */
static int
mana_map_doorbell_secondary(struct rte_eth_dev *eth_dev, int fd)
{
	struct mana_process_priv *priv = eth_dev->process_private;

	void *addr;

	addr = mmap(NULL, rte_mem_page_size(), PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED) {
		DRV_LOG(ERR, "Failed to map secondary doorbell port %u",
			eth_dev->data->port_id);
		return -ENOMEM;
	}

	DRV_LOG(INFO, "Secondary doorbell mapped to %p", addr);

	priv->db_page = addr;

	return 0;
}

/* Initialize shared data for the driver (all devices) */
static int
mana_init_shared_data(void)
{
	int ret =  0;
	const struct rte_memzone *secondary_mz;

	rte_spinlock_lock(&mana_shared_data_lock);

	/* Skip if shared data is already initialized */
	if (mana_shared_data)
		goto exit;

	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		mana_shared_mz = rte_memzone_reserve(MZ_MANA_SHARED_DATA,
						     sizeof(*mana_shared_data),
						     SOCKET_ID_ANY, 0);
		if (!mana_shared_mz) {
			DRV_LOG(ERR, "Cannot allocate mana shared data");
			ret = -rte_errno;
			goto exit;
		}

		mana_shared_data = mana_shared_mz->addr;
		memset(mana_shared_data, 0, sizeof(*mana_shared_data));
		rte_spinlock_init(&mana_shared_data->lock);
	} else {
		secondary_mz = rte_memzone_lookup(MZ_MANA_SHARED_DATA);
		if (!secondary_mz) {
			DRV_LOG(ERR, "Cannot attach mana shared data");
			ret = -rte_errno;
			goto exit;
		}

		mana_shared_data = secondary_mz->addr;
		memset(&mana_local_data, 0, sizeof(mana_local_data));
	}

exit:
	rte_spinlock_unlock(&mana_shared_data_lock);

	return ret;
}

/*
 * Init the data structures for use in primary and secondary processes.
 */
static int
mana_init_once(void)
{
	int ret;

	ret = mana_init_shared_data();
	if (ret)
		return ret;

	rte_spinlock_lock(&mana_shared_data->lock);

	switch (rte_eal_process_type()) {
	case RTE_PROC_PRIMARY:
		if (mana_shared_data->init_done)
			break;

		ret = mana_mp_init_primary();
		if (ret)
			break;
		DRV_LOG(ERR, "MP INIT PRIMARY");

		mana_shared_data->init_done = 1;
		break;

	case RTE_PROC_SECONDARY:

		if (mana_local_data.init_done)
			break;

		ret = mana_mp_init_secondary();
		if (ret)
			break;

		DRV_LOG(ERR, "MP INIT SECONDARY");

		mana_local_data.init_done = 1;
		break;

	default:
		/* Impossible, internal error */
		ret = -EPROTO;
		break;
	}

	rte_spinlock_unlock(&mana_shared_data->lock);

	return ret;
}

/*
 * Probe an IB port
 * Return value:
 * positive value: successfully probed port
 * 0: port not matching specified MAC address
 * negative value: error code
 */
static int
mana_probe_port(struct ibv_device *ibdev, struct ibv_device_attr_ex *dev_attr,
		uint8_t port, struct rte_pci_device *pci_dev, struct rte_ether_addr *addr)
{
	struct mana_priv *priv = NULL;
	struct rte_eth_dev *eth_dev = NULL;
	struct ibv_parent_domain_init_attr attr = {0};
	char address[64];
	char name[RTE_ETH_NAME_MAX_LEN];
	int ret;
	struct ibv_context *ctx = NULL;

	rte_ether_format_addr(address, sizeof(address), addr);
	DRV_LOG(INFO, "device located port %u address %s", port, address);

	priv = rte_zmalloc_socket(NULL, sizeof(*priv), RTE_CACHE_LINE_SIZE,
				  SOCKET_ID_ANY);
	if (!priv)
		return -ENOMEM;

	snprintf(name, sizeof(name), "%s_port%d", pci_dev->device.name, port);

	if (rte_eal_process_type() == RTE_PROC_SECONDARY) {
		int fd;

		eth_dev = rte_eth_dev_attach_secondary(name);
		if (!eth_dev) {
			DRV_LOG(ERR, "Can't attach to dev %s", name);
			ret =  -ENOMEM;
			goto failed;
		}

		eth_dev->device = &pci_dev->device;
		eth_dev->dev_ops = &mana_dev_secondary_ops;
		ret = mana_proc_priv_init(eth_dev);
		if (ret)
			goto failed;
		priv->process_priv = eth_dev->process_private;

		/* Get the IB FD from the primary process */
		fd = mana_mp_req_verbs_cmd_fd(eth_dev);
		if (fd < 0) {
			DRV_LOG(ERR, "Failed to get FD %d", fd);
			ret = -ENODEV;
			goto failed;
		}

		ret = mana_map_doorbell_secondary(eth_dev, fd);
		if (ret) {
			DRV_LOG(ERR, "Failed secondary map %d", fd);
			goto failed;
		}

		/* fd is no not used after mapping doorbell */
		close(fd);

		eth_dev->tx_pkt_burst = mana_tx_burst_removed;
		eth_dev->rx_pkt_burst = mana_rx_burst_removed;

		rte_spinlock_lock(&mana_shared_data->lock);
		mana_shared_data->secondary_cnt++;
		mana_local_data.secondary_cnt++;
		rte_spinlock_unlock(&mana_shared_data->lock);

		rte_eth_copy_pci_info(eth_dev, pci_dev);
		rte_eth_dev_probing_finish(eth_dev);

		return 0;
	}

	ctx = ibv_open_device(ibdev);
	if (!ctx) {
		DRV_LOG(ERR, "Failed to open IB device %s", ibdev->name);
		ret = -ENODEV;
		goto failed;
	}

	eth_dev = rte_eth_dev_allocate(name);
	if (!eth_dev) {
		ret = -ENOMEM;
		goto failed;
	}

	eth_dev->data->mac_addrs =
		rte_calloc("mana_mac", 1,
			   sizeof(struct rte_ether_addr), 0);
	if (!eth_dev->data->mac_addrs) {
		ret = -ENOMEM;
		goto failed;
	}

	rte_ether_addr_copy(addr, eth_dev->data->mac_addrs);

	priv->ib_pd = ibv_alloc_pd(ctx);
	if (!priv->ib_pd) {
		DRV_LOG(ERR, "ibv_alloc_pd failed port %d", port);
		ret = -ENOMEM;
		goto failed;
	}

	/* Create a parent domain with the port number */
	attr.pd = priv->ib_pd;
	attr.comp_mask = IBV_PARENT_DOMAIN_INIT_ATTR_PD_CONTEXT;
	attr.pd_context = (void *)(uint64_t)port;
	priv->ib_parent_pd = ibv_alloc_parent_domain(ctx, &attr);
	if (!priv->ib_parent_pd) {
		DRV_LOG(ERR, "ibv_alloc_parent_domain failed port %d", port);
		ret = -ENOMEM;
		goto failed;
	}

	priv->ib_ctx = ctx;
	priv->port_id = eth_dev->data->port_id;
	priv->dev_port = port;
	eth_dev->data->dev_private = priv;
	priv->dev_data = eth_dev->data;

	priv->max_rx_queues = dev_attr->orig_attr.max_qp;
	priv->max_tx_queues = dev_attr->orig_attr.max_qp;

	priv->max_rx_desc =
		RTE_MIN(dev_attr->orig_attr.max_qp_wr,
			dev_attr->orig_attr.max_cqe);
	priv->max_tx_desc =
		RTE_MIN(dev_attr->orig_attr.max_qp_wr,
			dev_attr->orig_attr.max_cqe);

	priv->max_send_sge = dev_attr->orig_attr.max_sge;
	priv->max_recv_sge = dev_attr->orig_attr.max_sge;

	priv->max_mr = dev_attr->orig_attr.max_mr;
	priv->max_mr_size = dev_attr->orig_attr.max_mr_size;

	DRV_LOG(INFO, "dev %s max queues %d desc %d sge %d",
		name, priv->max_rx_queues, priv->max_rx_desc,
		priv->max_send_sge);

	rte_eth_copy_pci_info(eth_dev, pci_dev);

	/* Create async interrupt handler */
	ret = mana_intr_install(priv);
	if (ret) {
		DRV_LOG(ERR, "Failed to install intr handler");
		goto failed;
	}

	rte_spinlock_lock(&mana_shared_data->lock);
	mana_shared_data->primary_cnt++;
	rte_spinlock_unlock(&mana_shared_data->lock);

	eth_dev->device = &pci_dev->device;

	DRV_LOG(INFO, "device %s at port %u", name, eth_dev->data->port_id);

	eth_dev->rx_pkt_burst = mana_rx_burst_removed;
	eth_dev->tx_pkt_burst = mana_tx_burst_removed;
	eth_dev->dev_ops = &mana_dev_ops;

	rte_eth_dev_probing_finish(eth_dev);

	return 0;

failed:
	/* Free the resource for the port failed */
	if (priv) {
		if (priv->ib_parent_pd)
			ibv_dealloc_pd(priv->ib_parent_pd);

		if (priv->ib_pd)
			ibv_dealloc_pd(priv->ib_pd);
	}

	if (eth_dev)
		rte_eth_dev_release_port(eth_dev);

	rte_free(priv);

	if (ctx)
		ibv_close_device(ctx);

	return ret;
}

/*
 * Goes through the IB device list to look for the IB port matching the
 * mac_addr. If found, create a rte_eth_dev for it.
 */
static int
mana_pci_probe_mac(struct rte_pci_device *pci_dev,
		   struct rte_ether_addr *mac_addr)
{
	struct ibv_device **ibv_list;
	int ibv_idx;
	struct ibv_context *ctx;
	int num_devices;
	int ret = 0;
	uint8_t port;

	ibv_list = ibv_get_device_list(&num_devices);
	for (ibv_idx = 0; ibv_idx < num_devices; ibv_idx++) {
		struct ibv_device *ibdev = ibv_list[ibv_idx];
		struct rte_pci_addr pci_addr;
		struct ibv_device_attr_ex dev_attr;

		DRV_LOG(INFO, "Probe device name %s dev_name %s ibdev_path %s",
			ibdev->name, ibdev->dev_name, ibdev->ibdev_path);

		if (mana_ibv_device_to_pci_addr(ibdev, &pci_addr))
			continue;

		/* Ignore if this IB device is not this PCI device */
		if (pci_dev->addr.domain != pci_addr.domain ||
		    pci_dev->addr.bus != pci_addr.bus ||
		    pci_dev->addr.devid != pci_addr.devid ||
		    pci_dev->addr.function != pci_addr.function)
			continue;

		ctx = ibv_open_device(ibdev);
		if (!ctx) {
			DRV_LOG(ERR, "Failed to open IB device %s",
				ibdev->name);
			continue;
		}
		ret = ibv_query_device_ex(ctx, NULL, &dev_attr);
		ibv_close_device(ctx);

		for (port = 1; port <= dev_attr.orig_attr.phys_port_cnt;
		     port++) {
			struct rte_ether_addr addr;
			ret = get_port_mac(ibdev, port, &addr);
			if (ret)
				continue;

			if (mac_addr && !rte_is_same_ether_addr(&addr, mac_addr))
				continue;

			ret = mana_probe_port(ibdev, &dev_attr, port, pci_dev, &addr);
			if (ret)
				DRV_LOG(ERR, "Probe on IB port %u failed %d", port, ret);
			else
				DRV_LOG(INFO, "Successfully probed on IB port %u", port);
		}
	}

	ibv_free_device_list(ibv_list);
	return ret;
}

/*
 * Main callback function from PCI bus to probe a device.
 */
static int
mana_pci_probe(struct rte_pci_driver *pci_drv __rte_unused,
	       struct rte_pci_device *pci_dev)
{
	struct rte_devargs *args = pci_dev->device.devargs;
	struct mana_conf conf = {0};
	unsigned int i;
	int ret;

	if (args && args->drv_str) {
		ret = mana_parse_args(args, &conf);
		if (ret) {
			DRV_LOG(ERR, "Failed to parse parameters args = %s",
				args->drv_str);
			return ret;
		}
	}

	ret = mana_init_once();
	if (ret) {
		DRV_LOG(ERR, "Failed to init PMD global data %d", ret);
		return ret;
	}

	/* If there are no driver parameters, probe on all ports */
	if (!conf.index)
		return mana_pci_probe_mac(pci_dev, NULL);

	for (i = 0; i < conf.index; i++) {
		ret = mana_pci_probe_mac(pci_dev, &conf.mac_array[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int
mana_dev_uninit(struct rte_eth_dev *dev)
{
	return mana_dev_close(dev);
}

/*
 * Callback from PCI to remove this device.
 */
static int
mana_pci_remove(struct rte_pci_device *pci_dev)
{
	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		rte_spinlock_lock(&mana_shared_data_lock);

		rte_spinlock_lock(&mana_shared_data->lock);

		RTE_VERIFY(mana_shared_data->primary_cnt > 0);
		mana_shared_data->primary_cnt--;
		if (!mana_shared_data->primary_cnt) {
			DRV_LOG(DEBUG, "mp uninit primary");
			mana_mp_uninit_primary();
		}

		rte_spinlock_unlock(&mana_shared_data->lock);

		/* Also free the shared memory if this is the last */
		if (!mana_shared_data->primary_cnt) {
			DRV_LOG(DEBUG, "free shared memezone data");
			rte_memzone_free(mana_shared_mz);
		}

		rte_spinlock_unlock(&mana_shared_data_lock);
	} else {
		rte_spinlock_lock(&mana_shared_data_lock);

		rte_spinlock_lock(&mana_shared_data->lock);
		RTE_VERIFY(mana_shared_data->secondary_cnt > 0);
		mana_shared_data->secondary_cnt--;
		rte_spinlock_unlock(&mana_shared_data->lock);

		RTE_VERIFY(mana_local_data.secondary_cnt > 0);
		mana_local_data.secondary_cnt--;
		if (!mana_local_data.secondary_cnt) {
			DRV_LOG(DEBUG, "mp uninit secondary");
			mana_mp_uninit_secondary();
		}

		rte_spinlock_unlock(&mana_shared_data_lock);
	}

	return rte_eth_dev_pci_generic_remove(pci_dev, mana_dev_uninit);
}

static const struct rte_pci_id mana_pci_id_map[] = {
	{
		RTE_PCI_DEVICE(PCI_VENDOR_ID_MICROSOFT,
			       PCI_DEVICE_ID_MICROSOFT_MANA)
	},
	{
		.vendor_id = 0
	},
};

static struct rte_pci_driver mana_pci_driver = {
	.id_table = mana_pci_id_map,
	.probe = mana_pci_probe,
	.remove = mana_pci_remove,
	.drv_flags = RTE_PCI_DRV_INTR_RMV,
};

RTE_PMD_REGISTER_PCI(net_mana, mana_pci_driver);
RTE_PMD_REGISTER_PCI_TABLE(net_mana, mana_pci_id_map);
RTE_PMD_REGISTER_KMOD_DEP(net_mana, "* ib_uverbs & mana_ib");
RTE_LOG_REGISTER_SUFFIX(mana_logtype_init, init, NOTICE);
RTE_LOG_REGISTER_SUFFIX(mana_logtype_driver, driver, NOTICE);
RTE_PMD_REGISTER_PARAM_STRING(net_mana, ETH_MANA_MAC_ARG "=<mac_addr>");
#ifndef K_CONVERTED
#define K_CONVERTED
#endif
#include "kmod.h"
/*
 * Copyright (c) 2015, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "en.h"

struct mlx5e_rq_param {
	u32                        rqc[MLX5_ST_SZ_DW(rqc)];
	struct mlx5_wq_param       wq;
};

struct mlx5e_sq_param {
	u32                        sqc[MLX5_ST_SZ_DW(sqc)];
	struct mlx5_wq_param       wq;
};

struct mlx5e_cq_param {
	u32                        cqc[MLX5_ST_SZ_DW(cqc)];
	struct mlx5_wq_param       wq;
	u16                        eq_ix;
};

struct mlx5e_channel_param {
	struct mlx5e_rq_param      rq;
	struct mlx5e_sq_param      sq;
	struct mlx5e_cq_param      rx_cq;
	struct mlx5e_cq_param      tx_cq;
};

static void mlx5e_update_carrier(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	u8 port_state;

	port_state = mlx5_query_vport_state(mdev,
		MLX5_QUERY_VPORT_STATE_IN_OP_MOD_VNIC_VPORT);

	if (port_state == VPORT_STATE_UP)
		netif_carrier_on(priv->netdev);
	else
		netif_carrier_off(priv->netdev);
}

static void mlx5e_update_carrier_work(struct work_struct *work)
{
	struct mlx5e_priv *priv = container_of(work, struct mlx5e_priv,
					       update_carrier_work);

	mutex_lock(&priv->state_lock);
	if (test_bit(MLX5E_STATE_OPENED, &priv->state))
		mlx5e_update_carrier(priv);
	mutex_unlock(&priv->state_lock);
}

static void mlx5e_update_pport_counters(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_pport_stats *s = &priv->stats.pport;
	u32 *in;
	u32 *out;
	int sz = MLX5_ST_SZ_BYTES(ppcnt_reg);

	in  = mlx5_vzalloc(sz);
	out = mlx5_vzalloc(sz);
	if (!in || !out)
		goto free_out;

	memset(in, 0, sz);
	memset(out, 0, sz);
	MLX5_SET(ppcnt_reg, in, local_port, 1);

	MLX5_SET(ppcnt_reg, in, grp, MLX5_IEEE_802_3_COUNTERS_GROUP);
	mlx5_core_access_reg(mdev, in, sz, out,
			     sz, MLX5_REG_PPCNT, 0, 0);
	memcpy(s->IEEE_802_3_counters,
	       MLX5_ADDR_OF(ppcnt_reg, out, counter_set),
	       sizeof(s->IEEE_802_3_counters));

	MLX5_SET(ppcnt_reg, in, grp, MLX5_RFC_2863_COUNTERS_GROUP);
	mlx5_core_access_reg(mdev, in, sz, out,
			     sz, MLX5_REG_PPCNT, 0, 0);
	memcpy(s->RFC_2863_counters,
	       MLX5_ADDR_OF(ppcnt_reg, out, counter_set),
	       sizeof(s->RFC_2863_counters));

	MLX5_SET(ppcnt_reg, in, grp, MLX5_RFC_2819_COUNTERS_GROUP);
	mlx5_core_access_reg(mdev, in, sz, out,
			     sz, MLX5_REG_PPCNT, 0, 0);
	memcpy(s->RFC_2819_counters,
	       MLX5_ADDR_OF(ppcnt_reg, out, counter_set),
	       sizeof(s->RFC_2819_counters));

free_out:
	kvfree(in);
	kvfree(out);
}

 
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
static void mlx5e_update_sw_lro_stats(struct mlx5e_priv *priv)
{
	int i;
	struct mlx5e_vport_stats *s = &priv->stats.vport;

	s->sw_lro_aggregated = 0;
	s->sw_lro_flushed = 0;
	s->sw_lro_no_desc = 0;

	for (i = 0; i < priv->params.num_channels; i++) {
		struct mlx5e_rq *rq = &priv->channel[i]->rq;

		s->sw_lro_aggregated += rq->sw_lro.lro_mgr.stats.aggregated;
		s->sw_lro_flushed += rq->sw_lro.lro_mgr.stats.flushed;
		s->sw_lro_no_desc += rq->sw_lro.lro_mgr.stats.no_desc;
	}
}
#endif

void mlx5e_update_stats(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5e_vport_stats *s = &priv->stats.vport;
	struct mlx5e_rq_stats *rq_stats;
	struct mlx5e_sq_stats *sq_stats;
	u32 in[MLX5_ST_SZ_DW(query_vport_counter_in)];
	u32 *out;
	int outlen = MLX5_ST_SZ_BYTES(query_vport_counter_out);
	u64 tx_offload_none;
	int i, j;

	out = mlx5_vzalloc(outlen);
	if (!out)
		return;

	/* Collect firts the SW counters and then HW for consistency */
	s->tso_packets		= 0;
	s->tso_bytes		= 0;
	s->tx_queue_stopped	= 0;
	s->tx_queue_wake	= 0;
	s->tx_queue_dropped	= 0;
	tx_offload_none		= 0;
	s->lro_packets		= 0;
	s->lro_bytes		= 0;
	s->rx_csum_none		= 0;
	s->rx_wqe_err		= 0;
	for (i = 0; i < priv->params.num_channels; i++) {
		rq_stats = &priv->channel[i]->rq.stats;

		s->lro_packets	+= rq_stats->lro_packets;
		s->lro_bytes	+= rq_stats->lro_bytes;
		s->rx_csum_none	+= rq_stats->csum_none;
		s->rx_wqe_err   += rq_stats->wqe_err;

		for (j = 0; j < priv->params.num_tc; j++) {
			sq_stats = &priv->channel[i]->sq[j].stats;

			s->tso_packets		+= sq_stats->tso_packets;
			s->tso_bytes		+= sq_stats->tso_bytes;
			s->tx_queue_stopped	+= sq_stats->stopped;
			s->tx_queue_wake	+= sq_stats->wake;
			s->tx_queue_dropped	+= sq_stats->dropped;
			tx_offload_none		+= sq_stats->csum_offload_none;
		}
	}

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	mlx5e_update_sw_lro_stats(priv);
#endif

	/* HW counters */
	memset(in, 0, sizeof(in));

	MLX5_SET(query_vport_counter_in, in, opcode,
		 MLX5_CMD_OP_QUERY_VPORT_COUNTER);
	MLX5_SET(query_vport_counter_in, in, op_mod, 0);
	MLX5_SET(query_vport_counter_in, in, other_vport, 0);

	memset(out, 0, outlen);

	if (mlx5_cmd_exec(mdev, in, sizeof(in), out, outlen))
		goto free_out;

#define MLX5_GET_CTR(p, x) \
	MLX5_GET64(query_vport_counter_out, p, x)

	s->rx_error_packets     =
		MLX5_GET_CTR(out, received_errors.packets);
	s->rx_error_bytes       =
		MLX5_GET_CTR(out, received_errors.octets);
	s->tx_error_packets     =
		MLX5_GET_CTR(out, transmit_errors.packets);
	s->tx_error_bytes       =
		MLX5_GET_CTR(out, transmit_errors.octets);

	s->rx_unicast_packets   =
		MLX5_GET_CTR(out, received_eth_unicast.packets);
	s->rx_unicast_bytes     =
		MLX5_GET_CTR(out, received_eth_unicast.octets);
	s->tx_unicast_packets   =
		MLX5_GET_CTR(out, transmitted_eth_unicast.packets);
	s->tx_unicast_bytes     =
		MLX5_GET_CTR(out, transmitted_eth_unicast.octets);

	s->rx_multicast_packets =
		MLX5_GET_CTR(out, received_eth_multicast.packets);
	s->rx_multicast_bytes   =
		MLX5_GET_CTR(out, received_eth_multicast.octets);
	s->tx_multicast_packets =
		MLX5_GET_CTR(out, transmitted_eth_multicast.packets);
	s->tx_multicast_bytes   =
		MLX5_GET_CTR(out, transmitted_eth_multicast.octets);

	s->rx_broadcast_packets =
		MLX5_GET_CTR(out, received_eth_broadcast.packets);
	s->rx_broadcast_bytes   =
		MLX5_GET_CTR(out, received_eth_broadcast.octets);
	s->tx_broadcast_packets =
		MLX5_GET_CTR(out, transmitted_eth_broadcast.packets);
	s->tx_broadcast_bytes   =
		MLX5_GET_CTR(out, transmitted_eth_broadcast.octets);

	s->rx_packets =
		s->rx_unicast_packets +
		s->rx_multicast_packets +
		s->rx_broadcast_packets;
	s->rx_bytes =
		s->rx_unicast_bytes +
		s->rx_multicast_bytes +
		s->rx_broadcast_bytes;
	s->tx_packets =
		s->tx_unicast_packets +
		s->tx_multicast_packets +
		s->tx_broadcast_packets;
	s->tx_bytes =
		s->tx_unicast_bytes +
		s->tx_multicast_bytes +
		s->tx_broadcast_bytes;

	/* Update calculated offload counters */
	s->tx_csum_offload = s->tx_packets - tx_offload_none;
	s->rx_csum_good    = s->rx_packets - s->rx_csum_none;

	mlx5e_update_pport_counters(priv);
free_out:
	kvfree(out);
}

static void mlx5e_update_stats_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct mlx5e_priv *priv = container_of(dwork, struct mlx5e_priv,
					       update_stats_work);
	mutex_lock(&priv->state_lock);
	if (test_bit(MLX5E_STATE_OPENED, &priv->state)) {
		mlx5e_update_stats(priv);
		schedule_delayed_work(dwork,
				      msecs_to_jiffies(
					      MLX5E_UPDATE_STATS_INTERVAL));
	}
	mutex_unlock(&priv->state_lock);
}

static void __mlx5e_async_event(struct mlx5e_priv *priv,
				enum mlx5_dev_event event)
{
	switch (event) {
	case MLX5_DEV_EVENT_PORT_UP:
	case MLX5_DEV_EVENT_PORT_DOWN:
		schedule_work(&priv->update_carrier_work);
		break;

	default:
		break;
	}
}

static void mlx5e_async_event(struct mlx5_core_dev *mdev, void *vpriv,
			      enum mlx5_dev_event event, unsigned long param)
{
	struct mlx5e_priv *priv = vpriv;

	spin_lock(&priv->async_events_spinlock);
	if (test_bit(MLX5E_STATE_ASYNC_EVENTS_ENABLE, &priv->state))
		__mlx5e_async_event(priv, event);
	spin_unlock(&priv->async_events_spinlock);
}

static void mlx5e_enable_async_events(struct mlx5e_priv *priv)
{
	set_bit(MLX5E_STATE_ASYNC_EVENTS_ENABLE, &priv->state);
}

static void mlx5e_disable_async_events(struct mlx5e_priv *priv)
{
	spin_lock_irq(&priv->async_events_spinlock);
	clear_bit(MLX5E_STATE_ASYNC_EVENTS_ENABLE, &priv->state);
	spin_unlock_irq(&priv->async_events_spinlock);
}

#define MLX5E_HW2SW_MTU(hwmtu) (hwmtu - (ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN))
#define MLX5E_SW2HW_MTU(swmtu) (swmtu + (ETH_HLEN + VLAN_HLEN + ETH_FCS_LEN))

static int mlx5e_create_rq(struct mlx5e_channel *c,
			   struct mlx5e_rq_param *param,
			   struct mlx5e_rq *rq)
{
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;
	void *rqc = param->rqc;
	void *rqc_wq = MLX5_ADDR_OF(rqc, rqc, wq);
	int wq_sz;
	int err;
	int i;

	param->wq.db_numa_node = cpu_to_node(c->cpu);

	err = mlx5_wq_ll_create(mdev, &param->wq, rqc_wq, &rq->wq,
				&rq->wq_ctrl);
	if (err)
		return err;

	rq->wq.db = &rq->wq.db[MLX5_RCV_DBR];

	wq_sz = mlx5_wq_ll_get_size(&rq->wq);
	rq->skb = kzalloc_node(wq_sz * sizeof(*rq->skb), GFP_KERNEL,
			       cpu_to_node(c->cpu));
	if (!rq->skb) {
		err = -ENOMEM;
		goto err_rq_wq_destroy;
	}

	rq->wqe_sz = (priv->params.lro_en) ? priv->params.lro_wqe_sz :
					     MLX5E_SW2HW_MTU(priv->netdev->mtu);
	rq->wqe_sz = SKB_DATA_ALIGN(rq->wqe_sz + MLX5E_NET_IP_ALIGN);
	for (i = 0; i < wq_sz; i++) {
		struct mlx5e_rx_wqe *wqe = mlx5_wq_ll_get_wqe(&rq->wq, i);
		u32 byte_count = rq->wqe_sz - MLX5E_NET_IP_ALIGN;

		wqe->data.lkey       = c->mkey_be;
		wqe->data.byte_count =
			cpu_to_be32(byte_count | MLX5_HW_START_PADDING);
	}

	rq->pdev    = c->pdev;
	rq->netdev  = c->netdev;
	rq->channel = c;
	rq->ix      = c->ix;

	return 0;

err_rq_wq_destroy:
	mlx5_wq_destroy(&rq->wq_ctrl);

	return err;
}

static void mlx5e_destroy_rq(struct mlx5e_rq *rq)
{
	kfree(rq->skb);
	mlx5_wq_destroy(&rq->wq_ctrl);
}

static int mlx5e_enable_rq(struct mlx5e_rq *rq, struct mlx5e_rq_param *param)
{
	struct mlx5e_channel *c = rq->channel;
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;

	void *in;
	void *rqc;
	void *wq;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(create_rq_in) +
		sizeof(u64) * rq->wq_ctrl.buf.npages;
	in = mlx5_vzalloc(inlen);
	if (!in)
		return -ENOMEM;

	rqc = MLX5_ADDR_OF(create_rq_in, in, ctx);
	wq  = MLX5_ADDR_OF(rqc, rqc, wq);

	memcpy(rqc, param->rqc, sizeof(param->rqc));

	MLX5_SET(rqc,  rqc, cqn,		c->rq.cq.mcq.cqn);
	MLX5_SET(rqc,  rqc, state,		MLX5_RQC_STATE_RST);
	MLX5_SET(rqc,  rqc, flush_in_error_en,	1);
	MLX5_SET(wq,   wq,  log_wq_pg_sz,	rq->wq_ctrl.buf.page_shift -
						PAGE_SHIFT);
	MLX5_SET64(wq, wq,  dbr_addr,		rq->wq_ctrl.db.dma);

	mlx5_fill_page_array(&rq->wq_ctrl.buf,
			     (__be64 *)MLX5_ADDR_OF(wq, wq, pas));

	err = mlx5_core_create_rq(mdev, in, inlen, &rq->rqn);

	kvfree(in);

	return err;
}

static int mlx5e_modify_rq(struct mlx5e_rq *rq, int curr_state, int next_state)
{
	struct mlx5e_channel *c = rq->channel;
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;

	void *in;
	void *rqc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_rq_in);
	in = mlx5_vzalloc(inlen);
	if (!in)
		return -ENOMEM;

	rqc = MLX5_ADDR_OF(modify_rq_in, in, ctx);

	MLX5_SET(modify_rq_in, in, rqn, rq->rqn);
	MLX5_SET(modify_rq_in, in, rq_state, curr_state);
	MLX5_SET(rqc, rqc, state, next_state);

	err = mlx5_core_modify_rq(mdev, in, inlen);

	kvfree(in);

	return err;
}

static void mlx5e_disable_rq(struct mlx5e_rq *rq)
{
	struct mlx5e_channel *c = rq->channel;
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;

	mlx5_core_destroy_rq(mdev, rq->rqn);
}

static int mlx5e_wait_for_min_rx_wqes(struct mlx5e_rq *rq)
{
	struct mlx5e_channel *c = rq->channel;
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_wq_ll *wq = &rq->wq;
	int i;

	for (i = 0; i < 1000; i++) {
		if (wq->cur_sz >= priv->params.min_rx_wqes)
			return 0;

		msleep(20);
	}

	return -ETIMEDOUT;
}

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
static int get_skb_hdr(struct sk_buff *skb, void **iphdr,
			void **tcph, u64 *hdr_flags, void *priv)
{
	unsigned int ip_len;
	struct iphdr *iph;

	if (unlikely(skb->protocol != htons(ETH_P_IP)))
		return -1;

	/*
	* In the future we may add an else clause that verifies the
	* checksum and allows devices which do not calculate checksum
	* to use LRO.
	*/
	if (unlikely(skb->ip_summed != CHECKSUM_UNNECESSARY))
		return -1;

	/* Check for non-TCP packet */
	skb_reset_network_header(skb);
	iph = ip_hdr(skb);
	if (iph->protocol != IPPROTO_TCP)
		return -1;

	ip_len = ip_hdrlen(skb);
	skb_set_transport_header(skb, ip_len);
	*tcph = tcp_hdr(skb);

	/* check if IP header and TCP header are complete */
	if (ntohs(iph->tot_len) < ip_len + tcp_hdrlen(skb))
		return -1;

	*hdr_flags = LRO_IPV4 | LRO_TCP;
	*iphdr = iph;

	return 0;
}

static void mlx5e_rq_sw_lro_init(struct mlx5e_rq *rq)
{
	struct mlx5e_priv *priv = netdev_priv(rq->netdev);

	rq->sw_lro.lro_mgr.max_aggr 		= 64;
	rq->sw_lro.lro_mgr.max_desc		= MLX5E_LRO_MAX_DESC;
	rq->sw_lro.lro_mgr.lro_arr		= rq->sw_lro.lro_desc;
	rq->sw_lro.lro_mgr.get_skb_header	= get_skb_hdr;
	rq->sw_lro.lro_mgr.features		= LRO_F_NAPI;
	rq->sw_lro.lro_mgr.frag_align_pad	= NET_IP_ALIGN;
	rq->sw_lro.lro_mgr.dev			= rq->netdev;
	rq->sw_lro.lro_mgr.ip_summed		= CHECKSUM_UNNECESSARY;
	rq->sw_lro.lro_mgr.ip_summed_aggr	= CHECKSUM_UNNECESSARY;
	rq->flags |= (priv->pflags & MLX5E_PRIV_FLAG_SWLRO) ? MLX5E_RQ_FLAG_SWLRO : 0;
}
#endif

static int mlx5e_open_rq(struct mlx5e_channel *c,
			 struct mlx5e_rq_param *param,
			 struct mlx5e_rq *rq)
{
	int err;

	err = mlx5e_create_rq(c, param, rq);
	if (err)
		return err;

	err = mlx5e_enable_rq(rq, param);
	if (err)
		goto err_destroy_rq;

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	mlx5e_rq_sw_lro_init(rq);
#endif

	err = mlx5e_modify_rq(rq, MLX5_RQC_STATE_RST, MLX5_RQC_STATE_RDY);
	if (err)
		goto err_disable_rq;

	set_bit(MLX5E_RQ_STATE_POST_WQES_ENABLE, &rq->state);
	mlx5e_send_nop(&c->sq[0], true); /* trigger mlx5e_post_rx_wqes() */

	return 0;

err_disable_rq:
	mlx5e_disable_rq(rq);
err_destroy_rq:
	mlx5e_destroy_rq(rq);

	return err;
}

static void mlx5e_close_rq(struct mlx5e_rq *rq)
{
	clear_bit(MLX5E_RQ_STATE_POST_WQES_ENABLE, &rq->state);
	napi_synchronize(&rq->channel->napi); /* prevent mlx5e_post_rx_wqes */

	mlx5e_modify_rq(rq, MLX5_RQC_STATE_RDY, MLX5_RQC_STATE_ERR);
	while (!mlx5_wq_ll_is_empty(&rq->wq))
		msleep(20);

	/* avoid destroying rq before mlx5e_poll_rx_cq() is done with it */
	napi_synchronize(&rq->channel->napi);

	mlx5e_disable_rq(rq);
	mlx5e_destroy_rq(rq);
}

static void mlx5e_free_sq_db(struct mlx5e_sq *sq)
{
	kfree(sq->dma_fifo);
	kfree(sq->skb);
}

static int mlx5e_alloc_sq_db(struct mlx5e_sq *sq, int numa)
{
	int wq_sz = mlx5_wq_cyc_get_size(&sq->wq);
	int df_sz = wq_sz * MLX5_SEND_WQEBB_NUM_DS;

	sq->skb = kzalloc_node(wq_sz * sizeof(*sq->skb), GFP_KERNEL, numa);
	sq->dma_fifo = kzalloc_node(df_sz * sizeof(*sq->dma_fifo), GFP_KERNEL,
				    numa);

	if (!sq->skb || !sq->dma_fifo) {
		mlx5e_free_sq_db(sq);
		return -ENOMEM;
	}

	sq->dma_fifo_mask = df_sz - 1;

	return 0;
}

static int mlx5e_create_sq(struct mlx5e_channel *c,
			   int tc,
			   struct mlx5e_sq_param *param,
			   struct mlx5e_sq *sq)
{
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;

	void *sqc = param->sqc;
	void *sqc_wq = MLX5_ADDR_OF(sqc, sqc, wq);
	int txq_ix;
	int err;

	err = mlx5_alloc_map_uar(mdev, &sq->uar);
	if (err)
		return err;

	param->wq.db_numa_node = cpu_to_node(c->cpu);

	err = mlx5_wq_cyc_create(mdev, &param->wq, sqc_wq, &sq->wq,
				 &sq->wq_ctrl);
	if (err)
		goto err_unmap_free_uar;

	sq->wq.db       = &sq->wq.db[MLX5_SND_DBR];
	sq->uar_map     = sq->uar.map;
	sq->uar_bf_map  = sq->uar.bf_map;
	sq->bf_buf_size = (1 << MLX5_CAP_GEN(mdev, log_bf_reg_size)) / 2;
	sq->max_inline  = sq->bf_buf_size -
			  sizeof(struct mlx5e_tx_wqe) +
			  2 /*sizeof(mlx5e_tx_wqe.inline_hdr_start)*/;

	err = mlx5e_alloc_sq_db(sq, cpu_to_node(c->cpu));
	if (err)
		goto err_sq_wq_destroy;

	txq_ix = c->ix + tc * priv->params.num_channels;
	sq->txq = netdev_get_tx_queue(priv->netdev, txq_ix);
	priv->txq_to_sq_map[txq_ix] = sq;

	sq->pdev      = c->pdev;
	sq->mkey_be   = c->mkey_be;
	sq->channel   = c;
	sq->tc        = tc;
	sq->bf_budget = MLX5E_SQ_BF_BUDGET;
	sq->edge      = (sq->wq.sz_m1 + 1) - MLX5_SEND_WQE_MAX_WQEBBS;

	return 0;

err_sq_wq_destroy:
	mlx5_wq_destroy(&sq->wq_ctrl);

err_unmap_free_uar:
	mlx5_unmap_free_uar(mdev, &sq->uar);

	return err;
}

static void mlx5e_destroy_sq(struct mlx5e_sq *sq)
{
	struct mlx5e_channel *c = sq->channel;
	struct mlx5e_priv *priv = c->priv;

	mlx5e_free_sq_db(sq);
	mlx5_wq_destroy(&sq->wq_ctrl);
	mlx5_unmap_free_uar(priv->mdev, &sq->uar);
}

static int mlx5e_enable_sq(struct mlx5e_sq *sq, struct mlx5e_sq_param *param)
{
	struct mlx5e_channel *c = sq->channel;
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;

	void *in;
	void *sqc;
	void *wq;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(create_sq_in) +
		sizeof(u64) * sq->wq_ctrl.buf.npages;
	in = mlx5_vzalloc(inlen);
	if (!in)
		return -ENOMEM;

	sqc = MLX5_ADDR_OF(create_sq_in, in, ctx);
	wq = MLX5_ADDR_OF(sqc, sqc, wq);

	memcpy(sqc, param->sqc, sizeof(param->sqc));

	MLX5_SET(sqc,  sqc, tis_num_0,		priv->tisn[sq->tc]);
	MLX5_SET(sqc,  sqc, cqn,		c->sq[sq->tc].cq.mcq.cqn);
	MLX5_SET(sqc,  sqc, state,		MLX5_SQC_STATE_RST);
	MLX5_SET(sqc,  sqc, tis_lst_sz,		1);
	MLX5_SET(sqc,  sqc, flush_in_error_en,	1);

	MLX5_SET(wq,   wq, wq_type,       MLX5_WQ_TYPE_CYCLIC);
	MLX5_SET(wq,   wq, uar_page,      sq->uar.index);
	MLX5_SET(wq,   wq, log_wq_pg_sz,  sq->wq_ctrl.buf.page_shift -
					  PAGE_SHIFT);
	MLX5_SET64(wq, wq, dbr_addr,      sq->wq_ctrl.db.dma);

	mlx5_fill_page_array(&sq->wq_ctrl.buf,
			     (__be64 *)MLX5_ADDR_OF(wq, wq, pas));

	err = mlx5_core_create_sq(mdev, in, inlen, &sq->sqn);

	kvfree(in);

	return err;
}

static int mlx5e_modify_sq(struct mlx5e_sq *sq, int curr_state, int next_state)
{
	struct mlx5e_channel *c = sq->channel;
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;

	void *in;
	void *sqc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(modify_sq_in);
	in = mlx5_vzalloc(inlen);
	if (!in)
		return -ENOMEM;

	sqc = MLX5_ADDR_OF(modify_sq_in, in, ctx);

	MLX5_SET(modify_sq_in, in, sqn, sq->sqn);
	MLX5_SET(modify_sq_in, in, sq_state, curr_state);
	MLX5_SET(sqc, sqc, state, next_state);

	err = mlx5_core_modify_sq(mdev, in, inlen);

	kvfree(in);

	return err;
}

static void mlx5e_disable_sq(struct mlx5e_sq *sq)
{
	struct mlx5e_channel *c = sq->channel;
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;

	mlx5_core_destroy_sq(mdev, sq->sqn);
}

static int mlx5e_open_sq(struct mlx5e_channel *c,
			 int tc,
			 struct mlx5e_sq_param *param,
			 struct mlx5e_sq *sq)
{
	int err;

	err = mlx5e_create_sq(c, tc, param, sq);
	if (err)
		return err;

	err = mlx5e_enable_sq(sq, param);
	if (err)
		goto err_destroy_sq;

	err = mlx5e_modify_sq(sq, MLX5_SQC_STATE_RST, MLX5_SQC_STATE_RDY);
	if (err)
		goto err_disable_sq;

	set_bit(MLX5E_SQ_STATE_WAKE_TXQ_ENABLE, &sq->state);
	netdev_tx_reset_queue(sq->txq);
	netif_tx_start_queue(sq->txq);

	return 0;

err_disable_sq:
	mlx5e_disable_sq(sq);
err_destroy_sq:
	mlx5e_destroy_sq(sq);

	return err;
}

/* TODO: make this function general, i.e move to netdevice.h */
static inline void netif_tx_disable_queue(struct netdev_queue *txq)
{
	__netif_tx_lock_bh(txq);
	netif_tx_stop_queue(txq);
	__netif_tx_unlock_bh(txq);
}

static void mlx5e_close_sq(struct mlx5e_sq *sq)
{
	clear_bit(MLX5E_SQ_STATE_WAKE_TXQ_ENABLE, &sq->state);
	napi_synchronize(&sq->channel->napi); /* prevent netif_tx_wake_queue */
	netif_tx_disable_queue(sq->txq);

	/* ensure hw is notified of all pending wqes */
	if (mlx5e_sq_has_room_for(sq, 1))
		mlx5e_send_nop(sq, true);

	mlx5e_modify_sq(sq, MLX5_SQC_STATE_RDY, MLX5_SQC_STATE_ERR);
	while (sq->cc != sq->pc) /* wait till sq is empty */
		msleep(20);

	/* avoid destroying sq before mlx5e_poll_tx_cq() is done with it */
	napi_synchronize(&sq->channel->napi);

	mlx5e_disable_sq(sq);
	mlx5e_destroy_sq(sq);
}

static int mlx5e_create_cq(struct mlx5e_channel *c,
			   struct mlx5e_cq_param *param,
			   struct mlx5e_cq *cq)
{
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5_core_cq *mcq = &cq->mcq;
	int eqn_not_used;
	int irqn;
	int err;
	u32 i;

	param->wq.buf_numa_node = cpu_to_node(c->cpu);
	param->wq.db_numa_node  = cpu_to_node(c->cpu);
	param->eq_ix   = c->ix;

	err = mlx5_cqwq_create(mdev, &param->wq, param->cqc, &cq->wq,
			       &cq->wq_ctrl);
	if (err)
		return err;

	mlx5_vector2eqn(mdev, param->eq_ix, &eqn_not_used, &irqn);

	cq->napi        = &c->napi;

	mcq->cqe_sz     = 64;
	mcq->set_ci_db  = cq->wq_ctrl.db.db;
	mcq->arm_db     = cq->wq_ctrl.db.db + 1;
	*mcq->set_ci_db = 0;
	*mcq->arm_db    = 0;
	mcq->vector     = param->eq_ix;
	mcq->comp       = mlx5e_completion_event;
	mcq->event      = mlx5e_cq_error_event;
	mcq->irqn       = irqn;
	mcq->uar        = &priv->cq_uar;

	for (i = 0; i < mlx5_cqwq_get_size(&cq->wq); i++) {
		struct mlx5_cqe64 *cqe = mlx5_cqwq_get_wqe(&cq->wq, i);

		cqe->op_own = 0xf1;
	}

	cq->channel = c;

	return 0;
}

static void mlx5e_destroy_cq(struct mlx5e_cq *cq)
{
	mlx5_wq_destroy(&cq->wq_ctrl);
}

static int mlx5e_enable_cq(struct mlx5e_cq *cq, struct mlx5e_cq_param *param)
{
	struct mlx5e_channel *c = cq->channel;
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5_core_cq *mcq = &cq->mcq;

	void *in;
	void *cqc;
	int inlen;
	int irqn_not_used;
	int eqn;
	int err;

	inlen = MLX5_ST_SZ_BYTES(create_cq_in) +
		sizeof(u64) * cq->wq_ctrl.buf.npages;
	in = mlx5_vzalloc(inlen);
	if (!in)
		return -ENOMEM;

	cqc = MLX5_ADDR_OF(create_cq_in, in, cq_context);

	memcpy(cqc, param->cqc, sizeof(param->cqc));

	mlx5_fill_page_array(&cq->wq_ctrl.buf,
			     (__be64 *)MLX5_ADDR_OF(create_cq_in, in, pas));

	mlx5_vector2eqn(mdev, param->eq_ix, &eqn, &irqn_not_used);

	MLX5_SET(cqc,   cqc, c_eqn,         eqn);
	MLX5_SET(cqc,   cqc, uar_page,      mcq->uar->index);
	MLX5_SET(cqc,   cqc, log_page_size, cq->wq_ctrl.buf.page_shift -
					    PAGE_SHIFT);
	MLX5_SET64(cqc, cqc, dbr_addr,      cq->wq_ctrl.db.dma);

	err = mlx5_core_create_cq(mdev, mcq, in, inlen);

	kvfree(in);

	if (err)
		return err;

	mlx5e_cq_arm(cq);

	return 0;
}

static void mlx5e_disable_cq(struct mlx5e_cq *cq)
{
	struct mlx5e_channel *c = cq->channel;
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;

	mlx5_core_destroy_cq(mdev, &cq->mcq);
}

static int mlx5e_open_cq(struct mlx5e_channel *c,
			 struct mlx5e_cq_param *param,
			 struct mlx5e_cq *cq,
			 u16 moderation_usecs,
			 u16 moderation_frames)
{
	int err;
	struct mlx5e_priv *priv = c->priv;
	struct mlx5_core_dev *mdev = priv->mdev;

	err = mlx5e_create_cq(c, param, cq);
	if (err)
		return err;

	err = mlx5e_enable_cq(cq, param);
	if (err)
		goto err_destroy_cq;

	err = mlx5_core_modify_cq_moderation(mdev, &cq->mcq,
					     moderation_usecs,
					     moderation_frames);
	if (err)
		goto err_destroy_cq;

	return 0;

err_destroy_cq:
	mlx5e_destroy_cq(cq);

	return err;
}

static void mlx5e_close_cq(struct mlx5e_cq *cq)
{
	mlx5e_disable_cq(cq);
	mlx5e_destroy_cq(cq);
}

static int mlx5e_get_cpu(struct mlx5e_priv *priv, int ix)
{
#ifdef CONFIG_CPUMASK_OFFSTACK
	cpumask_var_t affinity_mask = priv->mdev->priv.irq_info[ix].mask;

	return affinity_mask ? cpumask_first(affinity_mask) : 0;
#else                              
	return 0;                                     
#endif
}

static void mlx5e_build_tc_to_txq_map(struct mlx5e_channel *c,
				      int num_channels)
{
	int i;

	for (i = 0; i < MLX5E_MAX_NUM_TC; i++)
		c->tc_to_txq_map[i] = c->ix + i * num_channels;
}

static int mlx5e_open_tx_cqs(struct mlx5e_channel *c,
			     struct mlx5e_channel_param *cparam)
{
	struct mlx5e_priv *priv = c->priv;
	int err;
	int tc;

	for (tc = 0; tc < c->num_tc; tc++) {
		err = mlx5e_open_cq(c, &cparam->tx_cq, &c->sq[tc].cq,
				    priv->params.tx_cq_moderation_usec,
				    priv->params.tx_cq_moderation_pkts);
		if (err)
			goto err_close_tx_cqs;
	}

	return 0;

err_close_tx_cqs:
	for (tc--; tc >= 0; tc--)
		mlx5e_close_cq(&c->sq[tc].cq);

	return err;
}

static void mlx5e_close_tx_cqs(struct mlx5e_channel *c)
{
	int tc;

	for (tc = 0; tc < c->num_tc; tc++)
		mlx5e_close_cq(&c->sq[tc].cq);
}

static int mlx5e_open_sqs(struct mlx5e_channel *c,
			  struct mlx5e_channel_param *cparam)
{
	int err;
	int tc;

	for (tc = 0; tc < c->num_tc; tc++) {
		err = mlx5e_open_sq(c, tc, &cparam->sq, &c->sq[tc]);
		if (err)
			goto err_close_sqs;
	}

	return 0;

err_close_sqs:
	for (tc--; tc >= 0; tc--)
		mlx5e_close_sq(&c->sq[tc]);

	return err;
}

static void mlx5e_close_sqs(struct mlx5e_channel *c)
{
	int tc;

	for (tc = 0; tc < c->num_tc; tc++)
		mlx5e_close_sq(&c->sq[tc]);
}

static int mlx5e_open_channel(struct mlx5e_priv *priv, int ix,
			      struct mlx5e_channel_param *cparam,
			      struct mlx5e_channel **cp)
{
	struct net_device *netdev = priv->netdev;
	int cpu = mlx5e_get_cpu(priv, ix);
	struct mlx5e_channel *c;
	int err;

	c = kzalloc_node(sizeof(*c), GFP_KERNEL, cpu_to_node(cpu));
	if (!c)
		return -ENOMEM;

	c->priv     = priv;
	c->ix       = ix;
	c->cpu      = cpu;
	c->pdev     = &priv->mdev->pdev->dev;
	c->netdev   = priv->netdev;
	c->mkey_be  = cpu_to_be32(priv->mr.key);
	c->num_tc   = priv->params.num_tc;

	mlx5e_build_tc_to_txq_map(c, priv->params.num_channels);

	netif_napi_add(netdev, &c->napi, mlx5e_napi_poll, 64);

	err = mlx5e_open_tx_cqs(c, cparam);
	if (err)
		goto err_napi_del;

	err = mlx5e_open_cq(c, &cparam->rx_cq, &c->rq.cq,
			    priv->params.rx_cq_moderation_usec,
			    priv->params.rx_cq_moderation_pkts);
	if (err)
		goto err_close_tx_cqs;

	napi_enable(&c->napi);

	err = mlx5e_open_sqs(c, cparam);
	if (err)
		goto err_disable_napi;

	err = mlx5e_open_rq(c, &cparam->rq, &c->rq);
	if (err)
		goto err_close_sqs;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)) || \
	defined(CONFIG_COMPAT_IS_NETIF_SET_XPS_QUEUE_NOT_CONST_CPUMASK)
	netif_set_xps_queue(netdev, (struct cpumask *)get_cpu_mask(c->cpu), ix);
#else
	netif_set_xps_queue(netdev, get_cpu_mask(c->cpu), ix);
#endif
	*cp = c;

	return 0;

err_close_sqs:
	mlx5e_close_sqs(c);

err_disable_napi:
	napi_disable(&c->napi);
	mlx5e_close_cq(&c->rq.cq);

err_close_tx_cqs:
	mlx5e_close_tx_cqs(c);

err_napi_del:
	netif_napi_del(&c->napi);
	kfree(c);

	return err;
}

static void mlx5e_close_channel(struct mlx5e_channel *c)
{
	mlx5e_close_rq(&c->rq);
	mlx5e_close_sqs(c);
	napi_disable(&c->napi);
	mlx5e_close_cq(&c->rq.cq);
	mlx5e_close_tx_cqs(c);
	netif_napi_del(&c->napi);
	kfree(c);
}

static void mlx5e_build_rq_param(struct mlx5e_priv *priv,
				 struct mlx5e_rq_param *param)
{
	void *rqc = param->rqc;
	void *wq = MLX5_ADDR_OF(rqc, rqc, wq);

	MLX5_SET(wq, wq, wq_type,          MLX5_WQ_TYPE_LINKED_LIST);
	MLX5_SET(wq, wq, end_padding_mode, MLX5_WQ_END_PAD_MODE_ALIGN);
	MLX5_SET(wq, wq, log_wq_stride,    ilog2(sizeof(struct mlx5e_rx_wqe)));
	MLX5_SET(wq, wq, log_wq_sz,        priv->params.log_rq_size);
	MLX5_SET(wq, wq, pd,               priv->pdn);

	param->wq.buf_numa_node = dev_to_node(&priv->mdev->pdev->dev);
	param->wq.linear = 1;
}

static void mlx5e_build_sq_param(struct mlx5e_priv *priv,
				 struct mlx5e_sq_param *param)
{
	void *sqc = param->sqc;
	void *wq = MLX5_ADDR_OF(sqc, sqc, wq);

	MLX5_SET(wq, wq, log_wq_sz,     priv->params.log_sq_size);
	MLX5_SET(wq, wq, log_wq_stride, ilog2(MLX5_SEND_WQE_BB));
	MLX5_SET(wq, wq, pd,            priv->pdn);

	param->wq.buf_numa_node = dev_to_node(&priv->mdev->pdev->dev);
}

static void mlx5e_build_common_cq_param(struct mlx5e_priv *priv,
					struct mlx5e_cq_param *param)
{
	void *cqc = param->cqc;

	MLX5_SET(cqc, cqc, uar_page, priv->cq_uar.index);
}

static void mlx5e_build_rx_cq_param(struct mlx5e_priv *priv,
				    struct mlx5e_cq_param *param)
{
	void *cqc = param->cqc;

	MLX5_SET(cqc, cqc, log_cq_size,  priv->params.log_rq_size);

	mlx5e_build_common_cq_param(priv, param);
}

static void mlx5e_build_tx_cq_param(struct mlx5e_priv *priv,
				    struct mlx5e_cq_param *param)
{
	void *cqc = param->cqc;

	MLX5_SET(cqc, cqc, log_cq_size,  priv->params.log_sq_size);

	mlx5e_build_common_cq_param(priv, param);
}

static void mlx5e_build_channel_param(struct mlx5e_priv *priv,
				      struct mlx5e_channel_param *cparam)
{
	memset(cparam, 0, sizeof(*cparam));

	mlx5e_build_rq_param(priv, &cparam->rq);
	mlx5e_build_sq_param(priv, &cparam->sq);
	mlx5e_build_rx_cq_param(priv, &cparam->rx_cq);
	mlx5e_build_tx_cq_param(priv, &cparam->tx_cq);
}

static int mlx5e_open_channels(struct mlx5e_priv *priv)
{
	struct mlx5e_channel_param cparam;
	int nch = priv->params.num_channels;
	int err = -ENOMEM;
	int i;
	int j;

	priv->channel = kcalloc(nch, sizeof(struct mlx5e_channel *),
				GFP_KERNEL);

	priv->txq_to_sq_map = kcalloc(nch * priv->params.num_tc,
				      sizeof(struct mlx5e_sq *), GFP_KERNEL);

	if (!priv->channel || !priv->txq_to_sq_map)
		goto err_free_txq_to_sq_map;

	mlx5e_build_channel_param(priv, &cparam);
	for (i = 0; i < nch; i++) {
		err = mlx5e_open_channel(priv, i, &cparam, &priv->channel[i]);
		if (err)
			goto err_close_channels;
	}

	for (j = 0; j < nch; j++) {
		err = mlx5e_wait_for_min_rx_wqes(&priv->channel[j]->rq);
		if (err)
			goto err_close_channels;
	}

	return 0;

err_close_channels:
	for (i--; i >= 0; i--)
		mlx5e_close_channel(priv->channel[i]);

err_free_txq_to_sq_map:
	kfree(priv->txq_to_sq_map);
	kfree(priv->channel);

	return err;
}

static void mlx5e_rename_channels_eqs(struct mlx5e_priv *priv)
{
	int i;
	int err;

	for (i = 0; i < priv->params.num_channels; i++) {
		err = mlx5_rename_eq(priv->mdev, i, priv->netdev->name);
		if (err)
			netdev_err(priv->netdev,
				   "%s: mlx5_rename_eq failed: %d\n",
				   __func__, err);
	}
}

static void mlx5e_close_channels(struct mlx5e_priv *priv)
{
	int i;

	for (i = 0; i < priv->params.num_channels; i++)
		mlx5e_close_channel(priv->channel[i]);

	kfree(priv->txq_to_sq_map);
	kfree(priv->channel);
}

static int mlx5e_open_tis(struct mlx5e_priv *priv, int tc)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	u32 in[MLX5_ST_SZ_DW(create_tis_in)];
	void *tisc = MLX5_ADDR_OF(create_tis_in, in, ctx);

	memset(in, 0, sizeof(in));

	MLX5_SET(tisc, tisc, prio,  tc);
	MLX5_SET(tisc, tisc, transport_domain, priv->tdn);

	return mlx5_core_create_tis(mdev, in, sizeof(in), &priv->tisn[tc]);
}

static void mlx5e_close_tis(struct mlx5e_priv *priv, int tc)
{
	mlx5_core_destroy_tis(priv->mdev, priv->tisn[tc]);
}

static int mlx5e_open_tises(struct mlx5e_priv *priv)
{
	int err;
	int tc;

	for (tc = 0; tc < priv->params.num_tc; tc++) {
		err = mlx5e_open_tis(priv, tc);
		if (err)
			goto err_close_tises;
	}

	return 0;

err_close_tises:
	for (tc--; tc >= 0; tc--)
		mlx5e_close_tis(priv, tc);

	return err;
}

static void mlx5e_close_tises(struct mlx5e_priv *priv)
{
	int tc;

	for (tc = 0; tc < priv->params.num_tc; tc++)
		mlx5e_close_tis(priv, tc);
}

static int mlx5e_bits_invert(unsigned long a, int size)
{
	int i;
	int inv = 0;

	for (i = 0; i < size; i++)
		inv |= (test_bit(size - i - 1, &a) ? 1 : 0) << i;

	return inv;
}

static int mlx5e_open_rqt(struct mlx5e_priv *priv)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	u32 *in;
	u32 out[MLX5_ST_SZ_DW(create_rqt_out)];
	void *rqtc;
	int inlen;
	int err;
	int log_tbl_sz = priv->params.rx_hash_log_tbl_sz;
	int sz = 1 << log_tbl_sz;
	int i;

	inlen = MLX5_ST_SZ_BYTES(create_rqt_in) + sizeof(u32) * sz;
	in = mlx5_vzalloc(inlen);
	if (!in)
		return -ENOMEM;

	rqtc = MLX5_ADDR_OF(create_rqt_in, in, rqt_context);

	MLX5_SET(rqtc, rqtc, rqt_actual_size, sz);
	MLX5_SET(rqtc, rqtc, rqt_max_size, sz);

	for (i = 0; i < sz; i++) {
		int ix = i;

		if (priv->params.rss_hash_xor)
			ix = mlx5e_bits_invert(i, log_tbl_sz);

		ix = ix % priv->params.num_channels;
		MLX5_SET(rqtc, rqtc, rq_num[i], priv->channel[ix]->rq.rqn);
	}

	MLX5_SET(create_rqt_in, in, opcode, MLX5_CMD_OP_CREATE_RQT);

	memset(out, 0, sizeof(out));
	err = mlx5_cmd_exec_check_status(mdev, in, inlen, out, sizeof(out));
	if (!err)
		priv->rqtn = MLX5_GET(create_rqt_out, out, rqtn);

	kvfree(in);

	return err;
}

static void mlx5e_close_rqt(struct mlx5e_priv *priv)
{
	u32 in[MLX5_ST_SZ_DW(destroy_rqt_in)];
	u32 out[MLX5_ST_SZ_DW(destroy_rqt_out)];

	memset(in, 0, sizeof(in));

	MLX5_SET(destroy_rqt_in, in, opcode, MLX5_CMD_OP_DESTROY_RQT);
	MLX5_SET(destroy_rqt_in, in, rqtn, priv->rqtn);

	mlx5_cmd_exec_check_status(priv->mdev, in, sizeof(in), out,
				   sizeof(out));
}

static void mlx5e_build_tir_ctx(struct mlx5e_priv *priv, u32 *tirc, int tt)
{
	void *hfso = MLX5_ADDR_OF(tirc, tirc, rx_hash_field_selector_outer);

	MLX5_SET(tirc, tirc, transport_domain, priv->tdn);

#define ROUGH_MAX_L2_L3_HDR_SZ 256

#define MLX5_HASH_IP            (MLX5_HASH_FIELD_SEL_SRC_IP   |\
				 MLX5_HASH_FIELD_SEL_DST_IP)

#define MLX5_HASH_IP_L4PORTS    (MLX5_HASH_FIELD_SEL_SRC_IP   |\
				 MLX5_HASH_FIELD_SEL_DST_IP   |\
				 MLX5_HASH_FIELD_SEL_L4_SPORT |\
				 MLX5_HASH_FIELD_SEL_L4_DPORT)

#define MLX5_HASH_IP_IPSEC_SPI  (MLX5_HASH_FIELD_SEL_SRC_IP   |\
				 MLX5_HASH_FIELD_SEL_DST_IP   |\
				 MLX5_HASH_FIELD_SEL_IPSEC_SPI)

	if (priv->params.lro_en) {
		MLX5_SET(tirc, tirc, lro_enable_mask,
			 MLX5_TIRC_LRO_ENABLE_MASK_IPV4_LRO |
			 MLX5_TIRC_LRO_ENABLE_MASK_IPV6_LRO);
		MLX5_SET(tirc, tirc, lro_max_ip_payload_size,
			 (priv->params.lro_wqe_sz -
			  ROUGH_MAX_L2_L3_HDR_SZ) >> 8);
		/* TODO: add the option to choose timer value dynamically */
		MLX5_SET(tirc, tirc, lro_timeout_period_usecs,
			 MLX5_CAP_ETH(priv->mdev,
				      lro_timer_supported_periods[3]));
	}

	switch (tt) {
	case MLX5E_TT_ANY:
		MLX5_SET(tirc, tirc, disp_type,
			 MLX5_TIRC_DISP_TYPE_DIRECT);
		MLX5_SET(tirc, tirc, inline_rqn,
			 priv->channel[0]->rq.rqn);
		break;
	default:
		MLX5_SET(tirc, tirc, disp_type,
			 MLX5_TIRC_DISP_TYPE_INDIRECT);
		MLX5_SET(tirc, tirc, indirect_table,
			 priv->rqtn);
		if (priv->params.rss_hash_xor) {
			MLX5_SET(tirc, tirc, rx_hash_fn,
				 MLX5_TIRC_RX_HASH_FN_HASH_INVERTED_XOR8);
		} else {
			void *rss_key = MLX5_ADDR_OF(tirc, tirc,
						     rx_hash_toeplitz_key);
			size_t len = MLX5_FLD_SZ_BYTES(tirc,
						       rx_hash_toeplitz_key);

			MLX5_SET(tirc, tirc, rx_hash_fn,
				 MLX5_TIRC_RX_HASH_FN_HASH_TOEPLITZ);
			MLX5_SET(tirc, tirc, rx_hash_symmetric, 1);

			netdev_rss_key_fill(rss_key, len);
		}
		break;
	}

	switch (tt) {
	case MLX5E_TT_IPV4_TCP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV4);
		MLX5_SET(rx_hash_field_select, hfso, l4_prot_type,
			 MLX5_L4_PROT_TYPE_TCP);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_L4PORTS);
		break;

	case MLX5E_TT_IPV6_TCP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV6);
		MLX5_SET(rx_hash_field_select, hfso, l4_prot_type,
			 MLX5_L4_PROT_TYPE_TCP);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_L4PORTS);
		break;

	case MLX5E_TT_IPV4_UDP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV4);
		MLX5_SET(rx_hash_field_select, hfso, l4_prot_type,
			 MLX5_L4_PROT_TYPE_UDP);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_L4PORTS);
		break;

	case MLX5E_TT_IPV6_UDP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV6);
		MLX5_SET(rx_hash_field_select, hfso, l4_prot_type,
			 MLX5_L4_PROT_TYPE_UDP);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_L4PORTS);
		break;

	case MLX5E_TT_IPV4_IPSEC_AH:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV4);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_IPSEC_SPI);
		break;

	case MLX5E_TT_IPV6_IPSEC_AH:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV6);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_IPSEC_SPI);
		break;

	case MLX5E_TT_IPV4_IPSEC_ESP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV4);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_IPSEC_SPI);
		break;

	case MLX5E_TT_IPV6_IPSEC_ESP:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV6);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP_IPSEC_SPI);
		break;

	case MLX5E_TT_IPV4:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV4);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP);
		break;

	case MLX5E_TT_IPV6:
		MLX5_SET(rx_hash_field_select, hfso, l3_prot_type,
			 MLX5_L3_PROT_TYPE_IPV6);
		MLX5_SET(rx_hash_field_select, hfso, selected_fields,
			 MLX5_HASH_IP);
		break;
	}
}

static int mlx5e_open_tir(struct mlx5e_priv *priv, int tt)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	u32 *in;
	void *tirc;
	int inlen;
	int err;

	inlen = MLX5_ST_SZ_BYTES(create_tir_in);
	in = mlx5_vzalloc(inlen);
	if (!in)
		return -ENOMEM;

	tirc = MLX5_ADDR_OF(create_tir_in, in, ctx);

	mlx5e_build_tir_ctx(priv, tirc, tt);

	err = mlx5_core_create_tir(mdev, in, inlen, &priv->tirn[tt]);

	kvfree(in);

	return err;
}

static void mlx5e_close_tir(struct mlx5e_priv *priv, int tt)
{
	mlx5_core_destroy_tir(priv->mdev, priv->tirn[tt]);
}

static int mlx5e_open_tirs(struct mlx5e_priv *priv)
{
	int err;
	int i;

	for (i = 0; i < MLX5E_NUM_TT; i++) {
		err = mlx5e_open_tir(priv, i);
		if (err)
			goto err_close_tirs;
	}

	return 0;

err_close_tirs:
	for (i--; i >= 0; i--)
		mlx5e_close_tir(priv, i);

	return err;
}

static void mlx5e_close_tirs(struct mlx5e_priv *priv)
{
	int i;

	for (i = 0; i < MLX5E_NUM_TT; i++)
		mlx5e_close_tir(priv, i);
}

static void mlx5e_netdev_set_tcs(struct net_device *netdev)
{
#ifdef HAVE_NDO_SETUP_TC
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int nch = priv->params.num_channels;
	int ntc = priv->params.num_tc;
	int prio;
	int tc;

	netdev_reset_tc(netdev);

	if (ntc == 1)
		return;

	netdev_set_num_tc(netdev, ntc);

	for (tc = 0; tc < ntc; tc++)
		netdev_set_tc_queue(netdev, tc, nch, tc * nch);

	for (prio = 0; prio < MLX5E_MAX_NUM_PRIO; prio++)
		netdev_set_prio_tc_map(netdev, prio, prio % ntc);
#endif
}

static int mlx5e_set_dev_port_mtu(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
	int hw_mtu;
	int err;

	err = mlx5_set_port_mtu(mdev, MLX5E_SW2HW_MTU(netdev->mtu));
	if (err)
		return err;

	mlx5_query_port_oper_mtu(mdev, &hw_mtu);

	if (MLX5E_HW2SW_MTU(hw_mtu) != netdev->mtu)
		netdev_warn(netdev, "%s: Port MTU %d is different than netdev mtu %d\n",
			    __func__, MLX5E_HW2SW_MTU(hw_mtu), netdev->mtu);

	netdev->mtu = MLX5E_HW2SW_MTU(hw_mtu);
	return 0;
}

int mlx5e_open_locked(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int num_txqs;
	int err;

	mlx5e_netdev_set_tcs(netdev);

	num_txqs = priv->params.num_channels * priv->params.num_tc;
	netif_set_real_num_tx_queues(netdev, num_txqs);
	netif_set_real_num_rx_queues(netdev, priv->params.num_channels);

	err = mlx5e_set_dev_port_mtu(netdev);
	if (err)
		return err;

	err = mlx5e_open_tises(priv);
	if (err) {
		netdev_err(netdev, "%s: mlx5e_open_tises failed, %d\n",
			   __func__, err);
		return err;
	}

	err = mlx5e_open_channels(priv);
	if (err) {
		netdev_err(netdev, "%s: mlx5e_open_channels failed, %d\n",
			   __func__, err);
		goto err_close_tises;
	}

	err = mlx5e_open_rqt(priv);
	if (err) {
		netdev_err(netdev, "%s: mlx5e_open_rqt failed, %d\n",
			   __func__, err);
		goto err_close_channels;
	}

	err = mlx5e_open_tirs(priv);
	if (err) {
		netdev_err(netdev, "%s: mlx5e_open_tir failed, %d\n",
			   __func__, err);
		goto err_close_rqls;
	}

	err = mlx5e_open_flow_table(priv);
	if (err) {
		netdev_err(netdev, "%s: mlx5e_open_flow_table failed, %d\n",
			   __func__, err);
		goto err_close_tirs;
	}

	err = mlx5e_add_all_vlan_rules(priv);
	if (err) {
		netdev_err(netdev, "%s: mlx5e_add_all_vlan_rules failed, %d\n",
			   __func__, err);
		goto err_close_flow_table;
	}

	mlx5e_rename_channels_eqs(priv);
	mlx5e_init_eth_addr(priv);

	set_bit(MLX5E_STATE_OPENED, &priv->state);

	mlx5e_create_debugfs(priv);
	mlx5e_update_carrier(priv);
	mlx5e_set_rx_mode_core(priv);

	schedule_delayed_work(&priv->update_stats_work, 0);
	return 0;

err_close_flow_table:
	mlx5e_close_flow_table(priv);

err_close_tirs:
	mlx5e_close_tirs(priv);

err_close_rqls:
	mlx5e_close_rqt(priv);

err_close_channels:
	mlx5e_close_channels(priv);

err_close_tises:
	mlx5e_close_tises(priv);

	return err;
}

static int mlx5e_open(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int err;

	mutex_lock(&priv->state_lock);
	err = mlx5e_open_locked(netdev);
	mutex_unlock(&priv->state_lock);

	return err;
}

int mlx5e_close_locked(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	clear_bit(MLX5E_STATE_OPENED, &priv->state);

	mlx5e_set_rx_mode_core(priv);
	mlx5e_del_all_vlan_rules(priv);
	netif_carrier_off(priv->netdev);
	mlx5e_destroy_debugfs(priv);
	mlx5e_close_flow_table(priv);
	mlx5e_close_tirs(priv);
	mlx5e_close_rqt(priv);
	mlx5e_close_channels(priv);
	mlx5e_close_tises(priv);

	return 0;
}

static int mlx5e_close(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	int err;

	mutex_lock(&priv->state_lock);
	err = mlx5e_close_locked(netdev);
	mutex_unlock(&priv->state_lock);

	return err;
}

int mlx5e_update_priv_params(struct mlx5e_priv *priv,
			     struct mlx5e_params *new_params)
{
	int err = 0;
	int was_opened;

	WARN_ON(!mutex_is_locked(&priv->state_lock));

	was_opened = test_bit(MLX5E_STATE_OPENED, &priv->state);
	if (was_opened)
		mlx5e_close_locked(priv->netdev);

	priv->params = *new_params;

	if (was_opened)
		err = mlx5e_open_locked(priv->netdev);

	return err;
}

#ifdef HAVE_NDO_SETUP_TC
static int mlx5e_setup_tc(struct net_device *netdev, u8 tc)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5e_params new_params;
	int err;

	if (tc > MLX5E_MAX_NUM_TC)
		return -EINVAL;

	mutex_lock(&priv->state_lock);
	new_params = priv->params;
	new_params.num_tc = tc ? tc : 1;
	err = mlx5e_update_priv_params(priv, &new_params);
	mutex_unlock(&priv->state_lock);

	return err;
}
#endif

#ifdef HAVE_NDO_GET_STATS64
static struct rtnl_link_stats64 *
mlx5e_get_stats(struct net_device *dev, struct rtnl_link_stats64 *stats)
#else
static struct net_device_stats *mlx5e_get_stats(struct net_device *dev)
#endif
{
	struct mlx5e_priv *priv = netdev_priv(dev);
	struct mlx5e_vport_stats *vstats = &priv->stats.vport;

#ifndef HAVE_NDO_GET_STATS64
	struct net_device_stats *stats = &priv->netdev_stats;
#endif

	stats->rx_packets = vstats->rx_packets;
	stats->rx_bytes   = vstats->rx_bytes;
	stats->tx_packets = vstats->tx_packets;
	stats->tx_bytes   = vstats->tx_bytes;
	stats->multicast  = vstats->rx_multicast_packets +
			    vstats->tx_multicast_packets;
	stats->tx_errors  = vstats->tx_error_packets;
	stats->rx_errors  = vstats->rx_error_packets;
	stats->tx_dropped = vstats->tx_queue_dropped;
	/* TODO: replace 0s with true values */
	stats->rx_crc_errors = 0;
	stats->rx_length_errors = 0;

	return stats;
}

static void mlx5e_set_rx_mode(struct net_device *dev)
{
	struct mlx5e_priv *priv = netdev_priv(dev);

	schedule_work(&priv->set_rx_mode_work);
}

static int mlx5e_set_mac(struct net_device *netdev, void *addr)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct sockaddr *saddr = addr;

	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	netif_addr_lock_bh(netdev);
	ether_addr_copy(netdev->dev_addr, saddr->sa_data);
	netif_addr_unlock_bh(netdev);

	schedule_work(&priv->set_rx_mode_work);

	return 0;
}

#if (defined(HAVE_NDO_SET_FEATURES) || defined(HAVE_NET_DEVICE_OPS_EXT))
static int mlx5e_set_features(struct net_device *netdev,
#ifdef HAVE_NET_DEVICE_OPS_EXT
			      u32 features)
#else
			      netdev_features_t features)
#endif
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	netdev_features_t changes = features ^ netdev->features;
	struct mlx5e_params new_params;
	bool update_params = false;

	mutex_lock(&priv->state_lock);
	new_params = priv->params;

	if (changes & NETIF_F_LRO) {
		new_params.lro_en = !!(features & NETIF_F_LRO);
		update_params = true;
	}

	if (update_params)
		mlx5e_update_priv_params(priv, &new_params);

	if (changes & NETIF_F_HW_VLAN_CTAG_FILTER) {
		if (features & NETIF_F_HW_VLAN_CTAG_FILTER)
			mlx5e_enable_vlan_filter(priv);
		else
			mlx5e_disable_vlan_filter(priv);
	}

	mutex_unlock(&priv->state_lock);

	return 0;
}
#endif

static int mlx5e_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
	int max_mtu;
	int err;

	mlx5_query_port_max_mtu(mdev, &max_mtu);

	if (MLX5E_SW2HW_MTU(new_mtu) > min_t(int, MLX5E_MAX_MTU, max_mtu)) {
		netdev_err(netdev,
			   "%s: Bad MTU (%d) > (%d) Max\n",
			   __func__, new_mtu, max_mtu);
		return -EINVAL;
	}

	mutex_lock(&priv->state_lock);
	netdev->mtu = new_mtu;
	err = mlx5e_update_priv_params(priv, &priv->params);
	mutex_unlock(&priv->state_lock);

	return err;
}

#if defined HAVE_VLAN_GRO_RECEIVE || defined HAVE_VLAN_HWACCEL_RX
void mlx5e_vlan_register(struct net_device *netdev, struct vlan_group *grp)
{
        struct mlx5e_priv *priv = netdev_priv(netdev);
        priv->vlan_grp = grp;
}
#endif

static struct net_device_ops mlx5e_netdev_ops = {
	.ndo_open                = mlx5e_open,
	.ndo_stop                = mlx5e_close,
	.ndo_start_xmit          = mlx5e_xmit,
#ifdef HAVE_NDO_SETUP_TC
	.ndo_setup_tc            = mlx5e_setup_tc,
#endif
/*	.ndo_select_queue        = mlx5e_select_queue, // issue 549663 */
#ifdef HAVE_NDO_GET_STATS64
	.ndo_get_stats64         = mlx5e_get_stats,
#else
	.ndo_get_stats           = mlx5e_get_stats,
#endif
	.ndo_set_rx_mode         = mlx5e_set_rx_mode,
	.ndo_set_mac_address     = mlx5e_set_mac,
	.ndo_vlan_rx_add_vid	 = mlx5e_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	 = mlx5e_vlan_rx_kill_vid,

#if defined HAVE_VLAN_GRO_RECEIVE || defined HAVE_VLAN_HWACCEL_RX
	.ndo_vlan_rx_register	 = mlx5e_vlan_register,
#endif
#if (defined(HAVE_NDO_SET_FEATURES) && !defined(HAVE_NET_DEVICE_OPS_EXT))
	.ndo_set_features        = mlx5e_set_features,
#endif
	.ndo_change_mtu		 = mlx5e_change_mtu,
};

#ifdef HAVE_NET_DEVICE_OPS_EXT
static const struct net_device_ops_ext mlx5_netdev_ops_ext = {
	.size             = sizeof(struct net_device_ops_ext),
	.ndo_set_features = mlx5e_set_features,
};
#endif 

static int mlx5e_check_required_hca_cap(struct mlx5_core_dev *mdev)
{
	if (MLX5_CAP_GEN(mdev, port_type) != MLX5_CAP_PORT_TYPE_ETH)
		return -ENOTSUPP;
	/* TODO: cehck if more caps are needed */
	if (!MLX5_CAP_GEN(mdev, eth_net_offloads) ||
	    !MLX5_CAP_GEN(mdev, nic_flow_table) ||
	    /* TODO: move following caps to control path (NETDEV Flags/OPs) */
	    !MLX5_CAP_ETH(mdev, csum_cap) ||
	    !MLX5_CAP_ETH(mdev, max_lso_cap) ||
	    !MLX5_CAP_ETH(mdev, vlan_cap) ||
	    !MLX5_CAP_ETH(mdev, rss_ind_tbl_cap) ||
	    MLX5_CAP_FLOWTABLE(mdev,
			       flow_table_properties_nic_receive.max_ft_level)
			       < 3) {
		mlx5_core_warn(mdev,
			       "Not creating net device, some required device capabilities are missing\n");
		return -ENOTSUPP;
	}
	return 0;
}

static void mlx5e_build_netdev_priv(struct mlx5_core_dev *mdev,
				    struct net_device *netdev,
				    int num_comp_vectors)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	/* TODO: consider link speed for setting the following:
	 *       log_sq_size
	 *       log_rq_size
	 *       cq moderation?
	 *       lro_timeout_period_usecs@mlx5e_build_tir_ctx()
	 */
	priv->params.log_sq_size           =
		MLX5E_PARAMS_DEFAULT_LOG_SQ_SIZE;
	priv->params.log_rq_size           =
		MLX5E_PARAMS_DEFAULT_LOG_RQ_SIZE;
	priv->params.rx_cq_moderation_usec =
		MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_USEC;
	priv->params.rx_cq_moderation_pkts =
		MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_PKTS;
	priv->params.tx_cq_moderation_usec =
		MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_USEC;
	priv->params.tx_cq_moderation_pkts =
		MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_PKTS;
	priv->params.min_rx_wqes           =
		MLX5E_PARAMS_DEFAULT_MIN_RX_WQES;
	priv->params.rx_hash_log_tbl_sz    =
		(order_base_2(num_comp_vectors) >
		 MLX5E_PARAMS_DEFAULT_RX_HASH_LOG_TBL_SZ) ?
		order_base_2(num_comp_vectors)           :
		MLX5E_PARAMS_DEFAULT_RX_HASH_LOG_TBL_SZ;
	priv->params.num_tc                = 1;
	priv->params.default_vlan_prio     = 0;

	priv->params.rss_hash_xor = true;

	/* TODO: add user ability to configure lro wqe size */
	/* we disable lro by default, user can enable via ethtool */
	priv->params.lro_en = false && !!MLX5_CAP_ETH(priv->mdev, lro_cap);
	priv->params.lro_wqe_sz            =
		MLX5E_PARAMS_DEFAULT_LRO_WQE_SZ;

	priv->mdev                         = mdev;
	priv->netdev                       = netdev;
	priv->params.num_channels          = num_comp_vectors;
	priv->default_vlan_prio            = priv->params.default_vlan_prio;

	spin_lock_init(&priv->async_events_spinlock);
	mutex_init(&priv->state_lock);

	INIT_WORK(&priv->update_carrier_work, mlx5e_update_carrier_work);
	INIT_WORK(&priv->set_rx_mode_work, mlx5e_set_rx_mode_work);
	INIT_DELAYED_WORK(&priv->update_stats_work, mlx5e_update_stats_work);
}

static void mlx5e_set_netdev_dev_addr(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);

	mlx5_query_nic_vport_mac_address(priv->mdev, netdev->dev_addr);
	/* TODO: w4fw: set mac address in nic vport context */
}

static void mlx5e_build_netdev(struct net_device *netdev)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;

	SET_NETDEV_DEV(netdev, &mdev->pdev->dev);

	netdev->netdev_ops        = &mlx5e_netdev_ops;
	netdev->watchdog_timeo    = 15 * HZ;

#ifdef HAVE_ETHTOOL_OPS_EXT
	SET_ETHTOOL_OPS(netdev, &mlx5e_ethtool_ops);
	set_ethtool_ops_ext(netdev, &mlx5e_ethtool_ops_ext);
#else
	netdev->ethtool_ops       = &mlx5e_ethtool_ops;
#endif

	netdev->vlan_features     = NETIF_F_SG;
	netdev->vlan_features    |= NETIF_F_IP_CSUM;
	netdev->vlan_features    |= NETIF_F_IPV6_CSUM;
	netdev->vlan_features    |= NETIF_F_GRO;
	netdev->vlan_features    |= NETIF_F_TSO;
	netdev->vlan_features    |= NETIF_F_TSO6;
	netdev->vlan_features    |= NETIF_F_RXCSUM;
#ifdef HAVE_NETIF_F_RXHASH
	netdev->vlan_features    |= NETIF_F_RXHASH;
#endif

	if (!!MLX5_CAP_ETH(mdev, lro_cap))
		netdev->vlan_features    |= NETIF_F_LRO;

#ifdef HAVE_NETDEV_HW_FEATURES
	netdev->hw_features       = netdev->vlan_features;
	netdev->hw_features      |= NETIF_F_HW_VLAN_CTAG_RX;
	netdev->hw_features      |= NETIF_F_HW_VLAN_CTAG_FILTER;

	netdev->features          = netdev->hw_features;
#else /* HAVE_NETDEV_HW_FEATURES */
	netdev->features       = netdev->vlan_features;
	netdev->features      |= NETIF_F_HW_VLAN_CTAG_RX;
	netdev->features      |= NETIF_F_HW_VLAN_CTAG_FILTER;
#ifdef HAVE_SET_NETDEV_HW_FEATURES
        set_netdev_hw_features(netdev, netdev->features);
#endif
#endif /* HAVE_NETDEV_HW_FEATURES */
 
	if (!priv->params.lro_en)
		netdev->features  &= ~NETIF_F_LRO;

	netdev->features         |= NETIF_F_HIGHDMA;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,0))
	netdev->priv_flags       |= IFF_UNICAST_FLT;
#endif

#ifdef HAVE_NET_DEVICE_OPS_EXT
	set_netdev_ops_ext(netdev, &mlx5_netdev_ops_ext);
#endif

	mlx5e_set_netdev_dev_addr(netdev);
}

static int mlx5e_create_mkey(struct mlx5e_priv *priv, u32 pdn,
			     struct mlx5_core_mr *mr)
{
	struct mlx5_core_dev *mdev = priv->mdev;
	struct mlx5_create_mkey_mbox_in *in;
	int err;

	in = mlx5_vzalloc(sizeof(*in));
	if (!in)
		return -ENOMEM;

	in->seg.flags = MLX5_PERM_LOCAL_WRITE |
			MLX5_PERM_LOCAL_READ  |
			MLX5_ACCESS_MODE_PA;
	in->seg.flags_pd = cpu_to_be32(pdn | MLX5_MKEY_LEN64);
	in->seg.qpn_mkey7_0 = cpu_to_be32(0xffffff << 8);

	err = mlx5_core_create_mkey(mdev, mr, in, sizeof(*in), NULL, NULL,
				    NULL);

	kvfree(in);

	return err;
}

static void *mlx5e_create_netdev(struct mlx5_core_dev *mdev)
{
	struct net_device *netdev;
	struct mlx5e_priv *priv;
	int ncv = mdev->priv.eq_table.num_comp_vectors;
	int err;

	if (mlx5e_check_required_hca_cap(mdev))
		return NULL;
#ifdef HAVE_NEW_TX_RING_SCHEME
	netdev = alloc_etherdev_mqs(sizeof(struct mlx5e_priv),
				    ncv * MLX5E_MAX_NUM_TC,
				    ncv);
#else
	netdev = alloc_etherdev_mq(sizeof(struct mlx5e_priv), ncv);
#endif
	if (!netdev) {
		mlx5_core_err(mdev, "alloc_etherdev_mqs() failed\n");
		return NULL;
	}

	mlx5e_build_netdev_priv(mdev, netdev, ncv);
	mlx5e_build_netdev(netdev);

	netif_carrier_off(netdev);

	priv = netdev_priv(netdev);

	err = mlx5_alloc_map_uar(mdev, &priv->cq_uar);
	if (err) {
		netdev_err(netdev, "%s: mlx5_alloc_map_uar failed, %d\n",
			   __func__, err);
		goto err_free_netdev;
	}

	err = mlx5_core_alloc_pd(mdev, &priv->pdn);
	if (err) {
		netdev_err(netdev, "%s: mlx5_core_alloc_pd failed, %d\n",
			   __func__, err);
		goto err_unmap_free_uar;
	}

	err = mlx5_alloc_transport_domain(mdev, &priv->tdn);
	if (err) {
		netdev_err(netdev, "%s: mlx5_alloc_transport_domain failed, %d\n",
			   __func__, err);
		goto err_dealloc_pd;
	}

	err = mlx5e_create_mkey(priv, priv->pdn, &priv->mr);
	if (err) {
		netdev_err(netdev, "%s: mlx5e_create_mkey failed, %d\n",
			   __func__, err);
		goto err_dealloc_transport_domain;
	}

	err = register_netdev(netdev);
	if (err) {
		netdev_err(netdev, "%s: register_netdev failed, %d\n",
			   __func__, err);
		goto err_destroy_mkey;
	}

	mlx5e_enable_async_events(priv);

	return priv;

err_destroy_mkey:
	mlx5_core_destroy_mkey(mdev, &priv->mr);

err_dealloc_transport_domain:
	mlx5_dealloc_transport_domain(mdev, priv->tdn);

err_dealloc_pd:
	mlx5_core_dealloc_pd(mdev, priv->pdn);

err_unmap_free_uar:
	mlx5_unmap_free_uar(mdev, &priv->cq_uar);

err_free_netdev:
	free_netdev(netdev);

	return NULL;
}

static void mlx5e_destroy_netdev(struct mlx5_core_dev *mdev, void *vpriv)
{
	struct mlx5e_priv *priv = vpriv;
	struct net_device *netdev = priv->netdev;

	unregister_netdev(netdev);
	mlx5_core_destroy_mkey(priv->mdev, &priv->mr);
	mlx5_dealloc_transport_domain(priv->mdev, priv->tdn);
	mlx5_core_dealloc_pd(priv->mdev, priv->pdn);
	mlx5_unmap_free_uar(priv->mdev, &priv->cq_uar);
	mlx5e_disable_async_events(priv);
	flush_scheduled_work();
	free_netdev(netdev);
}

static void *mlx5e_get_netdev(void *vpriv)
{
	struct mlx5e_priv *priv = vpriv;

	return priv->netdev;
}

static struct mlx5_interface mlx5e_interface = {
	.add       = mlx5e_create_netdev,
	.remove    = mlx5e_destroy_netdev,
	.event     = mlx5e_async_event,
	.protocol  = MLX5_INTERFACE_PROTOCOL_ETH,
	.get_dev   = mlx5e_get_netdev,
};

void mlx5e_init(void)
{
	mlx5_register_interface(&mlx5e_interface);
}

void mlx5e_cleanup(void)
{
	mlx5_unregister_interface(&mlx5e_interface);
}

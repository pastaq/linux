// SPDX-License-Identifier: GPL-2.0
/*
 * Broadcom STB ASP 2.0 Driver
 *
 * Copyright (c) 2023 Broadcom
 */
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/clk.h>

#include "bcmasp.h"
#include "bcmasp_intf_defs.h"

static void _intr2_mask_clear(struct bcmasp_priv *priv, u32 mask)
{
	intr2_core_wl(priv, mask, ASP_INTR2_MASK_CLEAR);
	priv->irq_mask &= ~mask;
}

static void _intr2_mask_set(struct bcmasp_priv *priv, u32 mask)
{
	intr2_core_wl(priv, mask, ASP_INTR2_MASK_SET);
	priv->irq_mask |= mask;
}

void bcmasp_enable_phy_irq(struct bcmasp_intf *intf, int en)
{
	struct bcmasp_priv *priv = intf->parent;

	/* Only supported with internal phys */
	if (!intf->internal_phy)
		return;

	if (en)
		_intr2_mask_clear(priv, ASP_INTR2_PHY_EVENT(intf->channel));
	else
		_intr2_mask_set(priv, ASP_INTR2_PHY_EVENT(intf->channel));
}

void bcmasp_enable_tx_irq(struct bcmasp_intf *intf, int en)
{
	struct bcmasp_priv *priv = intf->parent;

	if (en)
		_intr2_mask_clear(priv, ASP_INTR2_TX_DESC(intf->channel));
	else
		_intr2_mask_set(priv, ASP_INTR2_TX_DESC(intf->channel));
}
EXPORT_SYMBOL_GPL(bcmasp_enable_tx_irq);

void bcmasp_enable_rx_irq(struct bcmasp_intf *intf, int en)
{
	struct bcmasp_priv *priv = intf->parent;

	if (en)
		_intr2_mask_clear(priv, ASP_INTR2_RX_ECH(intf->channel));
	else
		_intr2_mask_set(priv, ASP_INTR2_RX_ECH(intf->channel));
}
EXPORT_SYMBOL_GPL(bcmasp_enable_rx_irq);

static void bcmasp_intr2_mask_set_all(struct bcmasp_priv *priv)
{
	_intr2_mask_set(priv, 0xffffffff);
	priv->irq_mask = 0xffffffff;
}

static void bcmasp_intr2_clear_all(struct bcmasp_priv *priv)
{
	intr2_core_wl(priv, 0xffffffff, ASP_INTR2_CLEAR);
}

static void bcmasp_intr2_handling(struct bcmasp_intf *intf, u32 status)
{
	if (status & ASP_INTR2_RX_ECH(intf->channel)) {
		if (likely(napi_schedule_prep(&intf->rx_napi))) {
			bcmasp_enable_rx_irq(intf, 0);
			__napi_schedule_irqoff(&intf->rx_napi);
		}
	}

	if (status & ASP_INTR2_TX_DESC(intf->channel)) {
		if (likely(napi_schedule_prep(&intf->tx_napi))) {
			bcmasp_enable_tx_irq(intf, 0);
			__napi_schedule_irqoff(&intf->tx_napi);
		}
	}

	if (status & ASP_INTR2_PHY_EVENT(intf->channel))
		phy_mac_interrupt(intf->ndev->phydev);
}

static irqreturn_t bcmasp_isr(int irq, void *data)
{
	struct bcmasp_priv *priv = data;
	struct bcmasp_intf *intf;
	u32 status;

	status = intr2_core_rl(priv, ASP_INTR2_STATUS) &
		~intr2_core_rl(priv, ASP_INTR2_MASK_STATUS);

	intr2_core_wl(priv, status, ASP_INTR2_CLEAR);

	if (unlikely(status == 0)) {
		dev_warn(&priv->pdev->dev, "l2 spurious interrupt\n");
		return IRQ_NONE;
	}

	/* Handle intferfaces */
	list_for_each_entry(intf, &priv->intfs, list)
		bcmasp_intr2_handling(intf, status);

	return IRQ_HANDLED;
}

void bcmasp_flush_rx_port(struct bcmasp_intf *intf)
{
	struct bcmasp_priv *priv = intf->parent;
	u32 mask;

	switch (intf->port) {
	case 0:
		mask = ASP_CTRL_UMAC0_FLUSH_MASK;
		break;
	case 1:
		mask = ASP_CTRL_UMAC1_FLUSH_MASK;
		break;
	case 2:
		mask = ASP_CTRL_SPB_FLUSH_MASK;
		break;
	default:
		/* Not valid port */
		return;
	}

	rx_ctrl_core_wl(priv, mask, ASP_RX_CTRL_FLUSH);
}

static void bcmasp_netfilt_hw_en_wake(struct bcmasp_priv *priv,
				      struct bcmasp_net_filter *nfilt)
{
	rx_filter_core_wl(priv, ASP_RX_FILTER_NET_OFFSET_L3_1(64),
			  ASP_RX_FILTER_NET_OFFSET(nfilt->hw_index));

	rx_filter_core_wl(priv, ASP_RX_FILTER_NET_OFFSET_L2(32) |
			  ASP_RX_FILTER_NET_OFFSET_L3_0(32) |
			  ASP_RX_FILTER_NET_OFFSET_L3_1(96) |
			  ASP_RX_FILTER_NET_OFFSET_L4(32),
			  ASP_RX_FILTER_NET_OFFSET(nfilt->hw_index + 1));

	rx_filter_core_wl(priv, ASP_RX_FILTER_NET_CFG_CH(nfilt->port + 8) |
			  ASP_RX_FILTER_NET_CFG_EN |
			  ASP_RX_FILTER_NET_CFG_L2_EN |
			  ASP_RX_FILTER_NET_CFG_L3_EN |
			  ASP_RX_FILTER_NET_CFG_L4_EN |
			  ASP_RX_FILTER_NET_CFG_L3_FRM(2) |
			  ASP_RX_FILTER_NET_CFG_L4_FRM(2) |
			  ASP_RX_FILTER_NET_CFG_UMC(nfilt->port),
			  ASP_RX_FILTER_NET_CFG(nfilt->hw_index));

	rx_filter_core_wl(priv, ASP_RX_FILTER_NET_CFG_CH(nfilt->port + 8) |
			  ASP_RX_FILTER_NET_CFG_EN |
			  ASP_RX_FILTER_NET_CFG_L2_EN |
			  ASP_RX_FILTER_NET_CFG_L3_EN |
			  ASP_RX_FILTER_NET_CFG_L4_EN |
			  ASP_RX_FILTER_NET_CFG_L3_FRM(2) |
			  ASP_RX_FILTER_NET_CFG_L4_FRM(2) |
			  ASP_RX_FILTER_NET_CFG_UMC(nfilt->port),
			  ASP_RX_FILTER_NET_CFG(nfilt->hw_index + 1));
}

#define MAX_WAKE_FILTER_SIZE		256
enum asp_netfilt_reg_type {
	ASP_NETFILT_MATCH = 0,
	ASP_NETFILT_MASK,
	ASP_NETFILT_MAX
};

static int bcmasp_netfilt_get_reg_offset(struct bcmasp_priv *priv,
					 struct bcmasp_net_filter *nfilt,
					 enum asp_netfilt_reg_type reg_type,
					 u32 offset)
{
	u32 block_index, filter_sel;

	if (offset < 32) {
		block_index = ASP_RX_FILTER_NET_L2;
		filter_sel = nfilt->hw_index;
	} else if (offset < 64) {
		block_index = ASP_RX_FILTER_NET_L2;
		filter_sel = nfilt->hw_index + 1;
	} else if (offset < 96) {
		block_index = ASP_RX_FILTER_NET_L3_0;
		filter_sel = nfilt->hw_index;
	} else if (offset < 128) {
		block_index = ASP_RX_FILTER_NET_L3_0;
		filter_sel = nfilt->hw_index + 1;
	} else if (offset < 160) {
		block_index = ASP_RX_FILTER_NET_L3_1;
		filter_sel = nfilt->hw_index;
	} else if (offset < 192) {
		block_index = ASP_RX_FILTER_NET_L3_1;
		filter_sel = nfilt->hw_index + 1;
	} else if (offset < 224) {
		block_index = ASP_RX_FILTER_NET_L4;
		filter_sel = nfilt->hw_index;
	} else if (offset < 256) {
		block_index = ASP_RX_FILTER_NET_L4;
		filter_sel = nfilt->hw_index + 1;
	} else {
		return -EINVAL;
	}

	switch (reg_type) {
	case ASP_NETFILT_MATCH:
		return ASP_RX_FILTER_NET_PAT(filter_sel, block_index,
					     (offset % 32));
	case ASP_NETFILT_MASK:
		return ASP_RX_FILTER_NET_MASK(filter_sel, block_index,
					      (offset % 32));
	default:
		return -EINVAL;
	}
}

static void bcmasp_netfilt_wr(struct bcmasp_priv *priv,
			      struct bcmasp_net_filter *nfilt,
			      enum asp_netfilt_reg_type reg_type,
			      u32 val, u32 offset)
{
	int reg_offset;

	/* HW only accepts 4 byte aligned writes */
	if (!IS_ALIGNED(offset, 4) || offset > MAX_WAKE_FILTER_SIZE)
		return;

	reg_offset = bcmasp_netfilt_get_reg_offset(priv, nfilt, reg_type,
						   offset);

	rx_filter_core_wl(priv, val, reg_offset);
}

static u32 bcmasp_netfilt_rd(struct bcmasp_priv *priv,
			     struct bcmasp_net_filter *nfilt,
			     enum asp_netfilt_reg_type reg_type,
			     u32 offset)
{
	int reg_offset;

	/* HW only accepts 4 byte aligned writes */
	if (!IS_ALIGNED(offset, 4) || offset > MAX_WAKE_FILTER_SIZE)
		return 0;

	reg_offset = bcmasp_netfilt_get_reg_offset(priv, nfilt, reg_type,
						   offset);

	return rx_filter_core_rl(priv, reg_offset);
}

static int bcmasp_netfilt_wr_m_wake(struct bcmasp_priv *priv,
				    struct bcmasp_net_filter *nfilt,
				    u32 offset, void *match, void *mask,
				    size_t size)
{
	u32 shift, mask_val = 0, match_val = 0;
	bool first_byte = true;

	if ((offset + size) > MAX_WAKE_FILTER_SIZE)
		return -EINVAL;

	while (size--) {
		/* The HW only accepts 4 byte aligned writes, so if we
		 * begin unaligned or if remaining bytes less than 4,
		 * we need to read then write to avoid losing current
		 * register state
		 */
		if (first_byte && (!IS_ALIGNED(offset, 4) || size < 3)) {
			match_val = bcmasp_netfilt_rd(priv, nfilt,
						      ASP_NETFILT_MATCH,
						      ALIGN_DOWN(offset, 4));
			mask_val = bcmasp_netfilt_rd(priv, nfilt,
						     ASP_NETFILT_MASK,
						     ALIGN_DOWN(offset, 4));
		}

		shift = (3 - (offset % 4)) * 8;
		match_val &= ~GENMASK(shift + 7, shift);
		mask_val &= ~GENMASK(shift + 7, shift);
		match_val |= (u32)(*((u8 *)match) << shift);
		mask_val |= (u32)(*((u8 *)mask) << shift);

		/* If last byte or last byte of word, write to reg */
		if (!size || ((offset % 4) == 3)) {
			bcmasp_netfilt_wr(priv, nfilt, ASP_NETFILT_MATCH,
					  match_val, ALIGN_DOWN(offset, 4));
			bcmasp_netfilt_wr(priv, nfilt, ASP_NETFILT_MASK,
					  mask_val, ALIGN_DOWN(offset, 4));
			first_byte = true;
		} else {
			first_byte = false;
		}

		offset++;
		match++;
		mask++;
	}

	return 0;
}

static void bcmasp_netfilt_reset_hw(struct bcmasp_priv *priv,
				    struct bcmasp_net_filter *nfilt)
{
	int i;

	for (i = 0; i < MAX_WAKE_FILTER_SIZE; i += 4) {
		bcmasp_netfilt_wr(priv, nfilt, ASP_NETFILT_MATCH, 0, i);
		bcmasp_netfilt_wr(priv, nfilt, ASP_NETFILT_MASK, 0, i);
	}
}

static void bcmasp_netfilt_tcpip4_wr(struct bcmasp_priv *priv,
				     struct bcmasp_net_filter *nfilt,
				     struct ethtool_tcpip4_spec *match,
				     struct ethtool_tcpip4_spec *mask,
				     u32 offset)
{
	__be16 val_16, mask_16;

	val_16 = htons(ETH_P_IP);
	mask_16 = htons(0xFFFF);
	bcmasp_netfilt_wr_m_wake(priv, nfilt, (ETH_ALEN * 2) + offset,
				 &val_16, &mask_16, sizeof(val_16));
	bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 1,
				 &match->tos, &mask->tos,
				 sizeof(match->tos));
	bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 12,
				 &match->ip4src, &mask->ip4src,
				 sizeof(match->ip4src));
	bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 16,
				 &match->ip4dst, &mask->ip4dst,
				 sizeof(match->ip4dst));
	bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 20,
				 &match->psrc, &mask->psrc,
				 sizeof(match->psrc));
	bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 22,
				 &match->pdst, &mask->pdst,
				 sizeof(match->pdst));
}

static void bcmasp_netfilt_tcpip6_wr(struct bcmasp_priv *priv,
				     struct bcmasp_net_filter *nfilt,
				     struct ethtool_tcpip6_spec *match,
				     struct ethtool_tcpip6_spec *mask,
				     u32 offset)
{
	__be16 val_16, mask_16;

	val_16 = htons(ETH_P_IPV6);
	mask_16 = htons(0xFFFF);
	bcmasp_netfilt_wr_m_wake(priv, nfilt, (ETH_ALEN * 2) + offset,
				 &val_16, &mask_16, sizeof(val_16));
	val_16 = htons(match->tclass << 4);
	mask_16 = htons(mask->tclass << 4);
	bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset,
				 &val_16, &mask_16, sizeof(val_16));
	bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 8,
				 &match->ip6src, &mask->ip6src,
				 sizeof(match->ip6src));
	bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 24,
				 &match->ip6dst, &mask->ip6dst,
				 sizeof(match->ip6dst));
	bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 40,
				 &match->psrc, &mask->psrc,
				 sizeof(match->psrc));
	bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 42,
				 &match->pdst, &mask->pdst,
				 sizeof(match->pdst));
}

static int bcmasp_netfilt_wr_to_hw(struct bcmasp_priv *priv,
				   struct bcmasp_net_filter *nfilt)
{
	struct ethtool_rx_flow_spec *fs = &nfilt->fs;
	unsigned int offset = 0;
	__be16 val_16, mask_16;
	u8 val_8, mask_8;

	/* Currently only supports wake filters */
	if (!nfilt->wake_filter)
		return -EINVAL;

	bcmasp_netfilt_reset_hw(priv, nfilt);

	if (fs->flow_type & FLOW_MAC_EXT) {
		bcmasp_netfilt_wr_m_wake(priv, nfilt, 0, &fs->h_ext.h_dest,
					 &fs->m_ext.h_dest,
					 sizeof(fs->h_ext.h_dest));
	}

	if ((fs->flow_type & FLOW_EXT) &&
	    (fs->m_ext.vlan_etype || fs->m_ext.vlan_tci)) {
		bcmasp_netfilt_wr_m_wake(priv, nfilt, (ETH_ALEN * 2),
					 &fs->h_ext.vlan_etype,
					 &fs->m_ext.vlan_etype,
					 sizeof(fs->h_ext.vlan_etype));
		bcmasp_netfilt_wr_m_wake(priv, nfilt, ((ETH_ALEN * 2) + 2),
					 &fs->h_ext.vlan_tci,
					 &fs->m_ext.vlan_tci,
					 sizeof(fs->h_ext.vlan_tci));
		offset += VLAN_HLEN;
	}

	switch (fs->flow_type & ~(FLOW_EXT | FLOW_MAC_EXT)) {
	case ETHER_FLOW:
		bcmasp_netfilt_wr_m_wake(priv, nfilt, 0,
					 &fs->h_u.ether_spec.h_dest,
					 &fs->m_u.ether_spec.h_dest,
					 sizeof(fs->h_u.ether_spec.h_dest));
		bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_ALEN,
					 &fs->h_u.ether_spec.h_source,
					 &fs->m_u.ether_spec.h_source,
					 sizeof(fs->h_u.ether_spec.h_source));
		bcmasp_netfilt_wr_m_wake(priv, nfilt, (ETH_ALEN * 2) + offset,
					 &fs->h_u.ether_spec.h_proto,
					 &fs->m_u.ether_spec.h_proto,
					 sizeof(fs->h_u.ether_spec.h_proto));

		break;
	case IP_USER_FLOW:
		val_16 = htons(ETH_P_IP);
		mask_16 = htons(0xFFFF);
		bcmasp_netfilt_wr_m_wake(priv, nfilt, (ETH_ALEN * 2) + offset,
					 &val_16, &mask_16, sizeof(val_16));
		bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 1,
					 &fs->h_u.usr_ip4_spec.tos,
					 &fs->m_u.usr_ip4_spec.tos,
					 sizeof(fs->h_u.usr_ip4_spec.tos));
		bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 9,
					 &fs->h_u.usr_ip4_spec.proto,
					 &fs->m_u.usr_ip4_spec.proto,
					 sizeof(fs->h_u.usr_ip4_spec.proto));
		bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 12,
					 &fs->h_u.usr_ip4_spec.ip4src,
					 &fs->m_u.usr_ip4_spec.ip4src,
					 sizeof(fs->h_u.usr_ip4_spec.ip4src));
		bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 16,
					 &fs->h_u.usr_ip4_spec.ip4dst,
					 &fs->m_u.usr_ip4_spec.ip4dst,
					 sizeof(fs->h_u.usr_ip4_spec.ip4dst));
		if (!fs->m_u.usr_ip4_spec.l4_4_bytes)
			break;

		/* Only supports 20 byte IPv4 header */
		val_8 = 0x45;
		mask_8 = 0xFF;
		bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset,
					 &val_8, &mask_8, sizeof(val_8));
		bcmasp_netfilt_wr_m_wake(priv, nfilt,
					 ETH_HLEN + 20 + offset,
					 &fs->h_u.usr_ip4_spec.l4_4_bytes,
					 &fs->m_u.usr_ip4_spec.l4_4_bytes,
					 sizeof(fs->h_u.usr_ip4_spec.l4_4_bytes)
					 );
		break;
	case TCP_V4_FLOW:
		val_8 = IPPROTO_TCP;
		mask_8 = 0xFF;
		bcmasp_netfilt_tcpip4_wr(priv, nfilt, &fs->h_u.tcp_ip4_spec,
					 &fs->m_u.tcp_ip4_spec, offset);
		bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 9,
					 &val_8, &mask_8, sizeof(val_8));
		break;
	case UDP_V4_FLOW:
		val_8 = IPPROTO_UDP;
		mask_8 = 0xFF;
		bcmasp_netfilt_tcpip4_wr(priv, nfilt, &fs->h_u.udp_ip4_spec,
					 &fs->m_u.udp_ip4_spec, offset);

		bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 9,
					 &val_8, &mask_8, sizeof(val_8));
		break;
	case TCP_V6_FLOW:
		val_8 = IPPROTO_TCP;
		mask_8 = 0xFF;
		bcmasp_netfilt_tcpip6_wr(priv, nfilt, &fs->h_u.tcp_ip6_spec,
					 &fs->m_u.tcp_ip6_spec, offset);
		bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 6,
					 &val_8, &mask_8, sizeof(val_8));
		break;
	case UDP_V6_FLOW:
		val_8 = IPPROTO_UDP;
		mask_8 = 0xFF;
		bcmasp_netfilt_tcpip6_wr(priv, nfilt, &fs->h_u.udp_ip6_spec,
					 &fs->m_u.udp_ip6_spec, offset);
		bcmasp_netfilt_wr_m_wake(priv, nfilt, ETH_HLEN + offset + 6,
					 &val_8, &mask_8, sizeof(val_8));
		break;
	}

	bcmasp_netfilt_hw_en_wake(priv, nfilt);

	return 0;
}

void bcmasp_netfilt_suspend(struct bcmasp_intf *intf)
{
	struct bcmasp_priv *priv = intf->parent;
	bool write = false;
	int ret, i;

	/* Write all filters to HW */
	for (i = 0; i < priv->num_net_filters; i++) {
		/* If the filter does not match the port, skip programming. */
		if (!priv->net_filters[i].claimed ||
		    priv->net_filters[i].port != intf->port)
			continue;

		if (i > 0 && (i % 2) &&
		    priv->net_filters[i].wake_filter &&
		    priv->net_filters[i - 1].wake_filter)
			continue;

		ret = bcmasp_netfilt_wr_to_hw(priv, &priv->net_filters[i]);
		if (!ret)
			write = true;
	}

	/* Successfully programmed at least one wake filter
	 * so enable top level wake config
	 */
	if (write)
		rx_filter_core_wl(priv, (ASP_RX_FILTER_OPUT_EN |
				  ASP_RX_FILTER_LNR_MD |
				  ASP_RX_FILTER_GEN_WK_EN |
				  ASP_RX_FILTER_NT_FLT_EN),
				  ASP_RX_FILTER_BLK_CTRL);
}

int bcmasp_netfilt_get_all_active(struct bcmasp_intf *intf, u32 *rule_locs,
				  u32 *rule_cnt)
{
	struct bcmasp_priv *priv = intf->parent;
	int j = 0, i;

	for (i = 0; i < priv->num_net_filters; i++) {
		if (!priv->net_filters[i].claimed ||
		    priv->net_filters[i].port != intf->port)
			continue;

		if (i > 0 && (i % 2) &&
		    priv->net_filters[i].wake_filter &&
		    priv->net_filters[i - 1].wake_filter)
			continue;

		if (j == *rule_cnt)
			return -EMSGSIZE;

		rule_locs[j++] = priv->net_filters[i].fs.location;
	}

	*rule_cnt = j;

	return 0;
}

int bcmasp_netfilt_get_active(struct bcmasp_intf *intf)
{
	struct bcmasp_priv *priv = intf->parent;
	int cnt = 0, i;

	for (i = 0; i < priv->num_net_filters; i++) {
		if (!priv->net_filters[i].claimed ||
		    priv->net_filters[i].port != intf->port)
			continue;

		/* Skip over a wake filter pair */
		if (i > 0 && (i % 2) &&
		    priv->net_filters[i].wake_filter &&
		    priv->net_filters[i - 1].wake_filter)
			continue;

		cnt++;
	}

	return cnt;
}

bool bcmasp_netfilt_check_dup(struct bcmasp_intf *intf,
			      struct ethtool_rx_flow_spec *fs)
{
	struct bcmasp_priv *priv = intf->parent;
	struct ethtool_rx_flow_spec *cur;
	size_t fs_size = 0;
	int i;

	for (i = 0; i < priv->num_net_filters; i++) {
		if (!priv->net_filters[i].claimed ||
		    priv->net_filters[i].port != intf->port)
			continue;

		cur = &priv->net_filters[i].fs;

		if (cur->flow_type != fs->flow_type ||
		    cur->ring_cookie != fs->ring_cookie)
			continue;

		switch (fs->flow_type & ~(FLOW_EXT | FLOW_MAC_EXT)) {
		case ETHER_FLOW:
			fs_size = sizeof(struct ethhdr);
			break;
		case IP_USER_FLOW:
			fs_size = sizeof(struct ethtool_usrip4_spec);
			break;
		case TCP_V6_FLOW:
		case UDP_V6_FLOW:
			fs_size = sizeof(struct ethtool_tcpip6_spec);
			break;
		case TCP_V4_FLOW:
		case UDP_V4_FLOW:
			fs_size = sizeof(struct ethtool_tcpip4_spec);
			break;
		default:
			continue;
		}

		if (memcmp(&cur->h_u, &fs->h_u, fs_size) ||
		    memcmp(&cur->m_u, &fs->m_u, fs_size))
			continue;

		if (cur->flow_type & FLOW_EXT) {
			if (cur->h_ext.vlan_etype != fs->h_ext.vlan_etype ||
			    cur->m_ext.vlan_etype != fs->m_ext.vlan_etype ||
			    cur->h_ext.vlan_tci != fs->h_ext.vlan_tci ||
			    cur->m_ext.vlan_tci != fs->m_ext.vlan_tci ||
			    cur->h_ext.data[0] != fs->h_ext.data[0])
				continue;
		}
		if (cur->flow_type & FLOW_MAC_EXT) {
			if (memcmp(&cur->h_ext.h_dest,
				   &fs->h_ext.h_dest, ETH_ALEN) ||
			    memcmp(&cur->m_ext.h_dest,
				   &fs->m_ext.h_dest, ETH_ALEN))
				continue;
		}

		return true;
	}

	return false;
}

/* If no network filter found, return open filter.
 * If no more open filters return NULL
 */
struct bcmasp_net_filter *bcmasp_netfilt_get_init(struct bcmasp_intf *intf,
						  u32 loc, bool wake_filter,
						  bool init)
{
	struct bcmasp_net_filter *nfilter = NULL;
	struct bcmasp_priv *priv = intf->parent;
	int i, open_index = -1;

	/* Check whether we exceed the filter table capacity */
	if (loc != RX_CLS_LOC_ANY && loc >= priv->num_net_filters)
		return ERR_PTR(-EINVAL);

	/* If the filter location is busy (already claimed) and we are initializing
	 * the filter (insertion), return a busy error code.
	 */
	if (loc != RX_CLS_LOC_ANY && init && priv->net_filters[loc].claimed)
		return ERR_PTR(-EBUSY);

	/* We need two filters for wake-up, so we cannot use an odd filter */
	if (wake_filter && loc != RX_CLS_LOC_ANY && (loc % 2))
		return ERR_PTR(-EINVAL);

	/* Initialize the loop index based on the desired location or from 0 */
	i = loc == RX_CLS_LOC_ANY ? 0 : loc;

	for ( ; i < priv->num_net_filters; i++) {
		/* Found matching network filter */
		if (!init &&
		    priv->net_filters[i].claimed &&
		    priv->net_filters[i].hw_index == i &&
		    priv->net_filters[i].port == intf->port)
			return &priv->net_filters[i];

		/* If we don't need a new filter or new filter already found */
		if (!init || open_index >= 0)
			continue;

		/* Wake filter conslidates two filters to cover more bytes
		 * Wake filter is open if...
		 * 1. It is an even filter
		 * 2. The current and next filter is not claimed
		 */
		if (wake_filter && !(i % 2) && !priv->net_filters[i].claimed &&
		    !priv->net_filters[i + 1].claimed)
			open_index = i;
		else if (!priv->net_filters[i].claimed)
			open_index = i;
	}

	if (open_index >= 0) {
		nfilter = &priv->net_filters[open_index];
		nfilter->claimed = true;
		nfilter->port = intf->port;
		nfilter->hw_index = open_index;
	}

	if (wake_filter && open_index >= 0) {
		/* Claim next filter */
		priv->net_filters[open_index + 1].claimed = true;
		priv->net_filters[open_index + 1].wake_filter = true;
		nfilter->wake_filter = true;
	}

	return nfilter ? nfilter : ERR_PTR(-EINVAL);
}

void bcmasp_netfilt_release(struct bcmasp_intf *intf,
			    struct bcmasp_net_filter *nfilt)
{
	struct bcmasp_priv *priv = intf->parent;

	if (nfilt->wake_filter) {
		memset(&priv->net_filters[nfilt->hw_index + 1], 0,
		       sizeof(struct bcmasp_net_filter));
	}

	memset(nfilt, 0, sizeof(struct bcmasp_net_filter));
}

static void bcmasp_addr_to_uint(unsigned char *addr, u32 *high, u32 *low)
{
	*high = (u32)(addr[0] << 8 | addr[1]);
	*low = (u32)(addr[2] << 24 | addr[3] << 16 | addr[4] << 8 |
		     addr[5]);
}

static void bcmasp_set_mda_filter(struct bcmasp_intf *intf,
				  const unsigned char *addr,
				  unsigned char *mask,
				  unsigned int i)
{
	struct bcmasp_priv *priv = intf->parent;
	u32 addr_h, addr_l, mask_h, mask_l;

	/* Set local copy */
	ether_addr_copy(priv->mda_filters[i].mask, mask);
	ether_addr_copy(priv->mda_filters[i].addr, addr);

	/* Write to HW */
	bcmasp_addr_to_uint(priv->mda_filters[i].mask, &mask_h, &mask_l);
	bcmasp_addr_to_uint(priv->mda_filters[i].addr, &addr_h, &addr_l);
	rx_filter_core_wl(priv, addr_h, ASP_RX_FILTER_MDA_PAT_H(i));
	rx_filter_core_wl(priv, addr_l, ASP_RX_FILTER_MDA_PAT_L(i));
	rx_filter_core_wl(priv, mask_h, ASP_RX_FILTER_MDA_MSK_H(i));
	rx_filter_core_wl(priv, mask_l, ASP_RX_FILTER_MDA_MSK_L(i));
}

static void bcmasp_en_mda_filter(struct bcmasp_intf *intf, bool en,
				 unsigned int i)
{
	struct bcmasp_priv *priv = intf->parent;

	if (priv->mda_filters[i].en == en)
		return;

	priv->mda_filters[i].en = en;
	priv->mda_filters[i].port = intf->port;

	rx_filter_core_wl(priv, ((intf->channel + priv->tx_chan_offset) |
			  (en << ASP_RX_FILTER_MDA_CFG_EN_SHIFT) |
			  ASP_RX_FILTER_MDA_CFG_UMC_SEL(intf->port)),
			  ASP_RX_FILTER_MDA_CFG(i));
}

/* There are 32 MDA filters shared between all ports, we reserve 4 filters per
 * port for the following.
 * - Promisc: Filter to allow all packets when promisc is enabled
 * - All Multicast
 * - Broadcast
 * - Own address
 *
 * The reserved filters are identified as so.
 * - Promisc: (index * 4) + 0
 * - All Multicast: (index * 4) + 1
 * - Broadcast: (index * 4) + 2
 * - Own address: (index * 4) + 3
 */
enum asp_rx_filter_id {
	ASP_RX_FILTER_MDA_PROMISC = 0,
	ASP_RX_FILTER_MDA_ALLMULTI,
	ASP_RX_FILTER_MDA_BROADCAST,
	ASP_RX_FILTER_MDA_OWN_ADDR,
	ASP_RX_FILTER_MDA_RES_MAX,
};

#define ASP_RX_FILT_MDA(intf, name)	(((intf)->index * \
					  ASP_RX_FILTER_MDA_RES_MAX) \
					 + ASP_RX_FILTER_MDA_##name)

static int bcmasp_total_res_mda_cnt(struct bcmasp_priv *priv)
{
	return list_count_nodes(&priv->intfs) * ASP_RX_FILTER_MDA_RES_MAX;
}

void bcmasp_set_promisc(struct bcmasp_intf *intf, bool en)
{
	unsigned int i = ASP_RX_FILT_MDA(intf, PROMISC);
	unsigned char promisc[ETH_ALEN];

	eth_zero_addr(promisc);
	/* Set mask to 00:00:00:00:00:00 to match all packets */
	bcmasp_set_mda_filter(intf, promisc, promisc, i);
	bcmasp_en_mda_filter(intf, en, i);
}

void bcmasp_set_allmulti(struct bcmasp_intf *intf, bool en)
{
	unsigned char allmulti[] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned int i = ASP_RX_FILT_MDA(intf, ALLMULTI);

	/* Set mask to 01:00:00:00:00:00 to match all multicast */
	bcmasp_set_mda_filter(intf, allmulti, allmulti, i);
	bcmasp_en_mda_filter(intf, en, i);
}

void bcmasp_set_broad(struct bcmasp_intf *intf, bool en)
{
	unsigned int i = ASP_RX_FILT_MDA(intf, BROADCAST);
	unsigned char addr[ETH_ALEN];

	eth_broadcast_addr(addr);
	bcmasp_set_mda_filter(intf, addr, addr, i);
	bcmasp_en_mda_filter(intf, en, i);
}

void bcmasp_set_oaddr(struct bcmasp_intf *intf, const unsigned char *addr,
		      bool en)
{
	unsigned char mask[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	unsigned int i = ASP_RX_FILT_MDA(intf, OWN_ADDR);

	bcmasp_set_mda_filter(intf, addr, mask, i);
	bcmasp_en_mda_filter(intf, en, i);
}

void bcmasp_disable_all_filters(struct bcmasp_intf *intf)
{
	struct bcmasp_priv *priv = intf->parent;
	unsigned int i;
	int res_count;

	res_count = bcmasp_total_res_mda_cnt(intf->parent);

	/* Disable all filters held by this port */
	for (i = res_count; i < priv->num_mda_filters; i++) {
		if (priv->mda_filters[i].en &&
		    priv->mda_filters[i].port == intf->port)
			bcmasp_en_mda_filter(intf, 0, i);
	}
}

static int bcmasp_combine_set_filter(struct bcmasp_intf *intf,
				     unsigned char *addr, unsigned char *mask,
				     int i)
{
	struct bcmasp_priv *priv = intf->parent;
	u64 addr1, addr2, mask1, mask2, mask3;

	/* Switch to u64 to help with the calculations */
	addr1 = ether_addr_to_u64(priv->mda_filters[i].addr);
	mask1 = ether_addr_to_u64(priv->mda_filters[i].mask);
	addr2 = ether_addr_to_u64(addr);
	mask2 = ether_addr_to_u64(mask);

	/* Check if one filter resides within the other */
	mask3 = mask1 & mask2;
	if (mask3 == mask1 && ((addr1 & mask1) == (addr2 & mask1))) {
		/* Filter 2 resides within filter 1, so everything is good */
		return 0;
	} else if (mask3 == mask2 && ((addr1 & mask2) == (addr2 & mask2))) {
		/* Filter 1 resides within filter 2, so swap filters */
		bcmasp_set_mda_filter(intf, addr, mask, i);
		return 0;
	}

	/* Unable to combine */
	return -EINVAL;
}

int bcmasp_set_en_mda_filter(struct bcmasp_intf *intf, unsigned char *addr,
			     unsigned char *mask)
{
	struct bcmasp_priv *priv = intf->parent;
	int ret, res_count;
	unsigned int i;

	res_count = bcmasp_total_res_mda_cnt(intf->parent);

	for (i = res_count; i < priv->num_mda_filters; i++) {
		/* If filter not enabled or belongs to another port skip */
		if (!priv->mda_filters[i].en ||
		    priv->mda_filters[i].port != intf->port)
			continue;

		/* Attempt to combine filters */
		ret = bcmasp_combine_set_filter(intf, addr, mask, i);
		if (!ret) {
			intf->mib.filters_combine_cnt++;
			return 0;
		}
	}

	/* Create new filter if possible */
	for (i = res_count; i < priv->num_mda_filters; i++) {
		if (priv->mda_filters[i].en)
			continue;

		bcmasp_set_mda_filter(intf, addr, mask, i);
		bcmasp_en_mda_filter(intf, 1, i);
		return 0;
	}

	/* No room for new filter */
	return -EINVAL;
}

static void bcmasp_core_init_filters(struct bcmasp_priv *priv)
{
	unsigned int i;

	/* Disable all filters and reset software view since the HW
	 * can lose context while in deep sleep suspend states
	 */
	for (i = 0; i < priv->num_mda_filters; i++) {
		rx_filter_core_wl(priv, 0x0, ASP_RX_FILTER_MDA_CFG(i));
		priv->mda_filters[i].en = 0;
	}

	for (i = 0; i < priv->num_net_filters; i++)
		rx_filter_core_wl(priv, 0x0, ASP_RX_FILTER_NET_CFG(i));

	/* Top level filter enable bit should be enabled at all times, set
	 * GEN_WAKE_CLEAR to clear the network filter wake-up which would
	 * otherwise be sticky
	 */
	rx_filter_core_wl(priv, (ASP_RX_FILTER_OPUT_EN |
			  ASP_RX_FILTER_MDA_EN |
			  ASP_RX_FILTER_GEN_WK_CLR |
			  ASP_RX_FILTER_NT_FLT_EN),
			  ASP_RX_FILTER_BLK_CTRL);
}

/* ASP core initialization */
static void bcmasp_core_init(struct bcmasp_priv *priv)
{
	rx_edpkt_core_wl(priv, 0x1b, ASP_EDPKT_BURST_BUF_PSCAL_TOUT);
	rx_edpkt_core_wl(priv, 0x3e8, ASP_EDPKT_BURST_BUF_WRITE_TOUT);

	rx_edpkt_core_wl(priv, ASP_EDPKT_ENABLE_EN, ASP_EDPKT_ENABLE);

	/* Disable and clear both UniMAC's wake-up interrupts to avoid
	 * sticky interrupts.
	 */
	_intr2_mask_set(priv, ASP_INTR2_UMC0_WAKE | ASP_INTR2_UMC1_WAKE);
	intr2_core_wl(priv, ASP_INTR2_UMC0_WAKE | ASP_INTR2_UMC1_WAKE,
		      ASP_INTR2_CLEAR);
}

static void bcmasp_core_clock_select_many(struct bcmasp_priv *priv, bool slow)
{
	u32 reg;

	reg = ctrl2_core_rl(priv, ASP_CTRL2_CORE_CLOCK_SELECT);
	if (slow)
		reg &= ~ASP_CTRL2_CORE_CLOCK_SELECT_MAIN;
	else
		reg |= ASP_CTRL2_CORE_CLOCK_SELECT_MAIN;
	ctrl2_core_wl(priv, reg, ASP_CTRL2_CORE_CLOCK_SELECT);

	reg = ctrl2_core_rl(priv, ASP_CTRL2_CPU_CLOCK_SELECT);
	if (slow)
		reg &= ~ASP_CTRL2_CPU_CLOCK_SELECT_MAIN;
	else
		reg |= ASP_CTRL2_CPU_CLOCK_SELECT_MAIN;
	ctrl2_core_wl(priv, reg, ASP_CTRL2_CPU_CLOCK_SELECT);
}

static void bcmasp_core_clock_select_one(struct bcmasp_priv *priv, bool slow)
{
	u32 reg;

	reg = ctrl_core_rl(priv, ASP_CTRL_CORE_CLOCK_SELECT);
	if (slow)
		reg &= ~ASP_CTRL_CORE_CLOCK_SELECT_MAIN;
	else
		reg |= ASP_CTRL_CORE_CLOCK_SELECT_MAIN;
	ctrl_core_wl(priv, reg, ASP_CTRL_CORE_CLOCK_SELECT);
}

static void bcmasp_core_clock_select_one_ctrl2(struct bcmasp_priv *priv, bool slow)
{
	u32 reg;

	reg = ctrl2_core_rl(priv, ASP_CTRL2_CORE_CLOCK_SELECT);
	if (slow)
		reg &= ~ASP_CTRL2_CORE_CLOCK_SELECT_MAIN;
	else
		reg |= ASP_CTRL2_CORE_CLOCK_SELECT_MAIN;
	ctrl2_core_wl(priv, reg, ASP_CTRL2_CORE_CLOCK_SELECT);
}

static void bcmasp_core_clock_set_ll(struct bcmasp_priv *priv, u32 clr, u32 set)
{
	u32 reg;

	reg = ctrl_core_rl(priv, ASP_CTRL_CLOCK_CTRL);
	reg &= ~clr;
	reg |= set;
	ctrl_core_wl(priv, reg, ASP_CTRL_CLOCK_CTRL);

	reg = ctrl_core_rl(priv, ASP_CTRL_SCRATCH_0);
	reg &= ~clr;
	reg |= set;
	ctrl_core_wl(priv, reg, ASP_CTRL_SCRATCH_0);
}

static void bcmasp_core_clock_set(struct bcmasp_priv *priv, u32 clr, u32 set)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->clk_lock, flags);
	bcmasp_core_clock_set_ll(priv, clr, set);
	spin_unlock_irqrestore(&priv->clk_lock, flags);
}

void bcmasp_core_clock_set_intf(struct bcmasp_intf *intf, bool en)
{
	u32 intf_mask = ASP_CTRL_CLOCK_CTRL_ASP_RGMII_DIS(intf->port);
	struct bcmasp_priv *priv = intf->parent;
	unsigned long flags;
	u32 reg;

	/* When enabling an interface, if the RX or TX clocks were not enabled,
	 * enable them. Conversely, while disabling an interface, if this is
	 * the last one enabled, we can turn off the shared RX and TX clocks as
	 * well. We control enable bits which is why we test for equality on
	 * the RGMII clock bit mask.
	 */
	spin_lock_irqsave(&priv->clk_lock, flags);
	if (en) {
		intf_mask |= ASP_CTRL_CLOCK_CTRL_ASP_TX_DISABLE |
			     ASP_CTRL_CLOCK_CTRL_ASP_RX_DISABLE;
		bcmasp_core_clock_set_ll(priv, intf_mask, 0);
	} else {
		reg = ctrl_core_rl(priv, ASP_CTRL_SCRATCH_0) | intf_mask;
		if ((reg & ASP_CTRL_CLOCK_CTRL_ASP_RGMII_MASK) ==
		    ASP_CTRL_CLOCK_CTRL_ASP_RGMII_MASK)
			intf_mask |= ASP_CTRL_CLOCK_CTRL_ASP_TX_DISABLE |
				     ASP_CTRL_CLOCK_CTRL_ASP_RX_DISABLE;
		bcmasp_core_clock_set_ll(priv, 0, intf_mask);
	}
	spin_unlock_irqrestore(&priv->clk_lock, flags);
}

static irqreturn_t bcmasp_isr_wol(int irq, void *data)
{
	struct bcmasp_priv *priv = data;
	u32 status;

	/* No L3 IRQ, so we good */
	if (priv->wol_irq <= 0)
		goto irq_handled;

	status = wakeup_intr2_core_rl(priv, ASP_WAKEUP_INTR2_STATUS) &
		~wakeup_intr2_core_rl(priv, ASP_WAKEUP_INTR2_MASK_STATUS);
	wakeup_intr2_core_wl(priv, status, ASP_WAKEUP_INTR2_CLEAR);

irq_handled:
	pm_wakeup_event(&priv->pdev->dev, 0);
	return IRQ_HANDLED;
}

static int bcmasp_get_and_request_irq(struct bcmasp_priv *priv, int i)
{
	struct platform_device *pdev = priv->pdev;
	int irq, ret;

	irq = platform_get_irq_optional(pdev, i);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(&pdev->dev, irq, bcmasp_isr_wol, 0,
			       pdev->name, priv);
	if (ret)
		return ret;

	return irq;
}

static void bcmasp_init_wol(struct bcmasp_priv *priv)
{
	struct platform_device *pdev = priv->pdev;
	struct device *dev = &pdev->dev;
	int irq;

	irq = bcmasp_get_and_request_irq(priv, 1);
	if (irq < 0) {
		dev_warn(dev, "Failed to init WoL irq: %d\n", irq);
		return;
	}

	priv->wol_irq = irq;
	priv->wol_irq_enabled_mask = 0;
	device_set_wakeup_capable(&pdev->dev, 1);
}

void bcmasp_enable_wol(struct bcmasp_intf *intf, bool en)
{
	struct bcmasp_priv *priv = intf->parent;
	struct device *dev = &priv->pdev->dev;

	if (en) {
		if (priv->wol_irq_enabled_mask) {
			set_bit(intf->port, &priv->wol_irq_enabled_mask);
			return;
		}

		/* First enable */
		set_bit(intf->port, &priv->wol_irq_enabled_mask);
		enable_irq_wake(priv->wol_irq);
		device_set_wakeup_enable(dev, 1);
	} else {
		if (!priv->wol_irq_enabled_mask)
			return;

		clear_bit(intf->port, &priv->wol_irq_enabled_mask);
		if (priv->wol_irq_enabled_mask)
			return;

		/* Last disable */
		disable_irq_wake(priv->wol_irq);
		device_set_wakeup_enable(dev, 0);
	}
}

static void bcmasp_wol_irq_destroy(struct bcmasp_priv *priv)
{
	if (priv->wol_irq > 0)
		free_irq(priv->wol_irq, priv);
}

static void bcmasp_eee_fixup(struct bcmasp_intf *intf, bool en)
{
	u32 reg, phy_lpi_overwrite;

	reg = rx_edpkt_core_rl(intf->parent, ASP_EDPKT_SPARE_REG);
	phy_lpi_overwrite = intf->internal_phy ? ASP_EDPKT_SPARE_REG_EPHY_LPI :
			    ASP_EDPKT_SPARE_REG_GPHY_LPI;

	if (en)
		reg |= phy_lpi_overwrite;
	else
		reg &= ~phy_lpi_overwrite;

	rx_edpkt_core_wl(intf->parent, reg, ASP_EDPKT_SPARE_REG);

	usleep_range(50, 100);
}

static const struct bcmasp_plat_data v21_plat_data = {
	.core_clock_select = bcmasp_core_clock_select_one,
	.num_mda_filters = 32,
	.num_net_filters = 32,
	.tx_chan_offset = 8,
	.rx_ctrl_offset = 0x0,
};

static const struct bcmasp_plat_data v22_plat_data = {
	.core_clock_select = bcmasp_core_clock_select_many,
	.eee_fixup = bcmasp_eee_fixup,
	.num_mda_filters = 32,
	.num_net_filters = 32,
	.tx_chan_offset = 8,
	.rx_ctrl_offset = 0x0,
};

static const struct bcmasp_plat_data v30_plat_data = {
	.core_clock_select = bcmasp_core_clock_select_one_ctrl2,
	.num_mda_filters = 20,
	.num_net_filters = 16,
	.tx_chan_offset = 0,
	.rx_ctrl_offset = 0x10000,
};

static void bcmasp_set_pdata(struct bcmasp_priv *priv, const struct bcmasp_plat_data *pdata)
{
	priv->core_clock_select = pdata->core_clock_select;
	priv->eee_fixup = pdata->eee_fixup;
	priv->num_mda_filters = pdata->num_mda_filters;
	priv->num_net_filters = pdata->num_net_filters;
	priv->tx_chan_offset = pdata->tx_chan_offset;
	priv->rx_ctrl_offset = pdata->rx_ctrl_offset;
}

static const struct of_device_id bcmasp_of_match[] = {
	{ .compatible = "brcm,asp-v2.1", .data = &v21_plat_data },
	{ .compatible = "brcm,asp-v2.2", .data = &v22_plat_data },
	{ .compatible = "brcm,asp-v3.0", .data = &v30_plat_data },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, bcmasp_of_match);

static const struct of_device_id bcmasp_mdio_of_match[] = {
	{ .compatible = "brcm,asp-v2.1-mdio", },
	{ .compatible = "brcm,asp-v2.2-mdio", },
	{ .compatible = "brcm,asp-v3.0-mdio", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, bcmasp_mdio_of_match);

static void bcmasp_remove_intfs(struct bcmasp_priv *priv)
{
	struct bcmasp_intf *intf, *n;

	list_for_each_entry_safe(intf, n, &priv->intfs, list) {
		list_del(&intf->list);
		bcmasp_interface_destroy(intf);
	}
}

static int bcmasp_probe(struct platform_device *pdev)
{
	const struct bcmasp_plat_data *pdata;
	struct device *dev = &pdev->dev;
	struct device_node *ports_node;
	struct bcmasp_priv *priv;
	struct bcmasp_intf *intf;
	int ret = 0, count = 0;
	unsigned int i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq <= 0)
		return -EINVAL;

	priv->clk = devm_clk_get_optional_enabled(dev, "sw_asp");
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "failed to request clock\n");

	/* Base from parent node */
	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return dev_err_probe(dev, PTR_ERR(priv->base), "failed to iomap\n");

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret)
		return dev_err_probe(dev, ret, "unable to set DMA mask: %d\n", ret);

	dev_set_drvdata(&pdev->dev, priv);
	priv->pdev = pdev;
	spin_lock_init(&priv->mda_lock);
	spin_lock_init(&priv->clk_lock);
	mutex_init(&priv->wol_lock);
	mutex_init(&priv->net_lock);
	INIT_LIST_HEAD(&priv->intfs);

	pdata = device_get_match_data(&pdev->dev);
	if (!pdata)
		return dev_err_probe(dev, -EINVAL, "unable to find platform data\n");

	bcmasp_set_pdata(priv, pdata);

	/* Enable all clocks to ensure successful probing */
	bcmasp_core_clock_set(priv, ASP_CTRL_CLOCK_CTRL_ASP_ALL_DISABLE, 0);

	/* Switch to the main clock */
	priv->core_clock_select(priv, false);

	bcmasp_intr2_mask_set_all(priv);
	bcmasp_intr2_clear_all(priv);

	ret = devm_request_irq(&pdev->dev, priv->irq, bcmasp_isr, 0,
			       pdev->name, priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request ASP interrupt: %d", ret);

	/* Register mdio child nodes */
	of_platform_populate(dev->of_node, bcmasp_mdio_of_match, NULL, dev);

	/* ASP specific initialization, Needs to be done regardless of
	 * how many interfaces come up.
	 */
	bcmasp_core_init(priv);

	priv->mda_filters = devm_kcalloc(dev, priv->num_mda_filters,
					 sizeof(*priv->mda_filters), GFP_KERNEL);
	if (!priv->mda_filters)
		return -ENOMEM;

	priv->net_filters = devm_kcalloc(dev, priv->num_net_filters,
					 sizeof(*priv->net_filters), GFP_KERNEL);
	if (!priv->net_filters)
		return -ENOMEM;

	bcmasp_core_init_filters(priv);

	ports_node = of_find_node_by_name(dev->of_node, "ethernet-ports");
	if (!ports_node) {
		dev_warn(dev, "No ports found\n");
		return -EINVAL;
	}

	i = 0;
	for_each_available_child_of_node_scoped(ports_node, intf_node) {
		intf = bcmasp_interface_create(priv, intf_node, i);
		if (!intf) {
			dev_err(dev, "Cannot create eth interface %d\n", i);
			bcmasp_remove_intfs(priv);
			ret = -ENOMEM;
			goto of_put_exit;
		}
		list_add_tail(&intf->list, &priv->intfs);
		i++;
	}

	/* Check and enable WoL */
	bcmasp_init_wol(priv);

	/* Drop the clock reference count now and let ndo_open()/ndo_close()
	 * manage it for us from now on.
	 */
	bcmasp_core_clock_set(priv, 0, ASP_CTRL_CLOCK_CTRL_ASP_ALL_DISABLE);

	clk_disable_unprepare(priv->clk);

	/* Now do the registration of the network ports which will take care
	 * of managing the clock properly.
	 */
	list_for_each_entry(intf, &priv->intfs, list) {
		ret = register_netdev(intf->ndev);
		if (ret) {
			netdev_err(intf->ndev,
				   "failed to register net_device: %d\n", ret);
			bcmasp_wol_irq_destroy(priv);
			bcmasp_remove_intfs(priv);
			goto of_put_exit;
		}
		count++;
	}

	dev_info(dev, "Initialized %d port(s)\n", count);

of_put_exit:
	of_node_put(ports_node);
	return ret;
}

static void bcmasp_remove(struct platform_device *pdev)
{
	struct bcmasp_priv *priv = dev_get_drvdata(&pdev->dev);

	if (!priv)
		return;

	bcmasp_wol_irq_destroy(priv);
	bcmasp_remove_intfs(priv);
}

static void bcmasp_shutdown(struct platform_device *pdev)
{
	bcmasp_remove(pdev);
}

static int __maybe_unused bcmasp_suspend(struct device *d)
{
	struct bcmasp_priv *priv = dev_get_drvdata(d);
	struct bcmasp_intf *intf;
	int ret;

	list_for_each_entry(intf, &priv->intfs, list) {
		ret = bcmasp_interface_suspend(intf);
		if (ret)
			break;
	}

	ret = clk_prepare_enable(priv->clk);
	if (ret)
		return ret;

	/* Whether Wake-on-LAN is enabled or not, we can always disable
	 * the shared TX clock
	 */
	bcmasp_core_clock_set(priv, 0, ASP_CTRL_CLOCK_CTRL_ASP_TX_DISABLE);

	priv->core_clock_select(priv, true);

	clk_disable_unprepare(priv->clk);

	return ret;
}

static int __maybe_unused bcmasp_resume(struct device *d)
{
	struct bcmasp_priv *priv = dev_get_drvdata(d);
	struct bcmasp_intf *intf;
	int ret;

	ret = clk_prepare_enable(priv->clk);
	if (ret)
		return ret;

	/* Switch to the main clock domain */
	priv->core_clock_select(priv, false);

	/* Re-enable all clocks for re-initialization */
	bcmasp_core_clock_set(priv, ASP_CTRL_CLOCK_CTRL_ASP_ALL_DISABLE, 0);

	bcmasp_core_init(priv);
	bcmasp_core_init_filters(priv);

	/* And disable them to let the network devices take care of them */
	bcmasp_core_clock_set(priv, 0, ASP_CTRL_CLOCK_CTRL_ASP_ALL_DISABLE);

	clk_disable_unprepare(priv->clk);

	list_for_each_entry(intf, &priv->intfs, list) {
		ret = bcmasp_interface_resume(intf);
		if (ret)
			break;
	}

	return ret;
}

static SIMPLE_DEV_PM_OPS(bcmasp_pm_ops,
			 bcmasp_suspend, bcmasp_resume);

static struct platform_driver bcmasp_driver = {
	.probe = bcmasp_probe,
	.remove = bcmasp_remove,
	.shutdown = bcmasp_shutdown,
	.driver = {
		.name = "brcm,asp-v2",
		.of_match_table = bcmasp_of_match,
		.pm = &bcmasp_pm_ops,
	},
};
module_platform_driver(bcmasp_driver);

MODULE_DESCRIPTION("Broadcom ASP 2.0 Ethernet controller driver");
MODULE_ALIAS("platform:brcm,asp-v2");
MODULE_LICENSE("GPL");

// SPDX-License-Identifier:    GPL-2.0
/*
 * Copyright (C) 2020 Marvell International Ltd.
 *
 * https://spdx.org/licenses
 */

#include <common.h>
#include <net.h>
#include <malloc.h>
#include <dm.h>
#include <misc.h>
#include <pci.h>
#include <memalign.h>
#include <watchdog.h>
#include <asm/types.h>
#include <asm/io.h>
#include <linux/types.h>
#include <linux/log2.h>
#include <linux/delay.h>
#include <asm/arch/board.h>
#include "nix.h"
#include "lmt.h"
#include "rpm.h"

/**
 * NIX needs a lot of memory areas. Rather than handle all the failure cases,
 * we'll use a wrapper around alloc that prints an error if a memory
 * allocation fails.
 *
 * @param num_elements
 *                  Number of elements to allocate
 * @param elem_size Size of each element
 * @param msg       Text string to show when allocation fails
 *
 * @return A valid memory location or NULL on failure
 */
static void *nix_memalloc(int num_elements, size_t elem_size, const char *msg)
{
	size_t alloc_size = num_elements * elem_size;
	void *base = memalign(CONFIG_SYS_CACHELINE_SIZE, alloc_size);

	if (!base)
		printf("NIX: Mem alloc failed for %s (%d * %zu = %zu bytes)\n",
		       msg ? msg : __func__, num_elements, elem_size,
		       alloc_size);
	else
		memset(base, 0, alloc_size);

	debug("NIX: Memory alloc for %s (%d * %zu = %zu bytes) at %p\n",
	      msg ? msg : __func__, num_elements, elem_size, alloc_size, base);
	return base;
}

int npc_lf_setup(struct nix *nix)
{
	int err;

	err = npc_lf_admin_setup(nix);
	if (err) {
		printf("%s: Error setting up npc lf admin\n", __func__);
		return err;
	}

	return 0;
}

static int npa_setup_pool(struct npa *npa, u32 pool_id,
			  size_t buffer_size, u32 queue_length, void *buffers[])
{
	int index;
	u64 lmt_data[2], val, lmt_addr = readq(npa->lmt_base);

	for (index = 0; index < queue_length; index++) {
		buffers[index] = memalign(CONFIG_SYS_CACHELINE_SIZE,
					  buffer_size);
		if (!buffers[index]) {
			printf("%s: Out of memory %d, size: %zu\n",
			       __func__, index, buffer_size);
			return -ENOMEM;
		}
		debug("%s: allocating buffer %d, addr %p size: %zu\n",
		      __func__, index, buffers[index], buffer_size);

		/* Add the newly obtained pointer to the pool.  128 bit
		 * writes only.
		 */
		lmt_data[1] = (u64)buffers[index];
		lmt_data[0] = pool_id | (1ULL << 32);
		val = pool_id;
		memcpy((void *)lmt_addr + (pool_id * 0x80), lmt_data, 16);
		__iowmb();
		lmt_submit((u64)(npa->npa_base + NPA_LF_AURA_BATCH_FREE0()),
			   val);
	}

	return 0;
}

int npa_lf_setup(struct nix *nix)
{
	struct rvu_pf *rvu = dev_get_priv(nix->dev);
	struct nix_af *nix_af = nix->nix_af;
	struct npa *npa;
	union npa_af_const npa_af_const;
	union npa_aura_s *aura;
	union npa_pool_s *pool;
	union rvu_func_addr_s block_addr;
	int idx;
	int stack_page_pointers;
	int stack_page_bytes;
	int err;

	npa = (struct npa *)calloc(1, sizeof(struct npa));
	if (!npa) {
		printf("%s: out of memory for npa instance\n", __func__);
		return -ENOMEM;
	}
	block_addr.u = 0;
	block_addr.s.block = RVU_BLOCK_ADDR_E_NPA;
	npa->npa_base = rvu->pf_base + block_addr.u;
	npa->lmt_base = rvu->pf_base + RVU_PF_LMTLINE_ADDR();
	npa->npa_af = nix_af->npa_af;
	nix->npa = npa;

	npa_af_const.u = npa_af_reg_read(npa->npa_af, NPA_AF_CONST());
	stack_page_pointers = npa_af_const.s.stack_page_ptrs;
	stack_page_bytes = npa_af_const.s.stack_page_bytes;

	npa->stack_pages[NPA_POOL_RX] = (RQ_QLEN + stack_page_pointers - 1) /
							stack_page_pointers;
	npa->stack_pages[NPA_POOL_TX] = (SQ_QLEN + stack_page_pointers - 1) /
							stack_page_pointers;
	npa->stack_pages[NPA_POOL_SQB] = (SQB_QLEN + stack_page_pointers - 1) /
							stack_page_pointers;
	npa->stack_pages[NPA_POOL_RX_LPB] = (RQ_LPB_QLEN + stack_page_pointers - 1) /
							stack_page_pointers;
	npa->pool_stack_pointers = stack_page_pointers;

	npa->q_len[NPA_POOL_RX] = RQ_QLEN;
	npa->q_len[NPA_POOL_TX] = SQ_QLEN;
	npa->q_len[NPA_POOL_SQB] = SQB_QLEN;
	npa->q_len[NPA_POOL_RX_LPB] = RQ_LPB_QLEN;

	npa->buf_size[NPA_POOL_RX] = MAX_MTU + CONFIG_SYS_CACHELINE_SIZE;
	npa->buf_size[NPA_POOL_TX] = MAX_MTU + CONFIG_SYS_CACHELINE_SIZE;
	npa->buf_size[NPA_POOL_SQB] = nix_af->sqb_size;
	npa->buf_size[NPA_POOL_RX_LPB] = NIX_MAX_HW_MTU + CONFIG_SYS_CACHELINE_SIZE;

	npa->aura_ctx = nix_memalloc(NPA_POOL_COUNT,
				     sizeof(union npa_aura_s),
				     "aura context");
	if (!npa->aura_ctx) {
		printf("%s: Out of memory for aura context\n", __func__);
		return -ENOMEM;
	}

	for (idx = 0; idx < NPA_POOL_COUNT; idx++) {
		npa->pool_ctx[idx] = nix_memalloc(1,
						  sizeof(union npa_pool_s),
						  "pool context");
		if (!npa->pool_ctx[idx]) {
			printf("%s: Out of memory for pool context\n",
			       __func__);
			return -ENOMEM;
		}
		npa->pool_stack[idx] = nix_memalloc(npa->stack_pages[idx],
						    stack_page_bytes,
						    "pool stack");
		if (!npa->pool_stack[idx]) {
			printf("%s: Out of memory for pool stack\n", __func__);
			return -ENOMEM;
		}
	}

	err = npa_lf_admin_setup(npa, nix->lf, (dma_addr_t)npa->aura_ctx);
	if (err) {
		printf("%s: Error setting up NPA LF admin for lf %d\n",
		       __func__, nix->lf);
		return err;
	}

	/* Set up the auras */
	for (idx = 0; idx < NPA_POOL_COUNT; idx++) {
		aura = npa->aura_ctx + (idx * sizeof(union npa_aura_s));
		pool = npa->pool_ctx[idx];
		debug("%s aura %p pool %p\n", __func__, aura, pool);
		memset(aura, 0, sizeof(union npa_aura_s));
		aura->s.fc_ena = 0;
		aura->s.pool_addr = (u64)npa->pool_ctx[idx];
		aura->s.shift = 64 - __builtin_clzll(npa->q_len[idx]) - 8;
		aura->s.count = npa->q_len[idx];
		aura->s.limit = npa->q_len[idx];
		aura->s.ena = 1;
		err = npa_attach_aura(nix_af, nix->lf, aura, idx);
		if (err)
			return err;

		memset(pool, 0, sizeof(*pool));
		pool->s.fc_ena = 0;
		pool->s.nat_align = 1;
		pool->s.stack_base = (u64)(npa->pool_stack[idx]);
		debug("%s pool.s.stack_base %llx stack_base %p\n", __func__,
		      pool->s.stack_base, npa->pool_stack[idx]);
		pool->s.buf_size =
			npa->buf_size[idx] / CONFIG_SYS_CACHELINE_SIZE;
		pool->s.stack_max_pages = npa->stack_pages[idx];
		pool->s.shift =
			64 - __builtin_clzll(npa->pool_stack_pointers) - 8;
		pool->s.ptr_start = 0;
		pool->s.ptr_end = (1ULL << 40) -  1;
		pool->s.ena = 1;
		err = npa_attach_pool(nix_af, nix->lf, pool, idx);
		if (err)
			return err;
	}

	for (idx = 0; idx < NPA_POOL_COUNT; idx++) {
		npa->buffers[idx] = nix_memalloc(npa->q_len[idx],
						 sizeof(void *),
						 "buffers");
		if (!npa->buffers[idx]) {
			printf("%s: Out of memory\n", __func__);
			return -ENOMEM;
		}
	}

	for (idx = 0; idx < NPA_POOL_COUNT; idx++) {
		err = npa_setup_pool(npa, idx, npa->buf_size[idx],
				     npa->q_len[idx], npa->buffers[idx]);
		if (err) {
			printf("%s: Error setting up pool %d\n",
			       __func__, idx);
			return err;
		}
	}
	return 0;
}

int npa_lf_shutdown(struct nix *nix)
{
	struct npa *npa = nix->npa;
	int err;
	int pool;

	err = npa_lf_admin_shutdown(nix->nix_af, nix->lf, NPA_POOL_COUNT);
	if (err) {
		printf("%s: Error %d shutting down NPA LF admin\n",
		       __func__, err);
		return err;
	}
	free(npa->aura_ctx);
	npa->aura_ctx = NULL;

	for (pool = 0; pool < NPA_POOL_COUNT; pool++) {
		free(npa->pool_ctx[pool]);
		npa->pool_ctx[pool] = NULL;
		free(npa->pool_stack[pool]);
		npa->pool_stack[pool] = NULL;
		free(npa->buffers[pool]);
		npa->buffers[pool] = NULL;
	}

	return 0;
}

int nix_lf_setup(struct nix *nix)
{
	struct nix_af *nix_af = nix->nix_af;
	int idx;
	int err = -1;

	/* Alloc NIX RQ HW context memory */
	nix->rq_ctx_base = nix_memalloc(nix->rq_cnt, nix_af->rq_ctx_sz,
					"RQ CTX");
	if (!nix->rq_ctx_base)
		goto error;
	memset(nix->rq_ctx_base, 0, nix_af->rq_ctx_sz);

	/* Alloc NIX SQ HW context memory */
	nix->sq_ctx_base = nix_memalloc(nix->sq_cnt, nix_af->sq_ctx_sz,
					"SQ CTX");
	if (!nix->sq_ctx_base)
		goto error;
	memset(nix->sq_ctx_base, 0, nix_af->sq_ctx_sz);

	/* Alloc NIX CQ HW context memory */
	nix->cq_ctx_base = nix_memalloc(nix->cq_cnt, nix_af->cq_ctx_sz,
					"CQ CTX");
	if (!nix->cq_ctx_base)
		goto error;
	memset(nix->cq_ctx_base, 0, nix_af->cq_ctx_sz * NIX_CQ_COUNT);
	/* Alloc NIX CQ Ring memory */
	for (idx = 0; idx < NIX_CQ_COUNT; idx++) {
		err = qmem_alloc(&nix->cq[idx], CQ_ENTRIES, CQ_ENTRY_SIZE);
		if (err)
			goto error;
	}

	/* Alloc memory for Qints HW contexts */
	nix->qint_base = nix_memalloc(nix_af->qints, nix_af->qint_ctx_sz,
				      "Qint CTX");
	if (!nix->qint_base)
		goto error;
	/* Alloc memory for CQints HW contexts */
	nix->cint_base = nix_memalloc(nix_af->cints, nix_af->cint_ctx_sz,
				      "Cint CTX");
	if (!nix->cint_base)
		goto error;

	err = nix_lf_admin_setup(nix);
	if (err) {
		printf("%s: Error setting up LF\n", __func__);
		goto error;
	}

	return 0;

error:
	if (nix->rq_ctx_base)
		free(nix->rq_ctx_base);
	nix->rq_ctx_base = NULL;
	if (nix->sq_ctx_base)
		free(nix->sq_ctx_base);
	nix->sq_ctx_base = NULL;
	if (nix->cq_ctx_base)
		free(nix->cq_ctx_base);
	nix->cq_ctx_base = NULL;

	for (idx = 0; idx < NIX_CQ_COUNT; idx++)
		qmem_free(&nix->cq[idx]);

	return err;
}

int nix_lf_shutdown(struct nix *nix)
{
	struct nix_af *nix_af = nix->nix_af;
	int index;
	int err;

	err = nix_lf_admin_shutdown(nix_af, nix->lf, nix->cq_cnt,
				    nix->rq_cnt, nix->sq_cnt);
	if (err) {
		printf("%s: Error shutting down LF admin\n", __func__);
		return err;
	}

	if (nix->rq_ctx_base)
		free(nix->rq_ctx_base);
	nix->rq_ctx_base = NULL;
	if (nix->sq_ctx_base)
		free(nix->sq_ctx_base);
	nix->sq_ctx_base = NULL;
	if (nix->cq_ctx_base)
		free(nix->cq_ctx_base);
	nix->cq_ctx_base = NULL;

	for (index = 0; index < NIX_CQ_COUNT; index++)
		qmem_free(&nix->cq[index]);

	return 0;
}

struct nix *nix_lf_alloc(struct udevice *dev)
{
	union rvu_func_addr_s block_addr;
	struct nix *nix;
	struct rvu_pf *rvu = dev_get_priv(dev);
	struct rvu_af *rvu_af = dev_get_priv(rvu->afdev);
	union rvu_pf_func_s pf_func;
	int err;

	nix = (struct nix *)calloc(1, sizeof(*nix));
	if (!nix) {
		printf("%s: Out of memory for nix instance\n", __func__);
		return NULL;
	}
	nix->nix_af = rvu_af->nix_af;

	block_addr.u = 0;
	block_addr.s.block = RVU_BLOCK_ADDR_E_NIXX(0);
	nix->nix_base = rvu->pf_base + block_addr.u;
	block_addr.u = 0;
	block_addr.s.block = RVU_BLOCK_ADDR_E_NPC;
	nix->npc_base = rvu->pf_base + block_addr.u;

	nix->lmt_base = rvu->pf_base + RVU_PF_LMTLINE_ADDR();

	pf_func.u = 0;
	pf_func.s.pf = rvu->pfid;
	nix->pf_func = pf_func.u;
	nix->lf = rvu->nix_lfid;
	nix->pf = rvu->pfid;
	nix->dev = dev;
	nix->sq_cnt = 1;
	nix->rq_cnt = 1;
	nix->rss_grps = 1;
	nix->cq_cnt = 2;
	nix->xqe_sz = NIX_CQE_SIZE_W16;

	nix->lmac = nix_get_rpm_lmac(nix->pf);
	if (!nix->lmac) {
		printf("%s: Error: could not find lmac for pf %d\n",
		       __func__, nix->pf);
		free(nix);
		return NULL;
	}
	nix->lmac->link_num =
		NIX_LINK_E_RPMX_LMACX(nix->lmac->rpm->rpm_id,
				      nix->lmac->lmac_id);
	nix->lmac->chan_num =
		NIX_CHAN_E_RPMX_LMACX_CHX(nix->lmac->rpm->rpm_id,
					  nix->lmac->lmac_id, 0);
	/* This is rx pkind in 1:1 mapping to NIX_LINK_E */
	nix->lmac->pknd = nix->lmac->link_num;

	rpm_lmac_set_pkind(nix->lmac, nix->lmac->lmac_id, nix->lmac->pknd);
	rpm_lmac_set_chan(nix->lmac);
	debug("%s(%s RPM%x LMAC%x)\n", __func__, dev->name,
	      nix->lmac->rpm->rpm_id, nix->lmac->lmac_id);
	debug("%s(%s Link %x Chan %x Pknd %x)\n", __func__, dev->name,
	      nix->lmac->link_num, nix->lmac->chan_num,	nix->lmac->pknd);

	err = npa_lf_setup(nix);
	if (err) {
		free(nix);
		return NULL;
	}

	err = npc_lf_setup(nix);
	if (err) {
		free(nix);
		return NULL;
	}

	err = nix_lf_setup(nix);
	if (err) {
		free(nix);
		return NULL;
	}

	return nix;
}

u64 npa_aura_op_alloc(struct npa *npa, u64 aura_id)
{
	union npa_lf_aura_op_allocx op_allocx;

	op_allocx.u = atomic_fetch_and_add64_nosync(npa->npa_base +
			NPA_LF_AURA_OP_ALLOCX(0), aura_id);
	return op_allocx.s.addr;
}

u64 nix_cq_op_status(struct nix *nix, u64 cq_id)
{
	union nixx_lf_cq_op_status op_status;
	s64 *reg = nix->nix_base + NIXX_LF_CQ_OP_STATUS();

	op_status.u = atomic_fetch_and_add64_nosync(reg, cq_id << 32);
	return op_status.u;
}

/* TX */
static inline void nix_write_lmt(struct nix *nix, void *buffer,
				 int num_words)
{
	int i;

	u64 *lmt_ptr = (u64 *)(readq(nix->lmt_base) + (0x80 * 0x10));
	u64 *ptr = buffer;

	for (i = 0; i < num_words; i++) {
		debug("%s data %llx lmt_ptr %p\n", __func__, ptr[i],
		      lmt_ptr + i);
		lmt_ptr[i] = ptr[i];
	}
}

void nix_cqe_tx_pkt_handler(struct nix *nix, void *cqe)
{
	union nix_cqe_hdr_s *txcqe = (union nix_cqe_hdr_s *)cqe;

	if (txcqe->s.cqe_type != NIX_XQE_TYPE_E_SEND) {
		printf("%s: Error: Unsupported CQ header type %d\n",
		       __func__, (u8)txcqe->s.cqe_type);
		return;
	}
	nix_pf_reg_write(nix, NIXX_LF_CQ_OP_DOOR(),
			 (NIX_CQ_TX << 32) | 1);
}

void nix_lf_flush_tx(struct udevice *dev)
{
	struct rvu_pf *rvu = dev_get_priv(dev);
	struct nix *nix = rvu->nix;
	union nixx_lf_cq_op_status op_status;
	u32 head, tail;
	void *cq_tx_base = nix->cq[NIX_CQ_TX].base;
	union nix_cqe_hdr_s *cqe;

	/* ack tx cqe entries */
	op_status.u = nix_cq_op_status(nix, NIX_CQ_TX);
	head = op_status.s.head;
	tail = op_status.s.tail;
	head &= (nix->cq[NIX_CQ_TX].qsize - 1);
	tail &= (nix->cq[NIX_CQ_TX].qsize - 1);

	while (head != tail) {
		cqe = cq_tx_base + head * nix->cq[NIX_CQ_TX].entry_sz;
		nix_cqe_tx_pkt_handler(nix, cqe);
		op_status.u = nix_cq_op_status(nix, NIX_CQ_TX);
		head = op_status.s.head;
		tail = op_status.s.tail;
		head &= (nix->cq[NIX_CQ_TX].qsize - 1);
		tail &= (nix->cq[NIX_CQ_TX].qsize - 1);
		debug("%s cq tx head %d tail %d\n", __func__, head, tail);
	}
}

int nix_lf_xmit(struct udevice *dev, void *pkt, int pkt_len)
{
	struct rvu_pf *rvu = dev_get_priv(dev);
	struct nix *nix = rvu->nix;
	struct nix_tx_dr tx_dr;
	int dr_sz = (sizeof(struct nix_tx_dr) + 15) / 16 - 1;
	u64 val, addr;
	void *packet;

	nix_lf_flush_tx(dev);
	memset((void *)&tx_dr, 0, sizeof(struct nix_tx_dr));
	/* Dump TX packet in to NPA buffer */
	packet = (void *)npa_aura_op_alloc(nix->npa, NPA_POOL_TX);
	if (!packet) {
		printf("%s TX buffers unavailable\n", __func__);
		return -1;
	}
	memcpy(packet, pkt, pkt_len);
	debug("\n%s TX buffer %p\n", __func__, packet);

	tx_dr.hdr.s.aura = NPA_POOL_TX;
	tx_dr.hdr.s.df = 0;
	tx_dr.hdr.s.pnc = 1;
	tx_dr.hdr.s.sq = 0;
	tx_dr.hdr.s.total = pkt_len;
	tx_dr.hdr.s.sizem1 = dr_sz - 2; /* FIXME - for now hdr+sg+sg1addr */
	debug("%s dr_sz %d\n", __func__, dr_sz);

	tx_dr.tx_sg.s.segs = 1;
	tx_dr.tx_sg.s.subdc = NIX_SUBDC_E_SG;
	tx_dr.tx_sg.s.seg1_size = pkt_len;
	tx_dr.tx_sg.s.ld_type = NIX_SENDLDTYPE_E_LDT;
	tx_dr.sg1_addr = (dma_addr_t)packet;

#define DEBUG_PKT
#ifdef DEBUG_PKT
	debug("TX PKT Data\n");
	for (int i = 0; i < pkt_len; i++) {
		if (i && (i % 8 == 0))
			debug("\n");
		debug("%02x ", *((u8 *)pkt + i));
	}
	debug("\n");
#endif
	nix_write_lmt(nix, &tx_dr, (dr_sz - 1) * 2);
	__iowmb();
	val = 0x10;
	addr = (u64)(nix->nix_base + NIXX_LF_OP_SENDX(0));
	addr += (1 << 4); /* 2 LMT 128 bit words */
	lmt_submit(addr, val);

	return 0;
}

void npa_lf_rx_free_ptr(struct npa *npa, u64 pkt_addr, int pool_id)
{
	u64 lmt_data[2], val, lmt_addr = readq(npa->lmt_base);

	/* Push pointer to the pool. 128 bit writes only */
	lmt_data[1] = pkt_addr;
	lmt_data[0] = pool_id | (1ULL << 32);
	val = pool_id;
	memcpy((void *)lmt_addr + (pool_id * 0x80), lmt_data, 16);
	__iowmb();
	lmt_submit((u64)(npa->npa_base + NPA_LF_AURA_BATCH_FREE0()),
		   val);
}

/* RX */
void nix_lf_flush_rx(struct udevice *dev)
{
	struct rvu_pf *rvu = dev_get_priv(dev);
	struct nix *nix = rvu->nix;
	union nixx_lf_cq_op_status op_status;
	void *cq_rx_base = nix->cq[NIX_CQ_RX].base;
	struct nix_rx_dr *rx_dr;
	union nix_rx_parse_s *rxparse;
	u32 head, tail;
	u32 rx_cqe_sz = nix->cq[NIX_CQ_RX].entry_sz;
	u64 *seg;
	union nix_rx_sg_s *rxsg;
	int pool_id = NPA_POOL_RX;

	/* flush rx cqe entries */
	op_status.u = nix_cq_op_status(nix, NIX_CQ_RX);
	head = op_status.s.head;
	tail = op_status.s.tail;
	head &= (nix->cq[NIX_CQ_RX].qsize - 1);
	tail &= (nix->cq[NIX_CQ_RX].qsize - 1);
	debug("%s cq rx head %d tail %d\n", __func__, head, tail);

	while (head != tail) {
		rx_dr = (struct nix_rx_dr *)(cq_rx_base + head * rx_cqe_sz);
		rxparse = &rx_dr->rx_parse;
		rxsg = &rx_dr->rx_sg;

		debug("%s: rx parse: desc_sizem1 %x pkt_lenm1 %x\n",
		      __func__, (u8)rxparse->s.desc_sizem1,
		      (u16)rxparse->s.pkt_lenm1);

		seg = (dma_addr_t *)(rxsg + 1);

		if (rxparse->s.pkt_lenm1 > MAX_MTU)
			pool_id = NPA_POOL_RX_LPB;
		npa_lf_rx_free_ptr(nix->npa, (u64)(*seg), pool_id);
		debug("%s return %llx to NPA pool %d\n", __func__,
		      *seg, pool_id);
		nix_pf_reg_write(nix, NIXX_LF_CQ_OP_DOOR(),
				 (NIX_CQ_RX << 32) | 1);

		op_status.u = nix_cq_op_status(nix, NIX_CQ_RX);
		head = op_status.s.head;
		tail = op_status.s.tail;
		head &= (nix->cq[NIX_CQ_RX].qsize - 1);
		tail &= (nix->cq[NIX_CQ_RX].qsize - 1);
		debug("%s cq rx head %d tail %d\n", __func__, head, tail);
	}
}

int nix_lf_free_pkt(struct udevice *dev, uchar *pkt, int pkt_len)
{
	struct rvu_pf *rvu = dev_get_priv(dev);
	struct nix *nix = rvu->nix;
	int pool_id = NPA_POOL_RX;
	union nixx_lf_cq_op_status op_status;
	u32 head, tail;

	op_status.u = nix_cq_op_status(nix, NIX_CQ_RX);
	head = op_status.s.head;
	tail = op_status.s.tail;
	head &= (nix->cq[NIX_CQ_RX].qsize - 1);
	tail &= (nix->cq[NIX_CQ_RX].qsize - 1);
	debug("%s cq rx head %d tail %d\n", __func__, head, tail);
	/*
	 * In case halt method flushes out RX CQE, nothing to do.
	 * Otherwise duplicate free occurs.
	 */
	if (head == tail)
		return 0;

	/* Return rx packet to NPA */
	if (pkt_len > MAX_MTU)
		pool_id = NPA_POOL_RX_LPB;

	debug("%s return %p to NPA pool %d\n", __func__, pkt, pool_id);

	npa_lf_rx_free_ptr(nix->npa, (u64)pkt, pool_id);

	nix_pf_reg_write(nix, NIXX_LF_CQ_OP_DOOR(),
			 (NIX_CQ_RX << 32) | 1);

	nix_lf_flush_tx(dev);
	return 0;
}

int nix_lf_recv(struct udevice *dev, int flags, uchar **packetp)
{
	struct rvu_pf *rvu = dev_get_priv(dev);
	struct nix *nix = rvu->nix;
	union nixx_lf_cq_op_status op_status;
	void *cq_rx_base = nix->cq[NIX_CQ_RX].base;
	struct nix_rx_dr *rx_dr;
	union nix_rx_parse_s *rxparse;
	union nix_rx_sg_s *rxsg;
	void *pkt, *cqe;
	int pkt_len = 0;
	u64 *addr;
	u32 head, tail;

	/* fetch rx cqe entries */
	op_status.u = nix_cq_op_status(nix, NIX_CQ_RX);
	head = op_status.s.head;
	tail = op_status.s.tail;
	head &= (nix->cq[NIX_CQ_RX].qsize - 1);
	tail &= (nix->cq[NIX_CQ_RX].qsize - 1);
	debug("%s cq rx head %d tail %d\n", __func__, head, tail);
	if (head == tail)
		return -EAGAIN;

	debug("%s: rx_base %p head %d sz %d\n", __func__, cq_rx_base, head,
	      nix->cq[NIX_CQ_RX].entry_sz);
	cqe = cq_rx_base + head * nix->cq[NIX_CQ_RX].entry_sz;
	rx_dr = (struct nix_rx_dr *)cqe;
	rxparse = &rx_dr->rx_parse;
	rxsg = &rx_dr->rx_sg;

	debug("%s: rx parse: desc_sizem1 %x pkt_lenm1 %x\n",
	      __func__, (u8)rxparse->s.desc_sizem1, (u16)rxparse->s.pkt_lenm1);
	debug("%s: rx parse: pkind %x chan %x\n",
	      __func__, rxparse->s.pkind, rxparse->s.chan);

	if (rx_dr->hdr.s.cqe_type != NIX_XQE_TYPE_E_RX) {
		printf("%s: Error: Unsupported CQ header type in Rx %d\n",
		       __func__, rx_dr->hdr.s.cqe_type);
		return -1;
	}

	pkt_len = rxparse->s.pkt_lenm1 + 1;
	addr = (dma_addr_t *)(rxsg + 1);
	pkt = (void *)addr[0];

	debug("%s: segs: %d (%d@0x%llx, %d@0x%llx, %d@0x%llx)\n",
	      __func__, rxsg->s.segs, rxsg->s.seg1_size, addr[0],
	      rxsg->s.seg2_size, addr[1],
	      rxsg->s.seg3_size, addr[2]);
	if (pkt_len < rxsg->s.seg1_size) {
		debug("%s: Error: rx buffer size too small\n", __func__);
		return -1;
	}
	__iowmb();

#define DEBUG_PKT
#ifdef DEBUG_PKT
	debug("RX PKT Data\n");
	for (int i = 0; i < pkt_len; i++) {
		if (i && (i % 8 == 0))
			debug("\n");
		debug("%02x ", *((u8 *)pkt + i));
	}
	debug("\n");
#endif

	*packetp = (uchar *)pkt;

	return pkt_len;
}

int nix_lf_setup_mac(struct udevice *dev)
{
	struct rvu_pf *rvu = dev_get_priv(dev);
	struct nix *nix = rvu->nix;
	struct eth_pdata *pdata = dev_get_platdata(dev);

	/* If lower level firmware fails to set proper MAC
	 * u-boot framework updates MAC to random address.
	 * Use this hook to update mac address in rpm lmac
	 * and call mac filter setup to update new address.
	 */
	if (memcmp(nix->lmac->mac_addr, pdata->enetaddr, ARP_HLEN)) {
		memcpy(nix->lmac->mac_addr, pdata->enetaddr, 6);
		eth_env_set_enetaddr_by_index("eth", rvu->dev->seq,
					      pdata->enetaddr);
		rpm_lmac_mac_filter_setup(nix->lmac);
		/* Update user given MAC address to ATF for update
		 * in sh_fwdata to use in Linux.
		 */
		eth_intf_set_macaddr(dev);
		debug("%s: lMAC %pM\n", __func__, nix->lmac->mac_addr);
		debug("%s: pMAC %pM\n", __func__, pdata->enetaddr);
	}
	debug("%s: setupMAC %pM\n", __func__, pdata->enetaddr);
	return 0;
}

void nix_lf_halt(struct udevice *dev)
{
	struct rvu_pf *rvu = dev_get_priv(dev);
	struct nix *nix = rvu->nix;

	rpm_lmac_rx_tx_enable(nix->lmac, nix->lmac->lmac_id, false);

	mdelay(1);

	/* Flush tx and rx descriptors */
	nix_lf_flush_rx(dev);
	nix_lf_flush_tx(dev);
}

int nix_lf_init(struct udevice *dev)
{
	struct rvu_pf *rvu = dev_get_priv(dev);
	struct nix *nix = rvu->nix;
	struct lmac *lmac = nix->lmac;
	int ret;
	u64 link_sts;
	u8 link, speed, type;
	u16 errcode;

	printf("Waiting for RPM%d LMAC%d link status...",
	       lmac->rpm->rpm_id, lmac->lmac_id);

	if (lmac->init_pend) {
		/* Bring up LMAC */
		ret = rpm_lmac_link_enable(lmac, lmac->lmac_id,
					   true, &link_sts);
		lmac->init_pend = 0;
	} else {
		ret = rpm_lmac_link_status(lmac, lmac->lmac_id, &link_sts);
	}

	if (ret) {
		printf(" [Down]\n");
		return -1;
	}

	link = link_sts & 0x1;
	speed = (link_sts >> 2) & 0xf;
	errcode = (link_sts >> 6) & 0x2ff;
	type = (link_sts >> 19) & 0xff;
	debug("%s: link %x speed %x errcode %x\n",
	      __func__, link, speed, errcode);

	/* Print link status */
	printf(" %s [%s]\n",
	       link ? lmac_type_to_str[type] : "",
	       link ? lmac_speed_to_str[speed] : "Down");
	if (!link)
		return -1;

	if (!lmac->init_pend)
		rpm_lmac_rx_tx_enable(lmac, lmac->lmac_id, true);

	return 0;
}

void nix_get_rpm_lmac_id(struct udevice *dev, int *rpmid, int *lmacid)
{
	struct rvu_pf *rvu = dev_get_priv(dev);
	struct nix *nix = rvu->nix;
	struct lmac *lmac = nix->lmac;

	*rpmid = lmac->rpm->rpm_id;
	*lmacid = lmac->lmac_id;
}

void nix_print_mac_info(struct udevice *dev)
{
	struct rvu_pf *rvu = dev_get_priv(dev);
	struct nix *nix = rvu->nix;
	struct lmac *lmac = nix->lmac;

	printf(" RPM%d LMAC%d", lmac->rpm->rpm_id, lmac->lmac_id);
}


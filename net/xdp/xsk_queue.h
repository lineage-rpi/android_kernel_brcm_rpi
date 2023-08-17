/* SPDX-License-Identifier: GPL-2.0 */
/* XDP user-space ring structure
 * Copyright(c) 2018 Intel Corporation.
 */

#ifndef _LINUX_XSK_QUEUE_H
#define _LINUX_XSK_QUEUE_H

#include <linux/types.h>
#include <linux/if_xdp.h>
#include <net/xdp_sock.h>

#define RX_BATCH_SIZE 16
#define LAZY_UPDATE_THRESHOLD 128

struct xdp_ring {
	u32 producer ____cacheline_aligned_in_smp;
	u32 consumer ____cacheline_aligned_in_smp;
};

/* Used for the RX and TX queues for packets */
struct xdp_rxtx_ring {
	struct xdp_ring ptrs;
	struct xdp_desc desc[0] ____cacheline_aligned_in_smp;
};

/* Used for the fill and completion queues for buffers */
struct xdp_umem_ring {
	struct xdp_ring ptrs;
	u64 desc[0] ____cacheline_aligned_in_smp;
};

struct xsk_queue {
	struct xdp_umem_props umem_props;
	u32 ring_mask;
	u32 nentries;
	u32 prod_head;
	u32 prod_tail;
	u32 cons_head;
	u32 cons_tail;
	struct xdp_ring *ring;
	u64 invalid_descs;
};

/* Common functions operating for both RXTX and umem queues */

static inline u64 xskq_nb_invalid_descs(struct xsk_queue *q)
{
	return q ? q->invalid_descs : 0;
}

static inline u32 xskq_nb_avail(struct xsk_queue *q, u32 dcnt)
{
	u32 entries = q->prod_tail - q->cons_tail;

	if (entries == 0) {
		/* Refresh the local pointer */
		q->prod_tail = READ_ONCE(q->ring->producer);
		entries = q->prod_tail - q->cons_tail;
	}

	return (entries > dcnt) ? dcnt : entries;
}

static inline u32 xskq_nb_free(struct xsk_queue *q, u32 producer, u32 dcnt)
{
	u32 free_entries = q->nentries - (producer - q->cons_tail);

	if (free_entries >= dcnt)
		return free_entries;

	/* Refresh the local tail pointer */
	q->cons_tail = READ_ONCE(q->ring->consumer);
	return q->nentries - (producer - q->cons_tail);
}

/* UMEM queue */

static inline bool xskq_is_valid_addr(struct xsk_queue *q, u64 addr)
{
	if (addr >= q->umem_props.size) {
		q->invalid_descs++;
		return false;
	}

	return true;
}

static inline u64 *xskq_validate_addr(struct xsk_queue *q, u64 *addr)
{
	while (q->cons_tail != q->cons_head) {
		struct xdp_umem_ring *ring = (struct xdp_umem_ring *)q->ring;
		unsigned int idx = q->cons_tail & q->ring_mask;

		*addr = READ_ONCE(ring->desc[idx]) & q->umem_props.chunk_mask;
		if (xskq_is_valid_addr(q, *addr))
			return addr;

		q->cons_tail++;
	}

	return NULL;
}

static inline u64 *xskq_peek_addr(struct xsk_queue *q, u64 *addr)
{
	if (q->cons_tail == q->cons_head) {
		WRITE_ONCE(q->ring->consumer, q->cons_tail);
		q->cons_head = q->cons_tail + xskq_nb_avail(q, RX_BATCH_SIZE);

		/* Order consumer and data */
		smp_rmb();
	}

	return xskq_validate_addr(q, addr);
}

static inline void xskq_discard_addr(struct xsk_queue *q)
{
	q->cons_tail++;
}

static inline int xskq_produce_addr(struct xsk_queue *q, u64 addr)
{
	struct xdp_umem_ring *ring = (struct xdp_umem_ring *)q->ring;

	if (xskq_nb_free(q, q->prod_tail, 1) == 0)
		return -ENOSPC;

	ring->desc[q->prod_tail++ & q->ring_mask] = addr;

	/* Order producer and data */
	smp_wmb();

	WRITE_ONCE(q->ring->producer, q->prod_tail);
	return 0;
}

static inline int xskq_produce_addr_lazy(struct xsk_queue *q, u64 addr)
{
	struct xdp_umem_ring *ring = (struct xdp_umem_ring *)q->ring;

	if (xskq_nb_free(q, q->prod_head, LAZY_UPDATE_THRESHOLD) == 0)
		return -ENOSPC;

	ring->desc[q->prod_head++ & q->ring_mask] = addr;
	return 0;
}

static inline void xskq_produce_flush_addr_n(struct xsk_queue *q,
					     u32 nb_entries)
{
	/* Order producer and data */
	smp_wmb();

	q->prod_tail += nb_entries;
	WRITE_ONCE(q->ring->producer, q->prod_tail);
}

static inline int xskq_reserve_addr(struct xsk_queue *q)
{
	if (xskq_nb_free(q, q->prod_head, 1) == 0)
		return -ENOSPC;

	q->prod_head++;
	return 0;
}

/* Rx/Tx queue */

static inline bool xskq_is_valid_desc(struct xsk_queue *q, struct xdp_desc *d)
{
	if (!xskq_is_valid_addr(q, d->addr))
		return false;

	if (((d->addr + d->len) & q->umem_props.chunk_mask) !=
	    (d->addr & q->umem_props.chunk_mask)) {
		q->invalid_descs++;
		return false;
	}

	return true;
}

static inline struct xdp_desc *xskq_validate_desc(struct xsk_queue *q,
						  struct xdp_desc *desc)
{
	while (q->cons_tail != q->cons_head) {
		struct xdp_rxtx_ring *ring = (struct xdp_rxtx_ring *)q->ring;
		unsigned int idx = q->cons_tail & q->ring_mask;

		*desc = READ_ONCE(ring->desc[idx]);
		if (xskq_is_valid_desc(q, desc))
			return desc;

		q->cons_tail++;
	}

	return NULL;
}

static inline struct xdp_desc *xskq_peek_desc(struct xsk_queue *q,
					      struct xdp_desc *desc)
{
	if (q->cons_tail == q->cons_head) {
		WRITE_ONCE(q->ring->consumer, q->cons_tail);
		q->cons_head = q->cons_tail + xskq_nb_avail(q, RX_BATCH_SIZE);

		/* Order consumer and data */
		smp_rmb();
	}

	return xskq_validate_desc(q, desc);
}

static inline void xskq_discard_desc(struct xsk_queue *q)
{
	q->cons_tail++;
}

static inline int xskq_produce_batch_desc(struct xsk_queue *q,
					  u64 addr, u32 len)
{
	struct xdp_rxtx_ring *ring = (struct xdp_rxtx_ring *)q->ring;
	unsigned int idx;

	if (xskq_nb_free(q, q->prod_head, 1) == 0)
		return -ENOSPC;

	idx = (q->prod_head++) & q->ring_mask;
	ring->desc[idx].addr = addr;
	ring->desc[idx].len = len;

	return 0;
}

static inline void xskq_produce_flush_desc(struct xsk_queue *q)
{
	/* Order producer and data */
	smp_wmb();

	q->prod_tail = q->prod_head;
	WRITE_ONCE(q->ring->producer, q->prod_tail);
}

static inline bool xskq_full_desc(struct xsk_queue *q)
{
	/* No barriers needed since data is not accessed */
	return READ_ONCE(q->ring->producer) - READ_ONCE(q->ring->consumer) ==
		q->nentries;
}

static inline bool xskq_empty_desc(struct xsk_queue *q)
{
	/* No barriers needed since data is not accessed */
	return READ_ONCE(q->ring->consumer) == READ_ONCE(q->ring->producer);
}

void xskq_set_umem(struct xsk_queue *q, struct xdp_umem_props *umem_props);
struct xsk_queue *xskq_create(u32 nentries, bool umem_queue);
void xskq_destroy(struct xsk_queue *q_ops);

#endif /* _LINUX_XSK_QUEUE_H */

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2014 Christoph Hellwig.
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM nfsd

#if !defined(_NFSD_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _NFSD_TRACE_H

#include <linux/tracepoint.h>
#include "nfsfh.h"

TRACE_EVENT(nfsd_compound,
	TP_PROTO(const struct svc_rqst *rqst,
		 u32 args_opcnt),
	TP_ARGS(rqst, args_opcnt),
	TP_STRUCT__entry(
		__field(u32, xid)
		__field(u32, args_opcnt)
	),
	TP_fast_assign(
		__entry->xid = be32_to_cpu(rqst->rq_xid);
		__entry->args_opcnt = args_opcnt;
	),
	TP_printk("xid=0x%08x opcnt=%u",
		__entry->xid, __entry->args_opcnt)
)

TRACE_EVENT(nfsd_compound_status,
	TP_PROTO(u32 args_opcnt,
		 u32 resp_opcnt,
		 __be32 status,
		 const char *name),
	TP_ARGS(args_opcnt, resp_opcnt, status, name),
	TP_STRUCT__entry(
		__field(u32, args_opcnt)
		__field(u32, resp_opcnt)
		__field(int, status)
		__string(name, name)
	),
	TP_fast_assign(
		__entry->args_opcnt = args_opcnt;
		__entry->resp_opcnt = resp_opcnt;
		__entry->status = be32_to_cpu(status);
		__assign_str(name, name);
	),
	TP_printk("op=%u/%u %s status=%d",
		__entry->resp_opcnt, __entry->args_opcnt,
		__get_str(name), __entry->status)
)

DECLARE_EVENT_CLASS(nfsd_io_class,
	TP_PROTO(struct svc_rqst *rqstp,
		 struct svc_fh	*fhp,
		 u64		offset,
		 u32		len),
	TP_ARGS(rqstp, fhp, offset, len),
	TP_STRUCT__entry(
		__field(u32, xid)
		__field(u32, fh_hash)
		__field(u64, offset)
		__field(u32, len)
	),
	TP_fast_assign(
		__entry->xid = be32_to_cpu(rqstp->rq_xid);
		__entry->fh_hash = knfsd_fh_hash(&fhp->fh_handle);
		__entry->offset = offset;
		__entry->len = len;
	),
	TP_printk("xid=0x%08x fh_hash=0x%08x offset=%llu len=%u",
		  __entry->xid, __entry->fh_hash,
		  __entry->offset, __entry->len)
)

#define DEFINE_NFSD_IO_EVENT(name)		\
DEFINE_EVENT(nfsd_io_class, nfsd_##name,	\
	TP_PROTO(struct svc_rqst *rqstp,	\
		 struct svc_fh	*fhp,		\
		 u64		offset,		\
		 u32		len),		\
	TP_ARGS(rqstp, fhp, offset, len))

DEFINE_NFSD_IO_EVENT(read_start);
DEFINE_NFSD_IO_EVENT(read_splice);
DEFINE_NFSD_IO_EVENT(read_vector);
DEFINE_NFSD_IO_EVENT(read_io_done);
DEFINE_NFSD_IO_EVENT(read_done);
DEFINE_NFSD_IO_EVENT(write_start);
DEFINE_NFSD_IO_EVENT(write_opened);
DEFINE_NFSD_IO_EVENT(write_io_done);
DEFINE_NFSD_IO_EVENT(write_done);

DECLARE_EVENT_CLASS(nfsd_err_class,
	TP_PROTO(struct svc_rqst *rqstp,
		 struct svc_fh	*fhp,
		 loff_t		offset,
		 int		status),
	TP_ARGS(rqstp, fhp, offset, status),
	TP_STRUCT__entry(
		__field(u32, xid)
		__field(u32, fh_hash)
		__field(loff_t, offset)
		__field(int, status)
	),
	TP_fast_assign(
		__entry->xid = be32_to_cpu(rqstp->rq_xid);
		__entry->fh_hash = knfsd_fh_hash(&fhp->fh_handle);
		__entry->offset = offset;
		__entry->status = status;
	),
	TP_printk("xid=0x%08x fh_hash=0x%08x offset=%lld status=%d",
		  __entry->xid, __entry->fh_hash,
		  __entry->offset, __entry->status)
)

#define DEFINE_NFSD_ERR_EVENT(name)		\
DEFINE_EVENT(nfsd_err_class, nfsd_##name,	\
	TP_PROTO(struct svc_rqst *rqstp,	\
		 struct svc_fh	*fhp,		\
		 loff_t		offset,		\
		 int		len),		\
	TP_ARGS(rqstp, fhp, offset, len))

DEFINE_NFSD_ERR_EVENT(read_err);
DEFINE_NFSD_ERR_EVENT(write_err);

#include "state.h"

DECLARE_EVENT_CLASS(nfsd_stateid_class,
	TP_PROTO(stateid_t *stp),
	TP_ARGS(stp),
	TP_STRUCT__entry(
		__field(u32, cl_boot)
		__field(u32, cl_id)
		__field(u32, si_id)
		__field(u32, si_generation)
	),
	TP_fast_assign(
		__entry->cl_boot = stp->si_opaque.so_clid.cl_boot;
		__entry->cl_id = stp->si_opaque.so_clid.cl_id;
		__entry->si_id = stp->si_opaque.so_id;
		__entry->si_generation = stp->si_generation;
	),
	TP_printk("client %08x:%08x stateid %08x:%08x",
		__entry->cl_boot,
		__entry->cl_id,
		__entry->si_id,
		__entry->si_generation)
)

#define DEFINE_STATEID_EVENT(name) \
DEFINE_EVENT(nfsd_stateid_class, nfsd_##name, \
	TP_PROTO(stateid_t *stp), \
	TP_ARGS(stp))
DEFINE_STATEID_EVENT(layoutstate_alloc);
DEFINE_STATEID_EVENT(layoutstate_unhash);
DEFINE_STATEID_EVENT(layoutstate_free);
DEFINE_STATEID_EVENT(layout_get_lookup_fail);
DEFINE_STATEID_EVENT(layout_commit_lookup_fail);
DEFINE_STATEID_EVENT(layout_return_lookup_fail);
DEFINE_STATEID_EVENT(layout_recall);
DEFINE_STATEID_EVENT(layout_recall_done);
DEFINE_STATEID_EVENT(layout_recall_fail);
DEFINE_STATEID_EVENT(layout_recall_release);

#endif /* _NFSD_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE trace
#include <trace/define_trace.h>

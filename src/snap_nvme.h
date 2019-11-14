/*
 * Copyright (c) 2012 Mellanox Technologies, Inc.  All rights reserved.
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

#ifndef SNAP_NVME_H
#define SNAP_NVME_H

#include <stdlib.h>

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <pthread.h>
#include <linux/types.h>

#include "snap.h"
#include "mlx5_ifc.h"

struct snap_nvme_device;
struct snap_nvme_cq;

struct snap_nvme_namespace_attr {
	int			src_nsid;
	int			dst_nsid;
	int			lba_size;
	int			md_size;
};

struct snap_nvme_namespace {
	struct mlx5_snap_devx_obj		*ns;
	int					src_id;
	int					dst_id;
	TAILQ_ENTRY(snap_nvme_namespace)	entry;
};

enum snap_nvme_queue_type {
	SNAP_NVME_SQE_MODE	= 1 << 0,
	SNAP_NVME_CC_MODE	= 1 << 1,
};

struct snap_nvme_sq_attr {
	enum snap_nvme_queue_type	type;
	uint32_t			id;
	uint32_t			doorbell_offset;
	uint16_t			queue_depth;
	uint64_t			base_addr;
	struct snap_nvme_cq		*cq;
};

struct snap_nvme_sq {
	uint32_t				id;
	struct mlx5_snap_devx_obj		*sq;
};

struct snap_nvme_cq_attr {
	enum snap_nvme_queue_type	type;
	uint32_t			id;
	uint32_t			doorbell_offset;
	uint16_t			msix;
	uint16_t			queue_depth;
	uint64_t			base_addr;
	uint16_t			cq_period;
	uint16_t			cq_max_count;
};

struct snap_nvme_cq {
	uint32_t				id;
	struct mlx5_snap_devx_obj		*cq;
};

struct snap_nvme_device {
	struct snap_device			*sdev;
	uint32_t				num_queues;

	pthread_mutex_t				lock;
	TAILQ_HEAD(, snap_nvme_namespace)	ns_list;

	struct snap_nvme_cq			*cqs;
	struct snap_nvme_sq			*sqs;
};

int snap_nvme_init_device(struct snap_device *sdev);
int snap_nvme_teardown_device(struct snap_device *sdev);
struct snap_nvme_namespace*
snap_nvme_create_namespace(struct snap_device *sdev,
		struct snap_nvme_namespace_attr *attr);
int snap_nvme_destroy_namespace(struct snap_nvme_namespace *ns);
struct snap_nvme_cq*
snap_nvme_create_cq(struct snap_device *sdev, struct snap_nvme_cq_attr *attr);
int snap_nvme_destroy_cq(struct snap_nvme_cq *cq);
struct snap_nvme_sq*
snap_nvme_create_sq(struct snap_device *sdev, struct snap_nvme_sq_attr *attr);
int snap_nvme_destroy_sq(struct snap_nvme_sq *sq);

#endif
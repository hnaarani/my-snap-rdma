/*
 * Copyright (c) 2019 Mellanox Technologies, Inc.  All rights reserved.
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

#ifndef SNAP_VIRTIO_COMMON_H
#define SNAP_VIRTIO_COMMON_H

#include <stdlib.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <pthread.h>
#include <linux/types.h>

enum snap_virtq_type {
	SNAP_VIRTQ_SPLIT_MODE	= 1 << 0,
	SNAP_VIRTQ_PACKED_MODE	= 1 << 1,
};

enum snap_virtq_event_mode {
	SNAP_VIRTQ_NO_MSIX_MODE	= 1 << 0,
	SNAP_VIRTQ_QP_MODE	= 1 << 1,
	SNAP_VIRTQ_MSIX_MODE	= 1 << 2,
};

struct snap_virtio_queue_attr {
	enum snap_virtq_type		type;
	enum snap_virtq_event_mode	ev_mode;
	uint16_t			idx;
	uint16_t			size;
	uint16_t			msix_vector;
	uint16_t			enable;
	uint16_t			notify_off;
	uint64_t			desc;
	uint64_t			driver;
	uint64_t			device;
};

struct snap_virtio_queue {
	uint32_t				idx;
	struct mlx5_snap_devx_obj		*virtq;
};

struct snap_virtio_device_attr {
	uint64_t			device_feature;
	uint64_t			driver_feature;
	uint16_t			msix_config;
	uint8_t				status;
	bool				enabled;
};

struct snap_virtio_device {
	struct snap_device			*sdev;
	uint32_t				num_queues;
};

void snap_virtio_get_queue_attr(struct snap_virtio_queue_attr *vattr,
	void *q_configuration);
void snap_virtio_get_device_attr(struct snap_virtio_device_attr *vattr,
	void *device_configuration);
int snap_virtio_query_device(struct snap_device *sdev,
	enum snap_emulation_type type, uint8_t *out, int outlen);

#endif

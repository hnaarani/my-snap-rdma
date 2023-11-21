/*
 * Copyright © 2021 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of Nvidia Corporation and its affiliates
 * (the "Company") and all right, title, and interest in and to the software
 * product, including all associated intellectual property rights, are and
 * shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#include "snap_macros.h"
#include "snap_virtio_fs.h"
#include "snap_internal.h"
#include "mlx5_ifc.h"
#include "snap_lib_log.h"

SNAP_LIB_LOG_REGISTER(VIRTIO_FS)

/**
 * snap_virtio_fs_query_device() - Query an Virtio fs snap device
 * @sdev:       snap device
 * @attr:       Virtio fs snap device attr container (output)
 *
 * Query a Virtio fs snap device. Attr argument must have enough space for
 * the output data.
 *
 * Return: Returns 0 in case of success and attr is filled.
 */
int snap_virtio_fs_query_device(struct snap_device *sdev,
	struct snap_virtio_fs_device_attr *attr)
{
	uint8_t *out;
	struct snap_context *sctx = sdev->sctx;
	uint8_t *device_emulation_out;
	uint8_t *tag;
	int i, ret, out_size;
	uint64_t dev_allowed;

	if (attr->queues > sctx->virtio_fs_caps.max_emulated_virtqs)
		return -EINVAL;

	out_size = DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr) +
		   DEVX_ST_SZ_BYTES(virtio_fs_device_emulation) +
		   attr->queues * DEVX_ST_SZ_BYTES(virtio_q_layout);
	out = calloc(1, out_size);
	if (!out)
		return -ENOMEM;

	ret = snap_virtio_query_device(sdev, SNAP_VIRTIO_FS, out, out_size);
	if (ret)
		goto out_free;

	device_emulation_out = out + DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr);

	snap_get_pci_attr(&sdev->pci->pci_attr,
			  DEVX_ADDR_OF(virtio_fs_device_emulation,
				       device_emulation_out,
				       pci_params));

	attr->vattr.num_of_vfs = sdev->pci->pci_attr.num_of_vfs;
	snap_virtio_get_device_attr(sdev, &attr->vattr,
				    DEVX_ADDR_OF(virtio_fs_device_emulation,
						 device_emulation_out,
						 virtio_device));

	if (attr->queues) {
		for (i = 0; i < attr->queues; i++)
			snap_virtio_get_queue_attr(&attr->q_attrs[i].vattr,
						   DEVX_ADDR_OF(virtio_fs_device_emulation,
								device_emulation_out,
								virtio_q_configuration[i]));
	}

	snap_update_pci_bdf(sdev->pci, attr->vattr.pci_bdf);

	tag = DEVX_ADDR_OF(virtio_fs_device_emulation, device_emulation_out,
				virtio_fs_config.tag);
	memcpy(attr->tag, tag, SNAP_VIRTIO_FS_DEV_CFG_TAG_LEN);

	attr->num_request_queues = DEVX_GET(virtio_fs_device_emulation,
					    device_emulation_out,
					    virtio_fs_config.num_request_queues);

	attr->crossed_vhca_mkey = DEVX_GET(virtio_fs_device_emulation,
					   device_emulation_out,
					   emulated_device_crossed_vhca_mkey);

	attr->vattr.enabled = DEVX_GET(virtio_fs_device_emulation,
				       device_emulation_out, enabled);
	attr->vattr.reset = DEVX_GET(virtio_fs_device_emulation,
				     device_emulation_out, reset);
	attr->modifiable_fields = 0;
	dev_allowed = DEVX_GET64(virtio_fs_device_emulation,
				 device_emulation_out, modify_field_select);
	if (dev_allowed) {
		if (dev_allowed & MLX5_VIRTIO_DEVICE_MODIFY_STATUS)
			attr->modifiable_fields |= SNAP_VIRTIO_MOD_DEV_STATUS;
		if (dev_allowed & MLX5_VIRTIO_DEVICE_MODIFY_RESET)
			attr->modifiable_fields |= SNAP_VIRTIO_MOD_RESET;
		if (dev_allowed & MLX5_VIRTIO_DEVICE_MODIFY_PCI_COMMON_CFG)
			attr->modifiable_fields |= SNAP_VIRTIO_MOD_PCI_COMMON_CFG;
		if (dev_allowed & MLX5_VIRTIO_DEVICE_MODIFY_DEV_CFG)
			attr->modifiable_fields |= SNAP_VIRTIO_MOD_DEV_CFG;
		if (dev_allowed & MLX5_VIRTIO_DEVICE_MODIFY_ALL)
			attr->modifiable_fields |= SNAP_VIRTIO_MOD_ALL;
		if (dev_allowed & MLX5_VIRTIO_DEVICE_MODIFY_QUEUE_CFG)
			attr->modifiable_fields |= SNAP_VIRTIO_MOD_QUEUE_CFG;
	}

out_free:
	free(out);
	return ret;
}

static int
snap_virtio_fs_get_modifiable_device_fields(struct snap_device *sdev)
{
	struct snap_virtio_fs_device_attr attr = {};
	int ret;

	ret = snap_virtio_fs_query_device(sdev, &attr);
	if (ret)
		return ret;

	sdev->mod_allowed_mask = attr.modifiable_fields;

	return 0;
}

/**
 * snap_virtio_fs_modify_device() - Modify Virtio fs snap device
 * @sdev:       snap device
 * @mask:       selected params to modify (mask of enum snap_virtio_dev_modify)
 * @attr:       attributes for the fs device modify
 *
 * Modify Virtio fs snap device object according to a given mask.
 *
 * Return: 0 on success.
 */
int snap_virtio_fs_modify_device(struct snap_device *sdev, uint64_t mask,
		struct snap_virtio_fs_device_attr *attr)
{
	int ret;

	if (!sdev->mod_allowed_mask) {
		ret = snap_virtio_fs_get_modifiable_device_fields(sdev);
		if (ret)
			return ret;
	}

	return snap_virtio_modify_device(sdev, SNAP_VIRTIO_FS, mask,
					 &attr->vattr);
}

/**
 * snap_virtio_fs_init_device() - Initialize a new snap device with VIRTIO
 *                                 fs characteristics
 * @sdev:       snap device
 *
 * Initialize a snap device for Virtio fs emulation. Allocate the needed
 * resources in the HCA and setup internal context.
 *
 * Return: Returns 0 in case of success.
 */
int snap_virtio_fs_init_device(struct snap_device *sdev)
{
	struct snap_virtio_fs_device *vbdev;
	int ret, i;

	if (sdev->pci->type != SNAP_VIRTIO_FS_PF &&
	    sdev->pci->type != SNAP_VIRTIO_FS_VF)
		return -EINVAL;

	vbdev = calloc(1, sizeof(*vbdev));
	if (!vbdev)
		return -ENOMEM;

	vbdev->num_queues = sdev->sctx->virtio_fs_caps.max_emulated_virtqs;

	vbdev->virtqs = calloc(vbdev->num_queues, sizeof(*vbdev->virtqs));
	if (!vbdev->virtqs) {
		ret = -ENOMEM;
		goto out_free;
	}

	for (i = 0; i < vbdev->num_queues; i++) {
		vbdev->virtqs[i].vbdev = vbdev;
		vbdev->virtqs[i].virtq.ctrs_obj = snap_virtio_create_queue_counters(sdev);
		if (!vbdev->virtqs[i].virtq.ctrs_obj) {
			ret = -ENODEV;
			goto out_destroy_counters;
		}
	}

	ret = snap_init_device(sdev);
	if (ret)
		goto out_destroy_counters;

	sdev->dd_data = vbdev;

	return 0;

out_destroy_counters:
	for (--i; i >= 0; i--)
		snap_devx_obj_destroy(vbdev->virtqs[i].virtq.ctrs_obj);
	free(vbdev->virtqs);
out_free:
	free(vbdev);
	return ret;
}

/**
 * snap_virtio_fs_teardown_device() - Teardown Virtio fs specifics from a
 *                                     snap device
 * @sdev:       snap device
 *
 * Teardown and free Virtio fs context from a snap device.
 *
 * Return: Returns 0 in case of success.
 */
int snap_virtio_fs_teardown_device(struct snap_device *sdev)
{
	struct snap_virtio_fs_device *vbdev;
	int i, ret = 0;

	vbdev = (struct snap_virtio_fs_device *)sdev->dd_data;
	if (sdev->pci->type != SNAP_VIRTIO_FS_PF &&
	    sdev->pci->type != SNAP_VIRTIO_FS_VF)
		return -EINVAL;

	sdev->dd_data = NULL;

	ret = snap_teardown_device(sdev);

	for (i = 0; i < vbdev->num_queues; i++)
		snap_devx_obj_destroy(vbdev->virtqs[i].virtq.ctrs_obj);

	free(vbdev->virtqs);
	free(vbdev);

	return ret;
}

static struct virtq_q_ops snap_virtq_fs_hw_ops = {
	.create = snap_virtio_fs_create_queue,
	.destroy = snap_virtio_fs_destroy_queue,
	.query = snap_virtio_fs_query_queue,
	.modify = snap_virtio_fs_modify_queue,
};

/**
 * snap_virtio_fs_query_queue() - Query a Virtio fs queue object
 * @vfsq:       snap Virtio fs queue
 * @attr:       attributes for the queue query (output)
 *
 * Query a Virtio fs snap queue object.
 *
 * Return: 0 on success, and attr is filled with the query result.
 */
int snap_virtio_fs_query_queue(struct snap_virtio_queue *vq,
			       struct snap_virtio_common_queue_attr *attr)
{
	return snap_virtio_query_queue(vq, &attr->vattr);
}

/**
 * snap_virtio_fs_create_queue() - Create a new Virtio fs snap queue object
 * @sdev:       snap device
 * @attr:       attributes for the queue creation
 *
 * Create a Virtio fs snap queue object with the given attributes.
 *
 * Return: Returns snap_virtio_fs_queue in case of success, NULL otherwise and
 * errno will be set to indicate the failure reason.
 */
struct snap_virtio_queue*
snap_virtio_fs_create_queue(struct snap_device *sdev,
	struct snap_virtio_common_queue_attr *attr)
{
	struct snap_virtio_fs_device *vbdev;
	struct snap_virtio_fs_queue *vfsq;
	int ret;

	vbdev = (struct snap_virtio_fs_device *)sdev->dd_data;

	if (attr->vattr.idx >= vbdev->num_queues) {
		errno = EINVAL;
		goto out;
	}

	vfsq = &vbdev->virtqs[attr->vattr.idx];

	ret = snap_virtio_create_hw_queue(sdev, &vfsq->virtq,
					&sdev->sctx->virtio_fs_caps,
					&attr->vattr);
	if (ret) {
		SNAP_LIB_LOG_ERR("Failed to create hw queue, err(%d)", ret);
		return NULL;
	}

	vfsq->virtq.q_ops = &snap_virtq_fs_hw_ops;
	return &vfsq->virtq;

out:
	return NULL;
}

/**
 * snap_virtio_fs_destroy_queue() - Destroy Virtio fs queue object
 * @vfsq:       Virtio fs queue
 *
 * Destroy and free a snap virtio fs queue context.
 *
 * Return: Returns 0 on success.
 */
int snap_virtio_fs_destroy_queue(struct snap_virtio_queue *vq)
{
	return snap_virtio_destroy_hw_queue(vq);
}

static int
snap_virtio_fs_get_modifiable_virtq_fields(struct snap_virtio_fs_queue *vfsq)
{
	struct snap_virtio_common_queue_attr attr = {};
	int ret;

	ret = snap_virtio_fs_query_queue(&vfsq->virtq, &attr);
	if (ret)
		return ret;

	vfsq->virtq.mod_allowed_mask = attr.modifiable_fields;

	return 0;
}

/**
 * snap_virtio_fs_modify_queue() - Modify a Virtio blk queue object
 * @vfsq:        snap Virtio blk queue
 * @mask:       selected params to modify (mask of enum
 *              snap_virtio_fs_queue_modify)
 * @attr:       attributes for the virtq modify
 *
 * Modify a Virtio blk queue snap object according to a given mask.
 *
 * Return: 0 on success.
 */
int snap_virtio_fs_modify_queue(struct snap_virtio_queue *vq,
		uint64_t mask, struct snap_virtio_common_queue_attr *attr)
{
	int ret;

	if (!vq->mod_allowed_mask) {
		ret = snap_virtio_fs_get_modifiable_virtq_fields(to_fs_queue(vq));
		if (ret)
			return ret;
	}

	return snap_virtio_modify_queue(vq, mask, &attr->vattr);
}

/**
 * snap_virtio_fs_pci_functions_cleanup() - Remove remaining hot-unplugged virtio_fs functions
 * @sctx:       snap_context for virtio_fs pfs
 *
 * Go over virtio_fs pfs and check their hotunplug state.
 * Complete hot-unplug for any pf with state POWER_OFF or HOTUNPLUG_PREPARE.
 *
 * Return: void.
 */
void snap_virtio_fs_pci_functions_cleanup(struct snap_context *sctx)
{
	struct snap_pci **pfs;
	int num_pfs, i;
	struct snap_virtio_fs_device_attr attr = {};
	struct snap_device_attr sdev_attr = {};
	struct snap_device *sdev;

	if (sctx->virtio_fs_pfs.max_pfs <= 0)
		return;

	pfs = calloc(sctx->virtio_fs_pfs.max_pfs, sizeof(*pfs));
	if (!pfs)
		return;

	sdev = calloc(1, sizeof(*sdev));
	if (!sdev) {
		free(pfs);
		return;
	}

	num_pfs = snap_get_pf_list(sctx, SNAP_VIRTIO_FS, pfs);
	for (i = 0; i < num_pfs; i++) {
		if (!pfs[i]->hotplugged)
			continue;
		sdev->sctx = sctx;
		sdev->pci = pfs[i];
		sdev->mdev.device_emulation = snap_emulation_device_create(sdev, &sdev_attr);
		if (!sdev->mdev.device_emulation) {
			SNAP_LIB_LOG_ERR("Failed to create device emulation");
			goto err;
		}

		snap_virtio_fs_query_device(sdev, &attr);
		snap_emulation_device_destroy(sdev);
		/*
		 * We rely on the driver to clean itself up.
		 * If the state is POWER OFF or PREPARE we need to unplug the function.
		 */
		if (attr.vattr.pci_hotplug_state == MLX5_EMULATION_HOTPLUG_STATE_POWER_OFF ||
			attr.vattr.pci_hotplug_state == MLX5_EMULATION_HOTPLUG_STATE_HOTUNPLUG_PREPARE)
			snap_hotunplug_pf(pfs[i]);

		SNAP_LIB_LOG_DBG("hotplug virtio fs function pf id =%d bdf=%02x:%02x.%d with state %d.",
			  pfs[i]->id, pfs[i]->pci_bdf.bdf.bus, pfs[i]->pci_bdf.bdf.device,
			  pfs[i]->pci_bdf.bdf.function, attr.vattr.pci_hotplug_state);
	}

err:
	free(pfs);
	free(sdev);
}

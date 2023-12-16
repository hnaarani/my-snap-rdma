
#ifndef __SNAP_CROSS_GVMI__
#define __SNAP_CROSS_GVMI__

#include "snap_dpa.h"


#if HAVE_FLEXIO
#include <libflexio/flexio.h>
#if HAVE_FLEXIO_SUBPROJECT
#include <libflexio/src/flexio_exp.h>
#else
#include <flexio_exp.h>
#endif
#endif



#define SNAP_ACCESS_KEY_LENGTH DEVX_FLD_SZ_BYTES(allow_other_vhca_access_in, access_key)

struct snap_alias_object {
	struct mlx5dv_devx_obj *obj;
	uint32_t obj_id;

	struct ibv_context *src_context;
	struct ibv_context *dst_context;
	uint32_t dst_obj_id;
	uint8_t access_key[SNAP_ACCESS_KEY_LENGTH];
};

uint16_t snap_get_dev_vhca_id(struct ibv_context *context);

int snap_allow_other_vhca_access(struct ibv_context *context,
				 enum mlx5_obj_type obj_type,
                 enum cross_vhca_object_support_bit cross_type,
				 uint32_t obj_id,
				 uint8_t access_key[SNAP_ACCESS_KEY_LENGTH]);

struct snap_alias_object *
snap_create_alias_object(struct ibv_context *src_context,
			 enum mlx5_obj_type obj_type,
             enum cross_vhca_object_support_bit cross_type,
			 struct ibv_context *dst_context,
			 uint32_t dst_obj_id,
			 uint8_t access_key[SNAP_ACCESS_KEY_LENGTH]);
void snap_destroy_alias_object(struct snap_alias_object *obj);

int snap_check_create_alias_uar(struct snap_dpa_ctx *dpa_ctx, struct ibv_context *sf_ctx, uint32_t *uar_id);
int snap_check_create_alias_thread(struct snap_dpa_ctx *dpa_ctx, struct ibv_context *sf_ctx, struct snap_dpa_thread *thread, uint32_t *thread_id);
int snap_check_create_alias_dumem(struct snap_dpa_ctx *dpa_ctx, struct ibv_context *sf_ctx, uint32_t *umem_id);

#endif
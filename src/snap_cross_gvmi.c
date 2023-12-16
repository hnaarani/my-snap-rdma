#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include "config.h"

#include "mlx5_ifc.h"
#include "snap_lib_log.h"

#include "snap_cross_gvmi.h"

SNAP_LIB_LOG_REGISTER(DPA)


/**
 * snap_get_dev_vhca_id() - Return the vhca id for a given device.
 * @context:	Device context.
 *
 * Return: Returns vhca id for a given context on success and -1 otherwise.
 */
uint16_t snap_get_dev_vhca_id(struct ibv_context *context)
{
	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {0};
	int ret;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE);

	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in), out,
				      sizeof(out));
	if (ret)
		return -1;

	return DEVX_GET(query_hca_cap_out, out,
			capability.cmd_hca_cap.vhca_id);
}


static bool
snap_allow_other_vhca_access_is_supported(struct ibv_context *context,
					  enum mlx5_obj_type obj_type, 
					  enum cross_vhca_object_support_bit cross_type)
{
	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {};
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {};
	uint64_t allowed_obj_types_mask;
	int ret;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE2);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in),
				      out, sizeof(out));
	if (ret)
		return false;

	allowed_obj_types_mask = DEVX_GET64(query_hca_cap_out, out,
	       capability.cmd_hca_cap2.allowed_object_for_other_vhca_access);
	if (!!(cross_type & allowed_obj_types_mask))
		return true;

	return false;
}

int snap_allow_other_vhca_access(struct ibv_context *context,
				 enum mlx5_obj_type obj_type,
				 enum cross_vhca_object_support_bit cross_type,
				 uint32_t obj_id,
				 uint8_t access_key[SNAP_ACCESS_KEY_LENGTH])
{
	int ret;
	uint8_t in[DEVX_ST_SZ_BYTES(allow_other_vhca_access_in)] = {};
	uint8_t out[DEVX_ST_SZ_BYTES(allow_other_vhca_access_out)] = {};

	if (!snap_allow_other_vhca_access_is_supported(context, obj_type, cross_type)) {
		SNAP_LIB_LOG_ERR("snap_allow_other_vhca_access_is_supported not supported\n");
		return -ENOTSUP;
	}

	DEVX_SET(allow_other_vhca_access_in, in, opcode,
		 MLX5_CMD_OP_ALLOW_OTHER_VHCA_ACCESS);
	DEVX_SET(allow_other_vhca_access_in, in,
		 object_type_to_be_accessed, obj_type);
	DEVX_SET(allow_other_vhca_access_in, in,
		 object_id_to_be_accessed, obj_id);
	if (access_key) {
		memcpy(DEVX_ADDR_OF(allow_other_vhca_access_in,
				    in, access_key),
		       access_key,
		       DEVX_FLD_SZ_BYTES(allow_other_vhca_access_in,
					 access_key));
	}
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in),
				      out, sizeof(out));
	if (ret) {
		SNAP_LIB_LOG_ERR("Failed to allow other vhca access %x", obj_type);
		return ret;
	}

	SNAP_LIB_LOG_DBG("Other VHCA access is allowed for object 0x%x", obj_id);
	return 0;
}

// static bool
// snap_cross_vhca_object_is_supported(struct ibv_context *context,
// 				    enum cross_vhca_object_support_bit cross_type)
// {
// 	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {};
// 	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {};
// 	uint32_t supported_obj_types_mask;
// 	int ret;

// 	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
// 	DEVX_SET(query_hca_cap_in, in, op_mod,
// 		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE2);
// 	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in),
// 				      out, sizeof(out));
// 	if (ret)
// 		return false;

// 	supported_obj_types_mask = DEVX_GET(query_hca_cap_out, out,
// 	       capability.cmd_hca_cap2.cross_vhca_object_to_object_supported);
// 	if (!(supported_obj_types_mask & cross_type))
// 		return false;

// 	return true;
// }

struct snap_alias_object *
snap_create_alias_object(struct ibv_context *src_context,
			 enum mlx5_obj_type obj_type,
			 enum cross_vhca_object_support_bit cross_type,
			 struct ibv_context *dst_context,
			 uint32_t dst_obj_id,
			 uint8_t access_key[SNAP_ACCESS_KEY_LENGTH])
{
	struct snap_alias_object *alias;
	uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr) +
		   DEVX_ST_SZ_BYTES(alias_context)] = {};
	uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)] = {};
	uint8_t *alias_in = in + DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr);

	// if (!snap_cross_vhca_object_is_supported(src_context,
	// 		 cross_type)) {
	// 	printf("not snap_cross_vhca_object_is_supported\n");
	// 	errno = ENOTSUP;
	// 	goto err;
	// }

	alias = calloc(1, sizeof(*alias));
	if (!alias) {
		errno = ENOMEM;
		goto err;
	}

	DEVX_SET(general_obj_in_cmd_hdr, in, opcode,
		 MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type, obj_type);
	DEVX_SET(general_obj_in_cmd_hdr, in, alias_object, 1);
	DEVX_SET(alias_context, alias_in, vhca_id_to_be_accessed,
		 snap_get_dev_vhca_id(dst_context));
	DEVX_SET(alias_context, alias_in, object_id_to_be_accessed,
		 dst_obj_id);
	if (access_key)
		memcpy(DEVX_ADDR_OF(alias_context, alias_in, access_key),
		       access_key,
		       DEVX_FLD_SZ_BYTES(alias_context, access_key));
	alias->obj = mlx5dv_devx_obj_create(src_context, in, sizeof(in),
					    out, sizeof(out));
	if (!alias->obj)
		goto free_alias;

	alias->obj_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);
	alias->src_context = src_context;
	alias->dst_context = dst_context;
	alias->dst_obj_id = dst_obj_id;
	if (access_key)
		memcpy(alias->access_key, access_key, SNAP_ACCESS_KEY_LENGTH);

	return alias;

free_alias:
	free(alias);
err:
	return NULL;
}

void snap_destroy_alias_object(struct snap_alias_object *alias)
{
	mlx5dv_devx_obj_destroy(alias->obj);
	free(alias);
}



/*--------------------------*/

#define ALIAS_ACCESS_KEY_BUF_SIZE (ALIAS_ACCESS_KEY_NUM_DWORD/*8*/ * sizeof(uint32_t))
#include <sys/random.h>

static int generate_alias_access_key(unsigned int seed, uint32_t *buf, size_t len)
{
    ssize_t result;

	srand(seed);
    result = getrandom(buf, len, 0);
    if (result < 0) {
        SNAP_LIB_LOG_ERR("getrandom() failed with error %#x\n", errno);
        return -1;
    }

    return 0;
}


static struct snap_alias_object * snap_create_alias_obj_wrap(struct snap_dpa_ctx *dpa_ctx, struct ibv_context *sf_ctx, 
								struct flexio_aliasable_obj *aliasable_obj,enum mlx5_obj_type obj_type,
				 				enum cross_vhca_object_support_bit cross_type)  {
	
	int rv = -1; 
	struct snap_alias_object *alias_obj = NULL;
	if (snap_get_dev_vhca_id(dpa_ctx->pd->context) !=
	    snap_get_dev_vhca_id(sf_ctx)) { 

		uint8_t access_key[SNAP_ACCESS_KEY_LENGTH] = {0};
		uint32_t access_key_be[ALIAS_ACCESS_KEY_NUM_DWORD] = {0};

		if (!aliasable_obj->is_allowed) { 

			generate_alias_access_key(aliasable_obj->id, aliasable_obj->access_key,
							SNAP_ACCESS_KEY_LENGTH);
			
			for(int i=0; i<ALIAS_ACCESS_KEY_NUM_DWORD; i++) {
				access_key_be[i] = htobe32(aliasable_obj->access_key[i]);
			}
			memcpy(access_key, access_key_be, SNAP_ACCESS_KEY_LENGTH);
			rv = snap_allow_other_vhca_access(dpa_ctx->pd->context,
					obj_type,
					cross_type,
					aliasable_obj->id,
					access_key);
			if (rv) { 
				SNAP_LIB_LOG_ERR("Failed to allow cross vhca access");
				goto out;
			}
			aliasable_obj->is_allowed = 1;
		}

		for(int i=0; i<ALIAS_ACCESS_KEY_NUM_DWORD; i++) {
			access_key_be[i] = htobe32(aliasable_obj->access_key[i]);
		}
		memcpy(access_key, access_key_be, SNAP_ACCESS_KEY_LENGTH);
		alias_obj = snap_create_alias_object(sf_ctx,
								obj_type,
								cross_type,
								dpa_ctx->pd->context,					
								aliasable_obj->id,
								access_key);

		if (!alias_obj) {
			SNAP_LIB_LOG_ERR("Failed to create alias");
			goto out;
		}

	}

out: 
	return alias_obj;
}

int snap_check_create_alias_dumem(struct snap_dpa_ctx *dpa_ctx, struct ibv_context *sf_ctx, uint32_t *umem_id) {
	
	int rv = -1; 
	if (snap_get_dev_vhca_id(dpa_ctx->pd->context) !=
	    snap_get_dev_vhca_id(sf_ctx)) { 
		struct snap_alias_object *alias_obj = snap_create_alias_obj_wrap(dpa_ctx, sf_ctx, &dpa_ctx->dpa_proc->dumem, 
									MLX5_OBJ_TYPE_DPA_DUMEM, CROSS_VHCA_OBJ_SUPPORT_UMEM);
		if(!alias_obj) {
			SNAP_LIB_LOG_ERR("Failed to create dumem alias\n");
			goto out;
		}
		*umem_id = alias_obj->obj_id;
	}
	rv = 0;

out: 
	return rv;
}

int snap_check_create_alias_thread(struct snap_dpa_ctx *dpa_ctx, struct ibv_context *sf_ctx, struct snap_dpa_thread *thread, uint32_t *thread_id) {
	int rv = -1; 
	if (snap_get_dev_vhca_id(dpa_ctx->pd->context) !=
	    snap_get_dev_vhca_id(sf_ctx)) { 

		struct snap_alias_object *alias_obj = snap_create_alias_obj_wrap(dpa_ctx, sf_ctx, &thread->dpa_thread->thread->aliasable, 
									MLX5_OBJ_TYPE_DPA_THREAD, CROSS_VHCA_OBJ_SUPPORT_DPA_THREAD);
		if(!alias_obj) {
			SNAP_LIB_LOG_ERR("Failed to create thread alias\n");
			goto out;
		}
		*thread_id = alias_obj->obj_id;
	
	}
	rv = 0;

out: 
	return rv;
}

int snap_check_create_alias_uar(struct snap_dpa_ctx *dpa_ctx, struct ibv_context *sf_ctx, uint32_t *uar_id) {

	int rv = -1; 
	if (snap_get_dev_vhca_id(dpa_ctx->pd->context) !=
	    snap_get_dev_vhca_id(sf_ctx)) {
		struct flexio_uar *alias_uar;
		if (flexio_uar_extend(dpa_ctx->flexio_uar, sf_ctx, &alias_uar)) { 
			SNAP_LIB_LOG_ERR("Failed to extend uar \n");
			goto out;
		}
		*uar_id = flexio_uar_get_id(alias_uar);
	}
	rv = 0;
out: 
	return rv;
}
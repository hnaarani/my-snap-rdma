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

#include "snap.h"
#include "mlx5_ifc.h"
#include "snap_lib_log.h"

SNAP_LIB_LOG_REGISTER(CRYPTO)

int snap_query_crypto_caps(struct snap_context *sctx)
{
	int ret;
	uint8_t in[DEVX_ST_SZ_BYTES(query_hca_cap_in)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(query_hca_cap_out)] = {0};
	struct ibv_context *context = sctx->context;

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_SET_HCA_CAP_OP_MOD_GENERAL_DEVICE);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in),
				      out, sizeof(out));
	if (ret) {
		SNAP_LIB_LOG_ERR("Query hca_cap failed, ret:%d", ret);
		return ret;
	}

	sctx->crypto.hca_crypto = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.crypto);
	sctx->crypto.hca_aes_xts = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.aes_xts);
	if (sctx->crypto.hca_crypto == 0)
		goto out;

	memset(in, 0, DEVX_ST_SZ_BYTES(query_hca_cap_in));
	memset(out, 0, DEVX_ST_SZ_BYTES(query_hca_cap_out));

	DEVX_SET(query_hca_cap_in, in, opcode, MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod, MLX5_SET_HCA_CAP_OP_MOD_CRYPTO);
	ret = mlx5dv_devx_general_cmd(context, in, sizeof(in),
				      out, sizeof(out));
	if (ret) {
		SNAP_LIB_LOG_ERR("Query crypto_cap failed, ret:%d", ret);
		return ret;
	}

	sctx->crypto.wrapped_crypto_operational = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_cap.wrapped_crypto_operational);
	sctx->crypto.wrapped_crypto_going_to_commissioning = DEVX_GET(query_hca_cap_out,
			out, capability.crypto_cap.wrapped_crypto_going_to_commissioning);
	sctx->crypto.wrapped_import_method = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_cap.wrapped_import_method);
	sctx->crypto.log_max_num_deks = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_cap.log_max_num_deks);
	sctx->crypto.log_max_num_import_keks = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_cap.log_max_num_import_keks);
	sctx->crypto.log_max_num_creds = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_cap.log_max_num_creds);
	sctx->crypto.failed_selftests = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_cap.failed_selftests);
	sctx->crypto.num_nv_import_keks = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_cap.num_nv_import_keks);
	sctx->crypto.num_nv_credentials = DEVX_GET(query_hca_cap_out, out,
			capability.crypto_cap.num_nv_credentials);

out:
	return 0;
}

struct snap_crypto_obj*
snap_create_dek_obj(struct ibv_context *context,
			struct snap_dek_attr *attr)
{
	uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr) +
		DEVX_ST_SZ_BYTES(dek)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)] = {0};
	uint8_t *dek_in;
	struct snap_crypto_obj *dek;
	void *dek_key;

	if ((attr->key_size != SNAP_CRYPTO_DEK_KEY_SIZE_128)
	    || (strlen((char *)attr->key) != SNAP_CRYPTO_DEK_SIZE)) {
		SNAP_LIB_LOG_ERR("Only support 128bit key!");
		goto out_err;
	}

	dek = calloc(1, sizeof(*dek));
	if (!dek)
		goto out_err;

	DEVX_SET(general_obj_in_cmd_hdr, in, opcode,
		MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type,
		MLX5_OBJ_TYPE_DEK);

	dek_in = in + DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr);
	DEVX_SET(dek, dek_in, key_size, attr->key_size);
	DEVX_SET(dek, dek_in, has_keytag, attr->has_keytag);
	DEVX_SET(dek, dek_in, key_purpose, attr->key_purpose);
	DEVX_SET(dek, dek_in, pd, attr->pd);
	DEVX_SET64(dek, dek_in, opaque, attr->opaque);
	dek_key = DEVX_ADDR_OF(dek, dek_in, key);
	memcpy(dek_key, attr->key, SNAP_CRYPTO_DEK_SIZE);

	dek->obj = mlx5dv_devx_obj_create(context, in, sizeof(in),
		out, sizeof(out));
	if (!dek->obj)
		goto out_free;

	dek->obj_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);

	return dek;

out_free:
	free(dek);

out_err:
	return NULL;
}

int snap_query_dek_obj(struct snap_crypto_obj *dek, struct snap_dek_attr *attr)
{
	uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr) +
		DEVX_ST_SZ_BYTES(dek)] = {0};
	uint8_t *dek_out;
	int ret;

	DEVX_SET(general_obj_in_cmd_hdr, in, opcode,
		MLX5_CMD_OP_QUERY_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type,
		MLX5_OBJ_TYPE_DEK);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_id, dek->obj_id);

	ret = mlx5dv_devx_obj_query(dek->obj, in, sizeof(in), out, sizeof(out));
	if (ret)
		return ret;

	dek_out = out + DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr);
	attr->modify_field_select = DEVX_GET64(dek, dek_out,
			modify_field_select);
	attr->state = DEVX_GET(dek, dek_out, state);

	return 0;
}

struct snap_crypto_obj *
snap_create_crypto_login_obj(struct ibv_context *context,
			struct snap_crypto_login_attr *attr)
{
	uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr) +
		DEVX_ST_SZ_BYTES(crypto_login)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr)] = {0};
	uint8_t *crypto_login_in;
	struct snap_crypto_obj *crypto_login;
	void *crypto_login_credential;

	if ((attr->credential_pointer & 0xff000000)
	    || (attr->session_import_kek_ptr & 0xff000000)) {
		SNAP_LIB_LOG_ERR(" credential_pointer or import_kek_ptr is invalid");
		goto out_err;
	}

	crypto_login = calloc(1, sizeof(*crypto_login));
	if (!crypto_login)
		goto out_err;

	DEVX_SET(general_obj_in_cmd_hdr, in, opcode,
		MLX5_CMD_OP_CREATE_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type,
		MLX5_OBJ_TYPE_CRYPTO_LOGIN);

	crypto_login_in = in + DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr);
	DEVX_SET(crypto_login, crypto_login_in, credential_pointer,
		attr->credential_pointer);
	DEVX_SET(crypto_login, crypto_login_in, session_import_kek_ptr,
		attr->session_import_kek_ptr);
	crypto_login_credential = DEVX_ADDR_OF(crypto_login, crypto_login_in,
		credential);
	memcpy(crypto_login_credential, attr->credential,
		SNAP_CRYPTO_CREDENTIAL_SIZE);

	crypto_login->obj = mlx5dv_devx_obj_create(context, in, sizeof(in),
		out, sizeof(out));
	if (!crypto_login->obj)
		goto out_free;

	crypto_login->obj_id = DEVX_GET(general_obj_out_cmd_hdr, out, obj_id);

	return crypto_login;

out_free:
	free(crypto_login);

out_err:
	return NULL;
}

int snap_query_crypto_login_obj(struct snap_crypto_obj *crypto_login,
				struct snap_crypto_login_attr *attr)
{
	uint8_t in[DEVX_ST_SZ_BYTES(general_obj_in_cmd_hdr)] = {0};
	uint8_t out[DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr) +
		DEVX_ST_SZ_BYTES(crypto_login)] = {0};
	uint8_t *crypto_login_out;
	int ret;

	DEVX_SET(general_obj_in_cmd_hdr, in, opcode,
		MLX5_CMD_OP_QUERY_GENERAL_OBJECT);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_type,
		MLX5_OBJ_TYPE_CRYPTO_LOGIN);
	DEVX_SET(general_obj_in_cmd_hdr, in, obj_id, crypto_login->obj_id);

	ret = mlx5dv_devx_obj_query(crypto_login->obj, in, sizeof(in),
			out, sizeof(out));
	if (ret)
		return ret;

	crypto_login_out = out + DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr);
	attr->modify_field_select = DEVX_GET64(crypto_login, crypto_login_out,
			modify_field_select);
	attr->state = DEVX_GET(crypto_login, crypto_login_out, state);

	return 0;
}

int snap_destroy_crypto_obj(struct snap_crypto_obj *obj)
{
	int ret = -EINVAL;

	ret = mlx5dv_devx_obj_destroy(obj->obj);
	if (ret)
		SNAP_LIB_LOG_ERR("Failed to destroy crypto obj:%p", obj);

	free(obj);

	return ret;
}

/*
 * Copyright © 2023 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of Nvidia Corporation and its affiliates
 * (the "Company") and all right, title, and interest in and to the software
 * product, including all associated intellectual property rights, are and
 * shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#include <string.h>
#include <stdio.h>
#include "snap_lib_log.h"

static struct log_register_info_t lib_log_info = {};

int snap_lib_log_source_register(const char *source_name)
{
	if (!lib_log_info.log_source_cb || !lib_log_info.log_sources_data)
		return 0;

	return lib_log_info.log_source_cb(source_name, lib_log_info.log_sources_data);
}

void snap_lib_log_register(struct log_register_info_t *info)
{
	memcpy(&lib_log_info, info, sizeof(struct log_register_info_t));
}

void snap_lib_log(uint32_t level, uint32_t source, int line, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	if (!lib_log_info.log_msg_cb || !lib_log_info.log_sources_data) {
		vprintf(format, ap);
		printf("\n");
	} else
		lib_log_info.log_msg_cb(level, source, lib_log_info.log_sources_data, line, format, ap);

	va_end(ap);
}

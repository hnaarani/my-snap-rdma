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

#ifndef SNAP_LIB_LOG_H
#define SNAP_LIB_LOG_H

#include <stdint.h>
#include <stdarg.h>

typedef void (*log_msg_func_t) (uint32_t level, uint32_t source, void *source_arr, int line, const char *format, va_list ap);
typedef int (*log_source_func_t) (const char *source_name, void *source_data);

struct log_register_info_t {
	log_msg_func_t log_msg_cb;
	log_source_func_t log_source_cb;
	void *log_sources_data;
};

enum SNAP_LIB_LOG_LEVEL {
	SNAP_LIB_LOG_LEVEL_CRIT,    /**< Critical log level */
	SNAP_LIB_LOG_LEVEL_ERROR,   /**< Error log level */
	SNAP_LIB_LOG_LEVEL_WARNING, /**< Warning log level */
	SNAP_LIB_LOG_LEVEL_INFO,    /**< Info log level */
	SNAP_LIB_LOG_LEVEL_DEBUG,   /**< Debug log level */
	SNAP_LIB_LOG_LEVEL_TRACE    /**< Trace log level */
};

void snap_lib_log(uint32_t level, uint32_t source, int line, const char *format, ...);

#define SNAP_LIB_LOG(level, format...) snap_lib_log(SNAP_LIB_LOG_LEVEL_##level, log_id, __LINE__, format)

#define SNAP_LIB_LOG_CRIT(format...) SNAP_LIB_LOG(CRIT, format)
#define SNAP_LIB_LOG_ERR(format...) SNAP_LIB_LOG(ERROR, format)
#define SNAP_LIB_LOG_WARN(format...) SNAP_LIB_LOG(WARNING, format)
#define SNAP_LIB_LOG_INFO(format...) SNAP_LIB_LOG(INFO, format)
#define SNAP_LIB_LOG_DBG(format...) \
	do { if (SNAP_DEBUG) \
		SNAP_LIB_LOG(DEBUG, format); \
	} while (0)
#define SNAP_LIB_LOG_TRACE(format...) \
	do { if (SNAP_DEBUG) \
		SNAP_LIB_LOG(TRACE, format); \
	} while (0)

void snap_lib_log_register(struct log_register_info_t *info);

int snap_lib_log_source_register(const char *source_name);

/**
 * @brief Registers log source on program start.
 *
 * Should be used to register the log source.
 * For example
 *
 * SNAP_LIB_LOG_REGISTER(dpi)
 *
 * void foo {
 *       SNAP_LIB_LOG_INFO("Message");
 * }
 *
 * @param SOURCE
 * A string representing the source name.
 */
#define SNAP_LIB_LOG_REGISTER(SOURCE)                                                                              \
	static int log_id;                                                                                             \
	/* Use the highest priority so other Ctors will be able to use the log */                                      \
	static void __attribute__((constructor(101), used)) __##__LINE__(void)                                         \
	{                                                                                                              \
		log_id = snap_lib_log_source_register(#SOURCE);                                                            \
	}

#endif

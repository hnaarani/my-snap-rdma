#
# Copyright (c) 2021-2022 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
#
# This software product is a proprietary product of NVIDIA CORPORATION &
# AFFILIATES (the "Company") and all right, title, and interest in and to the
# software product, including all associated intellectual property rights, are
# and shall remain exclusively with the Company.
#
# This software product is governed by the End User License Agreement
# provided with the software product.
#

libsnap_core_include = include_directories('.', '..', '../ctrl', flexio_include)

libsnap_core_sources = [
	'snap_lib_log.c',
	'snap_dma.c',
	'snap_dma_control.c',
	'snap_dma_dv.c',
	'snap_dma_verbs.c',
	'snap_dpa.c',
	'snap_cross_gvmi.c',
	'snap_env.c',
	'snap_mr.c',
	'snap_qp.c',
	'snap_umr.c',
	'../ctrl/snap_dp_map.c'
]

if meson.get_compiler('c', native: true).get_id() == 'clang'
	common_cflags += '-Wno-microsoft-anon-tag'
	common_cflags += '-Wno-gnu-variable-sized-type-not-at-end'
endif

#TODO remove once DOCA moves to work over dev_emu_dma_* API
install_headers('snap_dma.h',
	'snap_lib_log.h',
	'snap_qp.h',
	'snap_macros.h',
	'snap_mr.h',
	'snap_env.h',
	'snap_dma_stat.h',
	'snap_mb.h',
	'../ctrl/snap_dp_map.h',
	'snap_dpa_common.h',
	'snap_dpa_virtq_common.h')

# install fails in cross build
if not meson.is_cross_build()
	libsnap_core = shared_library('snap_core',
				libsnap_core_sources,
				c_args : common_cflags,
				dependencies : libsnap_core_depends,
				include_directories : libsnap_core_include,
				install: true,
				install_dir : '/usr/local/snap_core',
				native: true)
else 
	libsnap_core = shared_library('snap_core',
				libsnap_core_sources,
				c_args : common_cflags,
				dependencies : libsnap_core_depends,
				include_directories : libsnap_core_include,
				install: false,
				native: true)
endif 

libsnap_core_dep = declare_dependency(
			compile_args : common_cflags,
			dependencies : libsnap_core_depends,
			include_directories : libsnap_core_include,
			sources : libsnap_core_sources)

if flexio.found()

libdpa_core_include = include_directories('.', '..', flexio_include)

libdpa_core_sources = [
	'snap_dpa_rt.c',
	'snap_dpa_p2p.c'
]

if not meson.is_cross_build()
	libdpa_core = shared_library('dpa_core',
				libdpa_core_sources,
				c_args : common_cflags,
				dependencies : libsnap_core_dep,
				include_directories : libdpa_core_include,
				install: true,
				install_dir : '/usr/local/snap_core',
				native: true)
else 
	libdpa_core = shared_library('dpa_core',
				libdpa_core_sources,
				c_args : common_cflags,
				dependencies : libsnap_core_dep,
				include_directories : libdpa_core_include,
				install: false,
				native: true)
endif

libdpa_core_dep = declare_dependency(
			compile_args : common_cflags,
			dependencies : libsnap_core_dep,
			include_directories : libdpa_core_include,
			sources : libdpa_core_sources)

endif

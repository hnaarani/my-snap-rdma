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

project('libsnap_core', 'C', 'CPP',
	# Get version number from file.
	# Fallback to "more" for Windows compatibility.
	version: run_command(find_program('cat', 'more'),
		files('VERSION'), check: false).stdout().strip(),
	default_options: ['buildtype=release',
			  'werror=true',
			  'b_pie=true',
			  'b_staticpic=true',
			  'prefix=/opt/mellanox/devemu'],
	meson_version: '>= 0.47.1'
)

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

libsnap_core_depends = []
libsnap_core_depends += dependency('libibverbs', required : true)
libsnap_core_depends += dependency('libmlx5', required : true)

conf_data = configuration_data()

conf_data.set10('HAVE_DECL_IBV_QUERY_GID_EX',
		cc.has_function('ibv_query_gid_ex', prefix : '#include <infiniband/verbs.h>', dependencies: libsnap_core_depends),
		description: 'Set HAVE_DECL_IBV_QUERY_GID_EX if ibv_query_gid_ex() is supported')

configure_file(output : 'config.h', configuration : conf_data)

subdir('src')
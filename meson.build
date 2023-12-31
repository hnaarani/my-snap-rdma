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

#Meson docs:
#
#For setting optimization levels and toggling debug, you can either set
#the buildtype option, or you can set the optimization and debug options
#which give finer control over the same.
#
project('libsnap_core', 'C',
	# Get version number from file.
	# Fallback to "more" for Windows compatibility.
	version: run_command(find_program('cat', 'more'),
		files('VERSION'), check: false).stdout().strip(),
	default_options: [
			  'werror=true',
			  'b_pie=true',
			  'b_staticpic=true',
			  'prefix=/opt/mellanox/devemu',
			  # always add debug symbols (-g)
			  'debug=true',
			  'optimization=3'
			  ],
	meson_version: '>= 0.55.0'
)

cc = meson.get_compiler('c', native : true)
#cpp = meson.get_compiler('cpp', native : true)

libsnap_core_depends = []
libsnap_core_depends += dependency('libibverbs', required : true, native : true)
libsnap_core_depends += dependency('libmlx5', required : true, native : true)
libsnap_core_depends += dependency('threads', required : true, native : true)

conf_data = configuration_data()

conf_data.set10('HAVE_DECL_IBV_QUERY_GID_EX',
		cc.has_function('ibv_query_gid_ex', prefix : '#include <infiniband/verbs.h>', dependencies: libsnap_core_depends),
		description: 'Set HAVE_DECL_IBV_QUERY_GID_EX if ibv_query_gid_ex() is supported')

if (meson.is_cross_build())
	add_global_arguments('-mno-relax', '-mcmodel=medany', '-march=rv64imac',
			'-mabi=lp64', '-Wno-ignored-optimization-argument', language: 'c')
endif

#todo: in the future consider subproject/system install
flexio_include = []
flexio_pfx = get_option('with-flexio')
if (flexio_pfx == 'subproject')
	message('Using flexio from subproject')
	flexio_proj = subproject('flexio', default_options: ['default_library=static'])
	flexio = flexio_proj.get_variable('flexiosdk_libflexio_dep')
	flexio_pfx = 'subprojects/flexio'
	libflexio_os_dep = flexio_proj.get_variable('flexiosdk_libflexio_os_dep')
	libflexio_dev_dep = flexio_proj.get_variable('flexiosdk_libflexio_dev_dep')
	flexio_include = []
	libsnap_core_depends += flexio
	conf_data.set10('HAVE_FLEXIO', 1, description : 'Enable DPA host side support')
	conf_data.set10('HAVE_FLEXIO_SUBPROJECT', 1, description : 'Enable DPA host side support')
elif (flexio_pfx != '')
	message('Checking flexio in ' + flexio_pfx)
	flexio_include = [flexio_pfx + '/include', flexio_pfx + '/include/libflexio']
	flexio = cc.find_library('flexio',
			dirs: flexio_pfx + '/lib',
			has_headers: 'libflexio/flexio.h',
			header_include_directories: include_directories(flexio_include),
			required : true)
	libsnap_core_depends += flexio
	conf_data.set10('HAVE_FLEXIO', 1, description : 'Enable DPA host side support')
	if (meson.is_cross_build())
		libflexio_os_dep = declare_dependency(
				include_directories : include_directories(flexio_pfx + '/include/libflexio-os')
				)
	endif
else
	flexio = disabler()
endif

if get_option('enable-fiu')
	libfiu_proj = subproject('libfiu', default_options: 'enable-fiu=true')
else
	libfiu_proj = subproject('libfiu')
endif
libsnap_core_depends += libfiu_proj.get_variable('libfiu_dep')

dpa_app_dir = '"' + get_option('prefix') + '/dpa_app' + '"'
conf_data.set('DPA_DEFAULT_APP_DIR' , dpa_app_dir, description : 'DPA applications default directory')

configure_file(output : 'config.h', configuration : conf_data)

# common cflags
common_cflags = ['-Werror', '-Wall', '-fms-extensions', '-DSNAP_QP_ISOLATE_VL_TC_ENABLE_DEFAULT=0']
if get_option('enable-debug')
	common_cflags += '-DSNAP_DEBUG=1'
endif
if get_option('enable-simx')
	common_cflags += '-DSIMX_BUILD=1'
endif

subdir('src')
subdir('dpa')
subdir('dpa_app')
subdir('tests')

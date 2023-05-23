#
# Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
#
# This software product is a proprietary product of NVIDIA CORPORATION &
# AFFILIATES (the "Company") and all right, title, and interest in and to the
# software product, including all associated intellectual property rights, are
# and shall remain exclusively with the Company.
#
# This software product is governed by the End User License Agreement
# provided with the software product.
#

if not meson.is_cross_build()
    message('DPA applications will not be compiled')
    subdir_done()
endif

if not flexio.found()
	error('FLEXIO is required for DPA compilation')
	subdirt_done()
endif

cross_cc = meson.get_compiler('c')
message('cross compiler is ' + cross_cc.get_id() + ' ' + cross_cc.version())

subproject('picolibc', default_options: [
		'optimization=3',
		'multilib=false',
		'picolib=false',
		'picocrt=false',
		'semihost=false',
		'c_std=c11',
		'newlib-iconv-encodings=none',
		'werror=false'
])

message('DPA applications will be compiled')

dpa_cflags = [
	'-fno-builtin-fprintf', '-fno-builtin-printf',
	'-DE_MODE_LE', '-DUSER_SPACE', '-D__DPA=1'
]

dpa_cflags += common_cflags

dpa_link_script = meson.current_source_dir() + '/flexio_linker.ld'

#
# Consider adding flexio_dev and :flexio_os.a as
# external dependancies
#
dpa_ldflags = [
	'-static', '-nostartfiles',
	'-T', dpa_link_script,
	'-L' + flexio_pfx + '/lib',
	'-lflexio_os'
]

#
# GCC can compile with the picolibc but app is not loaded on DPA
# TODO: check why
# meanwhile if compiler has builtin libc use it, if not (like clang)
# fallback to picolibc
#
picolibc_dep = cross_cc.find_library('libc', required: false)
if not picolibc_dep.found()
	dpa_ldflags += ['-nostdlib']
	picolibc_dep = dependency('libc', fallback: ['picolibc', 'picolibc_dep'])
endif

libdpa_include = include_directories('.', '..', '../src', flexio_include)
libdpa_sources = [
	'dpa_common.c',
	'dpa_start.c',
	'../src/snap_dma.c',
	'../src/snap_dma_dv.c',
	'../src/snap_dpa_p2p.c'
]

libdpa = static_library('dpa',
			libdpa_sources,
			c_args : dpa_cflags,
			dependencies : [ picolibc_dep ],
			include_directories : libdpa_include,
			install: false,
			native: false)

libdpa_dep = declare_dependency(
			dependencies : [],
			include_directories : libdpa_include,
			sources : libdpa_sources)

# test binaries
dpa_tests = [
	'dpa_hello',
	'dpa_hello_event',
	'dpa_rt_test_polling',
	'dpa_rt_test_event',
	'dpa_cmd_lat_bench',
	'dpa_dma_test'
]

foreach t : dpa_tests
	executable(t, t + '.c',
		c_args : dpa_cflags,
		link_args : dpa_ldflags,
		link_depends : dpa_link_script,
		dependencies : [ libdpa_dep, picolibc_dep ],
		install : false,
		native : false
		)
endforeach

#
# installable (todo) apps
dpa_apps = [
	'dpa_virtq_split',
	'dpa_nvme'
]

#
# TODO: setup install path
foreach app : dpa_apps
	executable(app, app + '.c',
		c_args : dpa_cflags,
		link_args : dpa_ldflags,
		link_depends : dpa_link_script,
		dependencies : [ libdpa_dep, picolibc_dep ],
		install : true,
		native : false
		)
endforeach

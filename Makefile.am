
ACLOCAL_AMFLAGS=-I m4

SUBDIRS = src dpa ctrl tests

include_HEADERS = blk/snap_blk_dev.h  blk/snap_blk_ops.h  blk/snap_null_blk_dev.h

EXTRA_DIST = mlnx-libsnap.spec README.md autogen.sh debian

style_check:
	./code_style_check.sh

if HAVE_GTEST
#
# Note: test target has a special meaning for the CI pipeline
# If present it must produce XML in the expected ("gtest") format
# or build will fail
#
GTEST_REPORT_DIR ?= .
test:
	tests/gtest_snap_rdma --gtest_output="xml:$(GTEST_REPORT_DIR)/testAll.xml"
endif

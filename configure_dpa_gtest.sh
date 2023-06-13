#!/bin/sh

#BUILD_DIR=/hpc/mtl_scrap/users/alexm/DPA
#PKG_CONFIG_PATH=/usr/local/gtest/lib64/pkgconfig \
#meson setup \
#      --cross-file cross-gcc-riscv64.txt \
#      -Denable-gtest=true -Dbuild.pkg_config_path=/usr/local/gtest/lib64/pkgconfig \
#      -Dwith-flexio=/labhome/amikheev/workspace/NVME/apu/flexio-sdk-new/inst_feb2022 \
#      $BUILD_DIR/meson_build_cross

#
# On x86 gcc
CROSS_FILE=cross-gcc-riscv64.txt
FLEXIO=/labhome/amikheev/workspace/NVME/apu/flexio-sdk-jan2023/inst

#
# dpa-clang on arm
CROSS_FILE=cross-dpa-clang-riscv64.txt
FLEXIO=/labhome/amikheev/workspace/NVME/apu/flexio-sdk-jan2023/inst_arm

# flexio as a subproject. This is now preferred way to work
FLEXIO=subproject

meson setup \
      --cross-file $CROSS_FILE \
      -Denable-gtest=true \
      -Dwith-flexio=$FLEXIO \
      $*

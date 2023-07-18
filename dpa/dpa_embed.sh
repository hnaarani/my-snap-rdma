#!/bin/sh

app=$1
destdir=$2

base_app=$(basename ${app})
dummy_app=/bin/ls

# embed app into dummy binary, add empty crypto section
dd if=/dev/zero of=/tmp/256b bs=256 count=1
objcopy --add-section .dpa_bin_${base_app}=${app} --add-section .dpa_sig_name_${base_app}=/tmp/256b $dummy_app $destdir/${base_app}.host


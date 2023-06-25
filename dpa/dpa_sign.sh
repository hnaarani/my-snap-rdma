#!/bin/sh

app=$1
# .der certificate
cert=$2
# .pem private key
pkey=$3

base_app=$(basename ${app})
dummy_app=/bin/ls

# embed app into dummy binary, add empty crypto section
dd if=/dev/zero of=/tmp/256b bs=256 count=1
objcopy --add-section .dpa_bin_${base_app}=${app} --add-section .dpa_sig_name_${base_app}=/tmp/256b $dummy_app /tmp/${base_app}.host

# run mlxdpa to sign it
mlxdpa -e /tmp/${base_app}.host -c ${cert} -p ${pkey} -o /tmp/${base_app}.host_signed sign_dpa_apps

# extract signature section
# it looks like .dpa_sig_name_xxx contains an actual section name 
# actual section name is in the format of sig_app_name
#
#objcopy --dump-section .dpa_sig_name_${base_app}=${app}.sig /tmp/${base_app}.host_signed
objcopy --dump-section sig_${base_app}=${app}.sig /tmp/${base_app}.host_signed

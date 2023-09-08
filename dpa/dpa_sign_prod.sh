#!/bin/sh -ex

export VAULT_ROLE_CREDS="${DPA_SIGN_USER}:${DPA_SIGN_PASS}"

app=$1
base_app=$(basename ${app})
dummy_app=/bin/ls
temp_dir=$(mktemp -d)

trap "rm -rf $temp_dir" EXIT

rm -f ${app}.sig

# embed app into dummy binary, add empty crypto section
dd if=/dev/zero of=${temp_dir}/256b bs=256 count=1
objcopy --add-section .dpa_bin_${base_app}=${app} --add-section .dpa_sig_name_${base_app}=${temp_dir}/256b $dummy_app ${temp_dir}/${base_app}.host

# run bf3_dpa_sign.sh to sign it
bf3_dpa_sign.sh -f ${temp_dir}/${base_app}.host --platform ARM --prod -d 'Signing SNAP DPA' -o ${base_app}.host_signed

# extract signature section
# it looks like .dpa_sig_name_xxx contains an actual section name 
# actual section name is in the format of sig_app_name
#
objcopy --dump-section sig_${base_app}=${app}.sig ${base_app}.host_signed

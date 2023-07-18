#!/bin/sh

app=$1
dummy_app=$2
base_app=$(basename ${app})

# extract signature section
# it looks like .dpa_sig_name_xxx contains an actual section name 
# actual section name is in the format of sig_app_name
#
#objcopy --dump-section .dpa_sig_name_${base_app}=${app}.sig /tmp/${base_app}.host_signed
objcopy --dump-section sig_${base_app}=${app}.sig $dummy_app

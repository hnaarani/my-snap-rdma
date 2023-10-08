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

install_dir=get_option('prefix') + '/dpa_app'

install_data('dpa_virtq_split', install_dir: install_dir)
install_data('dpa_nvme', install_dir: install_dir)
install_data('dpa_nvme_mp', install_dir: install_dir)

# TODO because signature is dependant on the fw version
#if get_option('dpa-signed-apps')
# Apps must now always have signatre
install_data('dpa_virtq_split.sig', install_dir: install_dir)
install_data('dpa_nvme.sig', install_dir: install_dir)
#install_data('dpa_nvme_mp.sig', install_dir: install_dir)
#endif


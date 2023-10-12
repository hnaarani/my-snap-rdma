/*
 * Copyright © 2021 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of Nvidia Corporation and its affiliates
 * (the "Company") and all right, title, and interest in and to the software
 * product, including all associated intellectual property rights, are and
 * shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */
#include <stdio.h>
#include <string.h>

#include "dpa.h"
#include "snap_dma_internal.h"
#include "snap_dpa_rt.h"

int dpa_init()
{
	dpa_rt_init();
	printf("dpa runtime test init done!\n");
	return 0;
}

int dpa_run()
{
	struct snap_dpa_tcb *tcb = dpa_tcb();
	struct mlx5_cqe64 *cqe;

	if (tcb->user_flag == SNAP_DPA_RT_THR_EVENT) {
		printf("RT event test starting\n");
		dpa_rt_start(false);
		/* hack to run once in event mode */
		tcb->user_flag++;
		return 0;
	}

	cqe = snap_dv_poll_cq(&tcb->cmd_cq, 64);
	if (!cqe)
		return 0;

	printf("Got  DPU command, tcb %p\n", tcb);
	snap_dpa_cmd_recv(dpa_mbox(), SNAP_DPA_CMD_STOP);

	printf("RT event test done. Exiting\n");
	snap_dpa_rsp_send(dpa_mbox(), SNAP_DPA_RSP_OK);
	return 0;
}

/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 Mellanox Technologies, Ltd
 */
#include <mlx5_prm.h>
#include <malloc.h>

#include "mlx5_regex.h"
#include "mlx5_regex_utils.h"


struct mlx5_database_ctx;

/**
 * Starts the engine.
 *
 * Engine will start processing jobs.
 *
 * @param ctx
 *   ibv device handle.
 * @param engine_id
 *   The engine id to start.
 *
 * @return
 *   0 on success, error otherwise
 */





static int mlx5_regex_query_cap(struct ibv_context *ctx __rte_unused,
				struct regex_caps *caps __rte_unused)
{
#ifdef REGEX_MLX5_NO_REAL_HW
	caps->supported = 1;
	caps->num_of_engines = 2;
	caps->log_crspace_size = 8;
	return 0;
#endif
	uint32_t out[DEVX_ST_SZ_DW(query_hca_cap_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(query_hca_cap_in)] = {};
	int err;

	DEVX_SET(query_hca_cap_in, in, opcode,
		 MLX5_CMD_OP_QUERY_HCA_CAP);
	DEVX_SET(query_hca_cap_in, in, op_mod,
		 MLX5_GET_HCA_CAP_OP_MOD_GENERAL_DEVICE |
		 MLX5_HCA_CAP_OPMOD_GET_CUR);

	err = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (err) {
		DRV_LOG(ERR, "Query general failed %d\n", err);
		return err;
	}

	caps->supported = DEVX_GET(query_hca_cap_out, out,
				   capability.cmd_hca_cap.regexp);
	caps->num_of_engines = DEVX_GET(query_hca_cap_out, out,
					capability.cmd_hca_cap.regexp_num_of_engines);
	caps->log_crspace_size = DEVX_GET(query_hca_cap_out, out,
					  capability.cmd_hca_cap.regexp_log_crspace_size);
	return 0;
}

int mlx5_regex_is_supported(struct ibv_context *ibv_ctx)
{
#ifdef REGEX_MLX5_NO_REAL_HW
	return 1;
#endif
	struct regex_caps caps;
	int err;

	err = mlx5_regex_query_cap(ibv_ctx, &caps);
	if (err)
		return 0;

	return caps.supported;
}

#define RQ_TYPE_NO_RQ 3

static int alloc_pd(struct ibv_context *ctx)
{
	uint32_t in[DEVX_ST_SZ_DW(alloc_pd_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(alloc_pd_out)] = {0};
	struct mlx5dv_devx_obj *pd;

	DEVX_SET(alloc_pd_in, in, opcode, MLX5_CMD_OP_ALLOC_PD);
	pd = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!pd) {
		fprintf(stderr, "pd devx creation failed\n");
		return -1;
	}

	return DEVX_GET(alloc_pd_out, out, pd);
}

static int create_cq(struct ibv_context *ctx, volatile uint8_t **buff_out,
		     struct mlx5dv_devx_uar *uar, volatile uint32_t **dbr_out,
		     uint32_t eq)
{
	uint32_t in[DEVX_ST_SZ_DW(create_cq_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_cq_out)] = {0};
	struct mlx5_cqe64 *cqe;
	struct mlx5dv_devx_obj *cq;
	struct mlx5dv_devx_umem *pas, *dbrm;
	uint8_t *buff;
	uint8_t *dbr;
	int ret = 0;
	int i;

	buff = (uint8_t *)memalign(0x1000, (1<<LOG_CQ_SIZE)*sizeof(*cqe));
	memset(buff, 0, LOG_CQ_SIZE*sizeof(*cqe));
	for (i = 0; i < CQ_SIZE; i++) {
		cqe = (struct mlx5_cqe64 *)(buff + i * sizeof(*cqe));
		cqe->op_own = 0xff;
	}

	if (!eq)
		ret = mlx5dv_devx_query_eqn(ctx, 0, &eq);
	if (!uar)
		uar = mlx5dv_devx_alloc_uar(ctx, 0);
	pas = mlx5dv_devx_umem_reg(ctx, buff, (1<<(LOG_CQ_SIZE))*sizeof(*cqe), 7);
	dbr = (uint8_t *)memalign(0x40, 0x948);
	//dbr = memalign(0x1000, 8);
	dbrm = mlx5dv_devx_umem_reg(ctx, dbr, 0x948, 7);

	if (ret || !uar || !pas || !dbr)
		return 0;

	DEVX_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);
	DEVX_SET(create_cq_in, in, cq_context.c_eqn, eq);
	DEVX_SET(create_cq_in, in, cq_context.cqe_sz, 0);
	DEVX_SET(create_cq_in, in, cq_context.log_cq_size, LOG_CQ_SIZE);
	DEVX_SET(create_cq_in, in, cq_context.uar_page, uar->page_id);
	DEVX_SET(create_cq_in, in, cq_umem_id, pas->umem_id);
	DEVX_SET(create_cq_in, in, cq_context.dbr_umem_id, dbrm->umem_id);
	DEVX_SET64(create_cq_in, in, cq_context.dbr_addr, 0x940);

	cq = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!cq) {
		fprintf(stderr, "cq devx creation failed\n");
		return 0;
	}

	if (dbr_out)
		*dbr_out = (uint32_t *)(dbr + 0x940);
	if (buff_out)
		*buff_out = buff;

	return DEVX_GET(create_cq_out, out, cqn);
}

static int create_sq(struct ibv_context *ctx, void **buff_out,
		     struct mlx5dv_devx_uar *uar, uint32_t **dbr_out,
		     int cqn, int pd, struct mlx5dv_devx_obj **q, int qid)
{
	u8 in[DEVX_ST_SZ_BYTES(create_sq_in)] = {0};
	u8 out[DEVX_ST_SZ_BYTES(create_sq_out)] = {0};
	struct mlx5dv_devx_umem *pas, *dbrm;
	void *buff, *sqc;
	uint8_t *dbr;

	size_t sq_byte_size = SQ_SIZE*64;
	buff = memalign(0x1000, sq_byte_size);
	memset(buff, 0, sq_byte_size);
	pas = mlx5dv_devx_umem_reg(ctx, buff, sq_byte_size, 0);
	dbr = (uint8_t *)memalign(0x40, 0x948);
	dbrm = mlx5dv_devx_umem_reg(ctx, dbr, 0x948, 0);

	if (!pas || !dbrm)
		return 0;

	DEVX_SET(create_sq_in, in, opcode, MLX5_CMD_OP_CREATE_SQ);

	sqc = DEVX_ADDR_OF(create_sq_in, in, ctx);
	DEVX_SET(sqc, sqc, state, MLX5_SQC_STATE_RST);
	DEVX_SET(sqc, sqc, tis_lst_sz, 0);
	DEVX_SET(sqc, sqc, user_index, qid);
	DEVX_SET(sqc, sqc, cqn, cqn);
	DEVX_SET(sqc, sqc, wq.uar_page, uar->page_id);
	DEVX_SET(sqc, sqc, wq.pd, pd);
	DEVX_SET(sqc, sqc, wq.wq_type, MLX5_WQ_TYPE_CYCLIC);
	DEVX_SET(sqc, sqc, wq.wq_umem_id, pas->umem_id);
	DEVX_SET(sqc, sqc, wq.dbr_umem_id, dbrm->umem_id);
	DEVX_SET64(sqc, sqc, wq.dbr_addr, 0x940);
	DEVX_SET(sqc, sqc, wq.log_wq_stride, 6);
	DEVX_SET(sqc, sqc, wq.log_wq_sz, LOG_SQ_SIZE);

	*q = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out, sizeof(out));
	if (!*q)
		return 0;

	if (dbr_out)
		*dbr_out = (uint32_t *)(dbr + 0x940);
	if (buff_out)
		*buff_out = buff;

	return DEVX_GET(create_sq_out, out, sqn);
}

static int sq_to_rdy(struct mlx5dv_devx_obj *obj, int sq)
{
	uint32_t in[DEVX_ST_SZ_DW(modify_sq_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(modify_sq_out)] = {0};
	void *sqc = DEVX_ADDR_OF(modify_sq_in, in, ctx);

	DEVX_SET(modify_sq_in, in, opcode, MLX5_CMD_OP_MODIFY_SQ);
	DEVX_SET(modify_sq_in, in, sqn, sq);

	DEVX_SET(sqc, sqc, state, MLX5_SQC_STATE_RDY);

	return mlx5dv_devx_obj_modify(obj, in, sizeof(in), out, sizeof(out));
}

static int mlx5_regex_wq_open(struct mlx5_regex_ctx *ctx,
			      struct mlx5_regex_sq *qp)
{
	qp->cq.cqn = create_cq(ctx->ibv_ctx, &qp->cq.cq_buff, ctx->uar,
			       &qp->cq.cq_dbr, ctx->eq);
	if (!qp->cq.cqn) {
		DRV_LOG(ERR, "cq creation failed\n");
		return -1;
	}
	qp->qpn = create_sq(ctx->ibv_ctx, &qp->wq_buff, ctx->uar, &qp->qp_dbr,
			    qp->cq.cqn, ctx->pd, &qp->devx_obj, 0);
	qp->last_wqe_counter = 0xffff;	
	qp->cq.ci = 0;
	qp->pi = 0;
	if (!qp->qpn || !qp->devx_obj) {
		DRV_LOG(ERR, "sq creation failed %s\n", strerror(errno));
		return -1;
	}
	if (sq_to_rdy(qp->devx_obj, qp->qpn)) {
		DRV_LOG(ERR, "sq modify failed %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int mlx5_regex_wq_close(struct mlx5_regex_ctx *ctx __rte_unused,
			      struct mlx5_regex_sq *sq __rte_unused)
{
	/*mlx5_glue->devx_obj_destroy(sq->devx_obj);
	mlx5_glue->devx_obj_destroy(sq->cq.devx_obj);
	mlx5_glue->devx_obj_destroy(ctx->pd);*/
	return 0;
}

static int mlx5_regex_wqs_open(struct mlx5_regex_ctx *ctx,
		      	       unsigned int num_wqs)
{
	int ret;

	ctx->qps = calloc(num_wqs, sizeof(struct mlx5_regex_sq));
	ctx->num_qps = num_wqs;
	ctx->pd = alloc_pd(ctx->ibv_ctx);

	if (!ctx->pd) {
		DRV_LOG(ERR, "pd creation failed %s\n", strerror(errno));
		return -1;
	}

	ctx->uar = mlx5_glue->devx_alloc_uar(ctx->ibv_ctx, 0);
	ctx->uuar = 0;
	if (!ctx->uar || !ctx->uar->base_addr) {
		DRV_LOG(ERR, "uar creation failed %s\n", strerror(errno));
		return -1;
	}

	ret = mlx5_glue->devx_query_eqn(ctx->ibv_ctx, 0, &ctx->eq);
	if (ret || !ctx->eq) {
		DRV_LOG(ERR, "eq creation failed  %s\n", strerror(errno));
		return -1;
	}


	for (unsigned int i = 0; i < num_wqs; i++)
		if (mlx5_regex_wq_open(ctx, &ctx->qps[i]))
			return -1;
	return 0;
}

static int mlx5_regex_wqs_close(struct mlx5_regex_ctx *ctx)
{
	for (unsigned int i = 0; i < ctx->num_qps; i++)
		mlx5_regex_wq_close(ctx, &ctx->qps[i]);

	free(ctx->qps);

	return 0;
}
struct mlx5_regex_ctx *mlx5_regex_device_open(struct ibv_context *ibv_ctx,
					      unsigned int num_wqs)
{
	struct mlx5_regex_ctx *regex_ctx;

	regex_ctx = calloc(1, sizeof(*regex_ctx));
	if (!regex_ctx)
		return NULL;

	regex_ctx->ibv_ctx = ibv_ctx;
	/*
	 * Uncomment when we have working BF2
	 * err = mlx5_regex_query_cap(ibv_ctx, &regex_ctx->caps);
	 * if (err || !regex_ctx->caps.supported)
	 *	goto regex_free;
	 */

	if (mlx5_regex_wqs_open(regex_ctx, num_wqs))
		goto regex_free;

	return regex_ctx;
regex_free:
	free(regex_ctx);
	return NULL;
}

void mlx5_regex_device_close(struct mlx5_regex_ctx * ctx) {
	mlx5_regex_wqs_close(ctx);
}


static MLX5DV_ALWAYS_INLINE
void mlx5dv_set_metadata_seg(struct mlx5_wqe_metadata_seg *seg,
			     uint32_t mmo_control_31_0, uint32_t lkey,
			     uintptr_t address)
{
	seg->mmo_control_31_0 = htobe32(mmo_control_31_0);
	seg->lkey       = htobe32(lkey);
	seg->addr       = htobe64(address);
}

#define	MLX5_OPCODE_MMO	0x2f
#define	MLX5_OPC_MOD_MMO_REGEX 0x4
#define	ALWAYS_COMP 0x8
#define SIZE_IN_DS(SEG) (sizeof(SEG)/16 + !!sizeof(SEG)%16)

// Return work_id, or -1 in case of err
int mlx5_regex_prep_work(struct mlx5_regex_ctx *ctx,
			 struct mlx5_regex_wqe_ctrl_seg *regex_ctrl_seg,
			 volatile uint8_t* metadata_p,  uint32_t lkey,
			 struct mlx5_wqe_data_seg *input,
			 struct mlx5_wqe_data_seg *output,
			 unsigned int qid, int req_cqe)
{
#ifndef REGEX_MLX5_NO_REAL_HW
	struct mlx5_wqe_metadata_seg *meta_seg;
	struct mlx5_wqe_ctrl_seg *ctrl_seg;
	struct mlx5_regex_sq *sq;
	size_t wqe_offset;

	if (ctx->num_qps <= qid)
		return -EEXIST;

	sq = &ctx->qps[qid];
	int ds = 4; //ctrl + meta + input + output

	wqe_offset = (sq->pi % SQ_SIZE) * MLX5_SEND_WQE_BB;
	ctrl_seg = ( struct mlx5_wqe_ctrl_seg *)((uint8_t*)sq->wq_buff + wqe_offset);
	mlx5dv_set_ctrl_seg((struct mlx5_wqe_ctrl_seg *)ctrl_seg, sq->pi, MLX5_OPCODE_MMO,
			    MLX5_OPC_MOD_MMO_REGEX, sq->qpn,
			    req_cqe ? MLX5_WQE_CTRL_CQ_UPDATE : 0, ds, 0,
			    regex_ctrl_seg->le_subset_id_0_subset_id_1);

	meta_seg = (struct mlx5_wqe_metadata_seg *)((uint8_t*)ctrl_seg + sizeof(*ctrl_seg));
	mlx5dv_set_metadata_seg(meta_seg,
				regex_ctrl_seg->ctrl_subset_id_2_subset_id_3,
				lkey, (uintptr_t)metadata_p);

	memcpy((uint8_t*)meta_seg + sizeof(*meta_seg), input, sizeof(*input));
	memcpy((uint8_t*)meta_seg + sizeof(*meta_seg) + sizeof(*input),
	       output, sizeof(*output));
	
	sq->db_pi = sq->pi;
	sq->pi = (sq->pi+1)%MAX_WQE_INDEX;
	/*uint64_t *doorbell_addr = (uint64_t *)((uint8_t *)ctx->uar->base_addr + 0x800);
	rte_cio_wmb();
	sq->qp_dbr[MLX5_SND_DBR] = htobe32(sq->pi);
	rte_wmb();
	*doorbell_addr = *(volatile uint64_t *)ctrl_seg;
	rte_wmb();*/
	//printf("Post wqe for qp 0x%x:\n", sq->qpn);
	//print_raw(ctrl_seg, 1);
	//ctx->uuar = !ctx->uuar;
	return sq->db_pi;
#else
	int work_id;
	unsigned int i, match;
	
	(void) regex_ctrl_seg;
	uint8_t *output_p = (uint8_t *)be64toh((uintptr_t)output->addr);

	match =  rand()%(input->byte_count/8);
	mlx5_regex_set_metadata(metadata_p, 0, 0, 0, 0, match, match +1, 0, 0);
	for(i = 0; i < match && i < output->byte_count/4; i++) {
		output_p[i] = rand();
	}

	work_id = mlx5_regex_send_nop(ctx, qid);
	return work_id;
#endif
}

int mlx5_regex_send_work(struct mlx5_regex_ctx *ctx, unsigned int qid)
{
	struct mlx5_regex_sq *sq = &ctx->qps[qid];
	int wqe_offset = (sq->db_pi % SQ_SIZE) * MLX5_SEND_WQE_BB;
	struct mlx5_wqe_ctrl_seg *ctrl_seg = (struct mlx5_wqe_ctrl_seg *)((uint8_t*)sq->wq_buff + wqe_offset);
	uint64_t *doorbell_addr = (uint64_t *)((uint8_t *)ctx->uar->base_addr + 0x800);
	rte_cio_wmb();
	sq->qp_dbr[MLX5_SND_DBR] = htobe32(sq->db_pi);
	rte_wmb();
	*doorbell_addr = *(volatile uint64_t *)ctrl_seg;
	rte_wmb();
//	printf("Post wqe for qp 0x%x:\n", sq->qpn);
//	print_raw(ctrl_seg, 1);
	ctx->uuar = !ctx->uuar;
	return 0;
}

// Return work_id, or -1 in case of err
int mlx5_regex_send_nop(struct mlx5_regex_ctx *ctx, unsigned int qid)
{
	struct mlx5_wqe_ctrl_seg *ctrl_seg;
	struct mlx5_regex_sq *sq;
	size_t wqe_offset;

	if (ctx->num_qps <= qid)
		return -EEXIST;

	sq = &ctx->qps[qid];

	wqe_offset = (sq->pi % SQ_SIZE) * MLX5_SEND_WQE_BB;
	ctrl_seg = (struct mlx5_wqe_ctrl_seg *)((uint8_t*)sq->wq_buff + wqe_offset);
	mlx5dv_set_ctrl_seg(ctrl_seg, sq->pi, MLX5_OPCODE_NOP,
			    0, sq->qpn,
			    MLX5_WQE_CTRL_CQ_UPDATE, 1, 0,
			    0);

	int work_id = sq->pi;
	uint64_t *doorbell_addr = (uint64_t *)((uint8_t *)ctx->uar->base_addr + 0x800);
	rte_cio_wmb();
	sq->pi = (sq->pi+1)%MAX_WQE_INDEX;
	sq->qp_dbr[MLX5_SND_DBR] = rte_cpu_to_be_32(sq->pi);
	/* Ensure ordering between DB record and BF copy. */
	rte_wmb();
	*doorbell_addr = *(uint64_t *)ctrl_seg;
	rte_wmb();

	return work_id;
}
/*static __rte_always_inline enum mlx5_cqe_status
check_cqe(volatile struct mlx5_cqe *cqe, const uint16_t cqes_n,
			  const uint16_t ci)
{
	const uint16_t idx = ci & cqes_n;
	const uint8_t op_own = cqe->op_own;
	const uint8_t op_owner = MLX5_CQE_OWNER(op_own);
	const uint8_t op_code = MLX5_CQE_OPCODE(op_own);
	if (unlikely((op_owner != (!!(idx))) || (op_code == MLX5_CQE_INVALID)))
			return MLX5_CQE_STATUS_HW_OWN;
	rte_cio_rmb();
	if (unlikely(op_code == MLX5_CQE_RESP_ERR ||
	     op_code == MLX5_CQE_REQ_ERR))
		return MLX5_CQE_STATUS_ERR;
	return MLX5_CQE_STATUS_SW_OWN;
}*/

int mlx5_regex_poll(struct mlx5_regex_ctx *ctx, unsigned int sqid)
{
	struct mlx5_regex_sq *sq = &ctx->qps[sqid];
	volatile struct mlx5_cqe *cqe;
	size_t next_cqe_offset;

	//printf("Before Poll cqe 0x%x for qp 0x%x:\n", sq->cq.cqn, sq->qpn);
	next_cqe_offset =  sq->cq.ci % CQ_SIZE * sizeof(*cqe);
	cqe = (volatile struct mlx5_cqe *)(sq->cq.cq_buff + next_cqe_offset);
    rte_cio_wmb();		
	if (check_cqe(cqe, CQ_SIZE, sq->cq.ci) == MLX5_CQE_STATUS_HW_OWN)
		return 0;

	sq->cq.ci++;// = (sq->cq.ci + 1) & 0xffffff;
	asm volatile("" ::: "memory");
	sq->cq.cq_dbr[0] = htobe32(sq->cq.ci & 0xffffff);
	//printf("After Poll cqe 0x%x for qp 0x%x:\n", sq->cq.cqn, sq->qpn);
	//print_raw(cqe, 1);	
	uint16_t wqe_counter = be16toh(cqe->wqe_counter);
	int total = wqe_counter > sq->last_wqe_counter ? 
			    wqe_counter - sq->last_wqe_counter : 
				(int)(0x10000 + wqe_counter) - sq->last_wqe_counter;

	sq->last_wqe_counter = wqe_counter;
	return total;
}

static int _mlx5_regex_database_set(struct ibv_context *ctx, int engine_id,
			    struct mlx5_database_ctx *db_ctx, int stop,
			    int resume)
{
	uint32_t out[DEVX_ST_SZ_DW(set_regexp_params_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(set_regexp_params_in)] = {};
	int err;

	DEVX_SET(set_regexp_params_in, in, opcode, MLX5_CMD_SET_REGEX_PARAMS);
	DEVX_SET(set_regexp_params_in, in, engine_id, engine_id);
	if (stop || resume) {
		DEVX_SET(set_regexp_params_in, in, regexp_params.stop_engine, stop);
		DEVX_SET(set_regexp_params_in, in, field_select.stop_engine, 1);
	}

	if (db_ctx) {
		DEVX_SET(set_regexp_params_in, in, regexp_params.db_umem_id, db_ctx->umem_id);
		DEVX_SET64(set_regexp_params_in, in, regexp_params.db_umem_offset, db_ctx->offset);
		DEVX_SET(set_regexp_params_in, in, field_select.db_umem_id, 1);
	}
	err = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (err) {
		fprintf(stderr, "Set regexp params failed %d syndrome= 0x%d\n", err, DEVX_GET(set_regexp_params_out, out, syndrome));
		return err;
	}
	return 0;
}

int mlx5_regex_database_set(struct ibv_context *ctx, int engine_id,
			    struct mlx5_database_ctx *db_ctx)
{
	printf("DB set\n");
	return _mlx5_regex_database_set(ctx, engine_id, db_ctx, 0, 0);
}

int mlx5_regex_engine_stop(struct ibv_context *ctx, int engine_id)
{
	printf("DB stop\n");
	return _mlx5_regex_database_set(ctx, engine_id, NULL, 1, 0);
}

int mlx5_regex_engine_resume(struct ibv_context *ctx, int engine_id)
{
	printf("DB resume\n");
	return _mlx5_regex_database_set(ctx, engine_id, NULL, 0, 1);
}

int mlx5_regex_database_query(struct ibv_context *ctx, int engine_id,
			    struct mlx5_database_ctx *db_ctx)
{
	uint32_t out[DEVX_ST_SZ_DW(query_regexp_params_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(query_regexp_params_in)] = {};
	int err;

	DEVX_SET(query_regexp_params_in, in, opcode, MLX5_CMD_QUERY_REGEX_PARAMS);
	DEVX_SET(query_regexp_params_in, in, engine_id, engine_id);

	err = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (err) {
		fprintf(stderr, "Query regexp params failed %d\n", err);
		return err;
	}
	db_ctx->umem_id = DEVX_GET(query_regexp_params_out, out, regexp_params.db_umem_id);
	db_ctx->offset = DEVX_GET(query_regexp_params_out, out, regexp_params.db_umem_offset);
	return 0;
}

int mlx5_regex_register_write(struct ibv_context *ctx, int engine_id,
			      uint32_t addr, uint32_t data) {
	uint32_t out[DEVX_ST_SZ_DW(set_regexp_register_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(set_regexp_register_in)] = {};
	int err;

	DEVX_SET(set_regexp_register_in, in, opcode, MLX5_CMD_SET_REGEX_REGISTERS);
	DEVX_SET(set_regexp_register_in, in, engine_id, engine_id);
	DEVX_SET(set_regexp_register_in, in, register_address, addr);
	DEVX_SET(set_regexp_register_in, in, register_data, data);

	err = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (err) {
		fprintf(stderr, "Set regexp register failed %d\n", err);
		return err;
	}
	return 0;
}

int mlx5_regex_register_read(struct ibv_context *ctx, int engine_id,
			     uint32_t addr, uint32_t *data) {
	uint32_t out[DEVX_ST_SZ_DW(query_regexp_register_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(query_regexp_register_in)] = {};
	int err;

	DEVX_SET(query_regexp_register_in, in, opcode, MLX5_CMD_QUERY_REGEX_REGISTERS);
	DEVX_SET(query_regexp_register_in, in, engine_id, engine_id);
	DEVX_SET(query_regexp_register_in, in, register_address, addr);

	err = mlx5dv_devx_general_cmd(ctx, in, sizeof(in), out, sizeof(out));
	if (err) {
		fprintf(stderr, "Query regexp register failed %d\n", err);
		return err;
	}
	*data = DEVX_GET(query_regexp_register_out, out, register_data);
	return 0;
}

struct mlx5_regex_buff {
	struct mlx5dv_devx_umem *mem;
	struct mlx5dv_devx_obj *mr;
	unsigned int lkey;
};

unsigned int mlx5_regex_get_lkey(struct mlx5_regex_buff* buff) {
	return buff->lkey;
}

struct mlx5_regex_buff *mlx5_regex_reg_buffer(struct mlx5_regex_ctx *ctx, void* buff, size_t size)
{
	uint32_t in[DEVX_ST_SZ_DW(create_mkey_in)] = {0};
	uint32_t out[DEVX_ST_SZ_DW(create_mkey_out)] = {0};
	struct mlx5dv_devx_umem *mem;
	struct mlx5dv_devx_obj *mr;
	int pd = ctx->pd;

	mem = mlx5dv_devx_umem_reg(ctx->ibv_ctx, buff, size, 7);
	if (!mem)
               return 0;

	DEVX_SET(create_mkey_in, in, opcode, MLX5_CMD_OP_CREATE_MKEY);
	DEVX_SET(create_mkey_in, in, memory_key_mkey_entry.access_mode_1_0, MLX5_MKC_ACCESS_MODE_MTT);
	DEVX_SET(create_mkey_in, in, memory_key_mkey_entry.a, 1);
	DEVX_SET(create_mkey_in, in, memory_key_mkey_entry.rw, 1);
	DEVX_SET(create_mkey_in, in, memory_key_mkey_entry.rr, 1);
	DEVX_SET(create_mkey_in, in, memory_key_mkey_entry.lw, 1);
	DEVX_SET(create_mkey_in, in, memory_key_mkey_entry.lr, 1);
	DEVX_SET64(create_mkey_in, in, memory_key_mkey_entry.start_addr, (intptr_t)buff);
	DEVX_SET64(create_mkey_in, in, memory_key_mkey_entry.len, size);
	DEVX_SET(create_mkey_in, in, memory_key_mkey_entry.pd, pd);
	DEVX_SET(create_mkey_in, in, memory_key_mkey_entry.translations_octword_size, 1);
	DEVX_SET(create_mkey_in, in, memory_key_mkey_entry.log_page_size, 12);
	DEVX_SET(create_mkey_in, in, memory_key_mkey_entry.qpn, 0xffffff);
	DEVX_SET(create_mkey_in, in, memory_key_mkey_entry.mkey_7_0, 0x42);
	DEVX_SET(create_mkey_in, in, translations_octword_actual_size, 1);
	DEVX_SET(create_mkey_in, in, pg_access, 1);
	DEVX_SET(create_mkey_in, in, mkey_umem_id, mem->umem_id);

	mr = mlx5dv_devx_obj_create(ctx->ibv_ctx, in, sizeof(in), out, sizeof(out));
	if (!mr)
		return 0;

	struct mlx5_regex_buff *regex_buff = (struct mlx5_regex_buff *)calloc(sizeof(struct mlx5_regex_buff), 1);
	regex_buff->mem = mem;
	regex_buff->mr = mr;
	regex_buff->lkey = DEVX_GET(create_mkey_out, out, mkey_index) << 8 | 0x42;
	return regex_buff;
}
void print_raw(volatile uint8_t *ptr, size_t size)
{
	uint32_t dump_index = 0;

	printf("0x%p:\n", ptr);
	while (size  > dump_index) { //printing some extra here
		printf("\n");
		for (unsigned int i = 0; i < MLX5_SEND_WQE_BB ; i += 16) {
			if (!i)
				printf("0x%x:\t", dump_index);
			else
				printf("\t");
			for (unsigned int j = 0; j < 16; j += 4) {
				printf("%02x", ((((ptr)))[dump_index*MLX5_SEND_WQE_BB + i + j + 0]));
				printf("%02x", ((((ptr)))[dump_index*MLX5_SEND_WQE_BB + i + j + 1]));
				printf("%02x", ((((ptr)))[dump_index*MLX5_SEND_WQE_BB + i + j + 2]));
				printf("%02x", ((((ptr)))[dump_index*MLX5_SEND_WQE_BB + i + j + 3]));
				printf(" ");
		}
		printf("\n");
	}
	dump_index++;
	}
}



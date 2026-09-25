#include <infiniband/verbs.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <mpi.h>
#include <stdint.h>
#include <sys/time.h>

#define IB_PORT 1
#define PA_BUFFER_SIZE (128 * 1024)       // 128 KB chunk size for Eager protocol
#define MAX_DATA_TEST (4 * 1024 * 1024)   // 4 MB max buffer size
#define ITERATIONS 100                     // Number of iterations per message size
#define WARMUP 10                          // Warmup iterations

enum transfer_mode {
    MODE_EAGER = 100,
    MODE_RENDEZVOUS = 101
};

struct send_details {
    uint16_t lid;
    uint32_t qp_num;
    uint32_t rkey;
    uint64_t vaddr;
} __attribute__((packed));

struct rdma_context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_port_attr port_attr;

    void *buf;               // Registered memory buffer
    struct ibv_mr *mr;       // Memory registration
    struct send_details remote_info;
};

// Setup InfiniBand RDMA Hardware Context
struct rdma_context* setup_rdma_hardware() {
    int num_devices;
    struct ibv_device **list = ibv_get_device_list(&num_devices);
    if (!list || num_devices == 0) {
        fprintf(stderr, "No RDMA devices found\n");
        return NULL;
    }

    struct rdma_context *rdma_ctx = calloc(1, sizeof(struct rdma_context));
    if (!rdma_ctx) {
        perror("Failed to allocate memory for rdma_ctx");
        return NULL;
    }

    rdma_ctx->ctx = ibv_open_device(list[0]);
    ibv_free_device_list(list);
    if (!rdma_ctx->ctx) {
        fprintf(stderr, "Failed to open IB device\n");
        free(rdma_ctx);
        return NULL;
    }

    rdma_ctx->pd = ibv_alloc_pd(rdma_ctx->ctx);
    if (!rdma_ctx->pd) {
        fprintf(stderr, "Failed to allocate PD\n");
        return NULL;
    }

    rdma_ctx->cq = ibv_create_cq(rdma_ctx->ctx, 128, NULL, NULL, 0);
    if (!rdma_ctx->cq) {
        fprintf(stderr, "Failed to create CQ\n");
        return NULL;
    }

    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = rdma_ctx->cq,
        .recv_cq = rdma_ctx->cq,
        .cap = {
            .max_send_wr  = 128,
            .max_recv_wr  = 128,
            .max_send_sge = 1,
            .max_recv_sge = 1,
            .max_inline_data = 0
        },
        .qp_type = IBV_QPT_RC,
        .sq_sig_all = 1
    };

    rdma_ctx->qp = ibv_create_qp(rdma_ctx->pd, &qp_init_attr);
    if (!rdma_ctx->qp) {
        fprintf(stderr, "Failed to create QP\n");
        return NULL;
    }

    if (ibv_query_port(rdma_ctx->ctx, IB_PORT, &rdma_ctx->port_attr) != 0) {
        fprintf(stderr, "Failed to query port\n");
        return NULL;
    }

    return rdma_ctx;
}

// Transition QP to INIT
void modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = IB_PORT,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ
    };
    int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(qp, &attr, mask) != 0) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        exit(1);
    }
}

// Transition QP to RTR (Ready To Receive)
void modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t remote_lid) {
    struct ibv_qp_attr attr = {
        .qp_state               = IBV_QPS_RTR,
        .path_mtu               = IBV_MTU_1024,
        .dest_qp_num            = remote_qpn,
        .rq_psn                 = 0,
        .max_dest_rd_atomic     = 1,
        .min_rnr_timer          = 12,
        .ah_attr = {
            .is_global  = 0,
            .dlid       = remote_lid,
            .sl         = 0,
            .src_path_bits = 0,
            .port_num   = IB_PORT
        }
    };
    int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | 
               IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(qp, &attr, mask) != 0) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        exit(1);
    }
}

// Transition QP to RTS (Ready To Send)
void modify_qp_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state       = IBV_QPS_RTS,
        .timeout        = 14,
        .retry_cnt      = 7,
        .rnr_retry      = 7,
        .sq_psn         = 0,
        .max_rd_atomic  = 1
    };
    int mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | 
               IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &attr, mask) != 0) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        exit(1);
    }
}

// Poll Completion Queue for an entry
void poll_completion(struct ibv_cq *cq) {
    struct ibv_wc wc;
    int num_comp;
    do {
        num_comp = ibv_poll_cq(cq, 1, &wc);
    } while (num_comp == 0);

    if (num_comp < 0 || wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "CQ error: %s (status %d)\n", ibv_wc_status_str(wc.status), wc.status);
        exit(1);
    }
}

// Post a Receive Request
void post_receive(struct ibv_qp *qp, struct ibv_mr *mr, void *buf, int size) {
    struct ibv_sge sge = {
        .addr = (uint64_t)buf,
        .length = size,
        .lkey = mr->lkey
    };
    struct ibv_recv_wr wr = {
        .sg_list = &sge,
        .num_sge = 1,
        .next = NULL
    };
    struct ibv_recv_wr *bad_wr;
    if (ibv_post_recv(qp, &wr, &bad_wr) != 0) {
        perror("Failed to post receive");
        exit(1);
    }
}

// Post a Send Request (Send/Recv channel semantics)
void post_send(struct ibv_qp *qp, struct ibv_mr *mr, void *buf, int size, enum ibv_wr_opcode opcode) {
    struct ibv_sge sge = {
        .addr = (uint64_t)buf,
        .length = size,
        .lkey = mr->lkey
    };
    struct ibv_send_wr wr = {
        .sg_list = &sge,
        .num_sge = 1,
        .opcode = opcode,
        .send_flags = IBV_SEND_SIGNALED,
        .next = NULL
    };
    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
        perror("Failed to post send");
        exit(1);
    }
}

// Post RDMA Write with Immediate Data
void post_rdma_write_imm(struct ibv_qp *qp, struct ibv_mr *mr, void *buf, int size, uint64_t remote_addr, uint32_t rkey) {
    struct ibv_sge sge = {
        .addr = (uint64_t)buf,
        .length = size,
        .lkey = mr->lkey
    };
    struct ibv_send_wr wr = {0};
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data = size;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;

    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
        perror("Failed to post RDMA write with imm");
        exit(1);
    }
}

// Initialize RDMA Resources and Exchange Handles via MPI Channel Semantics
struct rdma_context* myverbs_init(enum transfer_mode mode, int rank) {
    struct rdma_context *ctx = setup_rdma_hardware();
    if (!ctx) {
        MPI_Finalize();
        return NULL;
    }

    size_t alloc_size = (mode == MODE_EAGER) ? PA_BUFFER_SIZE : MAX_DATA_TEST;
    if (posix_memalign(&ctx->buf, 4096, alloc_size) != 0) {
        perror("posix_memalign failed");
        exit(1);
    }
    memset(ctx->buf, 0, alloc_size);

    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, alloc_size, access);
    if (!ctx->mr) {
        perror("ibv_reg_mr failed");
        exit(1);
    }

    struct send_details local_info = {
        .lid = ctx->port_attr.lid,
        .qp_num = ctx->qp->qp_num,
        .rkey = ctx->mr->rkey,
        .vaddr = (uint64_t)ctx->buf
    };

    if (rank == 0) {
        MPI_Send(&local_info, sizeof(local_info), MPI_BYTE, 1, 0, MPI_COMM_WORLD);
        MPI_Recv(&ctx->remote_info, sizeof(ctx->remote_info), MPI_BYTE, 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    } else if (rank == 1) {
        MPI_Recv(&ctx->remote_info, sizeof(ctx->remote_info), MPI_BYTE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Send(&local_info, sizeof(local_info), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
    }

    modify_qp_to_init(ctx->qp);
    modify_qp_to_rtr(ctx->qp, ctx->remote_info.qp_num, ctx->remote_info.lid);
    modify_qp_to_rts(ctx->qp);

    return ctx;
}

// Destroy RDMA Context
void myverbs_destroy(struct rdma_context *ctx) {
    if (!ctx) return;
    if (ctx->mr) ibv_dereg_mr(ctx->mr);
    if (ctx->buf) free(ctx->buf);
    if (ctx->qp) ibv_destroy_qp(ctx->qp);
    if (ctx->cq) ibv_destroy_cq(ctx->cq);
    if (ctx->pd) ibv_dealloc_pd(ctx->pd);
    if (ctx->ctx) ibv_close_device(ctx->ctx);
    free(ctx);
}

// Task 1: Eager Protocol Send
void myverbs_send_eager(struct rdma_context* ctx, const void *buf, int size) {
    int bytes_sent = 0;
    const char *ptr = (const char *)buf;

    while (bytes_sent < size) {
        int chunk = (size - bytes_sent > PA_BUFFER_SIZE) ? PA_BUFFER_SIZE : (size - bytes_sent);
        memcpy(ctx->buf, ptr + bytes_sent, chunk);
        post_send(ctx->qp, ctx->mr, ctx->buf, chunk, IBV_WR_SEND);
        poll_completion(ctx->cq);
        bytes_sent += chunk;
    }
}

// Task 1: Eager Protocol Recv
void myverbs_recv_eager(struct rdma_context* ctx, void *buf, int size) {
    int bytes_rcv = 0;
    char *ptr = (char *)buf;
    while (bytes_rcv < size) {
        int chunk = (size - bytes_rcv > PA_BUFFER_SIZE) ? PA_BUFFER_SIZE : (size - bytes_rcv);
        post_receive(ctx->qp, ctx->mr, ctx->buf, chunk);
        poll_completion(ctx->cq);
        memcpy(ptr + bytes_rcv, ctx->buf, chunk);
        bytes_rcv += chunk;
    }
}

// Task 2: Rendezvous Protocol Send (RDMA Write with Immediate)
void myverbs_send_rendezvous(struct rdma_context* ctx, const void *buf, int size) {
    if (buf != ctx->buf) {
        memcpy(ctx->buf, buf, size);
    }
    post_rdma_write_imm(ctx->qp, ctx->mr, ctx->buf, size, ctx->remote_info.vaddr, ctx->remote_info.rkey);
    poll_completion(ctx->cq);
}

// Task 2: Rendezvous Protocol Recv
void myverbs_recv_rendezvous(struct rdma_context* ctx, void *buf, int size) {
    post_receive(ctx->qp, ctx->mr, ctx->buf, 0);
    poll_completion(ctx->cq);
    if (buf != ctx->buf) {
        memcpy(buf, ctx->buf, size);
    }
}

// Benchmark Task 1: Eager Protocol
void run_task1_eager(int rank, struct rdma_context *ctx) {
    void *user_buff = NULL;
    if (posix_memalign(&user_buff, 4096, MAX_DATA_TEST) != 0) {
        perror("posix_memalign user_buff");
        exit(1);
    }
    memset(user_buff, 'A', MAX_DATA_TEST);

    if (rank == 0) {
        printf("\n=======================================================\n");
        printf("       Task 1: Eager Protocol Latency Benchmark        \n");
        printf("=======================================================\n");
        printf("%14s   %12s   %18s\n", "Size (bytes)", "Iterations", "Avg Latency (us)");
        printf("-------------------------------------------------------\n");
    }

    struct timeval start, end;
    for (int size = 1; size <= MAX_DATA_TEST; size *= 2) {
        // Warmup iterations
        for (int i = 0; i < WARMUP; i++) {
            if (rank == 0) {
                myverbs_send_eager(ctx, user_buff, size);
                myverbs_recv_eager(ctx, user_buff, size);
            } else if (rank == 1) {
                myverbs_recv_eager(ctx, user_buff, size);
                myverbs_send_eager(ctx, user_buff, size);
            }
        }

        MPI_Barrier(MPI_COMM_WORLD);
        gettimeofday(&start, NULL);

        for (int i = 0; i < ITERATIONS; i++) {
            if (rank == 0) {
                myverbs_send_eager(ctx, user_buff, size);
                myverbs_recv_eager(ctx, user_buff, size);
            } else if (rank == 1) {
                myverbs_recv_eager(ctx, user_buff, size);
                myverbs_send_eager(ctx, user_buff, size);
            }
        }

        gettimeofday(&end, NULL);

        if (rank == 0) {
            double total_time_us = (end.tv_sec - start.tv_sec) * 1000000.0 + (end.tv_usec - start.tv_usec);
            double one_way_latency_us = (total_time_us / 2.0) / ITERATIONS;
            printf("%14d   %12d   %18.2f\n", size, ITERATIONS, one_way_latency_us);
        }
    }
    free(user_buff);
}

// Benchmark Task 2: Rendezvous Protocol
void run_task2_rendezvous(int rank, struct rdma_context *ctx) {
    void *user_buff = NULL;
    if (posix_memalign(&user_buff, 4096, MAX_DATA_TEST) != 0) {
        perror("posix_memalign user_buff");
        exit(1);
    }
    memset(user_buff, 'B', MAX_DATA_TEST);

    if (rank == 0) {
        printf("\n=======================================================\n");
        printf("    Task 2: Rendezvous Protocol Latency Benchmark     \n");
        printf("=======================================================\n");
        printf("%14s   %12s   %18s\n", "Size (bytes)", "Iterations", "Avg Latency (us)");
        printf("-------------------------------------------------------\n");
    }

    struct timeval start, end;
    for (int size = 1; size <= MAX_DATA_TEST; size *= 2) {
        // Warmup iterations
        for (int i = 0; i < WARMUP; i++) {
            if (rank == 0) {
                myverbs_send_rendezvous(ctx, user_buff, size);
                myverbs_recv_rendezvous(ctx, user_buff, size);
            } else if (rank == 1) {
                myverbs_recv_rendezvous(ctx, user_buff, size);
                myverbs_send_rendezvous(ctx, user_buff, size);
            }
        }

        MPI_Barrier(MPI_COMM_WORLD);
        gettimeofday(&start, NULL);

        for (int i = 0; i < ITERATIONS; i++) {
            if (rank == 0) {
                myverbs_send_rendezvous(ctx, user_buff, size);
                myverbs_recv_rendezvous(ctx, user_buff, size);
            } else if (rank == 1) {
                myverbs_recv_rendezvous(ctx, user_buff, size);
                myverbs_send_rendezvous(ctx, user_buff, size);
            }
        }

        gettimeofday(&end, NULL);

        if (rank == 0) {
            double total_time_us = (end.tv_sec - start.tv_sec) * 1000000.0 + (end.tv_usec - start.tv_usec);
            double one_way_latency_us = (total_time_us / 2.0) / ITERATIONS;
            printf("%14d   %12d   %18.2f\n", size, ITERATIONS, one_way_latency_us);
        }
    }
    free(user_buff);
}

int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2) {
        if (rank == 0) fprintf(stderr, "This benchmark requires at least 2 MPI processes.\n");
        MPI_Finalize();
        return 1;
    }

    // Run Task 1: Eager Protocol
    struct rdma_context *ctx_eager = myverbs_init(MODE_EAGER, rank);
    MPI_Barrier(MPI_COMM_WORLD);
    run_task1_eager(rank, ctx_eager);
    myverbs_destroy(ctx_eager);

    MPI_Barrier(MPI_COMM_WORLD);

    // Run Task 2: Rendezvous Protocol
    struct rdma_context *ctx_rv = myverbs_init(MODE_RENDEZVOUS, rank);
    MPI_Barrier(MPI_COMM_WORLD);
    run_task2_rendezvous(rank, ctx_rv);
    myverbs_destroy(ctx_rv);

    MPI_Finalize();
    return 0;
}
#include <infiniband/verbs.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <mpi.h>
#include <stdint.h>
#include <sys/time.h>
#include <arpa/inet.h>

#define IB_PORT 1
#define PA_BUFFER_SIZE (128 * 1024)       // 128 KB preallocated buffer chunk size for Eager
#define MAX_DATA_TEST (4 * 1024 * 1024)   // 4 MB max test buffer size
#define ITERATIONS 100                     // Iterations per message size
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

struct rz_handle {
    uint64_t vaddr;
    uint32_t rkey;
    uint32_t size;
} __attribute__((packed));

struct rdma_context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *send_cq;  // Dedicated Send Completion Queue
    struct ibv_cq *recv_cq;  // Dedicated Recv Completion Queue
    struct ibv_qp *qp;
    struct ibv_port_attr port_attr;
    enum transfer_mode mode;
    int rank;

    void *eager_buf;         // Pre-allocated 128 KB buffer for Eager mode
    struct ibv_mr *eager_mr; // Pre-registered MR for Eager mode

    void *ctrl_buf;          // Pre-allocated 4 KB control buffer for Verbs handle exchange
    struct ibv_mr *ctrl_mr;  // Control MR for Verbs handle exchange

    struct send_details remote_info;
};

// Comparison function for qsort (median computation)
int compare_doubles(const void *a, const void *b) {
    double arg1 = *(const double *)a;
    double arg2 = *(const double *)b;
    if (arg1 < arg2) return -1;
    if (arg1 > arg2) return 1;
    return 0;
}

double compute_median(double *latencies, int count) {
    qsort(latencies, count, sizeof(double), compare_doubles);
    if (count % 2 == 0) {
        return (latencies[count / 2 - 1] + latencies[count / 2]) / 2.0;
    } else {
        return latencies[count / 2];
    }
}

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

    rdma_ctx->send_cq = ibv_create_cq(rdma_ctx->ctx, 256, NULL, NULL, 0);
    if (!rdma_ctx->send_cq) {
        fprintf(stderr, "Failed to create Send CQ\n");
        return NULL;
    }

    rdma_ctx->recv_cq = ibv_create_cq(rdma_ctx->ctx, 256, NULL, NULL, 0);
    if (!rdma_ctx->recv_cq) {
        fprintf(stderr, "Failed to create Recv CQ\n");
        return NULL;
    }

    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = rdma_ctx->send_cq,
        .recv_cq = rdma_ctx->recv_cq,
        .cap = {
            .max_send_wr  = 256,
            .max_recv_wr  = 256,
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

// Transition QP to RTR using DYNAMIC MTU from query port
void modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t remote_lid, enum ibv_mtu mtu) {
    struct ibv_qp_attr attr = {
        .qp_state               = IBV_QPS_RTR,
        .path_mtu               = mtu, // Dynamic MTU from port attributes
        .dest_qp_num            = remote_qpn,
        .rq_psn                 = 0,
        .max_dest_rd_atomic     = 1,
        .min_rnr_timer          = 12,
        .ah_attr = {
            .is_global  = 0, // Native InfiniBand subnet (LID-based addressing). Set is_global=1 with GID for RoCE.
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

// Transition QP to RTS
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

// Poll Completion Queue with Status & Opcode Validation
struct ibv_wc poll_completion_wc(struct ibv_cq *cq, enum ibv_wc_opcode expected_opcode) {
    struct ibv_wc wc;
    int num_comp;
    do {
        num_comp = ibv_poll_cq(cq, 1, &wc);
    } while (num_comp == 0);

    if (num_comp < 0 || wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "CQ error: %s (status %d, wr_id %lu)\n", 
                ibv_wc_status_str(wc.status), wc.status, (unsigned long)wc.wr_id);
        exit(1);
    }

    if (expected_opcode != (enum ibv_wc_opcode)-1 && wc.opcode != expected_opcode) {
        fprintf(stderr, "CQ opcode mismatch: expected %d, got %d\n", expected_opcode, wc.opcode);
        exit(1);
    }

    return wc;
}

void poll_completion(struct ibv_cq *cq) {
    poll_completion_wc(cq, (enum ibv_wc_opcode)-1);
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

// Post RDMA Write with Immediate Data using dynamic MR
void post_rdma_write_imm_mr(struct ibv_qp *qp, struct ibv_mr *mr, void *buf, int size, uint64_t remote_addr, uint32_t rkey) {
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
    wr.imm_data = htonl((uint32_t)size); // IB spec requires immediate data in network byte order (htonl)
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;

    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
        perror("Failed to post RDMA write with imm");
        exit(1);
    }
}

// Initialize RDMA Resources and Exchange Setup Details via MPI Bootstrap
struct rdma_context* myverbs_init(enum transfer_mode mode, int rank) {
    struct rdma_context *ctx = setup_rdma_hardware();
    if (!ctx) {
        MPI_Finalize();
        return NULL;
    }
    ctx->mode = mode;
    ctx->rank = rank;

    // Allocate & register 128 KB bounce buffer for Eager mode
    if (posix_memalign(&ctx->eager_buf, 4096, PA_BUFFER_SIZE) != 0) {
        perror("posix_memalign eager_buf failed");
        exit(1);
    }
    memset(ctx->eager_buf, 0, PA_BUFFER_SIZE);

    int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    ctx->eager_mr = ibv_reg_mr(ctx->pd, ctx->eager_buf, PA_BUFFER_SIZE, access);
    if (!ctx->eager_mr) {
        perror("ibv_reg_mr eager_buf failed");
        exit(1);
    }

    // Allocate & register 4 KB control buffer for Verbs handle exchange
    if (posix_memalign(&ctx->ctrl_buf, 4096, 4096) != 0) {
        perror("posix_memalign ctrl_buf failed");
        exit(1);
    }
    memset(ctx->ctrl_buf, 0, 4096);

    ctx->ctrl_mr = ibv_reg_mr(ctx->pd, ctx->ctrl_buf, 4096, access);
    if (!ctx->ctrl_mr) {
        perror("ibv_reg_mr ctrl_buf failed");
        exit(1);
    }

    // MPI Bootstrap ONLY for QP connection parameters (LID and QPN)
    struct send_details local_info = {
        .lid = ctx->port_attr.lid,
        .qp_num = ctx->qp->qp_num,
        .rkey = 0,
        .vaddr = 0
    };

    if (rank == 0) {
        MPI_Send(&local_info, sizeof(local_info), MPI_BYTE, 1, 0, MPI_COMM_WORLD);
        MPI_Recv(&ctx->remote_info, sizeof(ctx->remote_info), MPI_BYTE, 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    } else if (rank == 1) {
        MPI_Recv(&ctx->remote_info, sizeof(ctx->remote_info), MPI_BYTE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Send(&local_info, sizeof(local_info), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
    }

    // Transition QP using DYNAMIC MTU from port attributes
    modify_qp_to_init(ctx->qp);
    modify_qp_to_rtr(ctx->qp, ctx->remote_info.qp_num, ctx->remote_info.lid, ctx->port_attr.active_mtu);
    modify_qp_to_rts(ctx->qp);

    return ctx;
}

// Destroy RDMA Context
void myverbs_destroy(struct rdma_context *ctx) {
    if (!ctx) return;
    if (ctx->eager_mr) ibv_dereg_mr(ctx->eager_mr);
    if (ctx->eager_buf) free(ctx->eager_buf);
    if (ctx->ctrl_mr) ibv_dereg_mr(ctx->ctrl_mr);
    if (ctx->ctrl_buf) free(ctx->ctrl_buf);
    if (ctx->qp) ibv_destroy_qp(ctx->qp);
    if (ctx->send_cq) ibv_destroy_cq(ctx->send_cq);
    if (ctx->recv_cq) ibv_destroy_cq(ctx->recv_cq);
    if (ctx->pd) ibv_dealloc_pd(ctx->pd);
    if (ctx->ctx) ibv_close_device(ctx->ctx);
    free(ctx);
}

// =========================================================================
// TASK 1: EAGER PROTOCOL (128 KB Chunking via Pre-Allocated Bounce Buffer)
// =========================================================================

// Pre-posts receive buffer for the FIRST chunk in Eager transfer
void myverbs_prepost_recv_eager(struct rdma_context* ctx, int size) {
    int chunk = (size > PA_BUFFER_SIZE) ? PA_BUFFER_SIZE : size;
    post_receive(ctx->qp, ctx->eager_mr, ctx->eager_buf, chunk);
}

void myverbs_send_eager(struct rdma_context* ctx, const void *buf, int size) {
    int bytes_sent = 0;
    const char *ptr = (const char *)buf;

    while (bytes_sent < size) {
        int chunk = (size - bytes_sent > PA_BUFFER_SIZE) ? PA_BUFFER_SIZE : (size - bytes_sent);
        memcpy(ctx->eager_buf, ptr + bytes_sent, chunk);
        post_send(ctx->qp, ctx->eager_mr, ctx->eager_buf, chunk, IBV_WR_SEND);
        poll_completion_wc(ctx->send_cq, IBV_WC_SEND);
        bytes_sent += chunk;
    }
}

void myverbs_wait_recv_eager(struct rdma_context* ctx, void *buf, int size) {
    int bytes_rcv = 0;
    char *ptr = (char *)buf;
    while (bytes_rcv < size) {
        int chunk = (size - bytes_rcv > PA_BUFFER_SIZE) ? PA_BUFFER_SIZE : (size - bytes_rcv);
        poll_completion_wc(ctx->recv_cq, IBV_WC_RECV);
        memcpy(ptr + bytes_rcv, ctx->eager_buf, chunk);
        bytes_rcv += chunk;

        // Post receive WR for next chunk ONLY AFTER copying out current chunk from eager_buf
        // to prevent hardware DMA overwrite races on shared bounce buffer memory.
        if (bytes_rcv < size) {
            int next_chunk = (size - bytes_rcv > PA_BUFFER_SIZE) ? PA_BUFFER_SIZE : (size - bytes_rcv);
            post_receive(ctx->qp, ctx->eager_mr, ctx->eager_buf, next_chunk);
        }
    }
}

// =========================================================================
// TASK 2: TRUE ZERO-COPY RENDEZVOUS PROTOCOL
// (On-The-Fly MR Reg & Verbs Channel Handle Exchange)
// =========================================================================

// Rendezvous Sender: Registers user_buf on-the-fly, receives remote handle over Verbs, RDMA Writes directly, and deregisters
void myverbs_send_rendezvous(struct rdma_context* ctx, const void *user_buf, int size) {
    // 1. On-the-fly memory registration of sender's actual user buffer (ZERO memcpy!)
    struct ibv_mr *send_mr = ibv_reg_mr(ctx->pd, (void*)user_buf, size, IBV_ACCESS_LOCAL_WRITE);
    if (!send_mr) {
        perror("ibv_reg_mr sender on-the-fly failed");
        exit(1);
    }

    // 2. Pre-post receive to receive remote handle (vaddr, rkey) over Verbs Channel Semantics
    post_receive(ctx->qp, ctx->ctrl_mr, ctx->ctrl_buf, sizeof(struct rz_handle));

    // 3. Wait for receiver's handle message over Verbs Channel
    poll_completion_wc(ctx->recv_cq, IBV_WC_RECV);
    struct rz_handle remote_h;
    memcpy(&remote_h, ctx->ctrl_buf, sizeof(struct rz_handle));

    // 4. Perform TRUE ZERO-COPY RDMA Write with Immediate directly into receiver's user memory
    post_rdma_write_imm_mr(ctx->qp, send_mr, (void*)user_buf, size, remote_h.vaddr, remote_h.rkey);
    poll_completion_wc(ctx->send_cq, IBV_WC_RDMA_WRITE); // Wait for RDMA write send completion

    // 5. On-the-fly memory deregistration
    ibv_dereg_mr(send_mr);
}

// Rendezvous Receiver: Registers user_buf on-the-fly, sends handle over Verbs, catches RDMA Write, and deregisters
void myverbs_wait_recv_rendezvous(struct rdma_context* ctx, void *user_buf, int size) {
    // 1. On-the-fly memory registration of receiver's actual user buffer (ZERO memcpy!)
    struct ibv_mr *recv_mr = ibv_reg_mr(ctx->pd, user_buf, size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!recv_mr) {
        perror("ibv_reg_mr receiver on-the-fly failed");
        exit(1);
    }

    // 2. Pre-post 0-byte receive request to catch RDMA Write with Immediate notification
    post_receive(ctx->qp, recv_mr, user_buf, 0);

    // 3. Prepare control handle with receiver's user buffer vaddr and rkey
    struct rz_handle local_h = {
        .vaddr = (uint64_t)user_buf,
        .rkey = recv_mr->rkey,
        .size = size
    };
    memcpy(ctx->ctrl_buf, &local_h, sizeof(struct rz_handle));

    // 4. Send control handle to sender via Verbs Channel Semantics (ibv_post_send)
    post_send(ctx->qp, ctx->ctrl_mr, ctx->ctrl_buf, sizeof(struct rz_handle), IBV_WR_SEND);
    poll_completion_wc(ctx->send_cq, IBV_WC_SEND); // Wait for control send completion

    // 5. Wait for sender's RDMA Write with Immediate to complete directly into user_buf
    struct ibv_wc wc = poll_completion_wc(ctx->recv_cq, IBV_WC_RECV_RDMA_WITH_IMM);
    uint32_t imm_size = ntohl(wc.imm_data); // Convert from network byte order (ntohl) per IB spec
    if (imm_size != (uint32_t)size) {
        fprintf(stderr, "Immediate data size mismatch: expected %d, got %u\n", size, imm_size);
        exit(1);
    }

    // 6. On-the-fly memory deregistration
    ibv_dereg_mr(recv_mr);
}

// =========================================================================
// UNIFIED DROP-IN API SURFACE
// =========================================================================

void myverbs_send(struct rdma_context *ctx, const void *buf, int size) {
    if (ctx->mode == MODE_EAGER) {
        myverbs_send_eager(ctx, buf, size);
    } else {
        myverbs_send_rendezvous(ctx, buf, size);
    }
}

void myverbs_recv(struct rdma_context *ctx, void *buf, int size) {
    if (ctx->mode == MODE_EAGER) {
        myverbs_wait_recv_eager(ctx, buf, size);
    } else {
        myverbs_wait_recv_rendezvous(ctx, buf, size);
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

    double latencies[ITERATIONS];

    if (rank == 0) {
        printf("\n=======================================================\n");
        printf("   Task 1: Eager Protocol Median Latency Benchmark    \n");
        printf("=======================================================\n");
        printf("%14s   %12s   %18s\n", "Size (bytes)", "Iterations", "Median Latency (us)");
        printf("-------------------------------------------------------\n");
    }

    struct timespec start, end;
    for (int size = 1; size <= MAX_DATA_TEST; size *= 2) {
        // Warmup iterations
        myverbs_prepost_recv_eager(ctx, size);
        MPI_Barrier(MPI_COMM_WORLD);
        for (int i = 0; i < WARMUP; i++) {
            if (rank == 0) {
                myverbs_send(ctx, user_buff, size);
                myverbs_recv(ctx, user_buff, size);
                if (i < WARMUP - 1) myverbs_prepost_recv_eager(ctx, size);
            } else if (rank == 1) {
                myverbs_recv(ctx, user_buff, size);
                if (i < WARMUP - 1) myverbs_prepost_recv_eager(ctx, size);
                myverbs_send(ctx, user_buff, size);
            }
        }

        // Benchmark iterations
        myverbs_prepost_recv_eager(ctx, size);
        MPI_Barrier(MPI_COMM_WORLD);

        for (int i = 0; i < ITERATIONS; i++) {
            if (rank == 0) {
                clock_gettime(CLOCK_MONOTONIC, &start);

                myverbs_send(ctx, user_buff, size);
                myverbs_recv(ctx, user_buff, size);

                clock_gettime(CLOCK_MONOTONIC, &end);

                double rtt_us = (end.tv_sec - start.tv_sec) * 1000000.0 + (end.tv_nsec - start.tv_nsec) / 1000.0;
                latencies[i] = rtt_us / 2.0;

                if (i < ITERATIONS - 1) {
                    myverbs_prepost_recv_eager(ctx, size);
                }
            } else if (rank == 1) {
                myverbs_recv(ctx, user_buff, size);

                if (i < ITERATIONS - 1) {
                    myverbs_prepost_recv_eager(ctx, size);
                }

                myverbs_send(ctx, user_buff, size);
            }
        }

        if (rank == 0) {
            double median_lat = compute_median(latencies, ITERATIONS);
            printf("%14d   %12d   %18.2f\n", size, ITERATIONS, median_lat);
        }
    }
    free(user_buff);
}

// Benchmark Task 2: Rendezvous Protocol (True Zero-Copy On-the-Fly)
void run_task2_rendezvous(int rank, struct rdma_context *ctx) {
    void *user_buff = NULL;
    if (posix_memalign(&user_buff, 4096, MAX_DATA_TEST) != 0) {
        perror("posix_memalign user_buff");
        exit(1);
    }
    memset(user_buff, 'B', MAX_DATA_TEST);

    double latencies[ITERATIONS];

    if (rank == 0) {
        printf("\n=======================================================\n");
        printf(" Task 2: Zero-Copy Rendezvous Median Latency (On-the-Fly)\n");
        printf("=======================================================\n");
        printf("%14s   %12s   %18s\n", "Size (bytes)", "Iterations", "Median Latency (us)");
        printf("-------------------------------------------------------\n");
    }

    struct timespec start, end;
    for (int size = 1; size <= MAX_DATA_TEST; size *= 2) {
        // Warmup iterations
        MPI_Barrier(MPI_COMM_WORLD);
        for (int i = 0; i < WARMUP; i++) {
            if (rank == 0) {
                myverbs_send(ctx, user_buff, size);
                myverbs_recv(ctx, user_buff, size);
            } else if (rank == 1) {
                myverbs_recv(ctx, user_buff, size);
                myverbs_send(ctx, user_buff, size);
            }
        }

        // Benchmark iterations
        MPI_Barrier(MPI_COMM_WORLD);

        for (int i = 0; i < ITERATIONS; i++) {
            if (rank == 0) {
                clock_gettime(CLOCK_MONOTONIC, &start);

                myverbs_send(ctx, user_buff, size);
                myverbs_recv(ctx, user_buff, size);

                clock_gettime(CLOCK_MONOTONIC, &end);

                double rtt_us = (end.tv_sec - start.tv_sec) * 1000000.0 + (end.tv_nsec - start.tv_nsec) / 1000.0;
                latencies[i] = rtt_us / 2.0;
            } else if (rank == 1) {
                myverbs_recv(ctx, user_buff, size);
                myverbs_send(ctx, user_buff, size);
            }
        }

        if (rank == 0) {
            double median_lat = compute_median(latencies, ITERATIONS);
            printf("%14d   %12d   %18.2f\n", size, ITERATIONS, median_lat);
        }
    }
    free(user_buff);
}

int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);
    int rank, comm_size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &comm_size);

    // Enforce strict check for 2 ranks
    if (comm_size != 2) {
        if (rank == 0) fprintf(stderr, "This benchmark requires exactly 2 MPI processes.\n");
        MPI_Finalize();
        return 1;
    }

    // Run Task 1: Eager Protocol
    struct rdma_context *ctx_eager = myverbs_init(MODE_EAGER, rank);
    MPI_Barrier(MPI_COMM_WORLD);
    run_task1_eager(rank, ctx_eager);
    myverbs_destroy(ctx_eager);

    MPI_Barrier(MPI_COMM_WORLD);

    // Run Task 2: Rendezvous Protocol (True Zero-Copy On-the-Fly & Verbs Handle Exchange)
    struct rdma_context *ctx_rv = myverbs_init(MODE_RENDEZVOUS, rank);
    MPI_Barrier(MPI_COMM_WORLD);
    run_task2_rendezvous(rank, ctx_rv);
    myverbs_destroy(ctx_rv);

    MPI_Finalize();
    return 0;
}
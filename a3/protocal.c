#include <infiniband/verbs.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <mpi.h>
#include <stdint.h>

// This is the hardware port on your NIC (usually 1), NOT a TCP socket port
#define IB_PORT 1
#define PA_BUFFER_SIZE 64*1024          // 64 kb
#define MAX_DATA_TEST 4*1024*1024       // 4 MB
enum transfer_mode {
    Rendezvous = 100,
    Eager = 101
};
struct send_details {
    uint16_t lid;
    uint32_t qp_num;
    uint32_t rkey;
    uint64_t vaddr;
} __attribute__((packed));

struct rdma_context{
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_port_attr port_attr;

    void *prealloc_buf;
    struct ibv_mr *prealloc_mr;
    struct send_details remote_info;
};


struct rdma_context* setup_rdma_hardware(){
    int num_devices;
    struct ibv_device **list = ibv_get_device_list(&num_devices);   

    if(list == NULL || num_devices == 0) {
        perror("No RDMA devices found\n");
        return NULL;
    }
    // printf("Number of devices: %d\n", num_devices);

    struct rdma_context *rdma_ctx = calloc(1, sizeof(struct rdma_context));
    if (!rdma_ctx) {
        perror("Failed to allocate memory");
        return NULL;
    }
    rdma_ctx->ctx = ibv_open_device(list[0]);    //  open the first device for each of the node in the cluster
    ibv_free_device_list(list); // clean up the list

    rdma_ctx->pd = ibv_alloc_pd(rdma_ctx->ctx);  // allocate pd

    rdma_ctx->cq = ibv_create_cq(rdma_ctx->ctx, 10, NULL, NULL, 0);   // allocate cq 

    // initiallize struct for queue pair
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = rdma_ctx->cq,
        .recv_cq = rdma_ctx->cq,
        .cap = {
            .max_send_wr  = 10,
            .max_recv_wr  = 10,
            .max_send_sge = 1,

            .max_recv_sge = 1,
            .max_inline_data = 0
        },
        .qp_type = IBV_QPT_RC, // Reliable Connection
        .sq_sig_all = 1
    };


    rdma_ctx->qp = ibv_create_qp(rdma_ctx->pd, &qp_init_attr);

    if (ibv_query_port(rdma_ctx->ctx, IB_PORT, &rdma_ctx->port_attr) != 0) {
        perror("Failed to query port");
        return NULL; // Return NULL instead of 1
    }
    // printf("My Local Identifier (LID) is: %d\n", port_attr.lid);
    // printf("My Queue Pair Number (QPN) is: %d\n", qp->qp_num);

    return rdma_ctx;
}

// 1. Transition RESET -> INIT
void modify_qp_to_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {
        .qp_state        = IBV_QPS_INIT,
        .pkey_index      = 0,
        .port_num        = IB_PORT,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
    };
    
    // We tell the function which specific attributes we are changing using the mask
    int mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    
    if (ibv_modify_qp(qp, &attr, mask) != 0) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        exit(1);
    }
}

// 2. Transition INIT -> RTR (Ready To Receive)
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

// 3. Transition RTR -> RTS (Ready To Send)
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
// Helper to wait until the network card finishes a job
void poll_completion(struct ibv_cq *cq) {
    struct ibv_wc wc;
    int num_comp;
    do {
        // Keep checking the Completion Queue
        num_comp = ibv_poll_cq(cq, 1, &wc);
    } while (num_comp == 0);

    if (num_comp < 0 || wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "CQ error: %s\n", ibv_wc_status_str(wc.status));
        exit(1);
    }
}

// Helper to post a Receive Request
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

// Helper to post a Send Request (Used for both Task 1 and Task 2)
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
        .send_flags = IBV_SEND_SIGNALED, // Tells hardware to drop a receipt in the CQ
        .next = NULL
    };
    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
        perror("Failed to post send");
        exit(1);
    }
}

void post_rdma_write_imm(struct ibv_qp *qp, struct ibv_mr *mr, void *buf, int size, uint64_t remote_addr, uint32_t rkey) {
    struct ibv_sge sge = {
        .addr = (uint64_t)buf,
        .length = size,
        .lkey = mr->lkey
    };
    
    struct ibv_send_wr wr = {0}; // Zero-initialize the struct
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data = 0x1234; // Arbitrary 32-bit integer for the immediate data
    
    // RDMA specific fields
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey = rkey;
    
    struct ibv_send_wr *bad_wr;
    if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
        perror("Failed to post RDMA write");
        exit(1);
    }
}
void run_task2_rdma_write(int rank, struct ibv_qp *qp, struct ibv_cq *cq, struct ibv_mr *mr, void *net_buffer, struct send_details *remote) {
    struct timespec start, end;

    if (rank == 0) {
        printf("\n--- Task 2: RDMA Write with Immediate ---\n");
    }

    // Warm-up phase (100 iterations with 1-byte message)
    MPI_Barrier(MPI_COMM_WORLD);
    for (int i = 0; i < 100; i++) {
        MPI_Barrier(MPI_COMM_WORLD);
        post_receive(qp, mr, net_buffer, 1);
        if (rank == 0) {
            post_rdma_write_imm(qp, mr, net_buffer, 1, remote->vaddr, remote->rkey);
            poll_completion(cq);
            poll_completion(cq);
        } else if (rank == 1) {
            poll_completion(cq);
            post_send(qp, mr, net_buffer, 1, IBV_WR_SEND);
            poll_completion(cq);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    for (int size = 1; size <= 4194304; size *= 2) {
        MPI_Barrier(MPI_COMM_WORLD); 

        // BOTH sides must post a receive buffer. 
        // Node 1 needs it to catch the immediate data. Node 0 needs it to catch the ACK.
        post_receive(qp, mr, net_buffer, size);

        if (rank == 0) {
            // SENDER LOGIC
            clock_gettime(CLOCK_MONOTONIC_RAW, &start);

            // 1. Write directly to Node 1's memory
            post_rdma_write_imm(qp, mr, net_buffer, size, remote->vaddr, remote->rkey);
            
            poll_completion(cq); // Wait for our hardware to finish sending the write
            poll_completion(cq); // Wait for Node 1 to send an ACK (pong) back

            clock_gettime(CLOCK_MONOTONIC_RAW, &end);

            double rtt_ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
            double one_way_latency = rtt_ns / 2.0;

            printf("Size: %-6d bytes | Latency: %.2f ns\n", size, one_way_latency);

        } else if (rank == 1) {
            // RECEIVER LOGIC
            poll_completion(cq); // Wait for the immediate data to drop into our CQ
            
            // Send a standard ACK back to Node 0 so it can stop its timer
            post_send(qp, mr, net_buffer, size, IBV_WR_SEND); 
            poll_completion(cq); // Wait for our ACK send to finish
        }
    }
}

struct rdma_context* myverbs_init(enum transfer_mode mode, int rank){
    if(mode == Eager){
        struct rdma_context *ctx = setup_rdma_hardware();
        if (!ctx) { MPI_Finalize(); return 1; }

        posix_memalign(&ctx->prealloc_buf, 4096, PA_BUFFER_SIZE);
        int access  = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
        ctx->prealloc_mr = ibv_reg_mr(ctx->pd, ctx->prealloc_buf, PA_BUFFER_SIZE, access);


        struct send_details local_info = {
            .lid = ctx->port_attr.lid,
            .qp_num = ctx->qp->qp_num,
            .rkey = ctx->prealloc_mr->rkey,   
            .vaddr = (uint64_t)ctx->prealloc_buf
        };

        if (rank == 0) {
            MPI_Send(&local_info, sizeof(local_info), MPI_BYTE, 1, 0, MPI_COMM_WORLD);
            MPI_Recv(&ctx->remote_info, sizeof(ctx->remote_info), MPI_BYTE, 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        } 
        else if (rank == 1) {
            MPI_Recv(&ctx->remote_info, sizeof(ctx->remote_info), MPI_BYTE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Send(&local_info, sizeof(local_info), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
        }
        modify_qp_to_init(ctx->qp);
        modify_qp_to_rtr(ctx->qp, ctx->remote_info.qp_num, ctx->remote_info.lid);
        modify_qp_to_rts(ctx->qp);
        return ctx;
    }

}
void myverbs_send_eager(struct rdma_context* ctx, const void *buf, int size){
    int bytes_sent = 0;
    char *ptr = (char *)buf;

    while(bytes_sent < size){
        int chunk = (size - bytes_sent > PA_BUFFER_SIZE) ? PA_BUFFER_SIZE : (size - bytes_sent);
        memcpy(ctx->prealloc_buf, ptr + bytes_sent, chunk);
        post_send(ctx->qp, ctx->prealloc_mr, ctx->prealloc_buf, chunk, IBV_WR_SEND);
        poll_completion(ctx->cq);

        bytes_sent += chunk;
    }

}

void myverbs_recv_eager(struct rdma_context* ctx, void *buf, int size){
    int bytes_rcv = 0;
    char *ptr = (char *)buf;
    while(bytes_rcv < size){
        int chunk = (size - bytes_rcv > PA_BUFFER_SIZE) ? PA_BUFFER_SIZE : (size - bytes_rcv);
        post_receive(ctx->qp, ctx->prealloc_mr, ctx->prealloc_buf, chunk);
        poll_completion(ctx->cq);
        memcpy(ptr + bytes_rcv, ctx->prealloc_buf, chunk);
        bytes_rcv += chunk;
    }
}

void run_task1_eager(int rank, struct rdma_context *ctx) {
    if (rank == 0) {
        printf("\n--- Task 1:Eagar ---\n");
    }
    struct timespec start, end;
        void * user_buff = malloc(MAX_DATA_TEST);
        if (!user_buff) {
            perror("Failed to allocate send buffer");
            exit(1);
        }
        memset(user_buff, 'A',MAX_DATA_TEST);

    
    MPI_Barrier(MPI_COMM_WORLD);
    for (int size = 4*1024; size <= 4194304; size *= 2) {

        MPI_Barrier(MPI_COMM_WORLD); 
        if (rank == 0) {
            clock_gettime(CLOCK_MONOTONIC_RAW, &start);
            myverbs_send_eager(ctx, user_buff, size);
            myverbs_recv_eager(ctx, user_buff, size);
            clock_gettime(CLOCK_MONOTONIC_RAW, &end);
            double rtt_ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
            // Divide by 2 for one-way, then by 1,000,000 to convert nanoseconds to milliseconds
            double one_way_latency_ms = (rtt_ns / 2.0) / 1000000.0;

            // Use %.4f so you can actually see the tiny fractions of a millisecond
            printf("Size: %-8d bytes | Latency: %.4f ms\n", size, one_way_latency_ms);
        } else if (rank == 1) {

            myverbs_recv_eager(ctx, user_buff, size);
            myverbs_send_eager(ctx, user_buff, size);
        }
    }
    free(user_buff);
}
int main(int argc, char *argv[]) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    // Task - 1
    struct rdma_context *ctx = myverbs_init(Eager, rank);
    MPI_Barrier(MPI_COMM_WORLD);

    run_task1_eager(rank, ctx);
    // run_task2_rdma_write(rank, ctx->qp, ctx->cq, net_mr, net_buffer, &remote_info);

	// cleanup in reverse order 
	
    ibv_dereg_mr(ctx->prealloc_mr);
    free(ctx->prealloc_buf);

    ibv_destroy_qp(ctx->qp);
    ibv_destroy_cq(ctx->cq);
    ibv_dealloc_pd(ctx->pd);
    ibv_close_device(ctx->ctx);
    free(ctx);
    MPI_Finalize();

    return 0;
}
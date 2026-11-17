#define _POSIX_C_SOURCE 200809L
#include <mpi.h>
#include <omp.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Integer sums on one node, with separate input and output buffers. */
typedef struct {
    MPI_Comm node;
    MPI_Win window;
    int rank, ranks, threads;
    size_t capacity;
    int *local, **inputs, *sum;
} allreduce_context;

static void check_mpi(int error, const char *operation)
{
    if (error != MPI_SUCCESS) {
        char message[MPI_MAX_ERROR_STRING];
        int length;
        MPI_Error_string(error, message, &length);
        fprintf(stderr, "%s: %.*s\n", operation, length, message);
        MPI_Abort(MPI_COMM_WORLD, error);
    }
}
#define MPI_CHECK(call) check_mpi((call), #call)

static void *allocate(size_t bytes)
{
    void *pointer = NULL;
    if (posix_memalign(&pointer, 64, bytes ? bytes : 64)) {
        fprintf(stderr, "Allocation failed: %zu bytes\n", bytes);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    return pointer;
}

static void initialize(allreduce_context *ctx, size_t capacity, int threads)
{
    int world_size;
    memset(ctx, 0, sizeof(*ctx));
    MPI_CHECK(MPI_Comm_rank(MPI_COMM_WORLD, &ctx->rank));
    MPI_CHECK(MPI_Comm_size(MPI_COMM_WORLD, &world_size));
    MPI_CHECK(MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED,
                                  ctx->rank, MPI_INFO_NULL, &ctx->node));
    MPI_CHECK(MPI_Comm_size(ctx->node, &ctx->ranks));
    if (ctx->ranks != world_size) {
        fprintf(stderr, "All MPI_COMM_WORLD ranks must be on one node.\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    ctx->capacity = capacity;
    ctx->threads = threads;
    ctx->inputs = allocate((size_t)ctx->ranks * sizeof(*ctx->inputs));
    /* Put the shared result after rank 0's input buffer. */
    MPI_Aint bytes = (MPI_Aint)capacity * (MPI_Aint)sizeof(int);
    MPI_CHECK(MPI_Win_allocate_shared(bytes * (ctx->rank == 0 ? 2 : 1),
                                      sizeof(int), MPI_INFO_NULL, ctx->node,
                                      &ctx->local, &ctx->window));
    for (int peer = 0; peer < ctx->ranks; ++peer) {
        MPI_Aint size;
        int unit;
        MPI_CHECK(MPI_Win_shared_query(ctx->window, peer, &size, &unit,
                                       &ctx->inputs[peer]));
    }
    ctx->sum = ctx->inputs[0] + capacity;
    MPI_CHECK(MPI_Win_lock_all(0, ctx->window));
}

static void synchronize(allreduce_context *ctx)
{
    /* Make shared writes visible before the other ranks read them. */
    MPI_CHECK(MPI_Win_sync(ctx->window));
    MPI_CHECK(MPI_Barrier(ctx->node));
    MPI_CHECK(MPI_Win_sync(ctx->window));
}

static void my_mpi_allreduce(const int *restrict source,
                            int *restrict destination, size_t count,
                            allreduce_context *ctx)
{
    if (count > ctx->capacity) MPI_Abort(MPI_COMM_WORLD, 1);
    const size_t begin = count * (size_t)ctx->rank / (size_t)ctx->ranks;
    const size_t end = count * (size_t)(ctx->rank + 1) / (size_t)ctx->ranks;
    int *local = ctx->local;
    int *restrict sum = ctx->sum;
    const int *a = ctx->inputs[0];
    const int *b = ctx->ranks >= 2 ? ctx->inputs[1] : a;

    /* Worker threads copy and add; only the main thread calls MPI. */
    #pragma omp parallel num_threads(ctx->threads)
    {
        #pragma omp for schedule(static)
        for (size_t i = 0; i < count; ++i) local[i] = source[i];
        #pragma omp master
        { synchronize(ctx); }
        #pragma omp barrier

        /* Each rank adds its own chunk. These loops also allow SIMD. */
        if (ctx->ranks == 2) {
            #pragma omp for schedule(static)
            for (size_t i = begin; i < end; ++i) sum[i] = a[i] + b[i];
        } else if (ctx->ranks == 4) {
            const int *restrict c = ctx->inputs[2];
            const int *restrict d = ctx->inputs[3];
            #pragma omp for schedule(static)
            for (size_t i = begin; i < end; ++i)
                sum[i] = a[i] + b[i] + c[i] + d[i];
        } else {
            #pragma omp for schedule(static)
            for (size_t i = begin; i < end; ++i) {
                int value = 0;
                for (int peer = 0; peer < ctx->ranks; ++peer)
                    value += ctx->inputs[peer][i];
                sum[i] = value;
            }
        }
        #pragma omp master
        { synchronize(ctx); }
        #pragma omp barrier
        #pragma omp for schedule(static)
        for (size_t i = 0; i < count; ++i) destination[i] = sum[i];
    }
    /* The next call's first barrier waits for these output copies to finish. */
}

static void finalize(allreduce_context *ctx)
{
    MPI_CHECK(MPI_Barrier(ctx->node));
    MPI_CHECK(MPI_Win_unlock_all(ctx->window));
    MPI_CHECK(MPI_Win_free(&ctx->window));
    MPI_CHECK(MPI_Comm_free(&ctx->node));
    free(ctx->inputs);
}

static void fill(int *source, size_t count, int rank, int pattern)
{
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < count; ++i)
        source[i] = ((int)(i % 251) - 125) * (rank + 1) + pattern;
}

static void verify(const int *source, int *reference, int *custom, size_t count,
                   allreduce_context *ctx)
{
    MPI_CHECK(MPI_Allreduce(source, reference, (int)count, MPI_INT, MPI_SUM,
                            MPI_COMM_WORLD));
    my_mpi_allreduce(source, custom, count, ctx);
    int ok = memcmp(reference, custom, count * sizeof(int)) == 0;
    int all_ok;
    MPI_CHECK(MPI_Allreduce(&ok, &all_ok, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD));
    if (!all_ok) {
        fprintf(stderr, "memcmp failed on rank %d, count=%zu\n", ctx->rank, count);
        MPI_Abort(MPI_COMM_WORLD, 2);
    }
}

static double measure(int custom, const int *source, int *destination,
                      size_t count, int iterations, allreduce_context *ctx)
{
    MPI_CHECK(MPI_Barrier(MPI_COMM_WORLD));
    double start = MPI_Wtime();
    for (int i = 0; i < iterations; ++i) {
        if (custom) my_mpi_allreduce(source, destination, count, ctx);
        else MPI_CHECK(MPI_Allreduce(source, destination, (int)count, MPI_INT,
                                     MPI_SUM, MPI_COMM_WORLD));
    }
    double elapsed = (MPI_Wtime() - start) / iterations;
    double slowest;
    MPI_CHECK(MPI_Allreduce(&elapsed, &slowest, 1, MPI_DOUBLE, MPI_MAX,
                            MPI_COMM_WORLD));
    return slowest * 1e6;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int positive_number(const char *text)
{
    char *end;
    long value = strtol(text, &end, 10);
    if (!*text || *end || value < 1 || value > 100000) {
        fprintf(stderr, "Expected a positive integer (<=100000): %s\n", text);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    return (int)value;
}

int main(int argc, char **argv)
{
    int provided;
    MPI_CHECK(MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided));
    if (provided < MPI_THREAD_FUNNELED) MPI_Abort(MPI_COMM_WORLD, 1);
    int threads = omp_get_max_threads(), iterations = 20, trials = 7;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            threads = positive_number(argv[++i]);
        else if (!strcmp(argv[i], "--iterations") && i + 1 < argc)
            iterations = positive_number(argv[++i]);
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc)
            trials = positive_number(argv[++i]);
        else {
            fprintf(stderr, "Usage: %s [--threads N] [--iterations N] [--trials N]\n", argv[0]);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }
    omp_set_dynamic(0);
    omp_set_num_threads(threads);
    const size_t capacity = 64u * 1024u * 1024u / sizeof(int);
    allreduce_context ctx;
    initialize(&ctx, capacity, threads);
    int *source = allocate(capacity * sizeof(int));
    int *reference = allocate(capacity * sizeof(int));
    int *custom = allocate(capacity * sizeof(int));
    double *mpi_times = allocate((size_t)trials * sizeof(double));
    double *custom_times = allocate((size_t)trials * sizeof(double));
    char processor[MPI_MAX_PROCESSOR_NAME];
    int processor_length;
    MPI_CHECK(MPI_Get_processor_name(processor, &processor_length));
    if (!ctx.rank) {
        char version[MPI_MAX_LIBRARY_VERSION_STRING];
        int length;
        MPI_CHECK(MPI_Get_library_version(version, &length));
        fprintf(stderr, "Host=%s ranks=%d threads_per_rank=%d iterations=%d trials=%d\nMPI=%.*s\n",
                processor, ctx.ranks, threads, iterations, trials, length, version);
        puts("ranks,threads,bytes,mpi_us,custom_us,speedup,memcmp");
    }
    /* Check empty inputs, uneven chunks and reuse with different values. */
    const size_t edges[] = {0, 1, 3, 17, 1027};
    for (int repeat = 0; repeat < 3; ++repeat) {
        fill(source, 1027, ctx.rank, repeat - 1);
        for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i)
            verify(source, reference, custom, edges[i], &ctx);
    }
    for (size_t bytes = 1024u * 1024u; bytes <= capacity * sizeof(int); bytes *= 2) {
        size_t count = bytes / sizeof(int);
        fill(source, count, ctx.rank, (int)(bytes / (1024 * 1024)));
        /* Initialize both outputs with the same threads before timing. */
        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < count; ++i) reference[i] = custom[i] = 0;
        verify(source, reference, custom, count, &ctx);
        for (int warmup = 0; warmup < 5; ++warmup) {
            MPI_CHECK(MPI_Allreduce(source, reference, (int)count, MPI_INT,
                                    MPI_SUM, MPI_COMM_WORLD));
            my_mpi_allreduce(source, custom, count, &ctx);
        }
        for (int trial = 0; trial < trials; ++trial) {
            /* Swap the order each trial so neither version always runs first. */
            if (trial % 2 == 0) {
                mpi_times[trial] = measure(0, source, reference, count, iterations, &ctx);
                custom_times[trial] = measure(1, source, custom, count, iterations, &ctx);
            } else {
                custom_times[trial] = measure(1, source, custom, count, iterations, &ctx);
                mpi_times[trial] = measure(0, source, reference, count, iterations, &ctx);
            }
            /* Check the result again after repeated calls, outside the timer. */
            verify(source, reference, custom, count, &ctx);
            if (!ctx.rank)
                fprintf(stderr, "sample ranks=%d threads=%d bytes=%zu trial=%d mpi_us=%.6f custom_us=%.6f\n",
                        ctx.ranks, threads, bytes, trial, mpi_times[trial], custom_times[trial]);
        }
        qsort(mpi_times, (size_t)trials, sizeof(double), compare_double);
        qsort(custom_times, (size_t)trials, sizeof(double), compare_double);
        double mpi_median = (mpi_times[(trials - 1) / 2] + mpi_times[trials / 2]) / 2;
        double custom_median = (custom_times[(trials - 1) / 2] + custom_times[trials / 2]) / 2;
        if (!ctx.rank) {
            printf("%d,%d,%zu,%.6f,%.6f,%.4f,PASS\n", ctx.ranks, threads, bytes,
                   mpi_median, custom_median, mpi_median / custom_median);
            fflush(stdout);
        }
    }
    if (!ctx.rank) fprintf(stderr, "All correctness checks passed.\n");
    finalize(&ctx);
    free(source); free(reference); free(custom); free(mpi_times); free(custom_times);
    MPI_CHECK(MPI_Finalize());
    return 0;
}

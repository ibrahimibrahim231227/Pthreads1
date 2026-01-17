#include <iostream>
#include <fstream>
#include <chrono>
#include <cstdint>
#include <pthread.h>
#include <cstdlib>

constexpr int DIM = 1000;
alignas(64) long matrix_a[DIM][DIM];
alignas(64) long matrix_b[DIM][DIM];
alignas(64) long matrix_b_t[DIM][DIM]; // transposed copy for cache-friendly access
alignas(64) long matrix_c[DIM][DIM];
// Barrier to synchronize thread start of multiplication
pthread_barrier_t start_barrier;

// Initialize matrices with sample values
void init() {
    for (int i = 0; i < DIM; ++i) {
        for (int j = 0; j < DIM; ++j) {
            matrix_a[i][j] = i + j;
            matrix_b[i][j] = i - j;
            matrix_c[i][j] = 0;
        }
    }
    // build transposed copy of matrix_b to improve locality in multiplication
    for (int i = 0; i < DIM; ++i) {
        for (int j = 0; j < DIM; ++j) {
            matrix_b_t[j][i] = matrix_b[i][j];
        }
    }
}

// Perform matrix multiplication
void multiply() {
    // Use transposed B for better cache behavior: access A row and B_t row contiguously
    for (int i = 0; i < DIM; ++i) {
        for (int j = 0; j < DIM; ++j) {
            long sum = 0;
            long* arow = matrix_a[i];
            long* browt = matrix_b_t[j];
            for (int k = 0; k < DIM; ++k) {
                sum += arow[k] * browt[k];
            }
            matrix_c[i][j] = sum;
        }
    }
}

// pthread wrapper for running multiply() in a separate thread
void* multiply_thread_fn(void* /*arg*/) {
    multiply();
    return nullptr;
}

// Arguments for per-thread multiply range
struct ThreadArgs {
    int row_start;
    int row_end; // exclusive
};

// Worker: multiply rows [row_start, row_end)
void* multiply_range(void* arg) {
    ThreadArgs* a = static_cast<ThreadArgs*>(arg);
    // wait until main signals all threads to start together
    pthread_barrier_wait(&start_barrier);
    // per-thread work: use transposed B to minimize cache misses and memory contention
    for (int i = a->row_start; i < a->row_end; ++i) {
        long* arow = matrix_a[i];
        for (int j = 0; j < DIM; ++j) {
            long sum = 0;
            long* browt = matrix_b_t[j];
            // iterate k over contiguous memory for both arrays
            for (int k = 0; k < DIM; ++k) {
                sum += arow[k] * browt[k];
            }
            matrix_c[i][j] = sum;
        }
    }
    return nullptr;
}

// Write result to a file
void print() {
    std::ofstream fout("serial.txt");
    for (int i = 0; i < DIM; ++i) {
        for (int j = 0; j < DIM; ++j) {
            fout << matrix_c[i][j] << '\n';
        }
    }
    fout.close();
}

int main(int argc, char** argv) {
    using clock = std::chrono::high_resolution_clock;

    auto t0 = clock::now();
    init();
    auto t1 = clock::now();

    // Determine number of worker threads (optional argv[1])
    int nthreads = 4;
    if (argc > 1) {
        int parsed = std::atoi(argv[1]);
        if (parsed > 0) nthreads = parsed;
    }
    if (nthreads > DIM) nthreads = DIM;

    // initialize barrier: workers + main
    if (pthread_barrier_init(&start_barrier, nullptr, nthreads + 1) != 0) {
        std::cerr << "Failed to init barrier\n";
        return 1;
    }

    pthread_t* threads = new pthread_t[nthreads];
    ThreadArgs* targs = new ThreadArgs[nthreads];

    int base = DIM / nthreads;
    int rem = DIM % nthreads;
    int start = 0;
    for (int t = 0; t < nthreads; ++t) {
        int rows = base + (t < rem ? 1 : 0);
        int end = start + rows;
        targs[t].row_start = start;
        targs[t].row_end = end;
        if (pthread_create(&threads[t], nullptr, &multiply_range, &targs[t]) != 0) {
            std::cerr << "Failed to create multiply thread " << t << "\n";
            delete[] threads;
            delete[] targs;
            return 1;
        }
        start = end;
    }

    // all threads created; release them to start multiplication at the same time
    // main participates in the barrier so the timing starts when barrier returns
    if (pthread_barrier_wait(&start_barrier) != 0 && errno != 0) {
        // pthread_barrier_wait returns PTHREAD_BARRIER_SERIAL_THREAD for one thread
    }
    auto t_start_mult = clock::now();

    for (int t = 0; t < nthreads; ++t) {
        if (pthread_join(threads[t], nullptr) != 0) {
            std::cerr << "Failed to join multiply thread " << t << "\n";
            delete[] threads;
            delete[] targs;
            return 1;
        }
    }
    auto t2 = clock::now();
    // destroy barrier now that threads have finished
    pthread_barrier_destroy(&start_barrier);
    delete[] threads;
    delete[] targs;
    print();
    auto t3 = clock::now();

    std::chrono::duration<double> init_sec = t1 - t0;
    std::chrono::duration<double> mult_sec = t2 - t_start_mult;
    std::chrono::duration<double> print_sec = t3 - t2;
    std::chrono::duration<double> total_sec = t3 - t0;

    // Count approximate basic operations
    // init: two arithmetic ops per element (i+j, i-j) plus one store for matrix_c
    std::uint64_t init_ops = static_cast<std::uint64_t>(DIM) * DIM * 3; // (i+j),(i-j),c=0

    // multiply: one multiply and one add per k for each (i,j)
    std::uint64_t mul_ops = static_cast<std::uint64_t>(DIM) * DIM * DIM; // multiplications
    std::uint64_t add_ops = mul_ops; // additions
    std::uint64_t mult_total_ops = mul_ops + add_ops;

    // print: one write per element
    std::uint64_t print_ops = static_cast<std::uint64_t>(DIM) * DIM;

    double sec_per_init_op = init_sec.count() / double(init_ops);
    double sec_per_mult_op = mult_sec.count() / double(mult_total_ops);
    double sec_per_print_op = print_sec.count() / double(print_ops);

    // Print profiling summary
    std::cout << "Profiling summary:\n";
    std::cout << "  init:  " << init_sec.count() << " s\n";
    std::cout << "  mult:  " << mult_sec.count() << " s\n";
    std::cout << "  print: " << print_sec.count() << " s\n";
    std::cout << "  total: " << total_sec.count() << " s\n";
    std::cout << "Estimated basic ops:\n";
    std::cout << "  init ops:  " << init_ops << "\n";
    std::cout << "  mult muls: " << mul_ops << "\n";
    std::cout << "  mult adds: " << add_ops << "\n";
    std::cout << "  print ops: " << print_ops << "\n";
    std::cout << "Time per init-op:  " << sec_per_init_op << " s\n";
    std::cout << "Time per mult-op:  " << sec_per_mult_op << " s\n";
    std::cout << "Time per print-op: " << sec_per_print_op << " s\n";

    // Decide which part is the bottleneck by total time
    if (mult_sec.count() >= init_sec.count() && mult_sec.count() >= print_sec.count()) {
        std::cout << "Bottleneck: multiplication\n";
    } else if (init_sec.count() >= mult_sec.count() && init_sec.count() >= print_sec.count()) {
        std::cout << "Bottleneck: initialization\n";
    } else {
        std::cout << "Bottleneck: printing/output\n";
    }

    // Also write a small results file for HTML embedding
    std::ofstream rf("profile_results.txt");
    rf << "init " << init_sec.count() << "\n";
    rf << "mult " << mult_sec.count() << "\n";
    rf << "print " << print_sec.count() << "\n";
    rf << "total " << total_sec.count() << "\n";
    rf << "init_ops " << init_ops << "\n";
    rf << "mul_ops " << mul_ops << "\n";
    rf << "add_ops " << add_ops << "\n";
    rf << "print_ops " << print_ops << "\n";
    rf << "sec_per_init_op " << sec_per_init_op << "\n";
    rf << "sec_per_mult_op " << sec_per_mult_op << "\n";
    rf << "sec_per_print_op " << sec_per_print_op << "\n";
    rf.close();

    return 0;
}
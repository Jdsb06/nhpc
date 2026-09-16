#!/usr/bin/env python3
"""
plot.py - Performance Comparison Plotter: TCP Sockets vs RDMA Verbs
Parses benchmark raw data and generates logarithmic latency comparison graph.
"""

import os
import pandas as pd
import matplotlib.pyplot as plt

# Message sizes in bytes
sizes = [
    1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192,
    16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304
]

# TCP Sockets Latency (A1) - Raw measured data in microseconds (µs)
tcp_latency_us = [
    91.89, 68.55, 68.23, 68.91, 68.17, 70.57, 69.98, 73.08, 77.43,
    96.59, 161.30, 188.45, 253.21, 469.26, 528.31, 857.39, 1576.38,
    2648.32, 4650.06, 9254.04, 18086.93, 35941.96, 71549.95
]

# RDMA Channel Semantics / Send-Recv (A2 Task 1) - Raw measured data in nanoseconds (ns)
rdma_send_recv_ns = [
    2400.00, 2150.00, 2064.50, 2063.50, 2059.50, 2099.00, 2073.00,
    2248.50, 2243.00, 2575.50, 2785.00, 3779.00, 4735.50, 4586.00,
    5831.00, 8179.00, 13044.50, 25935.50, 46208.00, 88561.50,
    174135.00, 354221.00, 714947.50
]

# RDMA Memory Semantics / RDMA Write with Immediate (A2 Task 2) - Raw measured data in nanoseconds (ns)
rdma_write_ns = [
    3655.50, 3394.00, 2779.00, 2620.50, 2145.50, 2254.50, 1936.00,
    2546.50, 2816.50, 2361.00, 2474.50, 7732.50, 3923.50, 5003.00,
    7066.00, 9480.50, 14802.50, 28236.00, 58880.50, 112955.50,
    224284.50, 462489.50, 825517.50
]

# Unit Conversion: Divide nanoseconds (ns) by 1000 to get microseconds (µs)
rdma_send_recv_us = [ns / 1000.0 for ns in rdma_send_recv_ns]
rdma_write_us = [ns / 1000.0 for ns in rdma_write_ns]

# Assemble into Pandas DataFrame
df = pd.DataFrame({
    'Message Size (Bytes)': sizes,
    'TCP Sockets (A1)': tcp_latency_us,
    'RDMA Channel Semantics (A2 Task 1)': rdma_send_recv_us,
    'RDMA Memory Semantics (A2 Task 2)': rdma_write_us
})

def plot_benchmark():
    plt.figure(figsize=(10, 6))
    
    # Plot lines with specified legend labels
    plt.plot(df['Message Size (Bytes)'], df['TCP Sockets (A1)'],
             marker='o', linewidth=2, label='TCP Sockets (A1)', color='#d9534f')
    
    plt.plot(df['Message Size (Bytes)'], df['RDMA Channel Semantics (A2 Task 1)'],
             marker='s', linewidth=2, label='RDMA Channel Semantics (A2 Task 1)', color='#0275d8')
    
    plt.plot(df['Message Size (Bytes)'], df['RDMA Memory Semantics (A2 Task 2)'],
             marker='^', linewidth=2, label='RDMA Memory Semantics (A2 Task 2)', color='#5cb85c')

    # Axis settings
    plt.xscale('log', base=2)
    plt.yscale('log')
    
    plt.xlabel('Message Size (Bytes)', fontsize=12, fontweight='bold')
    plt.ylabel('Latency (µs) [Log Scale]', fontsize=12, fontweight='bold')
    plt.title('Latency Comparison: TCP Sockets vs RDMA Verbs', fontsize=14, fontweight='bold', pad=15)
    
    plt.grid(True, which="both", ls="--", alpha=0.5)
    plt.legend(fontsize=11, loc='upper left')
    plt.tight_layout()

    # Save figure
    output_dir = os.path.dirname(os.path.abspath(__file__))
    output_path = os.path.join(output_dir, 'latency_comparison.png')
    plt.savefig(output_path, dpi=300)
    print(f"Plot saved successfully to: {output_path}")

if __name__ == '__main__':
    plot_benchmark()

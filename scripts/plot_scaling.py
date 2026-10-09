#!/usr/bin/env python3
"""
TaskForge Scaling Curve Plotter
Generates a PNG chart of worker scaling, speedup factor, and contention ceiling.
"""

import sys
import os
import csv

def generate_plot(csv_path="benchmarks/scaling_results.csv", output_png="docs/scaling_curve.png"):
    if not os.path.exists(csv_path):
        print(f"Error: {csv_path} not found.")
        return 1

    required = {"workers", "throughput", "speedup", "efficiency"}
    workers = []
    throughputs = []
    speedups = []
    efficiencies = []

    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            print(f"Error: CSV must contain columns {sorted(required)}.")
            return 1
        for row in reader:
            try:
                workers.append(int(row['workers']))
                throughputs.append(float(row['throughput']))
                speedups.append(float(row['speedup']))
                efficiencies.append(float(row['efficiency']))
            except (KeyError, ValueError):
                print("Error: invalid benchmark row.")
                return 1

    if not workers:
        print("Error: benchmark CSV contains no data rows.")
        return 1

    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt

        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))

        # Chart 1: Throughput (tasks/sec) vs Worker Count
        ax1.plot(workers, throughputs, marker='o', linewidth=2.5, color='#1f77b4', label='Measured Throughput')
        # Ideal linear line
        if len(throughputs) > 0:
            ideal = [throughputs[0] * w for w in workers]
            ax1.plot(workers, ideal, linestyle='--', color='#2ca02c', alpha=0.7, label='Ideal Linear Scaling')

        ax1.set_title("Throughput vs. Worker Threads", fontsize=13, fontweight='bold')
        ax1.set_xlabel("Worker Thread Count", fontsize=11)
        ax1.set_ylabel("Throughput (Tasks / Second)", fontsize=11)
        ax1.grid(True, linestyle=':', alpha=0.6)
        ax1.legend()

        # Chart 2: Speedup and Efficiency
        ax2.plot(workers, speedups, marker='s', linewidth=2.5, label='Speedup Factor')
        ax2.plot(workers, workers, linestyle='--', alpha=0.7, label='Linear 1.0x Speedup')
        ax2.set_title("Speedup vs. Worker Threads", fontsize=13, fontweight='bold')
        ax2.set_xlabel("Worker Thread Count", fontsize=11)
        ax2.set_ylabel("Speedup (x-fold)", fontsize=11)
        ax2.grid(True, linestyle=':', alpha=0.6)

        efficiency_ax = ax2.twinx()
        efficiency_ax.plot(workers, efficiencies, marker='^', linewidth=2.0, label='Efficiency (%)')
        efficiency_ax.set_ylabel("Efficiency (%)")
        efficiency_ax.set_ylim(bottom=0)

        handles1, labels1 = ax2.get_legend_handles_labels()
        handles2, labels2 = efficiency_ax.get_legend_handles_labels()
        ax2.legend(handles1 + handles2, labels1 + labels2, loc='best')

        os.makedirs(os.path.dirname(output_png), exist_ok=True)
        plt.tight_layout()
        plt.savefig(output_png, dpi=150)
        print(f"[Plot] Scaling chart saved to {output_png}")
    except Exception as e:
        print(f"[Plot] Note: Matplotlib rendering skipped ({e}). CSV data is available.")

    return 0

if __name__ == "__main__":
    generate_plot()

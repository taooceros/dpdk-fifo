#!/usr/bin/env python3
"""
Benchmark Results Analyzer

This script analyzes the benchmark results and generates performance graphs
and detailed analysis reports.
"""

import os
import sys
import glob
import json
import argparse
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path
from datetime import datetime
import pandas as pd

def parse_results_file(file_path):
    """Parse a results file and extract metrics."""
    results = {}
    
    try:
        with open(file_path, 'r') as f:
            for line in f:
                line = line.strip()
                if ':' in line:
                    key, value = line.split(':', 1)
                    if key == 'avg_throughput':
                        results[key] = float(value)
                    elif key == 'burst_size':
                        results[key] = int(value)
                    elif key == 'test_duration':
                        results[key] = int(value)
                    else:
                        results[key] = value
    except Exception as e:
        print(f"Error parsing {file_path}: {e}")
        return None
    
    return results

def load_benchmark_data(results_dir):
    """Load all benchmark data from results directory."""
    data = []
    
    # Find all results files
    pattern = os.path.join(results_dir, "results_burst_*.txt")
    result_files = glob.glob(pattern)
    
    for file_path in result_files:
        result = parse_results_file(file_path)
        if result and 'burst_size' in result and 'avg_throughput' in result:
            data.append(result)
    
    # Sort by burst size
    data.sort(key=lambda x: x['burst_size'])
    return data

def generate_throughput_graph(data, output_dir):
    """Generate throughput vs burst size graph."""
    if not data:
        print("No data available for graphing")
        return
    
    burst_sizes = [d['burst_size'] for d in data]
    throughputs = [d['avg_throughput'] for d in data]
    
    plt.figure(figsize=(12, 8))
    
    # Main plot
    plt.subplot(2, 1, 1)
    plt.plot(burst_sizes, throughputs, 'bo-', linewidth=2, markersize=8)
    plt.xlabel('Burst Size')
    plt.ylabel('Throughput (packets/sec)')
    plt.title('DPDK Packet Rate vs Burst Size')
    plt.grid(True, alpha=0.3)
    plt.xscale('log', base=2)
    
    # Annotate data points
    for i, (bs, tp) in enumerate(zip(burst_sizes, throughputs)):
        plt.annotate(f'{tp:.0f}', (bs, tp), textcoords="offset points", 
                    xytext=(0,10), ha='center')
    
    # Efficiency plot (normalized to peak)
    plt.subplot(2, 1, 2)
    max_throughput = max(throughputs)
    efficiency = [tp / max_throughput * 100 for tp in throughputs]
    
    plt.plot(burst_sizes, efficiency, 'ro-', linewidth=2, markersize=8)
    plt.xlabel('Burst Size')
    plt.ylabel('Efficiency (%)')
    plt.title('Relative Efficiency vs Burst Size')
    plt.grid(True, alpha=0.3)
    plt.xscale('log', base=2)
    plt.ylim(0, 105)
    
    # Annotate efficiency points
    for i, (bs, eff) in enumerate(zip(burst_sizes, efficiency)):
        plt.annotate(f'{eff:.1f}%', (bs, eff), textcoords="offset points", 
                    xytext=(0,10), ha='center')
    
    plt.tight_layout()
    
    # Save graph
    graph_path = os.path.join(output_dir, 'throughput_analysis.png')
    plt.savefig(graph_path, dpi=300, bbox_inches='tight')
    print(f"Throughput graph saved to: {graph_path}")
    
    # Also save as PDF
    pdf_path = os.path.join(output_dir, 'throughput_analysis.pdf')
    plt.savefig(pdf_path, bbox_inches='tight')
    print(f"PDF graph saved to: {pdf_path}")
    
    plt.show()

def generate_detailed_report(data, output_dir):
    """Generate detailed analysis report."""
    if not data:
        print("No data available for analysis")
        return
    
    report_path = os.path.join(output_dir, 'detailed_analysis.txt')
    
    burst_sizes = [d['burst_size'] for d in data]
    throughputs = [d['avg_throughput'] for d in data]
    
    # Calculate statistics
    max_throughput = max(throughputs)
    min_throughput = min(throughputs)
    avg_throughput = np.mean(throughputs)
    
    best_burst = burst_sizes[throughputs.index(max_throughput)]
    worst_burst = burst_sizes[throughputs.index(min_throughput)]
    
    # Calculate improvement ratios
    improvement_from_min = max_throughput / min_throughput
    
    with open(report_path, 'w') as f:
        f.write("=== DETAILED BENCHMARK ANALYSIS ===\n")
        f.write(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"Total test configurations: {len(data)}\n\n")
        
        f.write("=== PERFORMANCE SUMMARY ===\n")
        f.write(f"Maximum throughput: {max_throughput:,.0f} packets/sec (burst size {best_burst})\n")
        f.write(f"Minimum throughput: {min_throughput:,.0f} packets/sec (burst size {worst_burst})\n")
        f.write(f"Average throughput: {avg_throughput:,.0f} packets/sec\n")
        f.write(f"Performance range: {improvement_from_min:.2f}x improvement from worst to best\n\n")
        
        f.write("=== DETAILED RESULTS ===\n")
        f.write(f"{'Burst Size':<12} {'Throughput':<15} {'Relative Perf':<15} {'Notes':<20}\n")
        f.write(f"{'-'*12:<12} {'-'*15:<15} {'-'*15:<15} {'-'*20:<20}\n")
        
        for d in data:
            bs = d['burst_size']
            tp = d['avg_throughput']
            rel_perf = tp / max_throughput * 100
            
            notes = ""
            if bs == best_burst:
                notes += "BEST "
            if bs == worst_burst:
                notes += "WORST "
            if rel_perf > 95:
                notes += "EXCELLENT "
            elif rel_perf > 80:
                notes += "GOOD "
            elif rel_perf < 50:
                notes += "POOR "
            
            f.write(f"{bs:<12} {tp:,.0f}{'':>7} {rel_perf:>6.1f}%{'':>7} {notes:<20}\n")
        
        f.write("\n=== RECOMMENDATIONS ===\n")
        
        # Find top 3 performers
        sorted_data = sorted(data, key=lambda x: x['avg_throughput'], reverse=True)
        top_3 = sorted_data[:3]
        
        f.write("Top 3 performing burst sizes:\n")
        for i, d in enumerate(top_3, 1):
            f.write(f"  {i}. Burst size {d['burst_size']}: {d['avg_throughput']:,.0f} packets/sec\n")
        
        f.write("\nOptimal burst size selection:\n")
        if best_burst <= 32:
            f.write("- Low latency applications: Consider burst sizes 1-16\n")
            f.write("- Balanced applications: Use burst size 32 or lower\n")
        else:
            f.write("- High throughput applications: Use larger burst sizes (64-256)\n")
            f.write("- Low latency applications: Consider smaller burst sizes (1-32)\n")
        
        f.write(f"- Best overall performance: Burst size {best_burst}\n")
        
        # Efficiency analysis
        f.write("\n=== EFFICIENCY ANALYSIS ===\n")
        efficient_configs = [d for d in data if d['avg_throughput'] / max_throughput > 0.9]
        f.write(f"Configurations achieving >90% efficiency: {len(efficient_configs)}\n")
        for d in efficient_configs:
            eff = d['avg_throughput'] / max_throughput * 100
            f.write(f"  - Burst size {d['burst_size']}: {eff:.1f}% efficiency\n")
    
    print(f"Detailed analysis saved to: {report_path}")

def generate_csv_export(data, output_dir):
    """Export data to CSV for further analysis."""
    if not data:
        return
    
    csv_path = os.path.join(output_dir, 'benchmark_data.csv')
    
    # Create DataFrame
    df = pd.DataFrame(data)
    
    # Add calculated columns
    if 'avg_throughput' in df.columns:
        max_tp = df['avg_throughput'].max()
        df['efficiency_percent'] = (df['avg_throughput'] / max_tp * 100).round(1)
        df['throughput_mpps'] = (df['avg_throughput'] / 1_000_000).round(3)
    
    # Reorder columns
    column_order = ['burst_size', 'avg_throughput', 'throughput_mpps', 
                   'efficiency_percent', 'test_duration', 'timestamp']
    available_columns = [col for col in column_order if col in df.columns]
    df = df[available_columns]
    
    df.to_csv(csv_path, index=False)
    print(f"CSV data exported to: {csv_path}")

def main():
    parser = argparse.ArgumentParser(description='Analyze DPDK benchmark results')
    parser.add_argument('results_dir', help='Results directory path')
    parser.add_argument('-o', '--output', help='Output directory for analysis', 
                       default=None)
    parser.add_argument('--no-graphs', action='store_true', 
                       help='Skip graph generation')
    parser.add_argument('--no-report', action='store_true',
                       help='Skip detailed report generation')
    parser.add_argument('--csv-only', action='store_true',
                       help='Only generate CSV export')
    
    args = parser.parse_args()
    
    # Validate input directory
    if not os.path.exists(args.results_dir):
        print(f"Error: Results directory '{args.results_dir}' not found")
        sys.exit(1)
    
    # Setup output directory
    if args.output:
        output_dir = args.output
    else:
        output_dir = os.path.join(args.results_dir, 'analysis')
    
    os.makedirs(output_dir, exist_ok=True)
    
    print(f"Analyzing results from: {args.results_dir}")
    print(f"Output directory: {output_dir}")
    
    # Load data
    data = load_benchmark_data(args.results_dir)
    
    if not data:
        print("No valid benchmark data found!")
        sys.exit(1)
    
    print(f"Found {len(data)} test results")
    
    # Generate outputs based on arguments
    if args.csv_only:
        generate_csv_export(data, output_dir)
    else:
        if not args.no_graphs:
            try:
                generate_throughput_graph(data, output_dir)
            except Exception as e:
                print(f"Error generating graphs: {e}")
        
        if not args.no_report:
            generate_detailed_report(data, output_dir)
        
        generate_csv_export(data, output_dir)
    
    print("\nAnalysis complete!")

if __name__ == '__main__':
    main()

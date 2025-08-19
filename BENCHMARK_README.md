# DPDK Burst Size Benchmark Suite

This benchmark suite is designed to test packet rates between a host system and a Marvell SmartNIC with different burst sizes. The SmartNIC is connected via Ethernet but without internet connectivity, only serial connection for management.

## Overview

The benchmark suite consists of three main components:

1. **`bench.zsh`** - Comprehensive benchmark script that tests multiple burst sizes
2. **`marvell_bench.zsh`** - Quick connectivity and testing script for Marvell SmartNIC
3. **`analyze_results.py`** - Results analysis and visualization script

## Quick Start

**Note**: The scripts use sudo only for DPDK applications, not for the entire script. You may be prompted for your password when DPDK client/server applications start.

### 1. Initial Setup and Connectivity Test

```bash
# Make scripts executable
chmod +x bench.zsh marvell_bench.zsh

# Check system info and setup environment
./marvell_bench.zsh setup
./marvell_bench.zsh info

# Quick connectivity test
./marvell_bench.zsh test
```

### 2. Run Full Benchmark

```bash
# Run comprehensive benchmark (tests burst sizes 1,2,4,8,16,32,64,128,256)
./bench.zsh

# Or with custom settings
./bench.zsh -d 60 -p 2a:00.0 -r release
```

### 3. Analyze Results

```bash
# Install Python dependencies
pip3 install -r requirements.txt

# Analyze results and generate graphs
python3 analyze_results.py benchmark_results_YYYYMMDD_HHMMSS/
```

## Detailed Usage

### bench.zsh - Main Benchmark Script

**Purpose**: Comprehensive benchmarking with multiple burst sizes and detailed logging.

**Usage**:
```bash
./bench.zsh [OPTIONS]

Options:
  -h, --help              Show help message
  -d, --duration SECONDS  Test duration per burst size (default: 30)
  -r, --release MODE      Build mode: release|debug (default: release)
  -o, --output DIR        Results directory prefix (default: benchmark_results)
  -p, --pci ADDRESS       Client PCI address (default: 2a:00.0)
  -t, --timeout SECONDS   Server startup timeout (default: 5)
```

**Examples**:
```bash
# Default 30-second tests for all burst sizes
./bench.zsh

# 60-second tests in debug mode
./bench.zsh -d 60 -r debug

# Custom PCI address and output directory
./bench.zsh -p 1a:00.0 -o my_results

# Quick 10-second tests
./bench.zsh -d 10
```

**What it does**:
- Tests burst sizes: 1, 2, 4, 8, 16, 32, 64, 128, 256
- For each burst size:
  - Starts server with specified burst configuration
  - Runs client for specified duration
  - Captures throughput, latency, and performance metrics
  - Logs detailed results
- Generates summary report with best performing configurations
- Creates CSV data for further analysis

### marvell_bench.zsh - Quick Testing Script

**Purpose**: Fast connectivity testing and environment setup for Marvell SmartNIC.

**Usage**:
```bash
./marvell_bench.zsh [OPTIONS] [COMMAND]

Commands:
  test        Run quick connectivity test (default)
  info        Show system information
  setup       Setup environment for testing
  full        Run full benchmark (calls bench.zsh)

Options:
  -h, --help              Show help
  -p, --pci ADDRESS       Marvell PCI address (default: 2a:00.0)
  -c, --cores RANGE       Host CPU cores (default: 0-7)
  -d, --duration SECONDS  Test duration (default: 10)
  -b, --bursts LIST       Comma-separated burst sizes (default: 1,8,32,128)
  -r, --release MODE      Build mode (default: release)
```

**Examples**:
```bash
# Quick connectivity test
./marvell_bench.zsh

# Show system information
./marvell_bench.zsh info

# Setup environment (huge pages, kernel modules)
./marvell_bench.zsh setup

# Test specific burst sizes
./marvell_bench.zsh -b "1,16,64,256" test

# Run full benchmark
./marvell_bench.zsh full
```

### analyze_results.py - Results Analysis

**Purpose**: Analyze benchmark results and generate performance graphs and reports.

**Usage**:
```bash
python3 analyze_results.py [OPTIONS] RESULTS_DIR

Options:
  -h, --help              Show help
  -o, --output DIR        Output directory for analysis
  --no-graphs             Skip graph generation
  --no-report             Skip detailed report generation
  --csv-only              Only generate CSV export
```

**Examples**:
```bash
# Full analysis with graphs and reports
python3 analyze_results.py benchmark_results_20240101_120000/

# CSV export only
python3 analyze_results.py --csv-only results_dir/

# Custom output directory
python3 analyze_results.py -o my_analysis/ results_dir/
```

**Generated outputs**:
- `throughput_analysis.png/pdf` - Performance graphs
- `detailed_analysis.txt` - Comprehensive analysis report
- `benchmark_data.csv` - Raw data in CSV format

## Configuration

### Environment Variables

```bash
# Marvell SmartNIC PCI address
export MARVELL_PCI="2a:00.0"

# CPU cores for host application
export HOST_CORES="0-7"

# Test duration in seconds
export TEST_DURATION="30"

# Build mode
export RELEASE="release"

# Results directory
export RESULTS_DIR="my_benchmark_results"
```

### System Requirements

**Hardware**:
- DPDK-compatible CPU with multiple cores
- Marvell SmartNIC connected via PCIe
- Sufficient RAM (>= 4GB recommended)
- Huge pages support

**Software**:
- Linux with DPDK support
- DPDK libraries installed
- xmake build system
- Python 3.7+ (for analysis)
- zsh shell
- sudo access for DPDK applications

**DPDK Setup**:
```bash
# Setup huge pages
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages

# Load DPDK kernel modules
sudo modprobe uio
sudo modprobe igb_uio  # or vfio-pci

# Bind Marvell SmartNIC to DPDK
sudo dpdk-devbind.py --bind=igb_uio 2a:00.0
```

## Troubleshooting

### Common Issues

**1. "Marvell SmartNIC not found"**
```bash
# Check PCI devices
lspci | grep -i ethernet

# Check DPDK device binding
sudo dpdk-devbind.py --status
```

**2. "Failed to setup huge pages"**
```bash
# Check current huge pages
cat /proc/meminfo | grep -i huge

# Setup huge pages manually
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages
```

**3. "Permission denied"**
```bash
# The scripts will automatically use sudo for DPDK applications only
# You may be prompted for password when DPDK apps start

# For passwordless operation, setup sudo without password for DPDK:
echo "$USER ALL=(ALL) NOPASSWD: /path/to/dpdk/apps/*" | sudo tee /etc/sudoers.d/dpdk-nopasswd
```

**4. "Build failed"**
```bash
# Check DPDK installation
pkg-config --exists libdpdk && echo "DPDK found" || echo "DPDK missing"

# Rebuild
xmake f -c && xmake f -m release && xmake
```

### Performance Optimization

**1. CPU affinity**:
- Use isolated CPU cores for DPDK applications
- Set `HOST_CORES` to cores not used by OS

**2. NUMA awareness**:
- Check NUMA topology: `numactl --hardware`
- Use cores and memory from same NUMA node as SmartNIC

**3. Interrupt handling**:
- Consider disabling irqbalance
- Pin interrupts to specific cores

## Results Interpretation

### Metrics Explained

**Throughput**: Packets per second successfully transmitted/received
**Efficiency**: Relative performance compared to best configuration
**Latency**: Round-trip time for packet processing (when available)

### Expected Results

**Typical patterns**:
- Low burst sizes (1-8): Lower throughput, better latency
- Medium burst sizes (16-64): Balanced performance
- High burst sizes (128-256): Maximum throughput, higher latency

**Optimal configurations**:
- **Low latency applications**: Burst sizes 1-16
- **High throughput applications**: Burst sizes 64-256
- **Balanced applications**: Burst sizes 16-64

### Performance Analysis

The analysis script provides:
- **Performance curves**: Throughput vs burst size
- **Efficiency analysis**: Relative performance metrics
- **Recommendations**: Optimal burst sizes for different use cases
- **Detailed statistics**: Min/max/average performance

## Advanced Usage

### Custom Burst Size Testing

```bash
# Edit bench.zsh to test custom burst sizes
BURST_SIZES=(1 4 16 64 256 512)
```

### Extended Duration Testing

```bash
# Long-duration stability testing
./bench.zsh -d 300  # 5-minute tests per burst size
```

### Multi-PCI Testing

```bash
# Test multiple SmartNICs
for pci in 2a:00.0 2b:00.0; do
    ./bench.zsh -p $pci -o results_$pci
done
```

### Continuous Monitoring

```bash
# Run benchmark every hour
while true; do
    ./bench.zsh -d 60 -o continuous_$(date +%Y%m%d_%H%M%S)
    sleep 3600
done
```

## File Structure

```
dpdk-fifo/
├── bench.zsh              # Main benchmark script
├── marvell_bench.zsh      # Quick testing script
├── analyze_results.py     # Results analysis script
├── requirements.txt       # Python dependencies
├── BENCHMARK_README.md    # This documentation
├── benchmark_results_*/   # Results directories (generated)
│   ├── client_burst_*.log
│   ├── server_burst_*.log
│   ├── results_burst_*.txt
│   ├── benchmark_summary.txt
│   └── analysis/
│       ├── throughput_analysis.png
│       ├── detailed_analysis.txt
│       └── benchmark_data.csv
└── [existing project files]
```

## Support and Contributing

For issues, improvements, or questions about the benchmark suite:

1. Check the troubleshooting section above
2. Review log files in the results directory
3. Verify system configuration and DPDK setup
4. Consider running the quick connectivity test first

The benchmark suite is designed to be modular and extensible. You can modify burst sizes, test durations, and analysis parameters to suit your specific testing needs.

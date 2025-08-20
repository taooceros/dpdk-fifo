#!/usr/bin/env zsh
set -e

# Benchmark script for testing packet rates with different burst sizes
# between host and Marvell SmartNIC

# Default configuration
RELEASE="${RELEASE:-release}"
TEST_DURATION="${TEST_DURATION:-5}"  # seconds per test
RESULTS_DIR="${RESULTS_DIR:-benchmark_results}"
SERVER_TIMEOUT="${SERVER_TIMEOUT:-5}"  # seconds to wait for server startup
CLIENT_PCI="${CLIENT_PCI:-2a:00.0}"   # Marvell SmartNIC PCI address
MODE="${MODE:-client}"

# Burst sizes to test (powers of 2)
BURST_SIZES=(4 8 16 32 64 128)
UNIT_SIZES=(64 128 256 512 1024)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log() {
    echo -e "${BLUE}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

# Function to check if process is running
is_process_running() {
    local pid=$1
    kill -0 "$pid" 2>/dev/null
}

# Function to kill process and its children
kill_process_tree() {
    local pid=$1
    if is_process_running "$pid"; then
        # Kill child processes first
        pkill -P "$pid" 2>/dev/null || true
        # Then kill the main process
        kill "$pid" 2>/dev/null || true
        # Force kill if still running after 2 seconds
        sleep 2
        if is_process_running "$pid"; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    fi
}

# Function to setup results directory
setup_results_dir() {
    local timestamp=$(date '+%Y%m%d_%H%M%S')
    RESULTS_DIR="${RESULTS_DIR}/${timestamp}"
    mkdir -p "$RESULTS_DIR"
    log "Results will be saved to: $RESULTS_DIR"
}

# Function to build the project
build_project() {
    log "Building project..."
    xmake f -m "$RELEASE"
    xmake
    
    if [[ ! -f "build/linux/${ARCH}/${RELEASE}/client" ]] || [[ ! -f "build/linux/${ARCH}/${RELEASE}/server" ]]; then
        error "Build failed - binaries not found"
        exit 1
    fi
    success "Build completed successfully"
}

# Function to start server in background
start_server() {
    local burst_size=$1
    local server_log="$RESULTS_DIR/server_burst_${burst_size}.log"
    
    echo ""
    log "Starting server with burst size $burst_size..."
    
    # Start server in background with output redirected
    sudo timeout "$((TEST_DURATION))" \
        "build/linux/${ARCH}/${RELEASE}/server" \
        -l 9-15 --file-prefix=server -- \
        --tx-burst "$burst_size" --rx-burst "$burst_size" \
        2>&1 > "$server_log"
    
    local server_pid=$!
    echo "$server_pid" > "$RESULTS_DIR/server_${burst_size}.pid"
    
    # Wait for server to initialize
    sleep "$SERVER_TIMEOUT"
    
    if ! is_process_running "$server_pid"; then
        error "Server failed to start. Check log: $server_log"
        return 1
    fi
    
    echo ""
    success "Server started (PID: $server_pid)"
    return 0
}

# Function to run client test
run_client_test() {
    local burst_size=$1
    local unit_size=$2
    local client_log="$RESULTS_DIR/client_burst_${burst_size}.log"
    local client_results="$RESULTS_DIR/results_burst_${burst_size}.txt"
    
    echo ""
    log "Running client test with burst size $burst_size for ${TEST_DURATION}s..."
    
    # Start client and capture output
    sudo timeout "$TEST_DURATION" \
        "build/linux/${ARCH}/${RELEASE}/client" \
        -l 0-7 -a "$CLIENT_PCI" --file-prefix=client -- \
        --tx-burst "$burst_size" --rx-burst "$burst_size" --size "$unit_size" \
        > "$client_log" 2>&1 || true
    
    # Extract metrics from client log
    if [[ -f "$client_log" ]]; then
        # Extract throughput data
        grep "Throughput:" "$client_log" | tail -10 > "$client_results" 2>/dev/null || true
        
        # Calculate average throughput from last 10 measurements
        local avg_throughput=$(grep "Throughput:" "$client_log" | tail -10 | \
            awk '{sum += $2; count++} END {if(count > 0) print sum/count; else print 0}')
        
        echo "burst_size:$burst_size" >> "$client_results"
        echo "unit_size:$unit_size" >> "$client_results"
        echo "avg_throughput:$avg_throughput" >> "$client_results"
        echo "test_duration:$TEST_DURATION" >> "$client_results"
        echo "timestamp:$(date '+%Y-%m-%d %H:%M:%S')" >> "$client_results"
        
        echo ""
        success "Client test completed. Avg throughput: $avg_throughput packets/sec"
    else
        error "Client log not found: $client_log"
    fi
}

run_server_test() {
    local burst_size=$1
    local unit_size=$2

    local server_log="$RESULTS_DIR/server_burst_${burst_size}.log"
    local server_results="$RESULTS_DIR/results_burst_${burst_size}.txt"
    
    echo ""
    log "Running server test with burst size $burst_size for ${TEST_DURATION}s..."

    # Start server and capture output
    sudo timeout "$TEST_DURATION" \
        "build/linux/${ARCH}/${RELEASE}/server" \
        -l 9-15 --file-prefix=server -- \
        --tx-burst "$burst_size" --rx-burst "$burst_size" --size "$unit_size" \
        > "$server_log" 2>&1 || true

    # Extract metrics from server log
    if [[ -f "$server_log" ]]; then
        # Extract throughput data
        grep "Throughput:" "$server_log" | tail -10 > "$server_results" 2>/dev/null || true

        # Calculate average throughput and hit rate from last 10 measurements
        # Expected format: "throughput: 21971517.134275, hit: 0.003079"
        metrics=$(
            grep -i 'throughput:' "$server_log" | tail -n 10 | awk '
            {
                # Extract throughput and hit values from format: "throughput: X, hit: Y"
                if ($0 ~ /throughput:/) {
                    # Split on comma to get throughput and hit parts
                    n = split($0, parts, ",")
                    if (n >= 2) {
                        # Extract throughput value
                        if (match(parts[1], /[0-9]+\.?[0-9]*([eE][+-]?[0-9]+)?/)) {
                            throughput_val = substr(parts[1], RSTART, RLENGTH)
                            throughput_sum += throughput_val
                        }
                        # Extract hit value
                        if (match(parts[2], /[0-9]+\.?[0-9]*([eE][+-]?[0-9]+)?/)) {
                            hit_val = substr(parts[2], RSTART, RLENGTH)
                            hit_sum += hit_val
                        }
                        cnt++
                    }
                }
            }
            END { 
                if (cnt > 0) {
                    printf "%.6f %.6f\n", throughput_sum/cnt, hit_sum/cnt
                } else {
                    print "0 0"
                }
            }'
        )
        
        avg_throughput=$(echo "$metrics" | awk '{print $1}')
        avg_hit_rate=$(echo "$metrics" | awk '{print $2}')

        echo "burst_size:$burst_size" >> "$server_results"
        echo "unit_size:$unit_size" >> "$server_results"
        echo "avg_throughput:$avg_throughput" >> "$server_results"
        echo "avg_hit_rate:$avg_hit_rate" >> "$server_results"
        echo "test_duration:$TEST_DURATION" >> "$server_results"
        echo "timestamp:$(date '+%Y-%m-%d %H:%M:%S')" >> "$server_results"
        
        echo ""
        success "Server test completed. Avg throughput: $avg_throughput packets/sec, Hit rate: $avg_hit_rate"
    else
        error "Server log not found: $server_log"
    fi
}


# Function to run single burst size test
run_burst_test() {
    local burst_size=$1
    local unit_size=$2
    
    echo ""
    echo "========================================"
    log "Starting test for burst size: $burst_size"
    echo "========================================"


    # Run client test
    if [[ "$MODE" == "client" ]]; then
        run_client_test "$burst_size" "$unit_size"
    elif [[ "$MODE" == "server" ]]; then
        run_server_test "$burst_size" "$unit_size"
    fi
    sleep 2
    
    echo ""
    success "Completed test for burst size: $burst_size"
    echo ""
    echo "========================================"
    echo ""
}

# Function to generate summary report
generate_summary() {
    local summary_file="$RESULTS_DIR/benchmark_summary.txt"
    
    {
        echo "=== DPDK Burst Size Benchmark Summary ==="
        echo "Test Date: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "Test Duration per burst size: ${TEST_DURATION}s"
        echo "Client PCI Address: $CLIENT_PCI"
        echo "Build Mode: $RELEASE"
        echo ""
        echo "Results by Burst Size:"
        echo "----------------------"
        if [[ "$MODE" == "server" ]]; then
            printf "%-12s %-20s %-15s %-15s\n" "Burst Size" "Avg Throughput" "Avg Hit Rate" "Status"
            printf "%-12s %-20s %-15s %-15s\n" "----------" "---------------" "------------" "------"
        else
            printf "%-12s %-20s %-15s %-15s\n" "Burst Size" "Avg Throughput" "Unit Size" "Status"
            printf "%-12s %-20s %-15s %-15s\n" "----------" "---------------" "------------" "------"
        fi
        
        for burst_size in "${BURST_SIZES[@]}"; do
            for unit_size in "${UNIT_SIZES[@]}"; do 
                local results_file="$RESULTS_DIR/results_burst_${burst_size}_${unit_size}.txt"
                if [[ -f "$results_file" ]]; then
                    local throughput=$(grep "avg_throughput:" "$results_file" | cut -d: -f2)
                    if [[ -n "$throughput" ]] && [[ "$throughput" != "0" ]]; then
                        if [[ "$MODE" == "server" ]]; then
                            local hit_rate=$(grep "avg_hit_rate:" "$results_file" | cut -d: -f2)
                            printf "%-12s %-20.0f %-15.6f %-15s\n" "$burst_size" "$throughput" "$hit_rate" "SUCCESS"
                        else
                            local unit_size=$(grep "unit_size:" "$results_file" | cut -d: -f2)
                            printf "%-12s %-20.0f %-15s %-15s\n" "$burst_size" "$throughput" "$unit_size" "SUCCESS"
                        fi
                    else
                        if [[ "$MODE" == "server" ]]; then
                            printf "%-12s %-20s %-15s %-15s\n" "$burst_size" "N/A" "N/A" "FAILED"
                        else
                            printf "%-12s %-20s %-15s\n" "$burst_size" "N/A" "FAILED"
                        fi
                    fi
                else
                    if [[ "$MODE" == "server" ]]; then
                        printf "%-12s %-20s %-15s %-15s\n" "$burst_size" "N/A" "N/A" "NOT_RUN"
                    else
                        printf "%-12s %-20s %-15s\n" "$burst_size" "N/A" "NOT_RUN"
                    fi
                fi
            done
        done
        
        echo ""
        echo "Detailed logs available in: $RESULTS_DIR"
        echo ""
        
        # Find best performing burst size
        local best_burst=0
        local best_throughput=0
        for burst_size in "${BURST_SIZES[@]}"; do
            local results_file="$RESULTS_DIR/results_burst_${burst_size}.txt"
            if [[ -f "$results_file" ]]; then
                local throughput=$(grep "avg_throughput:" "$results_file" | cut -d: -f2)
                if [[ -n "$throughput" ]] && (( $(echo "$throughput > $best_throughput" | bc -l) )); then
                    best_throughput=$throughput
                    best_burst=$burst_size
                fi
            fi
        done
        
        if [[ "$best_burst" != "0" ]]; then
            echo "Best Performance:"
            echo "Burst Size: $best_burst"
            echo "Throughput: $(printf "%.0f" "$best_throughput") packets/sec"
        fi
        
    } > "$summary_file"
    
    # Display summary
    echo ""
    cat "$summary_file"
    echo ""
    success "Summary saved to: $summary_file"
}

# Function to cleanup on exit
cleanup() {
    log "Cleaning up..."
    
    # Kill any remaining processes
    for burst_size in "${BURST_SIZES[@]}"; do
        stop_server "$burst_size" 2>/dev/null || true
    done
    
    # Kill any remaining DPDK processes
    sudo pkill -f "client\|server" 2>/dev/null || true
    
    log "Cleanup completed"

    exit 0
}

# Main execution
main() {
    echo ""
    echo "========================================"
    log "Starting DPDK Burst Size Benchmark"
    echo "========================================"
    echo ""
    log "Testing burst sizes: ${BURST_SIZES[*]}"
    log "Test duration per burst size: ${TEST_DURATION}s"
    echo ""
    
    ARCH=$(uname -m)

    if [[ "$ARCH" == "x86_64" ]]; then
        ARCH="x86_64"
    elif [[ "$ARCH" == "aarch64" ]]; then
        ARCH="arm64"
    else
        error "Unsupported architecture: $ARCH"
        exit 1
    fi

    # Setup
    setup_results_dir
    build_project
    
    # Setup signal handlers
    trap cleanup EXIT INT TERM
    
    # Run tests for each burst size
    local failed_tests=0
    for burst_size in "${BURST_SIZES[@]}"; do
        if ! run_burst_test "$burst_size"; then
            ((failed_tests++))
            warn "Test failed for burst size $burst_size"
        fi
    done
    
    # Generate summary
    echo ""
    echo "========================================"
    log "Generating benchmark summary"
    echo "========================================"
    generate_summary
    
    # Final status
    echo ""
    echo "========================================"
    if [[ "$failed_tests" -eq 0 ]]; then
        success "All tests completed successfully!"
    else
        warn "$failed_tests tests failed. Check individual logs for details."
    fi
    echo ""
    log "Benchmark completed. Results in: $RESULTS_DIR"
    echo "========================================"
    echo ""
}

# Help function
show_help() {
    cat << EOF
DPDK Burst Size Benchmark Script

Usage: $0 [OPTIONS]

Options:
    -h, --help              Show this help message
    -d, --duration SECONDS  Test duration per burst size (default: 5)
    -r, --release MODE      Build mode: release|debug (default: release)
    -o, --output DIR        Results directory prefix (default: benchmark_results)
    -p, --pci ADDRESS       Client PCI address (default: 2a:00.0)
    -t, --timeout SECONDS   Server startup timeout (default: 5)
    -m, --mode MODE         Test mode: client|server (default: client)

Environment Variables:
    TEST_DURATION   Test duration in seconds
    RELEASE         Build mode (release/debug)
    RESULTS_DIR     Results directory prefix
    CLIENT_PCI      Client PCI address

Examples:
    $0                              # Run with defaults
    $0 -d 60 -r debug              # 60s tests in debug mode
    $0 -p 1a:00.0 -o my_results    # Custom PCI and output dir

EOF
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -d|--duration)
            TEST_DURATION="$2"
            shift 2
            ;;
        -r|--release)
            RELEASE="$2"
            shift 2
            ;;
        -o|--output)
            RESULTS_DIR="$2"
            shift 2
            ;;
        -p|--pci)
            CLIENT_PCI="$2"
            shift 2
            ;;
        -t|--timeout)
            SERVER_TIMEOUT="$2"
            shift 2
            ;;
        -m|--mode)
            MODE="$2"
            shift 2
            ;;
        *)
            error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Validate numeric arguments
if ! [[ "$TEST_DURATION" =~ ^[0-9]+$ ]] || [[ "$TEST_DURATION" -lt 5 ]]; then
    error "TEST_DURATION must be a number >= 5"
    exit 1
fi

if ! [[ "$SERVER_TIMEOUT" =~ ^[0-9]+$ ]] || [[ "$SERVER_TIMEOUT" -lt 1 ]]; then
    error "SERVER_TIMEOUT must be a number >= 1"
    exit 1
fi

# Check if sudo is available for DPDK operations
if ! sudo -n true 2>/dev/null; then
    warn "DPDK applications will require sudo password prompts"
    log "You may be prompted for password when running DPDK client/server"
fi

# Check required tools
for tool in xmake timeout bc; do
    if ! command -v "$tool" &> /dev/null; then
        error "Required tool not found: $tool"
        exit 1
    fi
done

# Run main function
main "$@"

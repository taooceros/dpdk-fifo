#!/usr/bin/env zsh
set -e

# Quick benchmark script specifically for Marvell SmartNIC testing
# This script is optimized for testing the connection to the Marvell SmartNIC
# connected via Ethernet without internet access

# Default configuration
RELEASE="${RELEASE:-release}"
MARVELL_PCI="${MARVELL_PCI:-2a:00.0}"
HOST_CORES="${HOST_CORES:-0-7}"
TEST_DURATION="${TEST_DURATION:-10}"
QUICK_BURSTS="${QUICK_BURSTS:-1,8,32,128}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1" >&2; }
success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARNING]${NC} $1"; }

# Function to check Marvell SmartNIC connectivity
check_marvell_connection() {
    log "Checking Marvell SmartNIC connection..."
    
    # Check if PCI device exists
    if ! lspci -s "$MARVELL_PCI" &>/dev/null; then
        error "Marvell SmartNIC not found at PCI address $MARVELL_PCI"
        error "Available network devices:"
        lspci | grep -i "ethernet\|network" || true
        return 1
    fi
    
    # Get device info
    local device_info=$(lspci -s "$MARVELL_PCI" -v 2>/dev/null | head -3)
    success "Found Marvell SmartNIC:"
    echo "$device_info" | sed 's/^/  /'
    
    # Check if DPDK can bind to it
    if command -v dpdk-devbind.py &>/dev/null; then
        log "DPDK device status:"
        sudo dpdk-devbind.py --status 2>/dev/null | grep -A5 -B5 "$MARVELL_PCI" || warn "Device not shown in DPDK status"
    fi
    
    return 0
}

# Function to run a quick connectivity test
test_connectivity() {
    local burst_size=$1
    log "Testing connectivity with burst size $burst_size..."
    
    # Build if needed
    if [[ ! -f "build/linux/x86_64/${RELEASE}/client" ]]; then
        log "Building project..."
        xmake f -m "$RELEASE" && xmake
    fi
    
    # Create temporary log file
    local temp_log=$(mktemp)
    
    # Run client for short duration to test connectivity
    sudo timeout 5 "build/linux/x86_64/${RELEASE}/client" \
        -l "$HOST_CORES" -a "$MARVELL_PCI" --file-prefix=marvell_test -- \
        --tx-burst "$burst_size" --rx-burst "$burst_size" \
        > "$temp_log" 2>&1 || true
    
    # Check if we got any output indicating successful initialization
    if grep -q "Starting client" "$temp_log" && grep -q "SRPEndpoint started" "$temp_log"; then
        success "Client initialized successfully with burst size $burst_size"
        
        # Check for any error messages
        if grep -qi "error\|fail" "$temp_log"; then
            warn "Some warnings/errors detected:"
            grep -i "error\|fail" "$temp_log" | head -3
        fi
        
        rm -f "$temp_log"
        return 0
    else
        error "Client failed to initialize with burst size $burst_size"
        echo "Last few lines of output:"
        tail -5 "$temp_log"
        rm -f "$temp_log"
        return 1
    fi
}

# Function to run quick performance test
quick_performance_test() {
    log "Running quick performance test with multiple burst sizes..."
    
    # Convert comma-separated list to array
    local IFS=','
    local burst_array=($QUICK_BURSTS)
    
    echo ""
    printf "%-12s %-15s %-10s\n" "Burst Size" "Status" "Notes"
    printf "%-12s %-15s %-10s\n" "----------" "-------" "-----"
    
    local successful_tests=0
    for burst_size in "${burst_array[@]}"; do
        if test_connectivity "$burst_size"; then
            printf "%-12s %-15s %-10s\n" "$burst_size" "SUCCESS" "OK"
            ((successful_tests++))
        else
            printf "%-12s %-15s %-10s\n" "$burst_size" "FAILED" "Check logs"
        fi
        sleep 1  # Brief pause between tests
    done
    
    echo ""
    if [[ $successful_tests -eq ${#burst_array[@]} ]]; then
        success "All burst sizes tested successfully! Ready for full benchmark."
        log "To run full benchmark: ./bench.zsh -p $MARVELL_PCI -d $TEST_DURATION"
    elif [[ $successful_tests -gt 0 ]]; then
        warn "$successful_tests/${#burst_array[@]} tests passed. Some burst sizes may have issues."
    else
        error "All tests failed. Check Marvell SmartNIC connection and configuration."
        return 1
    fi
}

# Function to show system information
show_system_info() {
    log "System Information for Marvell SmartNIC Testing"
    echo ""
    
    echo "Hardware:"
    echo "  CPU cores available: $(nproc)"
    echo "  Memory: $(free -h | awk 'NR==2{printf "%.1f GB", $2/1024/1024/1024}')"
    echo "  NUMA nodes: $(lscpu | grep "NUMA node(s)" | awk '{print $3}')"
    echo ""
    
    echo "DPDK Configuration:"
    echo "  Target PCI: $MARVELL_PCI"
    echo "  Host cores: $HOST_CORES"
    echo "  Build mode: $RELEASE"
    echo ""
    
    echo "Huge pages:"
    if [[ -f /proc/meminfo ]]; then
        grep -i huge /proc/meminfo | sed 's/^/  /'
    fi
    echo ""
    
    echo "Network devices:"
    lspci | grep -i "ethernet\|network" | sed 's/^/  /'
}

# Function to setup environment for Marvell testing
setup_environment() {
    log "Setting up environment for Marvell SmartNIC testing..."
    
    # Check if sudo is available for DPDK operations
    if ! sudo -n true 2>/dev/null; then
        warn "DPDK operations will require sudo password prompts"
        log "You may be prompted for password during environment setup"
    fi
    
    # Setup huge pages if needed
    local hugepages=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo "0")
    if [[ "$hugepages" -lt 1024 ]]; then
        log "Setting up huge pages..."
        echo 1024 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
        success "Huge pages configured"
    fi
    
    # Load required kernel modules
    for module in uio igb_uio vfio-pci; do
        if ! lsmod | grep -q "^$module "; then
            if modinfo "$module" &>/dev/null; then
                log "Loading kernel module: $module"
                sudo modprobe "$module" 2>/dev/null || warn "Failed to load $module"
            fi
        fi
    done
    
    success "Environment setup complete"
}

# Show help
show_help() {
    cat << EOF
Marvell SmartNIC Quick Benchmark Script

Usage: $0 [OPTIONS] [COMMAND]

Commands:
    test        Run quick connectivity test (default)
    info        Show system information
    setup       Setup environment for testing
    full        Run full benchmark (calls bench.zsh)

Options:
    -h, --help              Show this help
    -p, --pci ADDRESS       Marvell PCI address (default: 2a:00.0)
    -c, --cores RANGE       Host CPU cores (default: 0-7)
    -d, --duration SECONDS  Test duration (default: 10)
    -b, --bursts LIST       Comma-separated burst sizes (default: 1,8,32,128)
    -r, --release MODE      Build mode (default: release)

Examples:
    $0                          # Quick connectivity test
    $0 info                     # Show system info
    $0 setup                    # Setup environment
    $0 -p 1a:00.0 test         # Test with different PCI
    $0 -b "1,16,64,256" test   # Test with custom burst sizes
    $0 full                     # Run full benchmark

Environment Variables:
    MARVELL_PCI     Marvell SmartNIC PCI address
    HOST_CORES      CPU cores for host application
    TEST_DURATION   Test duration in seconds
    QUICK_BURSTS    Comma-separated burst sizes to test

EOF
}

# Parse command line arguments
COMMAND="test"
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        -p|--pci)
            MARVELL_PCI="$2"
            shift 2
            ;;
        -c|--cores)
            HOST_CORES="$2"
            shift 2
            ;;
        -d|--duration)
            TEST_DURATION="$2"
            shift 2
            ;;
        -b|--bursts)
            QUICK_BURSTS="$2"
            shift 2
            ;;
        -r|--release)
            RELEASE="$2"
            shift 2
            ;;
        test|info|setup|full)
            COMMAND="$1"
            shift
            ;;
        *)
            error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Main execution
main() {
    case "$COMMAND" in
        info)
            show_system_info
            ;;
        setup)
            setup_environment
            ;;
        test)
            log "Starting Marvell SmartNIC connectivity test"
            setup_environment
            check_marvell_connection
            quick_performance_test
            ;;
        full)
            log "Running full benchmark suite..."
            if [[ -f "./bench.zsh" ]]; then
                exec ./bench.zsh -p "$MARVELL_PCI" -d "$TEST_DURATION"
            else
                error "bench.zsh not found in current directory"
                exit 1
            fi
            ;;
        *)
            error "Unknown command: $COMMAND"
            show_help
            exit 1
            ;;
    esac
}

# Check required tools
for tool in lspci xmake timeout; do
    if ! command -v "$tool" &>/dev/null; then
        error "Required tool not found: $tool"
        exit 1
    fi
done

# Run main function
main

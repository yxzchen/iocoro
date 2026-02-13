#!/usr/bin/env bash

set -e  # Exit on error

# Color definitions
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Project configuration
PROJECT_NAME="iocoro"
BUILD_DIR="build"
INSTALL_PREFIX="/usr/local"

# Default options
BUILD_TYPE="Debug"
CLEAN=false
INSTALL=false
TEST=false
BENCH=false
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
VERBOSE=false
SANITIZER=""
COVERAGE=false

# Print help information
print_help() {
    cat << EOF
Usage: $0 [OPTIONS]

Options:
    -h, --help              Show this help message
    -d, --debug             Build in Debug mode (default)
    -r, --release           Build in Release mode
    -c, --clean             Clean build directory before building
    -i, --install           Install after building
    -t, --test              Run tests after building
    -b, --bench             Build benchmarks
    -j, --jobs NUM          Number of parallel build jobs (default: $JOBS)
    -v, --verbose           Show verbose build output
    --asan                  Enable AddressSanitizer
    --tsan                  Enable ThreadSanitizer
    --ubsan                 Enable UndefinedBehaviorSanitizer
    --coverage              Enable gcov-compatible coverage instrumentation
    --prefix PATH           Set installation prefix (default: $INSTALL_PREFIX)

Examples:
    $0                      # Build in Debug mode (default)
    $0 -r                   # Build in Release mode
    $0 -d -t                # Build in Debug mode and run tests
    $0 -b                   # Build benchmarks
    $0 -c -r -i             # Clean, build in Release, and install
    $0 --asan -d -t         # Debug with AddressSanitizer and tests
    $0 --coverage -t        # Build with coverage instrumentation and run tests
EOF
}

# Print colored messages
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                print_help
                exit 0
                ;;
            -d|--debug)
                BUILD_TYPE="Debug"
                shift
                ;;
            -r|--release)
                BUILD_TYPE="Release"
                shift
                ;;
            -c|--clean)
                CLEAN=true
                shift
                ;;
            -i|--install)
                INSTALL=true
                shift
                ;;
            -t|--test)
                TEST=true
                shift
                ;;
            -b|--bench)
                BENCH=true
                shift
                ;;
            -j|--jobs)
                JOBS="$2"
                shift 2
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            --asan)
                SANITIZER="address"
                BUILD_TYPE="Debug"
                shift
                ;;
            --tsan)
                SANITIZER="thread"
                BUILD_TYPE="Debug"
                shift
                ;;
            --ubsan)
                SANITIZER="undefined"
                BUILD_TYPE="Debug"
                shift
                ;;
            --prefix)
                INSTALL_PREFIX="$2"
                shift 2
                ;;
            --coverage)
                COVERAGE=true
                BUILD_TYPE="Debug"
                shift
                ;;
            *)
                log_error "Unknown option: $1"
                print_help
                exit 1
                ;;
        esac
    done
}

# Check dependencies
check_dependencies() {
    log_info "Checking dependencies..."
    
    if ! command -v cmake &> /dev/null; then
        log_error "CMake is not installed"
        exit 1
    fi
    
    if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
        log_error "No C++ compiler found (g++ or clang++)"
        exit 1
    fi
    
    log_info "Dependency check passed"
}

# Clean build directory
clean_build() {
    if [ "$CLEAN" = true ]; then
        log_warn "Cleaning build directory: $BUILD_DIR"
        rm -rf "$BUILD_DIR"
    fi
}

# Configure project
configure_project() {
    log_info "Configuring project ($BUILD_TYPE mode)..."

    if [ "$COVERAGE" = true ] && [ -n "$SANITIZER" ]; then
        log_error "--coverage cannot be combined with sanitizer options"
        exit 1
    fi
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    CMAKE_ARGS=(
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
    )

    if [ "$TEST" = true ]; then
        CMAKE_ARGS+=(-DIOCORO_BUILD_TESTS=ON)
    else
        CMAKE_ARGS+=(-DIOCORO_BUILD_TESTS=OFF)
    fi
    
    if [ "$BENCH" = true ]; then
        CMAKE_ARGS+=(-DIOCORO_BUILD_BENCHMARKS=ON)
    fi

    if [ "$COVERAGE" = true ]; then
        log_info "Enabling coverage instrumentation"
        CMAKE_ARGS+=(-DIOCORO_ENABLE_COVERAGE=ON)
    fi
    
    # Add sanitizer options
    if [ -n "$SANITIZER" ]; then
        log_info "Enabling ${SANITIZER} sanitizer"
        # Prefer clang for ThreadSanitizer due to better platform support.
        if [ "$SANITIZER" = "thread" ]; then
            if command -v clang++ &> /dev/null && command -v clang &> /dev/null; then
                CMAKE_ARGS+=(-DCMAKE_C_COMPILER=clang)
                CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER=clang++)
            fi
        fi
        CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS="-fsanitize=${SANITIZER} -fno-omit-frame-pointer -g")
        CMAKE_ARGS+=(-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=${SANITIZER}")
    else
        # Ensure we don't accidentally reuse sanitizer flags from an existing build directory.
        CMAKE_ARGS+=(-DCMAKE_CXX_FLAGS=)
        CMAKE_ARGS+=(-DCMAKE_EXE_LINKER_FLAGS=)
    fi
    
    # Verbose mode
    if [ "$VERBOSE" = true ]; then
        CMAKE_ARGS+=(-DCMAKE_VERBOSE_MAKEFILE=ON)
    fi
    
    cmake "${CMAKE_ARGS[@]}" ..
    
    cd ..
}

# Build project
build_project() {
    log_info "Building project (using $JOBS parallel jobs)..."
    
    cd "$BUILD_DIR"
    
    if [ "$VERBOSE" = true ]; then
        cmake --build . -j "$JOBS" -- VERBOSE=1
    else
        cmake --build . -j "$JOBS"
    fi
    
    cd ..
    
    log_info "Build completed successfully"
}

# Run tests
run_tests() {
    if [ "$TEST" = true ]; then
        log_info "Running tests..."
        
        cd "$BUILD_DIR"
        
        if ! ctest --output-on-failure -j "$JOBS"; then
            log_error "Tests failed"
            exit 1
        fi
        
        cd ..
        
        log_info "All tests passed"
    fi
}

# Install project
install_project() {
    if [ "$INSTALL" = true ]; then
        log_info "Installing to $INSTALL_PREFIX..."
        
        cd "$BUILD_DIR"
        
        if [ -w "$INSTALL_PREFIX" ]; then
            cmake --install .
        else
            sudo cmake --install .
        fi
        
        cd ..
        
        log_info "Installation completed"
    fi
}

# Main function
main() {
    parse_args "$@"
    
    log_info "Starting build for $PROJECT_NAME"
    log_info "Build type: $BUILD_TYPE"
    
    check_dependencies
    clean_build
    configure_project
    build_project
    run_tests
    install_project
    
    log_info "All steps completed successfully"
    
    # Show build artifacts location
    if [ -d "$BUILD_DIR" ]; then
        log_info "Build artifacts located at: $BUILD_DIR/"
    fi
}

# Execute main function
main "$@"

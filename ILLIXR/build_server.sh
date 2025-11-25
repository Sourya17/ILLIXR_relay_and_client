#!/bin/bash
# ========================================================
# Build Script for ILLIXR Server
# ========================================================
set -e  # Exit immediately on error

BUILD_DIR=~/ILLIXR_server/ILLIXR/build
YAML_FILE=~/ILLIXR_server/ILLIXR/illixr.yaml

echo "📂 Switching to build directory: $BUILD_DIR"
cd "$BUILD_DIR"

echo "🧹 Cleaning build directory (except data.zip)..."
sudo find . -mindepth 1 ! -name 'data.zip' -exec rm -rf {} +

echo "⚙️ Running CMake configuration..."
cmake .. -DCMAKE_BUILD_TYPE=Debug \
    -DUSE_DEBUGVIEW=ON \
    -DUSE_GROUND_TRUTH_SLAM=ON \
    -DUSE_GTSAM_INTEGRATOR=ON \
    -DUSE_NATIVE_RENDERER=ON \
    -DUSE_OFFLINE_CAM=ON \
    -DUSE_OFFLINE_IMU=ON \
    -DUSE_OFFLOAD_VIO.DEVICE_RX=ON \
    -DUSE_OFFLOAD_VIO.DEVICE_TX=ON \
    -DUSE_TCP_MINIMAL_BACKEND=ON \
    -DUSE_POSE_PREDICTION=ON \
    -DUSE_TCP_NETWORK_BACKEND=ON \
    -DUSE_TIMEWARP_VK=ON \
    -DUSE_VKDEMO=ON

echo "🏗️ Building project..."
sudo cmake --build . -j"$(nproc)"

echo "📦 Installing..."
sudo cmake --install .

echo "📝 Writing illixr.yaml..."
cat > "$YAML_FILE" << 'EOF'
plugins: tcp_minimal_backend
install_prefix: /usr/local
env_vars:
  enable_offload: False
  enable_alignment: False
  enable_verbose_errors: False
  enable_pre_sleep: False
  data: /home/jammy/ILLIXR_server/ILLIXR/data/mav0
  demo_data: /home/jammy/ILLIXR_server/ILLIXR/demo_data
  ILLIXR_TCP_SERVER_IP: 0.0.0.0
  ILLIXR_TCP_SERVER_PORT: 5050
  ILLIXR_IS_CLIENT: '0'
  ILLIXR_FORWARD_IP: "0.0.0.0"
  ILLIXR_FORWARD_PORT: "5000"
EOF

echo "✅ Server build and configuration complete!"


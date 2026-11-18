#!/bin/bash
# RayZen Dependencies Installation Script
#
# NOTE: GPU Support Depends on Your Environment:
# - Native Linux: Install GPU drivers from your vendor (nvidia, amd, intel)
# - WSL2 with NVIDIA GPU: Run this script; if you still get llvmpipe, see GPU_SUPPORT.md
# - WSL2 without GPU: Use --allow-software-renderer flag to run on CPU

set -e

echo "=== Installing RayZen Core Dependencies ==="

# Update package lists
sudo apt update

# Install GLFW
echo "Installing GLFW..."
sudo apt install -y libglfw3-dev

# Install GLEW
echo "Installing GLEW..."
sudo apt install -y libglew-dev

# Install Assimp
echo "Installing Assimp..."
sudo apt install -y libassimp-dev

# Install GLM
echo "Installing GLM..."
sudo apt install -y libglm-dev

# Install PNG image decoding headers
echo "Installing libpng..."
sudo apt install -y libpng-dev

# Install OpenGL development headers
echo "Installing OpenGL headers..."
sudo apt install -y mesa-common-dev

echo ""
echo "=== Installation Complete ==="
echo ""
echo "To run RayZen:"
echo "  cd RayZen/build"
echo "  cmake .."
echo "  make"
echo "  ./RayZen"
echo ""
echo "GPU Rendering Notes:"
echo "  - If using software renderer (llvmpipe): try --allow-software-renderer flag"
echo "  - For WSL2: use a supported Windows GPU driver and Mesa D3D12 in Linux"
echo "  - For native Linux: install a matching GPU driver using your distribution's guidance"
echo "  - See GPU_SUPPORT.md for diagnosis and WSL-specific instructions"
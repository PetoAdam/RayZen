# Dockerfile for headless build and test of RayZen with LLVM/mesa
FROM ubuntu:24.04

# Install build tools and dependencies
RUN apt-get update && apt-get install -y \
    build-essential cmake git pkg-config \
    libglfw3-dev libglew-dev libglm-dev libassimp-dev \
    libx11-dev libxrandr-dev libxi-dev libxxf86vm-dev libxinerama-dev libxcursor-dev \
    mesa-utils mesa-utils-extra libgl1-mesa-dev libglu1-mesa-dev \
    xorg xvfb

# Set up a non-root user for safety
RUN useradd -ms /bin/bash rayzen
USER rayzen
WORKDIR /home/rayzen

# Copy project files
COPY --chown=rayzen:rayzen . /home/rayzen/RayZen
WORKDIR /home/rayzen/RayZen

# Build the project
RUN mkdir -p build && cd build && cmake .. && make -j$(nproc)

# Entrypoint for running the app with virtual framebuffer
ENTRYPOINT ["/bin/bash"]

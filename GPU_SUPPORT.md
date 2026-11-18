# GPU support

RayZen requires OpenGL 4.3 or newer. Check its startup log for `GL vendor`,
`GL renderer`, and `GL version`; `--require-hardware-renderer` rejects known
software renderers. The `--allow-software-renderer` option permits llvmpipe
for testing, but does not enable GPU acceleration.

## WSL2

WSLg uses Mesa's D3D12 OpenGL driver with a supported Windows GPU driver.
Install or update the GPU driver on Windows and use the Linux distribution's
Mesa packages. Do not install an arbitrary Linux NVIDIA driver or override
`LD_LIBRARY_PATH`/`LD_PRELOAD` to force native NVIDIA GL libraries into WSLg.
See Microsoft's [WSLg documentation](https://github.com/microsoft/wslg).

To explicitly select Mesa's D3D12 driver and require a hardware renderer:

```bash
GALLIUM_DRIVER=d3d12 ./RayZen --require-hardware-renderer
```

The renderer should identify the actual GPU, for example
`D3D12 (NVIDIA GeForce RTX 4070 SUPER)`. `nvidia-smi` detecting a GPU alone
does not prove that the OpenGL context uses it. If the context instead reports
llvmpipe, check WSLg, the Windows driver, and the Linux Mesa installation.
RayZen's WSL driver override can be disabled with `--no-wsl-d3d12-override`.

The tested WSL D3D12 driver does not expose usable OpenGL program binaries,
and its driver JIT recurs on a fresh process. See the [README](README.md) for
the optional `--keep-warm` workaround and its limitations.

## Native Linux and Windows

On native Linux, install the GPU driver recommended by your distribution and
GPU vendor. On Windows, use an up-to-date vendor OpenGL driver and follow the
MSVC/vcpkg instructions in the [README](README.md). Native NVIDIA OpenGL on
the tested Windows RTX 4070 SUPER supports RayZen's shader binary cache; this
is driver-specific and does not guarantee that all driver JIT is avoided.

Performance depends on the GPU, driver, resolution, scene, shading mode and
settings. Use the README's synchronized benchmark when comparing systems.

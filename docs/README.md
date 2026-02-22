# Meteor Build Environment

Cross-compilation environment for **Wyze Cam V3** and **Wyze Cam V2** cameras
using the [thingino-firmware](https://github.com/themactep/thingino-firmware) build system.

## Target Hardware

| Property        | Wyze Cam V3 (T31)                      | Wyze Cam V2 (T20)                      |
|-----------------|----------------------------------------|-----------------------------------------|
| SoC             | Ingenic T31X                           | Ingenic T20X                            |
| Architecture    | MIPS32r2 Little-Endian (XBurst1, o32)  | MIPS32r2 Little-Endian (XBurst1, o32)   |
| Sensor          | GC2053 (1920x1080)                     | JXF22 (1920x1080)                       |
| Sensor bus      | I2C, address 0x37, bus 0               | I2C, address 0x40, bus 0                |
| SDK version     | T31 1.1.6                              | T20 3.12.0                              |
| Thingino board  | `wyze_cam3_t31x_gc2053_rtl8189ftv`     | `wyze_cam2_t20x_jxf22_rtl8189ftv`       |

## Toolchain

The cross-compiler comes from thingino's Buildroot output. It is **not** inside the
`thingino-firmware/` source tree; Buildroot places it under `~/output/stable/`.

| Property    | Value                                          |
|-------------|------------------------------------------------|
| Compiler    | GCC 15.2.0 (Buildroot)                         |
| Triple      | `mipsel-thingino-linux-musl`                   |
| C library   | musl                                           |
| Prefix      | `mipsel-linux-`                                |
| Binary path | `~/output/stable/<board>-3.10/host/bin/` |

The `mipsel-linux-gcc` binary is a Buildroot toolchain-wrapper that delegates to
`host/opt/ext-toolchain/bin/mipsel-linux-gcc.br_real`.

### Building thingino (prerequisite)

The toolchain, SDK libraries, and headers are produced by building the thingino
firmware for the target board:

```bash
cd ~/work/github/thingino-firmware

# Wyze Cam V3
make BOARD=wyze_cam3_t31x_gc2053_rtl8189ftv all

# Wyze Cam V2
make BOARD=wyze_cam2_t20x_jxf22_rtl8189ftv all
```

Each board populates its own output directory under `~/output/stable/<board>-3.10/`:

```
~/output/stable/<board>-3.10/
├── host/
│   └── bin/
│       ├── mipsel-linux-gcc          # C compiler
│       ├── mipsel-linux-g++          # C++ compiler
│       ├── mipsel-linux-ld           # Linker
│       ├── mipsel-linux-ar           # Archiver
│       ├── mipsel-linux-strip        # Strip symbols
│       └── ...
├── staging/                          # Sysroot for cross-compilation
│   ├── lib/
│   │   └── libimp.so                # (symlink from usr/lib)
│   └── usr/
│       ├── include/                  # System headers (musl, etc.)
│       └── lib/
│           ├── libimp.so             # IMP media platform library
│           ├── libalog.so            # IMP logging library
│           ├── libsysutils.so        # IMP system utilities
│           └── libmuslshim.so        # uclibc-to-musl compatibility shim
└── build/
    └── prudynt-t-<hash>/
        └── include/<SOC>/<VERSION>/<lang>/
            ├── imp/                  # IMP SDK headers
            └── sysutils/             # Sysutils SDK headers
```

SDK header subdirectories by platform: T31 uses `en/`, T20 uses `zh/`.

## Ingenic IMP SDK

The IMP (Ingenic Media Platform) SDK provides the ISP, video, encoding, and
intelligent video analysis APIs. The SDK headers are vendored in the prudynt-t
package within the thingino build tree.

| Platform | SDK version | Header path                              |
|----------|-------------|------------------------------------------|
| T31      | 1.1.6       | `.../include/T31/1.1.6/en/`              |
| T20      | 3.12.0      | `.../include/T20/3.12.0/zh/`             |

### SDK Headers

Located at `<THINGINO_OUTPUT>/build/prudynt-t-<hash>/include/<SOC>/<VERSION>/<lang>/`:

| Header                 | Module                           |
|------------------------|----------------------------------|
| `imp/imp_system.h`     | System init, module binding      |
| `imp/imp_common.h`     | Shared types, pixel formats      |
| `imp/imp_isp.h`        | ISP control, sensor management   |
| `imp/imp_framesource.h`| Video frame source channels      |
| `imp/imp_ivs.h`        | IVS algorithm framework          |
| `imp/imp_ivs_move.h`   | Motion detection algorithm       |
| `imp/imp_ivs_base_move.h` | Base motion detection         |
| `imp/imp_encoder.h`    | H.264/H.265/JPEG encoding       |
| `imp/imp_osd.h`        | On-screen display overlay        |
| `imp/imp_audio.h`      | Audio capture/playback           |
| `imp/imp_log.h`        | IMP logging macros               |
| `sysutils/su_base.h`   | System utility base              |
| `sysutils/su_misc.h`   | Miscellaneous system utilities   |

The `<hash>` in the prudynt path varies per build. The CMake toolchain file uses a
`file(GLOB ...)` to locate it automatically.

### SDK Libraries

| Library           | Purpose                                         |
|-------------------|-------------------------------------------------|
| `libimp.so`       | Core IMP library (ISP, FrameSource, IVS, encoder, OSD) |
| `libalog.so`      | IMP internal logging                            |
| `libsysutils.so`  | System-level utilities                          |
| `libmuslshim.so`  | uclibc-to-musl compatibility (see below)        |

### The uclibc/musl Compatibility Problem

The IMP SDK binaries (`libimp.so`, `libalog.so`, `libsysutils.so`) were originally
compiled against **uclibc** and carry uclibc-style DT_NEEDED entries:

```
libimp.so:
  NEEDED  libpthread.so.0     ← uclibc soname
  NEEDED  libdl.so.0          ← uclibc soname
  NEEDED  libc.so.0           ← uclibc soname
```

The thingino toolchain uses **musl**, where:
- `libpthread` is built into `libc.so` (no separate `libpthread.so.0`)
- `libdl` is built into `libc.so` (no separate `libdl.so.0`)
- The libc soname is `libc.so`, not `libc.so.0`

The **`libmuslshim.so`** library (provided by thingino) bridges this gap. It exports
the uclibc-specific symbols (`__pthread_register_cancel`, `__pthread_unregister_cancel`,
`__assert`, `__fgetc_unlocked`, etc.) and satisfies the runtime link. Without it, the
linker produces undefined reference errors:

```
undefined reference to `__pthread_register_cancel'
undefined reference to `__pthread_unregister_cancel'
undefined reference to `__assert'
undefined reference to `__fgetc_unlocked'
```

**You must always link `-lmuslshim` when linking against the IMP SDK libraries.**

The linker will still emit warnings about `libpthread.so.0`, `libdl.so.0`, and
`libc.so.0` not being found. These warnings are harmless: at runtime on the camera,
the thingino rootfs provides the necessary shim libraries.

## Building Meteor

Each target platform uses its own toolchain file and build directory.

### Wyze Cam V3 (T31)

```bash
cd meteor
mkdir -p build-t31 && cd build-t31
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-t31.cmake ..
make
```

### Wyze Cam V2 (T20)

```bash
cd meteor
mkdir -p build-t20 && cd build-t20
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-t20.cmake ..
make
```

Both produce a `meteor` binary in their respective build directories.

### CMake Variables

These variables are set by the toolchain file but can be overridden with `-D`:

| Variable           | T31 default                                                          | T20 default                                                          | Description                          |
|--------------------|----------------------------------------------------------------------|----------------------------------------------------------------------|--------------------------------------|
| `METEOR_PLATFORM`  | `T31`                                                                | `T20`                                                                | Target platform (sets `-DPLATFORM_*` define) |
| `THINGINO_OUTPUT`  | `$HOME/output/stable/wyze_cam3_t31x_gc2053_rtl8189ftv-3.10`         | `$HOME/output/stable/wyze_cam2_t20x_jxf22_rtl8189ftv-3.10`          | Thingino Buildroot output directory  |
| `THINGINO_DIR`     | *(empty)*                                                            | *(empty)*                                                            | Thingino source tree (optional fallback for SDK headers) |
| `SOC_FAMILY`       | `T31`                                                                | `T20`                                                                | SoC family                           |
| `SDK_VERSION`      | `1.1.6`                                                              | `3.12.0`                                                             | IMP SDK version                      |
| `CROSS_COMPILE`    | `mipsel-linux-`                                                      | `mipsel-linux-`                                                      | Toolchain prefix                     |

Override example:

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-t31.cmake \
      -DTHINGINO_OUTPUT=/path/to/other/output ..
```

The `THINGINO_OUTPUT` environment variable is also respected if the CMake variable
is not explicitly set.

### Compiler Flags

```
-Wall -Wextra -O2 -march=mips32r2
```

### Link Libraries

```
libimp  libalog  libsysutils  libmuslshim  libpthread  libm  librt
```

### Build Output

```
build-t31/meteor    # T31 binary, 32-bit MIPS ELF, dynamically linked
build-t20/meteor    # T20 binary, 32-bit MIPS ELF, dynamically linked
```

Verify with:

```bash
file build-t31/meteor
# ELF 32-bit LSB pie executable, MIPS, MIPS32 rel2 version 1 (SYSV),
# dynamically linked, interpreter /lib/ld-musl-mipsel.so.1, not stripped

readelf -d build-t31/meteor | grep NEEDED
# NEEDED  libimp.so
# NEEDED  libalog.so
# NEEDED  libsysutils.so
# NEEDED  libmuslshim.so
# NEEDED  libc.so
```

## Deploying to Camera

Copy the binary to the camera via SCP:

```bash
# Wyze Cam V3
scp build-t31/meteor root@<camera-ip>:/tmp/

# Wyze Cam V2
scp build-t20/meteor root@<camera-ip>:/tmp/
```

The required shared libraries must be present on the camera in `/usr/lib/`:

```
/usr/lib/libimp.so
/usr/lib/libalog.so
/usr/lib/libsysutils.so
/usr/lib/libmuslshim.so
```

These are part of the thingino rootfs and should already be in place on a flashed
camera. Run the binary:

```bash
ssh root@<camera-ip>
/tmp/meteor
```

Stop with `Ctrl+C` (SIGINT) or `kill` (SIGTERM) — meteor handles both for graceful
teardown.

## Project Structure

```
meteor/
├── CMakeLists.txt              # Top-level build configuration
├── cmake/
│   ├── toolchain-t31.cmake     # Wyze Cam V3 (T31X) toolchain
│   └── toolchain-t20.cmake     # Wyze Cam V2 (T20X) toolchain
├── docs/
│   └── README.md               # This file
├── include/meteor/
│   ├── system.h                # IMP system init/exit/bind wrappers
│   ├── isp.h                   # ISP lifecycle (sensor, tuning)
│   ├── framesource.h           # FrameSource channel management
│   ├── ivs.h                   # IVS motion detection
│   └── log.h                   # Logging macros
├── scripts/
│   ├── cameras.json            # Camera inventory (name, platform, IP)
│   ├── deploy_binaries.sh      # Quick binary deploy via SCP
│   └── deploy_firmware.sh      # Full firmware rebuild + sysupgrade
└── src/
    ├── main.c                  # Entry point, pipeline setup, signal handling
    ├── system.c                # IMP_System_Init/Exit/Bind/UnBind
    ├── isp.c                   # Sensor config (GC2053/JXF22), ISP open/close
    ├── framesource.c           # NV12 channel create/enable/disable
    ├── ivs.c                   # Motion detection setup and poll loop
    └── log.c                   # Logging initialization
```

## Data Pipeline

```
[Sensor] -------> [ISP] --> [FrameSource Ch0] --> [IVS Grp0/Ch0]
 GC2053 (T31)                1920x1080 @25fps          |
 JXF22  (T20)                NV12, 3 VBs          Motion Detect
                                                       |
                                                  Poll in main()
                                                  Log motion events
```

Module binding uses `IMP_System_Bind()` with cell addresses:

| Cell             | deviceID     | groupID | outputID |
|------------------|--------------|---------|----------|
| FrameSource Ch0  | `DEV_ID_FS`  | 0       | 0        |
| IVS Grp0         | `DEV_ID_IVS` | 0       | 0        |

## Troubleshooting

### CMake: "Cannot find IMP SDK headers"

The toolchain file looks for headers in `<THINGINO_OUTPUT>/build/prudynt-t-*/`.
If the prudynt package hasn't been built, the headers won't be there. Either:

1. Build thingino fully (recommended), or
2. Pass `-DTHINGINO_DIR=/path/to/thingino-firmware` to use the headers from
   `thingino-firmware/dl/prudynt-t/git/include/` as a fallback.

### Linker: "undefined reference to `__pthread_register_cancel`"

You are missing `-lmuslshim` in your link libraries. This is required whenever
linking against `libimp.so` with the musl toolchain.

### Linker warnings: "libpthread.so.0 not found"

```
warning: libpthread.so.0, needed by .../libimp.so, not found (try using -rpath or -rpath-link)
warning: libdl.so.0, needed by .../libimp.so, not found (try using -rpath or -rpath-link)
warning: libc.so.0, needed by .../libimp.so, not found (try using -rpath or -rpath-link)
```

These are **harmless**. The uclibc sonames are baked into `libimp.so`'s ELF headers
and cannot be changed without relinking the SDK. At runtime on the camera, the
thingino rootfs resolves them correctly.

### Runtime: "error loading shared libraries"

Ensure all required `.so` files are on the camera. Check with:

```bash
# On the camera
ldd /tmp/meteor
# or
LD_TRACE_LOADED_OBJECTS=1 /tmp/meteor
```

### GCC 15: `-fno-common` by default

GCC 15 defaults to `-fno-common`, which causes multiple-definition errors for
tentative definitions (e.g., uninitialized global variables declared in headers
without `extern`). If you hit this, either:

- Fix the code to use proper `extern` declarations, or
- Add `-fcommon` to `CMAKE_C_FLAGS` as a workaround.

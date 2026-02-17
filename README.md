# Raspberry Pi ILI9488 SPI Display Driver (DMA + GPU-accelerated)

## Overview

A high-performance, zero-copy framebuffer driver for **ILI9488 SPI TFT displays** on Raspberry Pi Zero 2W with 64-bit OS. This driver leverages:

- **DMA-accelerated SPI transfers** (zero-copy framebuffer streaming at 65 Mbps)
- **CMA (Contiguous Memory Allocator)** for physically contiguous, GPU-accessible memory
- **Triple-buffering** for asynchronous rendering pipeline (app writes while daemon displays)
- **GPU-accelerated 2D rotation** with minimal CPU overhead
- **Systemd service** with automatic startup and configuration

The implementation is modernized for 64-bit Raspberry Pi OS (kernel ≥5.x) and inspired by [fbcp-ili9341](https://github.com/juj/fbcp-ili9341).

**Key characteristics:**
- **~12 FPS sustained framerate** on Pi Zero 2W (SPI bandwidth limited)
- **0.2-0.3% CPU usage** with zero-copy architecture
- **~3.5 MB memory footprint** (triple buffer + daemon overhead)
- **All rotation angles supported** (0°, 90°, 180°, 270°) with GPU DMA acceleration

**Note:** This is a display driver, not a terminal emulator. You must write your own application to convert and push pixel data to the shared memory framebuffer. Refer to [this section](#important-this-is-a-display-driver-not-a-terminal-emulator) for details.

## Target Hardware

**Recommended Setup:**
- Raspberry Pi Zero 2W (or compatible 64-bit ARM SBC)
- 64-bit Raspberry Pi OS (Bookworm or later recommended)
- Linux kernel ≥ 5.x
- [ILI9488 SPI display](https://de.aliexpress.com/item/1005005787550807.html?gatewayAdapt=glo2deu) (320×480 pixels, 3.5" or similar)

**Not Supported:**
- **32-bit OS or 32-bit Pi models** — the prebuilt `.deb` package is compiled for `arm64` only.
  If you are on a 32-bit system, you can try [building from source](#build-from-source) yourself, but this is untested and not officially supported.
- Other display controllers (ILI9488 only)
- Parallel/GPIO-based displays

## Installation

### 1. Install from Debian Package

```bash
sudo dpkg -i ili9488-daemon_1.1.0_arm64.deb
```

The post-install script automatically configures:
- **SPI:** Enable (`dtparam=spi=on`) and set buffer size (65536 bytes)
- **CMA:** Allocate 16MB (minimum required for triple-buffer + GPU DMA)
  - If already set to <16MB: Automatically upgraded to 16MB
  - If already set to >16MB: Kept as-is, with info message to verify sufficiency
- **GPU Memory:** Allocate 32MB (minimum required for framebuffers + GPU rotation)
  - If already set to <32MB: Automatically upgraded to 32MB
  - If already set to >32MB: Kept as-is, with info message to verify sufficiency
- **GPIO:** Configure DC (GPIO 24) and RESET (GPIO 25) as outputs

**After installation, you must reboot** for kernel parameter changes (`/boot/firmware/config.txt`|`/boot/config.txt`, CMA, GPU memory) to take effect. The daemon starts automatically after reboot.

### 2. Build from Source

**Native build on Pi:**
```bash
mkdir build && cd build
cmake ..
make -j
```

**Cross-compile (Ubuntu/Debian):**
```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchains/aarch64-rpi.cmake
make -j
```

## Hardware Wiring

### ILI9488 Display Pinout

Connect the display to your Raspberry Pi as follows:

| Display Pin | Function    | Raspberry Pi GPIO | Notes 
|-------------|-------------|-------------------|----------------------------
| GND         | Ground      | GND               | Multiple GND pins available
| VCC         | 3.3V / 5V Power | 3V3 / 5V       | Requires stable 3.3V supply
| SCL/SCLK    | SPI Clock   | GPIO11 (SPI0 CLK) | Standard SPI0 clock
| SDA/MOSI    | SPI MOSI    | GPIO10 (SPI0 MOSI)| Standard SPI0 data line
| MISO        | SPI MISO    | GPIO9 (SPI0 MISO) | Required (even if unused)
| CS/CE       | Chip Select | GPIO8 (SPI0 CE0)  | Standard SPI0 chip select
| DC          | Data/Cmd    | GPIO24            | Configurable in code
| RESET       | Hardware Reset | GPIO25         | Configurable in code
| BL          | Backlight   | 3V3 or GPIO       | Connect directly to 3V3 or control via GPIO

### SPI Bus Configuration

The driver uses **SPI0** (/dev/spidev0.0):
- **Clock speed:** 65 MHz
- **Mode:** 0 (CPOL=0, CPHA=0)
- **Word size:** 8 bits per word
- **Endianness:** Big-endian (MSB first)

## Important: This is a Display Driver, Not a Terminal Emulator

This driver does **not** automatically display the Linux login shell or terminal output. It is a **framebuffer transport layer** that copies RGB666 pixel data from shared memory to the physical display via DMA.

To display any content (login shell, graphics, UI, etc.), you must write a separate application that:
1. Reads content from a source (e.g., `/dev/fb0`, rendering engine, video stream)
2. Converts pixel data to **RGB666 format** (3 bytes per pixel, 6-bit color depth)
3. Writes the converted framebuffer to the shared memory region (`/ili9488_rgb666`)
4. Repeats at your desired refresh rate

This architecture enables maximum flexibility—you can render anything to the display, not just terminal output.

## Running the Daemon

```bash
sudo ili9488-daemon --shm /ili9488_rgb666 --width 320 --height 480 --rotation 90 --fps-overlay 1 --max-fps 20
```

### Command-line Options

| Option | Default | Description |
|--------|---------|-------------|
| `--shm <path>` | `/ili9488_rgb666` | Shared memory name for framebuffer |
| `--width <px>` | 320 | Display width in pixels |
| `--height <px>` | 480 | Display height in pixels |
| `--rotation <deg>` | 90¹ | Rotation: 0, 90, 180, or 270 degrees |
| `--fps-overlay <0\|1>` | 0 | Display FPS counter overlay on screen |
| `--max-fps <rate>` | 15¹ | Maximum frames per second (0 = unlimited) |

¹ **Defaults:** These values are set by `/etc/default/ili9488-daemon` (systemd service environment). When running manually, built-in defaults are `--rotation 0` and `--max-fps 20`. Override with command-line arguments.

**Note:** The daemon must run as root to access `/dev/vcio` (GPU Mailbox) and `/dev/mem`.

### Environment Variables

Alternatively, configure via `/etc/default/ili9488-daemon`:

```bash
ILI9488_SHM_NAME=/ili9488_rgb666
ILI9488_WIDTH=320
ILI9488_HEIGHT=480
ILI9488_ROTATION=90
ILI9488_FPS_OVERLAY=0
ILI9488_MAX_FPS=15
```

## Shared Memory Protocol

The daemon creates and manages a POSIX shared memory region (`/ili9488_rgb666`) containing the framebuffer and control header. Applications use a **triple-buffer architecture** with semaphore synchronization:

- **Front Buffer:** Displayed on screen (daemon reads continuously)
- **Back Buffer:** Rotation target (GPU DMA writes to, then becomes front)
- **Pending Buffer:** Application writes frames here, signals daemon when ready

### Memory Layout

```
Shared Memory: /ili9488_rgb666
+──────────────────────────────────────────+
│ TripleBufferShmHeader (256 bytes)        │  Header with control fields
│  - magic, version, dimensions            │  & semaphore
│  - buffer bus addresses                  │
│  - front/back/pending indices            │
│  - frame counter, daemon_ready flag      │
+──────────────────────────────────────────+
│ Buffer A (320×480×3 = 460,800 bytes)    │
│ Buffer B (320×480×3 = 460,800 bytes)    │  Triple-buffer
│ Buffer C (320×480×3 = 460,800 bytes)    │  (one per rotation pass)
+──────────────────────────────────────────+
Total: ~1.38 MB (header + 3 framebuffers)
```

### Header Structure

The shared memory header (must match `TripleBufferShmHeader` in `include/ili9488_mailbox.h`):

```c
struct TripleBufferShmHeader {
    uint32_t magic;                 // 0x49494C39 ("IIL9")
    uint32_t version;               // Protocol version
    uint32_t width;                 // Display width (320)
    uint32_t height;                // Display height (480)
    uint32_t bytes_per_pixel;       // 3 (RGB666)
    uint32_t buffer_a_bus_addr;     // Physical address (GPU-accessible)
    uint32_t buffer_b_bus_addr;     // Physical address
    uint32_t buffer_c_bus_addr;     // Physical address
    volatile uint32_t front_index;  // Currently displayed (0, 1, or 2)
    volatile uint32_t back_index;   // Rotation target (0, 1, or 2)
    volatile uint32_t pending_index; // App writes here (0, 1, or 2)
    sem_t pending_sem;              // Semaphore for pending_index sync
    volatile uint32_t frame_counter; // Incremented by app
    volatile uint32_t rotation_degrees; // Readable by app
    volatile uint32_t daemon_ready; // Set when daemon initialized
    volatile uint32_t app_connected; // Set by app (optional)
    uint8_t padding[64];            // Reserved for future use
};
```

### RGB666 Format

Each pixel occupies **3 bytes** in RGB666 format:

```
Byte 0: RRRRRRXX  (R: top 6 bits used, bottom 2 bits unused)
Byte 1: GGGGGGXX  (G: top 6 bits used, bottom 2 bits unused)
Byte 2: BBBBBBXX  (B: top 6 bits used, bottom 2 bits unused)
```

The display uses only the **top 6 bits of each byte** (18-bit color total), providing **262,144 unique colors** (64 × 64 × 64). The bottom 2 bits can be any value (usually masked to 0x00).

### Writing to Shared Memory

Applications should:
1. Open the shared memory region `/ili9488_rgb666`
2. Wait for `daemon_ready == 1` (daemon initialized)
3. Acquire `pending_sem` (semaphore)
4. Write frame data to buffer at offset `sizeof(header) + pending_index * buffer_size`
5. Increment `frame_counter` and post the semaphore
6. Repeat for next frame

#### Example: C Implementation

```c
#include <fcntl.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Must match ili9488_mailbox.h
struct TripleBufferShmHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_pixel;
    uint32_t buffer_a_bus_addr;
    uint32_t buffer_b_bus_addr;
    uint32_t buffer_c_bus_addr;
    volatile uint32_t front_index;
    volatile uint32_t back_index;
    volatile uint32_t pending_index;
    sem_t pending_sem;
    volatile uint32_t frame_counter;
    volatile uint32_t rotation_degrees;
    volatile uint32_t daemon_ready;
    volatile uint32_t app_connected;
    uint8_t padding[64];
};

int main() {
    const char *shm_name = "/ili9488_rgb666";

    // 1. Open shared memory
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        return 1;
    }

    // 2. Get size and map
    struct stat sb;
    if (fstat(shm_fd, &sb) < 0) {
        perror("fstat");
        return 1;
    }

    void *map = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    struct TripleBufferShmHeader *header = (struct TripleBufferShmHeader *)map;

    // 3. Verify magic and wait for daemon
    if (header->magic != 0x49494C39) {
        fprintf(stderr, "Invalid shared memory header\n");
        return 1;
    }

    while (!header->daemon_ready) {
        usleep(100000);  // Wait 100ms for daemon to initialize
    }

    uint32_t buffer_size = header->width * header->height * header->bytes_per_pixel;

    // 4. Render frames continuously
    for (int frame = 0; frame < 100; frame++) {
        // Try to acquire semaphore (non-blocking)
        if (sem_trywait(&header->pending_sem) != 0) {
            usleep(10000);  // Daemon busy, try again soon
            continue;
        }

        // Get pointer to pending buffer
        uint8_t *pending_buf = (uint8_t *)map + sizeof(struct TripleBufferShmHeader) +
                               header->pending_index * buffer_size;

        // Fill pending buffer with pixel data (RGB666 format)
        for (uint32_t y = 0; y < header->height; y++) {
            for (uint32_t x = 0; x < header->width; x++) {
                uint32_t pixel_idx = (y * header->width + x) * 3;

                // Example: Generate animated color pattern
                uint8_t r = (x + frame) % 256;
                uint8_t g = (y + frame) % 256;
                uint8_t b = ((x + y + frame) / 2) % 256;

                // Write RGB666 (keep top 6 bits, zero bottom 2)
                pending_buf[pixel_idx + 0] = r & 0xFC;  // R
                pending_buf[pixel_idx + 1] = g & 0xFC;  // G
                pending_buf[pixel_idx + 2] = b & 0xFC;  // B
            }
        }

        // Signal daemon that frame is ready
        header->frame_counter++;
        sem_post(&header->pending_sem);

        usleep(50000);  // ~20 FPS (50ms per frame)
    }

    // 5. Cleanup
    munmap(map, sb.st_size);
    close(shm_fd);
    return 0;
}
```

#### Compilation

```bash
gcc -o frame_app frame_app.c -lpthread -lm
sudo ./frame_app
```

#### Best Practices

- **Always check `daemon_ready`** before writing (daemon initializes buffers)
- **Use semaphore-protected access** to prevent tearing (sem_trywait + sem_post pattern)
- **Mask RGB values** with `0xFC` to zero the unused bottom 2 bits
- **Frame rate:** Use `usleep()` to match desired FPS (e.g., `50000` μs = 20 FPS)
- **Non-blocking I/O:** Use `sem_trywait()` (non-blocking) instead of `sem_wait()` (blocking) to keep rendering responsive
- **Monitor rotation_degrees** if your app needs to adapt to dynamic rotation changes

## Rotation

The daemon applies rotation **before SPI transfer** to match the physical display orientation. The `--width` and `--height` parameters specify the **physical display output dimensions**, and the daemon automatically calculates the required framebuffer dimensions from your application.

### Parameter Semantics

- `--width` and `--height`: Physical display output dimensions (320×480 for standard ILI9488)
- `--rotation`: How to transform the application framebuffer to match the display orientation

**Application framebuffer dimensions are determined by the rotation angle:**

| Rotation | Display Output | App Framebuffer | Method |
|----------|---|---|---|
| **0°**   | 320×480 | 320×480 | Index swap (pointer swap, no rotation) |
| **90°**  | 320×480 | 480×320 | GPU DMA rotation (270° internally) |
| **180°** | 320×480 | 320×480 | GPU DMA rotation (180°) |
| **270°** | 320×480 | 480×320 | GPU DMA rotation (90° internally) |

### Usage Examples

**Display in native portrait (0° rotation):**
```bash
sudo ili9488-daemon --width 320 --height 480 --rotation 0
```
- App writes: 320×480 frames (portrait orientation)
- Display shows: 320×480 (no rotation applied)
- Method: Index swap only (fastest, no GPU DMA)

**Display rotated 90° (landscape from portrait framebuffer):**
```bash
sudo ili9488-daemon --width 320 --height 480 --rotation 90
```
- App writes: 480×320 frames (landscape orientation)
- Display shows: 320×480 (rotated 270° to show landscape as portrait)
- Method: GPU DMA rotation (asynchronous with SPI transfer)

**Display upside-down (180° rotation):**
```bash
sudo ili9488-daemon --width 320 --height 480 --rotation 180
```
- App writes: 320×480 frames (portrait, upside-down)
- Display shows: 320×480 (inverted)
- Method: GPU DMA rotation (180°)

### Implementation Details

**0° (No rotation):**
- Method: Simple buffer index swap (updates pointer indices)
- Data movement: None (zero-copy, pointer operations only)
- GPU DMA: Not used
- CPU overhead: Minimal

**90° and 270° (GPU DMA with axis swap):**
- Method: BCM DMA channel 7 performs 2D rotation with stride
- Internally: Daemon rotates by (360° - user_angle) for GPU hardware
  - 90° user input → 270° GPU rotation
  - 270° user input → 90° GPU rotation
- Data movement: GPU DMA (asynchronous, overlaps with SPI)
- Axis swap: Framebuffer width ↔ height
- CPU overhead: Minimal (GPU handles all computation)

**180° (GPU DMA, no axis swap):**
- Method: BCM DMA channel 7 with 2D stride (180° rotation)
- Data movement: GPU DMA (asynchronous, overlaps with SPI)
- Axis swap: None (dimensions stay 320×480)
- CPU overhead: Minimal

**Performance:** All rotations maintain ~12 FPS (SPI bandwidth dominates). GPU DMA rotations happen asynchronously while previous frame is transferred to display. Triple-buffer architecture ensures rotation doesn't block the SPI pipeline.

## Performance Characteristics

### Benchmark Environment
- **Hardware:** Raspberry Pi Zero 2W (BCM2710, ARM Cortex-A53, 512 MB RAM)
- **Display:** ILI9488 SPI (320×480 pixels, RGB666 format)
- **SPI Clock:** 65 MHz
- **OS:** 64-bit Raspberry Pi OS Lite (Trixie, Headless)
- **Test Duration:** 15 seconds per rotation angle
- **Buffer Architecture:** Triple-buffer CMA (zero-copy)
- **FPS Overlay:** Enabled (generates continuous frame load)

### Measured Results

**Rotation: 0° (No rotation)**
| Metric | Min | Avg | Max |
|--------|-----|-----|-----|
| **FPS** | 11.7 | 12.1 | 14.4 |
| **CPU Usage** | 0.10% | 0.20% | 0.60% |
| **Memory (RSS)** | — | 6.11 MB | — |

**Rotation: 90° (GPU DMA)**
| Metric | Min | Avg | Max |
|--------|-----|-----|-----|
| **FPS** | 11.7 | 12.0 | 13.0 |
| **CPU Usage** | 0.10% | 0.20% | 0.60% |
| **Memory (RSS)** | — | 6.11 MB | — |

**Rotation: 180° (GPU DMA)**
| Metric | Min | Avg | Max |
|--------|-----|-----|-----|
| **FPS** | 11.7 | 12.1 | 13.5 |
| **CPU Usage** | 0.10% | 0.20% | 0.60% |
| **Memory (RSS)** | — | 6.09 MB | — |

**Rotation: 270° (GPU DMA)**
| Metric | Min | Avg | Max |
|--------|-----|-----|-----|
| **FPS** | 11.5 | 12.0 | 13.2 |
| **CPU Usage** | 0.10% | 0.32% | 0.90% |
| **Memory (RSS)** | — | 6.11 MB | — |

### Performance Analysis

**Framerate:**
- Consistent **~12 FPS** across all rotation angles
- SPI bandwidth is the primary bottleneck (65 Mbps for 460 KB = ~56 ms transfer time)
- FPS depends on application frame submission rate and SPI transfer bandwidth
- **Theoretical maximum:** ~17.6 FPS at 65 Mbps (practical limit)

**CPU Usage:**
- Exceptionally low: **0.2-0.3% average** with continuous frame stream
- Triple-buffer + zero-copy architecture minimizes CPU memcpy overhead
- GPU DMA rotation adds minimal CPU load (270° slightly higher at 0.32%)
- Idle daemon (no frames): <0.1% CPU
- Headless mode reduces overhead compared to desktop environments

**Memory Usage:**
- Daemon RSS: **~2-3 MB** (code + BSS)
- Triple framebuffers: **3 × 460 KB = 1.38 MB** (CMA, GPU-accessible)
- **Total: ~3.5 MB** typical operation
- Leaves **~508 MB** free on 512 MB Pi Zero 2W

**Rotation Performance:**
- **0°:** Index swap only (pointer operations, no GPU DMA needed, fastest)
- **90°/180°/270°:** GPU DMA rotation (BCM DMA channel 7, asynchronous with SPI)
- **All rotations:** Zero-copy architecture (GPU DMA handles all data movement, not CPU memcpy)
- **SPI transfer:** Always GPU DMA (DMA-BUF CMA buffers are GPU-capable via bus addresses)

### Resource Requirements
- **SPI Bandwidth:** 65 MHz (80 Mbps theoretical, 65 Mbps practical)
- **CMA Memory:** 16 MB (allocated at boot, contains triple-buffer)
- **GPU Memory:** 32 MB (framebuffer storage + GPU rotation working buffers)
- **Shared Memory:** 460,800 bytes per framebuffer (320×480×3 RGB666)

### System Impact
- **Power Consumption:** Minimal (DMA-driven, not CPU-spinning)
- **Latency:** ~56-80 ms frame-to-display (SPI transfer dominated)
- **Jitter:** Minimal (hardware DMA handles timing, not software)

## Technical Details

### Memory Allocation Strategy

The driver uses a **CMA-first approach** optimized for modern 64-bit Raspberry Pi:

1. **CMA (Contiguous Memory Allocator)** - Primary allocation
   - **Kernel configuration:** 16MB allocated at boot (`cma=16M` in `/boot/firmware/config.txt`)
   - **Access method:** DMA-BUF heap (`/dev/dma_heap/linux,cma`)
   - **Lifetime:** Persistent for entire daemon runtime
   - **GPU accessibility:** Direct via bus address (imported via VCSM-CMA)
   - **CPU accessibility:** Mmap'd and cache-coherent
   - **Benefits:** Zero-copy DMA, GPU acceleration, no memory fragmentation

2. **GPU Mailbox API** - Fallback for legacy systems
   - Legacy interface for GPU memory allocation
   - Issues on 64-bit Pi (can cause SIGBUS)
   - Not recommended; CMA preferred

3. **Buffer Architecture:**
   - **Triple-buffer:** Front (display), Back (rotation target), Pending (app writes)
   - **Total CMA usage:** 3 × 460 KB = 1.38 MB (plus header overhead)
   - **Leaves:** ~14.6 MB CMA free for GPU operations and future features

### Buffer Lifecycle (Triple-Buffer with DMA Rotation)

```
Timeline:
  T0: App writes to Pending[2]
  T1: App signals daemon (sem_post)
  T2: Daemon checks Pending[2], starts GPU DMA rotation → Back[1]
  T3: While GPU rotates, SPI transfer of Front[0] to display
  T4: GPU DMA complete, swap indices: Back[1]→Front, Pending[2]→Back
  T5: Next iteration begins (Pending cycles through 0, 1, 2)

Zero-copy property:
  - No CPU memcpy between buffers
  - GPU DMA handles rotation asynchronously
  - SPI transfer overlaps with GPU work (true parallelism)
```

### SPI DMA Transfer

- **Interface:** Linux kernel `/dev/spidev0.0` with DMA-capable buffers
- **Clock:** 65 MHz (60-70 MHz practical range on Pi Zero 2W)
- **Transfer size:** 460,800 bytes per frame (320×480×3 RGB666)
- **Bandwidth:** ~56 ms per frame at 65 Mbps
- **Chunk size:** 65536 bytes per DMA operation
- **Synchronization:** Blocking wait for SPI completion (triple-buffer prevents blocking app)

### GPU Acceleration (BCM DMA + Mailbox)

**When available (detected at startup):**
- **DMA Channel:** BCM DMA channel 7 (reserved, independent SPI DMA)
- **Operation:** 2D rotation (all angles: 0°, 90°, 180°, 270°)
  - **0°:** Index swap only (no GPU DMA used)
  - **90°, 180°, 270°:** GPU DMA 2D rotation with stride
- **Overhead:** ~14ms GPU time (overlaps with SPI transfer asynchronously)
- **Efficiency:** GPU operates in parallel with SPI, ~12 FPS maintained
- **Detection:** Automatic; logged as "GPU Rotation: Available" or "Unavailable"

**Fallback (if GPU unavailable):**
- Rotation still supported, but with CPU pixel manipulation (slower)
- 0° rotation always works (index swap, never requires GPU)
- Higher CPU load for 90°/180°/270° rotations (CPU memcpy)

### GPIO Configuration

The post-install script configures two GPIO pins as outputs:

```
gpio=24=op    # Data/Command control (DC line)
gpio=25=op    # Hardware Reset (RESET line)
```

These are set in `/boot/firmware/config.txt` (modern) or `/boot/config.txt` (legacy) during package installation.

### Frame Synchronization

**Semaphore-based protocol:**
1. App acquires `pending_sem` with `sem_trywait()` (non-blocking)
2. App writes frame data to `pending_buffer`
3. App increments `frame_counter` and posts `sem_post()`
4. Daemon waits on `pending_sem` to wake up
5. Daemon rotates (if needed) and displays frame
6. Daemon updates `front_index` atomically

This design ensures:
- **No tearing** (daemon reads atomically from front buffer)
- **No lost frames** (semaphore prevents overwrite during rotation)
- **Non-blocking app** (uses `sem_trywait()`, not `sem_wait()`)
- **Minimal latency** (semaphore-driven, not polling)

## Systemd Service

After installation, the daemon runs as a systemd service:

```bash
# View status
sudo systemctl status ili9488-daemon

# Restart
sudo systemctl restart ili9488-daemon

# View logs
sudo journalctl -u ili9488-daemon -f
```

The service is configured in `/etc/systemd/system/ili9488-daemon.service` and uses settings from `/etc/default/ili9488-daemon`.

## Troubleshooting

### Display shows garbage or is blank

**Check hardware first:**
1. Verify wiring: SPI pins (CLK=GPIO11, MOSI=GPIO10, MISO=GPIO9, CS=GPIO8), DC=GPIO24, RESET=GPIO25
2. Ensure 3.3V power supply is stable and not sagging under load
3. Try a different SPI cable (shorts at high frequency can cause corruption)

**Check software:**
1. Verify daemon is running: `sudo systemctl status ili9488-daemon`
2. Check daemon logs: `sudo journalctl -u ili9488-daemon -n 50`
3. Verify SPI is enabled: `grep "^dtparam=spi=on" /boot/firmware/config.txt`
4. Verify CMA and GPU memory: `grep "^cma=" /boot/firmware/config.txt` (should be ≥16M) and `grep "^gpu_mem=" /boot/firmware/config.txt` (should be ≥32)
5. Verify framebuffer exists: `ls -la /dev/shm/ili9488_rgb666`

**If still blank after reboot:**
- Check daemon exit code: `sudo systemctl status ili9488-daemon | grep "Exited\|Exit"`
- Test with frame generator: `gcc -o frame_gen scripts/frame_generator.c -lpthread -lm && sudo ./frame_gen`
- Verify display is properly powered

### Daemon crashes with "SIGBUS" or "Permission denied"

**SIGBUS (Segmentation Bus Error):**
- **Cause:** GPU mailbox memory access on 64-bit Pi (or insufficient CMA)
- **Fix:** Reboot after package installation (CMA must be configured at boot time)
- Check: `grep "^cma=" /boot/firmware/config.txt` (should be ≥16M)

**Permission Denied:**
- Ensure running as root: `sudo ./ili9488-daemon ...`
- Check `/dev/spidev0.0` exists and is readable: `ls -la /dev/spidev0.0`
- Check `/dev/vcio` or `/dev/vcsm-cma` exists: `ls -la /dev/vcio /dev/vcsm-cma`

**Kernel/Architecture Issues:**
1. Verify 64-bit OS: `uname -m` (must be `aarch64`, not `armv7l`)
2. Check kernel version: `uname -r` (should be ≥5.x)
3. Check architecture: `cat /proc/cpuinfo | grep -i "processor\|isa"` (should show ARMv8)

### Low frame rate or tearing

**Tearing (visual artifacts during scrolling):**
- Triple-buffer is enabled (should prevent tearing)
- Check daemon logs for GPU rotation errors: `sudo journalctl -u ili9488-daemon | grep -i "dma\|gpu"`
- Verify rotation angle: `sudo journalctl -u ili9488-daemon | grep "Rotation\|GPU"`

**Low FPS (<10 FPS):**
- **Expected:** ~12 FPS is the practical limit (SPI bandwidth bottleneck at 65 MHz)
- Check if application is feeding frames fast enough: Count `frame_counter` increment rate
- Verify SPI speed: Check daemon logs for actual SPI clock negotiated
- Check system load: `top` (should show <1% CPU when idle)
- For 90°/270° rotation: GPU DMA adds minimal overhead (still ~12 FPS)

**Measuring actual FPS:**
```bash
# Enable FPS overlay
sudo ili9488-daemon --fps-overlay 1 --max-fps 20

# Monitor in logs
sudo journalctl -u ili9488-daemon -f | grep -i fps
```

### High memory usage

**Daemon RSS >20 MB:**
- Check for memory leaks: Monitor over time with `ps aux --sort=-%mem | head -5`
- Restart daemon: `sudo systemctl restart ili9488-daemon`
- Check kernel logs: `dmesg | tail -20` (look for OOM or allocation failures)

**Shared memory not freed after app crash:**
- Manually cleanup: `sudo rm /dev/shm/ili9488_rgb666`
- Restart daemon: `sudo systemctl restart ili9488-daemon`

### Daemon won't start after installation

1. **Check reboot:** Did you reboot after `dpkg -i`? Kernel parameters require reboot.
2. **Check config:** `grep -E "cma=|gpu_mem=" /boot/firmware/config.txt` (fallback: `/boot/config.txt`)
3. **Check logs:** `sudo journalctl -u ili9488-daemon -n 100` for specific error
4. **Manual test:**
   ```bash
   sudo /usr/bin/ili9488-daemon --shm /ili9488_rgb666 --width 320 --height 480
   ```
5. **Reinstall package:**
   ```bash
   sudo dpkg --remove ili9488-daemon
   sudo dpkg -i ili9488-daemon_1.1.0_arm64.deb
   sudo reboot
   ```

## Build Scripts

- `scripts/setup.sh`: Install cross-compile dependencies on Ubuntu
- `scripts/build.sh`: Build natively or cross-compile (set `TOOLCHAIN_FILE`)
- `scripts/deploy.sh`: Deploy `ili9488-daemon` binary to Pi via SSH
- `scripts/benchmark.sh`: Run performance benchmarks (FPS, CPU, memory)
- `scripts/frame_generator.c`: Reference implementation for frame producer

## Conclusion

The **Raspberry Pi ILI9488 Display Driver** is a high-performance, production-ready solution for adding small SPI display capability to headless Pi Zero 2W systems. Key achievements:

### Performance Summary (Pi Zero 2W, 64-bit OS)
- **Framerate:** ~12 FPS sustained across all rotation angles (SPI bandwidth limited, not CPU)
- **CPU Efficiency:** 0.2-0.3% average CPU usage with continuous frame stream (idle: <0.1%)
- **Memory Footprint:** ~3.5 MB total (daemon + triple-buffer), leaves 508 MB free on 512 MB Pi
- **Latency:** ~56-80 ms frame-to-display (SPI transfer dominated)
- **All rotations supported:** 0°, 90°, 180°, 270° with identical performance

### Technical Strengths
1. **Zero-copy architecture** — Triple-buffer with GPU DMA eliminates CPU memory copies
2. **GPU acceleration** — 90°/270° rotation via BCM DMA (non-blocking, minimal overhead)
3. **CMA-based allocation** — Physically contiguous, GPU-accessible, cache-coherent buffers
4. **Semaphore synchronization** — Non-blocking I/O, prevents tearing, efficient event handling
5. **Headless-optimized** — ~0.2% CPU ideal for embedded applications, IoT, kiosk displays

### Limitations
- **Framerate ceiling:** ~12 FPS (hardware limit of 65 MHz SPI + 460 KB framebuffer)
- **Color depth:** RGB666 (262K colors, hardware limit of ILI9488 via SPI)
- **Resolution:** 320×480 pixels (ILI9488 specific, not scalable to other controllers)
- **Display-only:** This driver does NOT include a window system, X11, or Wayland backend; you must write your own renderer

## License

This project uses the [fbcp-ili9341](https://github.com/juj/fbcp-ili9341) license. See `LICENSE.txt`.

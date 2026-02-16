# Raspberry Pi ILI9488 SPI Display Driver (DMA + GPU-accelerated)

## Overview

A high-performance framebuffer driver for **ILI9488 SPI TFT displays** on Raspberry Pi Zero 2W with 64-bit OS. This driver leverages:

- **DMA-accelerated SPI transfers** for zero-copy framebuffer streaming
- **GPU Mailbox API** for physically contiguous memory allocation
- **Contiguous Memory Allocator (CMA)** for modern Pi models with LPAE
- **Triple-buffering** for asynchronous I/O pipeline
- **GPU-accelerated 2D rotation** when available

The implementation is modernized for 64-bit Raspberry Pi OS (kernel >=5.x) and inspired by fbcp-ili9341.

**Note:** This is a display driver, not a terminal emulator. You must write your own application to convert and push pixel data to the shared memory framebuffer. See [Important: This is a Display Driver, Not a Terminal Emulator](#important-this-is-a-display-driver-not-a-terminal-emulator) for details.

## Target Hardware

**Required:**
- Raspberry Pi Zero 2 W (or compatible 64-bit ARM SBC)
- 64-bit Raspberry Pi OS (Bookworm or later recommended)
- Linux kernel >=5.x
- ILI9488 SPI display (320×480 pixels, 3.5" or similar)

**Not supported:**
- 32-bit OS / 32-bit Pi models
- Other display controllers (not ILI9488)
- Parallel/GPIO-based displays

## Installation

### 1. Install from Debian Package

```bash
sudo apt install ./fbcp-daemon_1.0.0_arm64.deb
```

The post-install script automatically configures:
- SPI enablement (`dtparam=spi=on`)
- SPI buffer size (65536 bytes) in kernel module
- CMA allocation (16MB) for GPU Mailbox support
- GPU memory (128MB) for framebuffers
- GPIO pin configuration (DC=24, RESET=25)

**After installation, you must reboot** for kernel configuration changes to take effect.

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
| VCC         | 3.3V Power  | 3V3               | Requires stable 3.3V supply
| SCL/SCLK    | SPI Clock   | GPIO11 (SPI0 CLK) | Standard SPI0 clock
| SDA/MOSI    | SPI MOSI    | GPIO10 (SPI0 MOSI)| Standard SPI0 data line
| MISO        | SPI MISO    | GPIO9 (SPI0 MISO) | Required (even if unused)
| CS/CE       | Chip Select | GPIO8 (SPI0 CE0)  | Standard SPI0 chip select
| DC          | Data/Cmd    | GPIO24            | Configurable in code
| RESET       | Hardware Reset | GPIO25         | Configurable in code
| BL          | Backlight   | 3V3 or GPIO       | Connect directly to 3V3 or control via GPIO

**Power Considerations:**
- The display typically draws 50-100mA at 3.3V
- Use a stable regulated power supply
- Add 100µF capacitor between VCC and GND for filtering
- Recommended: 1kΩ series resistors on SPI signal lines for EMI protection

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
3. Writes the converted framebuffer to the shared memory region (`/fbcp_rgb666`)
4. Repeats at your desired refresh rate

This architecture enables maximum flexibility—you can render anything to the display, not just terminal output.

## Running the Daemon

```bash
sudo fbcp_daemon --shm /fbcp_rgb666 --width 480 --height 320 --rotation 270 --fps 1
```

### Command-line Options

| Option | Default | Description |
|--------|---------|-------------|
| `--shm <path>` | `/fbcp_rgb666` | Shared memory name for framebuffer |
| `--width <px>` | 480 | Display width in pixels |
| `--height <px>` | 320 | Display height in pixels |
| `--rotation <deg>` | 0 | Rotation: 0, 90, 180, or 270 degrees |
| `--fps <rate>` | 0 | Show FPS overlay (updates per second) |

**Note:** The daemon must run as root to access `/dev/vcio` (GPU Mailbox) and `/dev/mem`.

## Shared Memory Protocol

The daemon creates and manages a POSIX shared memory region containing the framebuffer. Client applications write rendered frames to this shared memory, and the daemon DMA-transfers them to the display.

### Memory Layout

```
Shared Memory: /fbcp_rgb666
Total size: width × height × 3 bytes (e.g., 480 × 320 × 3 = 460,800 bytes for default)
Pixel format: RGB666 (18 bits per pixel, 3 bytes per pixel)
Byte order: Big-endian (R first, then G, then B)
```

### RGB666 Format

Each pixel occupies **3 bytes** with the following bit layout:

```
Byte 0 (R): RRRRR1CC  (top 6 bits = red, bottom 2 bits = color component C)
Byte 1 (G): GGGGGG00  (top 6 bits = green, bottom 2 bits unused)
Byte 2 (B): BBBBBBMM  (top 6 bits = blue, bottom 2 bits unused)
```

The display only uses the **top 6 bits of each byte** (18-bit color), allowing 262,144 colors (64 × 64 × 64).

**Limitation:** Unlike RGB888 (24-bit color with 8 bits per channel), RGB666 reduces color depth by 2 bits per channel. This is a hardware limitation of the ILI9488's parallel interface when driven via SPI.

### Writing to Shared Memory

Example pseudocode in C/C++:

```cpp
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// Open shared memory
int shm_fd = shm_open("/fbcp_rgb666", O_RDWR, 0666);
if (shm_fd < 0) {
    perror("shm_open");
    return 1;
}

// Map to address space
size_t buffer_size = 480 * 320 * 3;  // width × height × 3
uint8_t* framebuffer = (uint8_t*)mmap(
    NULL,
    buffer_size,
    PROT_WRITE | PROT_READ,
    MAP_SHARED,
    shm_fd,
    0
);
close(shm_fd);

// Write a pixel (x, y) in RGB format
void set_pixel(uint8_t* fb, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    size_t offset = (y * 480 + x) * 3;
    fb[offset + 0] = (r & 0xFC) | 0x00;  // Keep top 6 bits of R
    fb[offset + 1] = (g & 0xFC) | 0x00;  // Keep top 6 bits of G
    fb[offset + 2] = (b & 0xFC) | 0x00;  // Keep top 6 bits of B
}

// Fill entire framebuffer with color
for (int y = 0; y < 320; y++) {
    for (int x = 0; x < 480; x++) {
        set_pixel(framebuffer, x, y, 255, 0, 0);  // Red
    }
}

// Clean up
munmap(framebuffer, buffer_size);
```

## Rotation

Rotation is applied **before** DMA transfer and uses GPU-accelerated 2D transformation when available (otherwise falls back to CPU).

When specifying rotation, adjust your width/height accordingly:

| Rotation | Input Size | Output Size |
|----------|-----------|------------|
| 0°       | 480×320   | 480×320    |
| 90°      | 320×480   | 480×320    |
| 180°     | 480×320   | 480×320    |
| 270°     | 320×480   | 480×320    |

Example: For a portrait display rotated 270°, provide `--width 320 --height 480` (note: order is reversed).

## Performance Characteristics

- **Refresh Rate:** Up to 60 FPS (limited by SPI bandwidth and display controller)
- **Latency:** Sub-millisecond (triple-buffering minimizes blocking)
- **CPU Usage:** ~1.1% at idle (DMA offloads transfers entirely, minimal CPU involvement)
- **Memory:** ~500 KB shared memory + kernel buffers

## Technical Details

### DMA and Memory Management

The driver uses three memory management strategies:

1. **CMA (Contiguous Memory Allocator)** - Primary on modern Pi models (Pi 4, 5)
   - Kernel allocates physically contiguous memory at boot (16MB configured)
   - Accessed via DMA-BUF heap interface (`/dev/dma_heap/`)
   - Enables zero-copy DMA transfers

2. **GPU Mailbox API** - Fallback for older systems
   - Legacy GPU memory allocation interface
   - May fail on Pi models with LPAE (>4GB physical address space)
   - Requires `/dev/vcio` access

3. **CPU Buffers** - Fallback when no GPU/CMA support
   - Reduces performance but maintains compatibility
   - Framebuffer copied to DMA-capable memory before transfer

### SPI DMA Transfer

- Buffers must be **page-aligned** (4KB) for kernel DMA engine
- Transfer chunk size: **65536 bytes** per DMA operation
- SPI transfers entire framebuffer without blocking application thread
- Triple-buffering allows continuous rendering while transfers are in-flight

### GPIO Configuration

The post-install script configures GPIO pins as outputs:
- **GPIO 24 (DC):** Data/Command control line
- **GPIO 25 (RESET):** Hardware reset line

These are automatically configured in `/boot/firmware/config.txt` (or `/boot/config.txt` on older systems):

```
gpio=24=op
gpio=25=op
```

## Systemd Service

After installation, the daemon runs as a systemd service:

```bash
# View status
sudo systemctl status fbcp-daemon

# Restart
sudo systemctl restart fbcp-daemon

# View logs
sudo journalctl -u fbcp-daemon -f
```

The service is configured in `/etc/systemd/system/fbcp-daemon.service` and uses settings from `/etc/default/fbcp-daemon`.

## Troubleshooting

### Display shows garbage or is blank

1. Verify SPI is enabled: `raspi-config` → Interface Options → SPI
2. Check GPIO wiring (especially DC and RESET pins)
3. Ensure 3.3V power supply is stable
4. Verify framebuffer is being written: `ls -la /dev/shm/fbcp_rgb666`

### Daemon crashes with "SIGBUS" or "Permission denied"

1. Ensure running as root: `sudo fbcp_daemon ...`
2. Reboot after package installation (configures CMA and GPU memory)
3. Check kernel version: `uname -r` (should be >=5.x)
4. Verify 64-bit OS: `uname -m` (should be `aarch64`)

### Low frame rate or tearing

1. Increase SPI speed (modify `spi_config.speed_hz` in source)
2. Use rotation=0 if possible (avoids GPU transformation overhead)
3. Ensure no other SPI devices on the bus
4. Check system load: `top` or `htop`

## Build Scripts

- `scripts/setup.sh`: Install cross-compile dependencies on Ubuntu
- `scripts/build.sh`: Build natively or cross-compile (set `TOOLCHAIN_FILE`)
- `scripts/deploy.sh`: Deploy `fbcp_daemon` binary to Pi via SSH

## License

This project uses the fbcp-ili9341 license. See `LICENSE.txt`.

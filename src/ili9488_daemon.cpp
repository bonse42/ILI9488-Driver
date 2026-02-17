#include "ili9488_dma.h"
#include "ili9488_mailbox.h"
#include "ili9488_rotate.h"
#include "spi_dma_linux.h"
#include "pixel_utils.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <semaphore.h>

namespace {
volatile std::sig_atomic_t g_running = 1;

void HandleSignal(int) {
    g_running = 0;
}

struct Options {
    std::string shm_name;
    uint32_t width = 0;
    uint32_t height = 0;
    int rotation_degrees = 0;
    bool overlay_fps = true;
    uint32_t max_fps = 20;
};

uint32_t ParseUintEnv(const char* value) {
    if (!value) {
        return 0;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    return (end && *end == '\0') ? static_cast<uint32_t>(parsed) : 0U;
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    if (const char* env_name = std::getenv("ILI9488_SHM_NAME")) {
        options.shm_name = env_name;
    }
    options.width = ParseUintEnv(std::getenv("ILI9488_WIDTH"));
    options.height = ParseUintEnv(std::getenv("ILI9488_HEIGHT"));
    options.rotation_degrees = static_cast<int>(ParseUintEnv(std::getenv("ILI9488_ROTATION")));
    options.overlay_fps = ParseUintEnv(std::getenv("ILI9488_FPS_OVERLAY")) != 0U;
    const uint32_t env_max_fps = ParseUintEnv(std::getenv("ILI9488_MAX_FPS"));
    if (env_max_fps > 0) {
        options.max_fps = env_max_fps;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        constexpr const char* kShmPrefix = "--shm=";
        constexpr const char* kWidthPrefix = "--width=";
        constexpr const char* kHeightPrefix = "--height=";
        constexpr const char* kRotationPrefix = "--rotation=";
        constexpr const char* kOverlayFpsPrefix = "--fps-overlay=";
        constexpr const char* kMaxFpsPrefix = "--max-fps=";
        if (arg.rfind(kShmPrefix, 0) == 0) {
            options.shm_name = arg.substr(std::strlen(kShmPrefix));
        } else if (arg == "--shm" && i + 1 < argc) {
            options.shm_name = argv[++i];
        } else if (arg.rfind(kWidthPrefix, 0) == 0) {
            options.width = ParseUintEnv(arg.c_str() + std::strlen(kWidthPrefix));
        } else if (arg == "--width" && i + 1 < argc) {
            options.width = ParseUintEnv(argv[++i]);
        } else if (arg.rfind(kHeightPrefix, 0) == 0) {
            options.height = ParseUintEnv(arg.c_str() + std::strlen(kHeightPrefix));
        } else if (arg == "--height" && i + 1 < argc) {
            options.height = ParseUintEnv(argv[++i]);
        } else if (arg.rfind(kRotationPrefix, 0) == 0) {
            options.rotation_degrees = static_cast<int>(ParseUintEnv(arg.c_str() + std::strlen(kRotationPrefix)));
        } else if (arg == "--rotation" && i + 1 < argc) {
            options.rotation_degrees = static_cast<int>(ParseUintEnv(argv[++i]));
        } else if (arg.rfind(kOverlayFpsPrefix, 0) == 0) {
            options.overlay_fps = ParseUintEnv(arg.c_str() + std::strlen(kOverlayFpsPrefix)) != 0U;
        } else if (arg == "--fps-overlay" && i + 1 < argc) {
            options.overlay_fps = ParseUintEnv(argv[++i]) != 0U;
        } else if (arg.rfind(kMaxFpsPrefix, 0) == 0) {
            options.max_fps = ParseUintEnv(arg.c_str() + std::strlen(kMaxFpsPrefix));
        } else if (arg == "--max-fps" && i + 1 < argc) {
            options.max_fps = ParseUintEnv(argv[++i]);
        }
    }
    return options;
}

constexpr uint8_t kFontHeight = 8;
constexpr uint8_t kFontWidth = 8;

struct Glyph {
    char ch;
    uint8_t rows[8];
};

constexpr Glyph kFont[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {':', {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00}},
    {'F', {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00}},
    {'P', {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00}},
    {'S', {0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00}},
    {'0', {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00}},
    {'1', {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}},
    {'2', {0x3C, 0x66, 0x06, 0x1C, 0x30, 0x60, 0x7E, 0x00}},
    {'3', {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00}},
    {'4', {0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00}},
    {'5', {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00}},
    {'6', {0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00}},
    {'7', {0x7E, 0x66, 0x0C, 0x18, 0x18, 0x18, 0x18, 0x00}},
    {'8', {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}},
    {'9', {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00}}
};

const Glyph* FindGlyph(char ch) {
    for (const auto& glyph : kFont) {
        if (glyph.ch == ch) {
            return &glyph;
        }
    }
    return &kFont[0];
}

void DrawChar(uint8_t* buffer, uint32_t width, uint32_t height, size_t stride_bytes,
              uint32_t x, uint32_t y, char ch, uint8_t r, uint8_t g, uint8_t b) {
    const Glyph* glyph = FindGlyph(ch);
    for (uint32_t row = 0; row < kFontHeight; ++row) {
        if (y + row >= height) {
            continue;
        }
        const uint8_t bits = glyph->rows[row];
        uint8_t* dst = buffer + static_cast<size_t>(y + row) * stride_bytes;
        for (uint32_t col = 0; col < kFontWidth; ++col) {
            if (x + col >= width) {
                continue;
            }
            if (bits & (0x80 >> col)) {
                size_t pixel_offset = static_cast<size_t>(x + col) * 3U;
                dst[pixel_offset + 0] = r;
                dst[pixel_offset + 1] = g;
                dst[pixel_offset + 2] = b;
            }
        }
    }
}

void DrawText(uint8_t* buffer, uint32_t width, uint32_t height, size_t stride_bytes,
              uint32_t x, uint32_t y, const std::string& text,
              uint8_t r, uint8_t g, uint8_t b) {
    uint32_t cursor_x = x;
    for (char ch : text) {
        DrawChar(buffer, width, height, stride_bytes, cursor_x, y, ch, r, g, b);
        cursor_x += kFontWidth;
        if (cursor_x >= width) {
            break;
        }
    }
}
}

int main(int argc, char** argv) {
    const Options options = ParseOptions(argc, argv);
    if (options.shm_name.empty() || options.width == 0 || options.height == 0) {
        std::cerr << "Usage: ili9488_daemon --shm <name> --width <w> --height <h>"
                     " [--rotation <deg>] [--fps <0|1>]\n"
                     "Or set ILI9488_SHM_NAME/ILI9488_WIDTH/ILI9488_HEIGHT/ILI9488_ROTATION/ILI9488_FPS"
                     " in /etc/default/ili9488-daemon.\n";
        return 1;
    }
    if (options.rotation_degrees != 0 && options.rotation_degrees != 90 &&
        options.rotation_degrees != 180 && options.rotation_degrees != 270) {
        std::cerr << "Rotation must be 0, 90, 180, or 270 degrees.\n";
        return 1;
    }
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    const bool swap_axes = options.rotation_degrees == 90 || options.rotation_degrees == 270;
    const uint32_t framebuffer_width = swap_axes ? options.height : options.width;
    const uint32_t framebuffer_height = swap_axes ? options.width : options.height;
    const int rotation_to_apply = (360 - options.rotation_degrees) % 360;
    const size_t stride_bytes = static_cast<size_t>(framebuffer_width) * 3U;
    const size_t framebuffer_bytes = stride_bytes * static_cast<size_t>(framebuffer_height);
    const size_t display_bytes = static_cast<size_t>(options.width) * options.height * 3U;

    ili9488::DisplayConfig cfg;
    cfg.width = options.width;
    cfg.height = options.height;
    cfg.output_format = ili9488::OutputFormat::Rgb666;
    cfg.rotation = ili9488::Rotation::Deg0;
    cfg.use_gpu_mailbox = true;
    ili9488::ILI9488Driver driver(cfg);
    if (!driver.initialize()) {
        std::cerr << "ERROR: Failed to initialize SPI DMA driver.\n";
        return 1;
    }

    ili9488::TripleBufferShmHeader* header = nullptr;
    int shm_fd = -1;
    if (!driver.getFramebuffer()->createTripleBufferSharedMemory(
        options.shm_name,
        framebuffer_width, framebuffer_height,
        &header, shm_fd)) {
        std::cerr << "ERROR: Failed to create triple-buffer shared memory.\n";
        return 1;
    }

    header->rotation_degrees = options.rotation_degrees;
    header->daemon_ready = 1;

    const bool use_zero_copy = driver.isUsingGpuMailbox();
    std::cerr << "\n=== ili9488-daemon startup (Zero-Copy Triple-Buffer) ===\n";
    std::cerr << "Display: " << options.width << "x" << options.height << " (RGB666)\n";
    std::cerr << "Rotation: " << options.rotation_degrees << "°\n";
    std::cerr << "Max FPS: " << options.max_fps << "\n";
    std::cerr << "FPS Overlay: " << (options.overlay_fps ? "enabled" : "disabled") << "\n";
    std::cerr << "\nFeature Status:\n";
    std::cerr << "  GPU Mailbox/CMA: " << (use_zero_copy ? "✓ AVAILABLE (zero-copy mode)" : "✗ UNAVAILABLE") << "\n";
    std::cerr << "  GPU Rotation: " << (options.rotation_degrees != 0 ? (use_zero_copy ? "✓ Available" : "✗ Fallback") : "- Not needed") << "\n";
    std::cerr << "  Shared Memory: " << options.shm_name << "\n";
    std::cerr << "==================================================\n\n";
    auto fps_start = std::chrono::steady_clock::now();
    size_t frames = 0;
    double fps = 0.0;
    const uint64_t frame_time_us = options.max_fps > 0 ? 1000000ULL / options.max_fps : 0ULL;
    auto frame_start = std::chrono::steady_clock::now();
    uint32_t last_frame_counter = 0;

    while (g_running) {
        if (sem_trywait(&header->pending_sem) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        uint8_t* pending_cpu = driver.getFramebuffer()->getPendingBuffer();
        uint8_t* back_cpu = driver.getFramebuffer()->getBackBuffer();

        if (pending_cpu == nullptr || back_cpu == nullptr) {
            sem_post(&header->pending_sem);
            break;
        }

        const uint32_t current_frame_counter = header->frame_counter;
        if (current_frame_counter != last_frame_counter) {
            uint8_t* shm_pending = driver.getFramebuffer()->getShmPendingBuffer();
            if (shm_pending != nullptr) {
                std::memcpy(pending_cpu, shm_pending, framebuffer_bytes);
            }
            last_frame_counter = current_frame_counter;
        }

        sem_post(&header->pending_sem);

        if (options.overlay_fps) {
            ++frames;
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_start);
            if (elapsed.count() >= 1000) {
                fps = (frames * 1000.0) / static_cast<double>(elapsed.count());
                frames = 0;
                fps_start = now;

                FILE* fps_log = std::fopen("/tmp/ili9488_benchmark.log", "a");
                if (fps_log != nullptr) {
                    std::fprintf(fps_log, "%.1f\n", fps);
                    std::fclose(fps_log);
                }
            }
            char fps_text[32];
            std::snprintf(fps_text, sizeof(fps_text), "FPS:%5.1f", fps);

            const uint32_t clear_x = 8;
            const uint32_t clear_y = 8;
            const uint32_t clear_w = static_cast<uint32_t>(std::strlen(fps_text)) * kFontWidth;
            const uint32_t clear_h = kFontHeight;
            for (uint32_t row = clear_y; row < clear_y + clear_h && row < framebuffer_height; ++row) {
                uint8_t* row_ptr = pending_cpu + static_cast<size_t>(row) * stride_bytes
                                   + static_cast<size_t>(clear_x) * 3U;
                std::memset(row_ptr, 0x00, static_cast<size_t>(clear_w) * 3U);
            }

            DrawText(pending_cpu, framebuffer_width, framebuffer_height, stride_bytes, 8, 8,
                     fps_text, 0xFC, 0xFC, 0xFC);
        }

        if (header->rotation_degrees == 0) {
            driver.getFramebuffer()->rotateBufferIndices();

            uint8_t* front_cpu = driver.getFramebuffer()->getFrontBuffer();
            driver.getTransport()->transferDma(front_cpu, display_bytes);
        } else {
            uint32_t pending_bus_addr = header->buffer_c_bus_addr;
            uint32_t back_bus_addr = header->buffer_b_bus_addr;

            bool rotated = false;
            if (pending_bus_addr != 0 && back_bus_addr != 0) {
                rotated = driver.getRotator()->rotateRgb666DmaMode(
                    pending_cpu, pending_bus_addr,
                    back_cpu, back_bus_addr,
                    framebuffer_width, framebuffer_height,
                    rotation_to_apply);
            }
            if (!rotated) {
                ili9488::pixel::RotateRgb666(pending_cpu, back_cpu,
                                             framebuffer_width, framebuffer_height,
                                             rotation_to_apply);
            }

            driver.getFramebuffer()->swapBackAndFront();

            uint8_t* front_cpu = driver.getFramebuffer()->getFrontBuffer();
            driver.getTransport()->transferDma(front_cpu, display_bytes);
        }

        if (frame_time_us > 0) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - frame_start).count();
            if (elapsed_us < static_cast<int64_t>(frame_time_us)) {
                const uint64_t sleep_us = frame_time_us - elapsed_us;
                std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
            }
            frame_start = std::chrono::steady_clock::now();
        }
    }

    driver.getFramebuffer()->cleanupSharedMemory();

    return 0;
}

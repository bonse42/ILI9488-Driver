#include "fbcp_dma.h"
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
#include <vector>

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
};

struct ShmHeader {
    uint32_t width;
    uint32_t height;
};

uint32_t ParseUintEnv(const char* value) {
    if (!value) {
        return 0;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    return (end && *end == '\0') ? static_cast<uint32_t>(parsed) : 0U;
}

int OpenSharedMemory(const std::string& name, size_t size) {
    umask(0);
    std::string shm_name = name.empty() ? "/fbcp_rgb666" : name;
    if (shm_name[0] != '/') {
        shm_name.insert(shm_name.begin(), '/');
    }
    int fd = shm_open(shm_name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0 && errno == EEXIST) {
        fd = shm_open(shm_name.c_str(), O_RDWR | O_CLOEXEC, 0);
    }
    if (fd < 0 && errno == EACCES) {
        shm_unlink(shm_name.c_str());
        fd = shm_open(shm_name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    }
    if (fd < 0 && (errno == EACCES || errno == ENOENT)) {
        std::string path = "/dev/shm" + shm_name;
        fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    }
    if (fd < 0) {
        std::perror("Failed to open shared memory");
        return -1;
    }
    if (fchmod(fd, 0666) < 0) {
        std::perror("Failed to chmod shared memory");
    }
    if (ftruncate(fd, static_cast<off_t>(size)) < 0) {
        std::perror("Failed to size shared memory");
        close(fd);
        return -1;
    }
    return fd;
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    if (const char* env_name = std::getenv("FBCP_SHM_NAME")) {
        options.shm_name = env_name;
    }
    options.width = ParseUintEnv(std::getenv("FBCP_WIDTH"));
    options.height = ParseUintEnv(std::getenv("FBCP_HEIGHT"));
    options.rotation_degrees = static_cast<int>(ParseUintEnv(std::getenv("FBCP_ROTATION")));
    options.overlay_fps = ParseUintEnv(std::getenv("FBCP_FPS")) != 0U;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        constexpr const char* kShmPrefix = "--shm=";
        constexpr const char* kWidthPrefix = "--width=";
        constexpr const char* kHeightPrefix = "--height=";
        constexpr const char* kRotationPrefix = "--rotation=";
        constexpr const char* kOverlayFpsPrefix = "--fps=";
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
        } else if (arg == "--fps" && i + 1 < argc) {
            options.overlay_fps = ParseUintEnv(argv[++i]) != 0U;
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
        std::cerr << "Usage: fbcp_daemon --shm <name> --width <w> --height <h>"
                     " [--rotation <deg>] [--fps <0|1>]\n"
                     "Or set FBCP_SHM_NAME/FBCP_WIDTH/FBCP_HEIGHT/FBCP_ROTATION/FBCP_FPS"
                     " in /etc/default/fbcp-daemon.\n";
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
    const size_t shm_bytes = sizeof(ShmHeader) + framebuffer_bytes;
    const int shm_fd = OpenSharedMemory(options.shm_name, shm_bytes);
    if (shm_fd < 0) {
        return 1;
    }
    void* shm_map = mmap(nullptr, shm_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_map == MAP_FAILED) {
        std::perror("Failed to mmap shared memory");
        close(shm_fd);
        return 1;
    }
    fbcp::DisplayConfig cfg;
    cfg.width = options.width;
    cfg.height = options.height;
    cfg.output_format = fbcp::OutputFormat::Rgb666;
    cfg.rotation = fbcp::Rotation::Deg0;
    cfg.use_gpu_mailbox = true;
    fbcp::FbcpDriver driver(cfg);
    if (!driver.initialize()) {
        std::cerr << "Failed to initialize SPI DMA driver.\n";
        munmap(shm_map, shm_bytes);
        close(shm_fd);
        return 1;
    }
    const size_t display_bytes = static_cast<size_t>(options.width) * options.height * 3U;
    const bool use_zero_copy = driver.isUsingGpuMailbox();
    std::vector<uint8_t> source_frame;
    std::vector<uint8_t> packed_frame(display_bytes);
    source_frame.resize(framebuffer_bytes);
    auto* shm_header = static_cast<ShmHeader*>(shm_map);
    shm_header->width = framebuffer_width;
    shm_header->height = framebuffer_height;
    auto* shm_base = reinterpret_cast<uint8_t*>(shm_header + 1);
    auto fps_start = std::chrono::steady_clock::now();
    size_t frames = 0;
    double fps = 0.0;
    while (g_running) {
        uint8_t* frame_ptr = nullptr;
        if (use_zero_copy) {
            frame_ptr = driver.gpuBackBuffer();
            if (frame_ptr == nullptr) {
                break;
            }
            if (reinterpret_cast<uintptr_t>(frame_ptr) < 0x1000) {
                break;
            }
            std::memcpy(frame_ptr, shm_base, framebuffer_bytes);
        } else {
            std::memcpy(source_frame.data(), shm_base, framebuffer_bytes);
            frame_ptr = source_frame.data();
        }
        if (options.overlay_fps) {
            ++frames;
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_start);
            if (elapsed.count() >= 1000) {
                fps = (frames * 1000.0) / static_cast<double>(elapsed.count());
                frames = 0;
                fps_start = now;
            }
            char fps_text[32];
            std::snprintf(fps_text, sizeof(fps_text), "FPS:%5.1f", fps);
            DrawText(frame_ptr, framebuffer_width, framebuffer_height, stride_bytes, 8, 8,
                     fps_text, 0xFC, 0xFC, 0xFC);
        }
        fbcp::pixel::RotateRgb666(frame_ptr, packed_frame.data(), framebuffer_width,
                                  framebuffer_height, rotation_to_apply);
        if (use_zero_copy) {
            uint8_t* gpu_buf = driver.gpuBackBuffer();
            if (gpu_buf == nullptr) {
                break;
            }
            std::memcpy(gpu_buf, packed_frame.data(), display_bytes);
        } else {
            driver.renderFrameRgb666(packed_frame.data());
        }
        driver.swapBuffers();
    }
    munmap(shm_map, shm_bytes);
    close(shm_fd);
    return 0;
}

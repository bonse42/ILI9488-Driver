#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ili9488 {

enum class Rotation {
    Deg0,
    Deg90,
    Deg180,
    Deg270
};

enum class InputFormat {
    Rgb888,
    Rgba8888
};

enum class OutputFormat {
    Rgb666,
    Rgb565
};

struct DisplayConfig {
    uint32_t width;
    uint32_t height;
    uint32_t spi_hz = 65000000;
    uint32_t spi_init_hz = 4000000;
    uint8_t spi_mode = 0;
    uint8_t bits_per_word = 8;
    std::string spi_device = "/dev/spidev0.0";
    int dc_gpio = 24;
    int reset_gpio = 25;
    Rotation rotation;
    OutputFormat output_format = OutputFormat::Rgb666;
    bool use_double_buffer = true;
    bool use_gpu_mailbox = true;
};

class ILI9488Transport;
class ILI9488Framebuffer;
namespace gpu {
class ILI9488Rotate;
}

class ILI9488Driver {
public:
    explicit ILI9488Driver(const DisplayConfig& cfg);
    ~ILI9488Driver();
    bool initialize();
    void renderFrameRgb666(const uint8_t* rgb666_pixels);
    void renderFrameRgb666ZeroCopy(uint32_t bus_addr, const uint8_t* cpu_addr);
    uint8_t* gpuBackBuffer();
    uint32_t gpuBackBufferBusAddr() const;
    uint32_t gpuFrontBufferBusAddr() const;
    void swapBuffers();
    bool isUsingGpuMailbox() const;
    bool rotateFrameGpu(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, int rotation_degrees);
    ILI9488Framebuffer* getFramebuffer() { return gpu_.get(); }
    ILI9488Transport* getTransport() { return spi_.get(); }
    gpu::ILI9488Rotate* getRotator() { return gpu_rotate_.get(); }

private:
    size_t bytesPerPixel() const;
    void writeFrameDma(const uint8_t* buf);
    void writeFrameDmaFromBusAddr(uint32_t bus_addr, size_t size);
    DisplayConfig config_;
    std::unique_ptr<ILI9488Transport> spi_;
    std::unique_ptr<ILI9488Framebuffer> gpu_;
    std::unique_ptr<gpu::ILI9488Rotate> gpu_rotate_;
    std::vector<uint8_t> backBuffer_;
    std::vector<uint8_t> frontBuffer_;
    bool zero_copy_mode_;
    uint32_t pending_bus_addr_;
};

}

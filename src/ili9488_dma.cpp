#include "ili9488_dma.h"
#include "ili9488_mailbox.h"
#include "ili9488_rotate.h"
#include "spi_dma_linux.h"
#include <cstring>

namespace ili9488 {

ILI9488Driver::ILI9488Driver(const DisplayConfig& cfg)
    : config_(cfg),
      spi_(std::make_unique<ILI9488Transport>()),
      gpu_(std::make_unique<ILI9488Framebuffer>()),
      gpu_rotate_(std::make_unique<gpu::ILI9488Rotate>()),
      zero_copy_mode_(false),
      pending_bus_addr_(0) {}

ILI9488Driver::~ILI9488Driver() = default;

bool ILI9488Driver::initialize() {
    SpiConfig spi_config {};
    spi_config.device = config_.spi_device;
    spi_config.speed_hz = config_.spi_hz;
    spi_config.init_speed_hz = config_.spi_init_hz;
    spi_config.mode = config_.spi_mode;
    spi_config.bits_per_word = config_.bits_per_word;
    spi_config.pixel_format = 0x66;
    spi_config.width = config_.width;
    spi_config.height = config_.height;
    spi_config.transfer_chunk_bytes = 65536;
    spi_config.rotation_degrees = static_cast<int>(config_.rotation == Rotation::Deg0   ? 0
                                               : config_.rotation == Rotation::Deg90  ? 90
                                               : config_.rotation == Rotation::Deg180 ? 180
                                                                                       : 270);
    spi_config.dc_gpio = config_.dc_gpio;
    spi_config.reset_gpio = config_.reset_gpio;
    if (!spi_->initialize(spi_config)) {
        return false;
    }
    bool enable_mailbox = config_.use_gpu_mailbox;
#ifndef ILI9488_DMA_USE_GPU_MAILBOX
    enable_mailbox = false;
#endif
    if (!gpu_->initialize(config_.width, config_.height, enable_mailbox)) {
        return false;
    }
    if (gpu_->usingMailbox()) {
        zero_copy_mode_ = true;
    } else {
        zero_copy_mode_ = false;
        const size_t buffer_bytes = static_cast<size_t>(config_.width) * config_.height * bytesPerPixel();
        frontBuffer_.resize(buffer_bytes);
        backBuffer_.resize(buffer_bytes);
    }
    bool enable_gpu_rotation = zero_copy_mode_;
    gpu_rotate_->initialize(enable_gpu_rotation);
    return true;
}

void ILI9488Driver::renderFrameRgb666(const uint8_t* rgb666_pixels) {
    if (zero_copy_mode_) {
        uint8_t* back_buf = gpu_->backBuffer();
        const size_t buffer_bytes = static_cast<size_t>(config_.width) * config_.height * 3U;
        std::memcpy(back_buf, rgb666_pixels, buffer_bytes);
    } else {
        const size_t buffer_bytes = static_cast<size_t>(config_.width) * config_.height * 3U;
        if (config_.use_double_buffer) {
            std::memcpy(backBuffer_.data(), rgb666_pixels, buffer_bytes);
        } else {
            std::memcpy(frontBuffer_.data(), rgb666_pixels, buffer_bytes);
        }
    }
}

void ILI9488Driver::renderFrameRgb666ZeroCopy(uint32_t bus_addr, const uint8_t* cpu_addr) {
    if (!zero_copy_mode_) {
        const size_t buffer_bytes = static_cast<size_t>(config_.width) * config_.height * 3U;
        renderFrameRgb666(cpu_addr);
        return;
    }
    pending_bus_addr_ = bus_addr;
}

uint8_t* ILI9488Driver::gpuBackBuffer() {
    return gpu_->backBuffer();
}

uint32_t ILI9488Driver::gpuBackBufferBusAddr() const {
    return gpu_->backBufferBusAddr();
}

uint32_t ILI9488Driver::gpuFrontBufferBusAddr() const {
    return gpu_->frontBufferBusAddr();
}

bool ILI9488Driver::isUsingGpuMailbox() const {
    return zero_copy_mode_;
}

void ILI9488Driver::swapBuffers() {
    if (zero_copy_mode_) {
        if (config_.use_double_buffer) {
            gpu_->swapBuffers();
        }
        writeFrameDma(gpu_->frontBuffer());
        pending_bus_addr_ = 0;
    } else {
        if (config_.use_double_buffer) {
            frontBuffer_.swap(backBuffer_);
        }
        writeFrameDma(frontBuffer_.data());
    }
}

size_t ILI9488Driver::bytesPerPixel() const {
    return 3U;
}

void ILI9488Driver::writeFrameDma(const uint8_t* buf) {
    spi_->transferDma(buf, static_cast<size_t>(config_.width) * config_.height * bytesPerPixel());
}

void ILI9488Driver::writeFrameDmaFromBusAddr(uint32_t bus_addr, size_t size) {
    spi_->transferDmaFromBusAddr(bus_addr, size);
}

bool ILI9488Driver::rotateFrameGpu(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, int rotation_degrees) {
    if (!gpu_rotate_) {
        return false;
    }
    return gpu_rotate_->rotateRgb666(src, 0, dst, 0, width, height, rotation_degrees);
}

}

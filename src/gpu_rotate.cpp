#include "gpu_rotate.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

namespace fbcp::gpu {

namespace {

constexpr uint32_t kBcm2835PeriphBase = 0x20000000;
constexpr uint32_t kDmaBaseOffset = 0x7000;
constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kBusAddressMask = 0x3FFFFFFF;

constexpr uint32_t kDefaultDmaChannel = 7;

constexpr uint32_t kDmaCs = 0x00;
constexpr uint32_t kDmaConblkAd = 0x04;
constexpr uint32_t kDmaTi = 0x08;
constexpr uint32_t kDmaSourceAd = 0x0C;
constexpr uint32_t kDmaDestAd = 0x10;
constexpr uint32_t kDmaLen = 0x14;
constexpr uint32_t kDmaStride = 0x18;
constexpr uint32_t kDmaNextconbk = 0x1C;

constexpr uint32_t kDmaCsActive = 1 << 0;
constexpr uint32_t kDmaCsEnd = 1 << 1;
constexpr uint32_t kDmaCsInt = 1 << 2;
constexpr uint32_t kDmaCsReset = 1 << 31;
constexpr uint32_t kDmaCsWaitWriteResp = 1 << 28;

constexpr uint32_t kDmaTiSrcInc = 1 << 8;
constexpr uint32_t kDmaTiDestInc = 1 << 4;
constexpr uint32_t kDmaTi2d = 1 << 1;

constexpr size_t kBytesPerPixel = 3;

bool TryReadPeripheralBase(uint32_t* base_out) {
    if (base_out == nullptr) {
        return false;
    }
    int fd = open("/proc/device-tree/soc/ranges", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    uint8_t buffer[8] = {};
    const ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
    close(fd);
    if (bytes_read < static_cast<ssize_t>(sizeof(buffer))) {
        return false;
    }
    const uint32_t base = (static_cast<uint32_t>(buffer[4]) << 24) |
                          (static_cast<uint32_t>(buffer[5]) << 16) |
                          (static_cast<uint32_t>(buffer[6]) << 8) |
                          static_cast<uint32_t>(buffer[7]);
    if (base == 0) {
        return false;
    }
    *base_out = base;
    return true;
}

}

GpuRotate::GpuRotate()
    : dma_available_(false),
      mem_fd_(-1),
      dma_channel_(kDefaultDmaChannel),
      dma_regs_map_(nullptr),
      dma_regs_(nullptr) {}

GpuRotate::~GpuRotate() {
    cleanupDmaController();
}

bool GpuRotate::initialize(bool enable_dma) {
    dma_available_ = false;

    if (!enable_dma) {
        return true;
    }

    if (!setupDmaController()) {
        return true;
    }

    dma_available_ = true;
    return true;
}

bool GpuRotate::setupDmaController() {
    uint32_t periph_base = kBcm2835PeriphBase;
    TryReadPeripheralBase(&periph_base);

    mem_fd_ = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (mem_fd_ < 0) {
        return false;
    }

    const uint32_t dma_base = periph_base + kDmaBaseOffset;
    const uint32_t dma_offset = dma_channel_ * 0x100;
    const uint32_t dma_reg_addr = dma_base + dma_offset;
    const uint32_t dma_map_base = dma_reg_addr & ~(kPageSize - 1U);
    const uint32_t dma_map_offset = dma_reg_addr - dma_map_base;

    void* dma_map = mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                         mem_fd_, dma_map_base);
    if (dma_map == MAP_FAILED) {
        close(mem_fd_);
        mem_fd_ = -1;
        return false;
    }

    dma_regs_map_ = dma_map;
    dma_regs_ = reinterpret_cast<volatile uint32_t*>(
        static_cast<uint8_t*>(dma_map) + dma_map_offset);

    dma_regs_[kDmaCs / 4] = kDmaCsReset;
    usleep(10);
    dma_regs_[kDmaCs / 4] = 0;

    return true;
}

void GpuRotate::cleanupDmaController() {
    if (dma_regs_ != nullptr && dma_regs_map_ != nullptr) {
        dma_regs_[kDmaCs / 4] = kDmaCsReset;
        usleep(10);
    }

    if (dma_regs_map_ != nullptr) {
        munmap(dma_regs_map_, kPageSize);
        dma_regs_map_ = nullptr;
        dma_regs_ = nullptr;
    }

    if (mem_fd_ >= 0) {
        close(mem_fd_);
        mem_fd_ = -1;
    }

    dma_available_ = false;
}

bool GpuRotate::isDmaAvailable() const {
    return dma_available_;
}

bool GpuRotate::configureAndWaitDma(
    uint32_t src_bus_addr, uint32_t dst_bus_addr,
    uint32_t width, uint32_t height,
    int rotation_degrees) {

    if (!dma_available_ || dma_regs_ == nullptr) {
        return false;
    }

    uint32_t src_stride = 0;
    uint32_t dst_stride = 0;
    uint32_t xlen = 0;
    uint32_t ylen = 0;

    switch (rotation_degrees) {
        case 0:
        case 180: {
            xlen = width * kBytesPerPixel;
            ylen = height;
            src_stride = xlen;
            dst_stride = xlen;
            break;
        }
        case 90: {
            xlen = height * kBytesPerPixel;
            ylen = width;
            src_stride = width * kBytesPerPixel;
            dst_stride = height * kBytesPerPixel;
            break;
        }
        case 270: {
            xlen = height * kBytesPerPixel;
            ylen = width;
            src_stride = width * kBytesPerPixel;
            dst_stride = height * kBytesPerPixel;
            break;
        }
        default:
            return false;
    }

    dma_regs_[kDmaSourceAd / 4] = src_bus_addr;
    dma_regs_[kDmaDestAd / 4] = dst_bus_addr;

    dma_regs_[kDmaLen / 4] = xlen * ylen;

    dma_regs_[kDmaStride / 4] = ((ylen - 1) << 16) | (xlen & 0xFFFF);

    uint32_t ti = kDmaTiSrcInc | kDmaTiDestInc | kDmaTi2d | kDmaCsWaitWriteResp;
    dma_regs_[kDmaTi / 4] = ti;

    dma_regs_[kDmaCs / 4] = kDmaCsActive;

    const int max_wait_ms = 1000;
    const auto start = std::chrono::steady_clock::now();
    while ((dma_regs_[kDmaCs / 4] & kDmaCsActive) != 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() > max_wait_ms) {
            dma_regs_[kDmaCs / 4] = kDmaCsReset;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    return true;
}

bool GpuRotate::rotateRgb666DmaMode(
    const uint8_t* src, uint32_t src_bus_addr,
    uint8_t* dst, uint32_t dst_bus_addr,
    uint32_t width, uint32_t height,
    int rotation_degrees) {

    if (!dma_available_) {
        return false;
    }

    if (rotation_degrees == 0) {
        const size_t bytes = static_cast<size_t>(width) * height * kBytesPerPixel;
        std::memcpy(dst, src, bytes);
        return true;
    }

    if (src_bus_addr == 0 || dst_bus_addr == 0) {
        return false;
    }

    return configureAndWaitDma(src_bus_addr, dst_bus_addr, width, height, rotation_degrees);
}

bool GpuRotate::rotateRgb666(
    const uint8_t* src, uint32_t src_bus_addr,
    uint8_t* dst, uint32_t dst_bus_addr,
    uint32_t width, uint32_t height,
    int rotation_degrees) {

    if (dma_available_) {
        if (rotateRgb666DmaMode(src, src_bus_addr, dst, dst_bus_addr, width, height, rotation_degrees)) {
            return true;
        }
    }

    return false;
}

}

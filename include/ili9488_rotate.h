#pragma once
#include <cstddef>
#include <cstdint>

namespace ili9488::gpu {

class ILI9488Rotate {
public:
    ILI9488Rotate();
    ~ILI9488Rotate();
    bool initialize(bool enable_dma = true);
    bool rotateRgb666DmaMode(
        const uint8_t* src, uint32_t src_bus_addr,
        uint8_t* dst, uint32_t dst_bus_addr,
        uint32_t width, uint32_t height,
        int rotation_degrees);
    bool rotateRgb666(
        const uint8_t* src, uint32_t src_bus_addr,
        uint8_t* dst, uint32_t dst_bus_addr,
        uint32_t width, uint32_t height,
        int rotation_degrees);
    bool isDmaAvailable() const;
private:
    bool setupDmaController();
    bool configureAndWaitDma(
        uint32_t src_bus_addr, uint32_t dst_bus_addr,
        uint32_t width, uint32_t height,
        int rotation_degrees);
    void cleanupDmaController();
    bool dma_available_;
    int mem_fd_;
    int dma_channel_;
    void* dma_regs_map_;
    volatile uint32_t* dma_regs_;
};

}

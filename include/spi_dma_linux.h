#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fbcp {

struct SpiConfig {
    std::string device;
    uint32_t speed_hz;
    uint32_t init_speed_hz;
    uint8_t mode;
    uint8_t bits_per_word;
    uint8_t pixel_format;
    uint32_t width;
    uint32_t height;
    size_t transfer_chunk_bytes;
    int rotation_degrees;
    int dc_gpio;
    int reset_gpio;
};

class SpiDmaTransport {
public:
    SpiDmaTransport();
    ~SpiDmaTransport();
    bool initialize(const SpiConfig& config);
    bool transferDma(const uint8_t* buf, size_t length);
    bool transferDmaFromBusAddr(uint32_t bus_addr, size_t length);
    bool supportsBusAddrTransfer() const;
private:
    bool setGpioValue(int line_fd, bool value);
    int configureGpioOutput(int gpio, bool value);
    bool sendCommand(uint8_t command);
    bool sendData(const uint8_t* data, size_t length);
    bool sendDataFromBusAddr(uint32_t bus_addr, size_t length);
    bool initializePanel();
    bool setupDirectDma();
    void cleanupDirectDma();
    int spi_fd_;
    int gpio_chip_fd_;
    int dc_line_fd_;
    int reset_line_fd_;
    uint32_t current_speed_hz_;
    SpiConfig config_;
    bool direct_dma_available_;
    int mem_fd_;
    void* dma_regs_map_;
    void* spi_regs_map_;
    volatile uint32_t* dma_regs_;
    volatile uint32_t* spi_regs_;
    uint32_t dma_channel_;
    void* dma_cb_mem_;
    uint32_t dma_cb_bus_addr_;
};

struct DmaControlBlock {
    uint32_t transfer_info;
    uint32_t source_addr;
    uint32_t dest_addr;
    uint32_t transfer_length;
    uint32_t stride;
    uint32_t next_cb;
    uint32_t reserved[2];
};

}

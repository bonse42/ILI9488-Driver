#include "spi_dma_linux.h"

#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <cstdio>
#include <iostream>

namespace ili9488 {

namespace {
constexpr uint8_t kIli9488CmdSleepOut = 0x11;
constexpr uint8_t kIli9488CmdDisplayOn = 0x29;
constexpr uint8_t kIli9488CmdPixelFormat = 0x3A;
constexpr uint8_t kIli9488CmdMemoryAccessControl = 0x36;
constexpr uint8_t kIli9488CmdColumnAddressSet = 0x2A;
constexpr uint8_t kIli9488CmdPageAddressSet = 0x2B;
constexpr uint8_t kIli9488CmdMemoryWrite = 0x2C;
constexpr uint8_t kIli9488PixelFormatRgb666 = 0x66;
constexpr uint8_t kIli9488PixelFormatRgb565 = 0x55;
constexpr size_t kDefaultChunkSize = 4096;

constexpr uint32_t kBcm2835PeriphBase = 0x20000000;
constexpr uint32_t kDmaBaseOffset = 0x7000;
constexpr uint32_t kSpi0BaseOffset = 0x204000;
constexpr uint32_t kBusAddressMask = 0x3FFFFFFF;
constexpr uint32_t kPageSize = 4096;

uint32_t ReadBe32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

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
    const uint32_t base = ReadBe32(buffer + 4);
    if (base == 0) {
        return false;
    }
    *base_out = base;
    return true;
}

constexpr uint32_t kDmaCs = 0x00;
constexpr uint32_t kDmaConblkAd = 0x04;
constexpr uint32_t kDmaTi = 0x08;
constexpr uint32_t kDmaSourceAd = 0x0C;
constexpr uint32_t kDmaDestAd = 0x10;

constexpr uint32_t kDmaCsActive = 1 << 0;
constexpr uint32_t kDmaCsEnd = 1 << 1;
constexpr uint32_t kDmaCsReset = 1 << 31;

constexpr uint32_t kDmaTiSrcInc = 1 << 8;
constexpr uint32_t kDmaTiDestDreq = 1 << 6;
constexpr uint32_t kDmaTiPerMapSpi = 6 << 16;
constexpr uint32_t kDmaTiWaitResp = 1 << 3;
}

ILI9488Transport::ILI9488Transport()
    : spi_fd_(-1),
      gpio_chip_fd_(-1),
      dc_line_fd_(-1),
      reset_line_fd_(-1),
      current_speed_hz_(0),
      config_{},
      direct_dma_available_(false),
      mem_fd_(-1),
      dma_regs_map_(nullptr),
      spi_regs_map_(nullptr),
      dma_regs_(nullptr),
      spi_regs_(nullptr),
      dma_channel_(5),
      dma_cb_mem_(nullptr),
      dma_cb_bus_addr_(0) {}

ILI9488Transport::~ILI9488Transport() {
    cleanupDirectDma();

    if (dc_line_fd_ >= 0) {
        close(dc_line_fd_);
    }
    if (reset_line_fd_ >= 0) {
        close(reset_line_fd_);
    }
    if (gpio_chip_fd_ >= 0) {
        close(gpio_chip_fd_);
    }
    if (spi_fd_ >= 0) {
        close(spi_fd_);
    }
}

bool ILI9488Transport::initialize(const SpiConfig& config) {
    config_ = config;
    current_speed_hz_ = config_.speed_hz;
    spi_fd_ = open(config_.device.c_str(), O_RDWR | O_CLOEXEC);
    if (spi_fd_ < 0) {
        return false;
    }

    if (ioctl(spi_fd_, SPI_IOC_WR_MODE, &config_.mode) < 0) {
        return false;
    }
    if (ioctl(spi_fd_, SPI_IOC_WR_BITS_PER_WORD, &config_.bits_per_word) < 0) {
        return false;
    }
    if (ioctl(spi_fd_, SPI_IOC_WR_MAX_SPEED_HZ, &config_.speed_hz) < 0) {
        return false;
    }

    gpio_chip_fd_ = open("/dev/gpiochip0", O_RDWR | O_CLOEXEC);
    if (gpio_chip_fd_ < 0) {
        return false;
    }

    dc_line_fd_ = configureGpioOutput(config_.dc_gpio, true);
    reset_line_fd_ = configureGpioOutput(config_.reset_gpio, true);
    if (dc_line_fd_ < 0 || reset_line_fd_ < 0) {
        return false;
    }

    if (!initializePanel()) {
        return false;
    }

    direct_dma_available_ = false;
    return true;
}

bool ILI9488Transport::transferDma(const uint8_t* buf, size_t length) {
    const size_t bytes_per_pixel = config_.pixel_format == kIli9488PixelFormatRgb565 ? 2U : 3U;
    const size_t line_bytes = static_cast<size_t>(config_.width) * bytes_per_pixel;
    const size_t expected_length = line_bytes * config_.height;
    if (length < expected_length) {
        return false;
    }
    const size_t transfer_length = expected_length;

    const uint32_t window_width = config_.width;
    const uint32_t window_height = config_.height;

    if (!sendCommand(kIli9488CmdColumnAddressSet)) {
        return false;
    }
    const uint16_t col_end = static_cast<uint16_t>(window_width - 1);
    const uint8_t col_data[] = {
        static_cast<uint8_t>(0x00),
        static_cast<uint8_t>(0x00),
        static_cast<uint8_t>(col_end >> 8),
        static_cast<uint8_t>(col_end & 0xFF)
    };
    if (!sendData(col_data, sizeof(col_data))) {
        return false;
    }

    if (!sendCommand(kIli9488CmdPageAddressSet)) {
        return false;
    }
    const uint16_t page_end = static_cast<uint16_t>(window_height - 1);
    const uint8_t page_data[] = {
        static_cast<uint8_t>(0x00),
        static_cast<uint8_t>(0x00),
        static_cast<uint8_t>(page_end >> 8),
        static_cast<uint8_t>(page_end & 0xFF)
    };
    if (!sendData(page_data, sizeof(page_data))) {
        return false;
    }

    if (!sendCommand(kIli9488CmdMemoryWrite)) {
        return false;
    }

    const size_t chunk_size = config_.transfer_chunk_bytes > 0 ? config_.transfer_chunk_bytes
                                                               : kDefaultChunkSize;
    size_t offset = 0;
    while (offset < transfer_length) {
        const size_t remaining = transfer_length - offset;
        const size_t send_size = std::min(chunk_size, remaining);
        if (!sendData(buf + offset, send_size)) {
            return false;
        }
        offset += send_size;
    }

    return true;
}

bool ILI9488Transport::setGpioValue(int line_fd, bool value) {
    if (line_fd < 0) {
        return false;
    }
    struct gpiohandle_data data {};
    data.values[0] = value ? 1 : 0;
    return ioctl(line_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) == 0;
}

int ILI9488Transport::configureGpioOutput(int gpio, bool value) {
    struct gpiohandle_request request {};
    request.lineoffsets[0] = gpio;
    request.flags = GPIOHANDLE_REQUEST_OUTPUT;
    request.default_values[0] = value ? 1 : 0;
    request.lines = 1;
    std::snprintf(request.consumer_label, sizeof(request.consumer_label), "ili9488_dma");
    if (ioctl(gpio_chip_fd_, GPIO_GET_LINEHANDLE_IOCTL, &request) < 0) {
        return -1;
    }
    return request.fd;
}

bool ILI9488Transport::sendCommand(uint8_t command) {
    if (!setGpioValue(dc_line_fd_, false)) {
        return false;
    }
    struct spi_ioc_transfer transfer {};
    transfer.tx_buf = reinterpret_cast<uint64_t>(&command);
    transfer.len = 1;
    transfer.speed_hz = current_speed_hz_;
    transfer.bits_per_word = config_.bits_per_word;
    return ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &transfer) >= 0;
}

bool ILI9488Transport::sendData(const uint8_t* data, size_t length) {
    if (!setGpioValue(dc_line_fd_, true)) {
        return false;
    }
    struct spi_ioc_transfer transfer {};
    transfer.tx_buf = reinterpret_cast<uint64_t>(data);
    transfer.len = static_cast<uint32_t>(length);
    transfer.speed_hz = current_speed_hz_;
    transfer.bits_per_word = config_.bits_per_word;
    return ioctl(spi_fd_, SPI_IOC_MESSAGE(1), &transfer) >= 0;
}

bool ILI9488Transport::initializePanel() {
    auto sendCommandWithData = [this](uint8_t command, const uint8_t* data, size_t length) {
        if (!sendCommand(command)) {
            return false;
        }
        if (length == 0) {
            return true;
        }
        return sendData(data, length);
    };

    const uint32_t normal_speed = current_speed_hz_;
    const uint32_t init_speed = config_.init_speed_hz > 0
                                    ? std::min(config_.init_speed_hz, config_.speed_hz)
                                    : config_.speed_hz;
    current_speed_hz_ = init_speed;

    setGpioValue(reset_line_fd_, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    setGpioValue(reset_line_fd_, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    const uint8_t gamma_positive[] = {
        0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78,
        0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F
    };
    if (!sendCommandWithData(0xE0, gamma_positive, sizeof(gamma_positive))) {
        return false;
    }

    const uint8_t gamma_negative[] = {
        0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45,
        0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F
    };
    if (!sendCommandWithData(0xE1, gamma_negative, sizeof(gamma_negative))) {
        return false;
    }

    const uint8_t power_control_1[] = {0x17, 0x15};
    if (!sendCommandWithData(0xC0, power_control_1, sizeof(power_control_1))) {
        return false;
    }

    const uint8_t power_control_2[] = {0x41};
    if (!sendCommandWithData(0xC1, power_control_2, sizeof(power_control_2))) {
        return false;
    }

    const uint8_t vcom_control[] = {0x00, 0x12, 0x80};
    if (!sendCommandWithData(0xC5, vcom_control, sizeof(vcom_control))) {
        return false;
    }

    if (!sendCommand(kIli9488CmdMemoryAccessControl)) {
        return false;
    }

    uint8_t madctl = 0x40;
    madctl |= 0x08;

    if (!sendData(&madctl, 1)) {
        return false;
    }

    const uint8_t pixel_format = config_.pixel_format;
    if (!sendCommandWithData(kIli9488CmdPixelFormat, &pixel_format, 1)) {
        return false;
    }

    const uint8_t interface_mode[] = {0x80};
    if (!sendCommandWithData(0xB0, interface_mode, sizeof(interface_mode))) {
        return false;
    }

    const uint8_t frame_rate[] = {0xA0};
    if (!sendCommandWithData(0xB1, frame_rate, sizeof(frame_rate))) {
        return false;
    }

    const uint8_t display_inversion_control[] = {0x02};
    if (!sendCommandWithData(0xB4, display_inversion_control, sizeof(display_inversion_control))) {
        return false;
    }

    if (!sendCommand(0x20)) {
        return false;
    }

    const uint8_t display_function[] = {0x02, 0x02};
    if (!sendCommandWithData(0xB6, display_function, sizeof(display_function))) {
        return false;
    }

    const uint8_t image_function[] = {0x00};
    if (!sendCommandWithData(0xE9, image_function, sizeof(image_function))) {
        return false;
    }

    const uint8_t adjust_control[] = {0xA9, 0x51, 0x2C, 0x82};
    if (!sendCommandWithData(0xF7, adjust_control, sizeof(adjust_control))) {
        return false;
    }

    if (!sendCommand(kIli9488CmdSleepOut)) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    if (!sendCommand(kIli9488CmdDisplayOn)) {
        return false;
    }

    if (!sendCommand(0x38)) {
        return false;
    }

    if (!sendCommand(0x13)) {
        return false;
    }

    current_speed_hz_ = normal_speed;

    return true;
}

bool ILI9488Transport::transferDmaFromBusAddr(uint32_t bus_addr, size_t length) {

    if (mem_fd_ < 0) {
        mem_fd_ = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
        if (mem_fd_ < 0) {
            return false;
        }
    }

    const uint32_t masked_bus_addr = bus_addr & kBusAddressMask;
    const uint32_t page_size = kPageSize;
    const uint32_t phys_addr = masked_bus_addr & ~(page_size - 1U);
    const uint32_t offset = masked_bus_addr - phys_addr;
    const size_t map_size = (offset + length + page_size - 1U) & ~(page_size - 1U);

    void* mapped = mmap(nullptr, map_size, PROT_READ, MAP_SHARED, mem_fd_, phys_addr);
    if (mapped == MAP_FAILED) {
        return false;
    }

    uint8_t* data_ptr = static_cast<uint8_t*>(mapped) + offset;
    const bool result = sendData(data_ptr, length);

    munmap(mapped, map_size);

    return result;
}

bool ILI9488Transport::supportsBusAddrTransfer() const {
    return direct_dma_available_;
}

bool ILI9488Transport::sendDataFromBusAddr(uint32_t bus_addr, size_t length) {
    return transferDmaFromBusAddr(bus_addr, length);
}

bool ILI9488Transport::setupDirectDma() {
    direct_dma_available_ = false;
    return false;
}

void ILI9488Transport::cleanupDirectDma() {
    if (dma_regs_ != nullptr && dma_regs_map_ != nullptr) {
        dma_regs_[kDmaCs / 4] = kDmaCsReset;
    }

    if (dma_regs_map_ != nullptr) {
        munmap(dma_regs_map_, kPageSize);
        dma_regs_map_ = nullptr;
        dma_regs_ = nullptr;
    }

    if (spi_regs_map_ != nullptr) {
        munmap(spi_regs_map_, kPageSize);
        spi_regs_map_ = nullptr;
        spi_regs_ = nullptr;
    }

    if (dma_cb_mem_ != nullptr) {
        munmap(dma_cb_mem_, sizeof(DmaControlBlock));
        dma_cb_mem_ = nullptr;
    }

    if (mem_fd_ >= 0) {
        close(mem_fd_);
        mem_fd_ = -1;
    }

    direct_dma_available_ = false;
}

}

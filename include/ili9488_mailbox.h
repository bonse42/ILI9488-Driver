#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <semaphore.h>

namespace ili9488 {

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

struct DmaBuffer {
    void* user_ptr = nullptr;
    uint32_t bus_addr = 0;
    uint32_t handle = 0;
    size_t size = 0;
};

class ILI9488Framebuffer {
public:
    ILI9488Framebuffer();
    ~ILI9488Framebuffer();

    bool initialize(uint32_t width, uint32_t height, bool enable_mailbox);
    uint8_t* backBuffer();
    uint8_t* frontBuffer();
    uint8_t* pendingBuffer();
    void swapBuffers();
    void rotateBuffers();
    size_t bufferSize() const;
    bool usingMailbox() const;

    bool createTripleBufferSharedMemory(
        const std::string& shm_name,
        uint32_t width, uint32_t height,
        TripleBufferShmHeader** out_header,
        int& out_shm_fd);

    void rotateBufferIndices();

    uint8_t* getPendingBuffer();
    uint8_t* getBackBuffer();
    uint8_t* getFrontBuffer();
    void swapBackAndFront();

    uint8_t* getShmPendingBuffer();
    void cleanupSharedMemory();

    uint32_t backBufferBusAddr() const;
    uint32_t frontBufferBusAddr() const;
    uint32_t pendingBufferBusAddr() const;

    bool allocateDmaBuffer(size_t size, DmaBuffer& out_buffer);
    void freeDmaBuffer(DmaBuffer& buffer);

    bool createDmaSharedMemory(const std::string& shm_name, size_t size,
                               DmaBuffer& out_buffer, int& out_shm_fd);

private:
    bool allocateMailboxBuffers();
    bool allocateCmaBuffers();
    bool allocateCpuBuffers();
    void releaseMailboxBuffers();
    void releaseCmaBuffers();
    bool openMailboxDevice();
    bool discoverCmaBusAddresses();

    uint32_t mailboxAllocate(size_t size, uint32_t align, uint32_t flags);
    uint32_t mailboxLock(uint32_t handle);
    bool mailboxUnlock(uint32_t handle);
    bool mailboxRelease(uint32_t handle);
    void* mapBusAddress(uint32_t bus_addr, size_t size);

    uint32_t width_;
    uint32_t height_;
    size_t buffer_size_;
    bool use_mailbox_;

    int mailbox_fd_;
    int mem_fd_;
    uint32_t mailbox_handle_[3];
    uint32_t mailbox_bus_addr_[3];
    void* mailbox_map_[3];

    int dma_heap_fd_;
    int dmabuf_fd_[3];
    void* cma_map_[3];
    bool using_cma_;

    int vcsm_fd_;
    uint32_t vcsm_handle_[3];

    std::vector<uint8_t> cpu_buffers_[3];
    int front_index_;
    int back_index_;
    int pending_index_;

    TripleBufferShmHeader* triple_buffer_header_;
    int triple_buffer_shm_fd_;
    uint8_t* triple_buffer_base_;
    size_t triple_buffer_total_size_;
    std::string shm_name_;
};

}

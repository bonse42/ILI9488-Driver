#include "ili9488_mailbox.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>

namespace ili9488 {

namespace {
constexpr uint32_t kMboxProperty = 0x00000000;
constexpr uint32_t kMboxTagAllocateMemory = 0x0003000c;
constexpr uint32_t kMboxTagLockMemory = 0x0003000d;
constexpr uint32_t kMboxTagUnlockMemory = 0x0003000e;
constexpr uint32_t kMboxTagReleaseMemory = 0x0003000f;
constexpr uint32_t kMboxTagLast = 0x00000000;

constexpr uint32_t kMboxMemFlagDirect = 1 << 2;
constexpr uint32_t kMboxMemFlagZero = 1 << 4;
constexpr uint32_t kMboxMemFlagCoherent = 1 << 3;

constexpr uint32_t kBusAddressMask = 0x3FFFFFFF;
constexpr size_t kPageAlign = 4096;

constexpr int kMailboxDeviceMajor = 100;
constexpr int kMailboxIoctl = _IOWR(kMailboxDeviceMajor, 0, char*);

void LogMailboxError(const char* action) {
    std::fprintf(stderr, "GPU Mailbox: %s failed: %s\n", action, std::strerror(errno));
}

struct DmaHeapAllocationData {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};

#ifndef DMA_HEAP_IOCTL_ALLOC
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct DmaHeapAllocationData)
#endif

constexpr int kVcsmCmaResourceName = 32;

struct VcsmCmaIoctlImportDmabuf {
    int32_t  dmabuf_fd;
    uint32_t cached;
    uint8_t  name[kVcsmCmaResourceName];
    int32_t  handle;
    uint32_t vc_handle;
    uint32_t size;
    uint32_t pad;
    uint64_t dma_addr;
};

#define VCSM_CMA_IOCTL_MEM_IMPORT_DMABUF \
    _IOR('J', 0x5B, struct VcsmCmaIoctlImportDmabuf)

int OpenDmaHeap(const char* heap_name) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/dma_heap/%.48s", heap_name);
    return open(path, O_RDWR | O_CLOEXEC);
}

int OpenAnyDmaHeap() {
    const char* heap_names[] = {
        "linux,cma",
        "reserved",
        "system",
        nullptr
    };

    for (int i = 0; heap_names[i] != nullptr; ++i) {
        int fd = OpenDmaHeap(heap_names[i]);
        if (fd >= 0) {
            return fd;
        }
    }

    DIR* dir = opendir("/dev/dma_heap");
    if (dir != nullptr) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] != '.') {
                int fd = OpenDmaHeap(entry->d_name);
                if (fd >= 0) {
                    closedir(dir);
                    return fd;
                }
            }
        }
        closedir(dir);
    }

    return -1;
}

int AllocateDmaHeapBuffer(int heap_fd, size_t size) {
    DmaHeapAllocationData alloc_data = {};
    alloc_data.len = size;
    alloc_data.fd = 0;
    alloc_data.fd_flags = O_RDWR | O_CLOEXEC;
    alloc_data.heap_flags = 0;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0) {
        return -1;
    }

    if (alloc_data.fd < 0) {
        return -1;
    }

    return alloc_data.fd;
}

}

struct alignas(16) MailboxBuffer {
    uint32_t size;
    uint32_t request;
    uint32_t tags[32];
};

ILI9488Framebuffer::ILI9488Framebuffer()
    : width_(0),
      height_(0),
      buffer_size_(0),
      use_mailbox_(false),
      mailbox_fd_(-1),
      mem_fd_(-1),
      mailbox_handle_{0, 0, 0},
      mailbox_bus_addr_{0, 0, 0},
      mailbox_map_{nullptr, nullptr, nullptr},
      dma_heap_fd_(-1),
      dmabuf_fd_{-1, -1, -1},
      cma_map_{nullptr, nullptr, nullptr},
      using_cma_(false),
      vcsm_fd_(-1),
      vcsm_handle_{0, 0, 0},
      front_index_(0),
      back_index_(1),
      pending_index_(2),
      triple_buffer_header_(nullptr),
      triple_buffer_shm_fd_(-1),
      triple_buffer_base_(nullptr),
      triple_buffer_total_size_(0) {}

ILI9488Framebuffer::~ILI9488Framebuffer() {
    cleanupSharedMemory();
    releaseCmaBuffers();
    releaseMailboxBuffers();
}

bool ILI9488Framebuffer::initialize(uint32_t width, uint32_t height, bool enable_mailbox) {
    width_ = width;
    height_ = height;
    buffer_size_ = static_cast<size_t>(width_) * height_ * 3;
    use_mailbox_ = enable_mailbox;

    if (use_mailbox_) {
        if (allocateCmaBuffers()) {
            using_cma_ = true;
            return true;
        }

        if (allocateMailboxBuffers()) {
            using_cma_ = false;
            return true;
        }

        use_mailbox_ = false;
        releaseMailboxBuffers();
        releaseCmaBuffers();
    }

    return allocateCpuBuffers();
}

uint8_t* ILI9488Framebuffer::backBuffer() {
    if (using_cma_) {
        return static_cast<uint8_t*>(cma_map_[back_index_]);
    } else if (use_mailbox_) {
        return static_cast<uint8_t*>(mailbox_map_[back_index_]);
    } else {
        return cpu_buffers_[back_index_].data();
    }
}

uint8_t* ILI9488Framebuffer::frontBuffer() {
    if (using_cma_) {
        return static_cast<uint8_t*>(cma_map_[front_index_]);
    } else if (use_mailbox_) {
        return static_cast<uint8_t*>(mailbox_map_[front_index_]);
    } else {
        return cpu_buffers_[front_index_].data();
    }
}

uint8_t* ILI9488Framebuffer::pendingBuffer() {
    if (using_cma_) {
        return static_cast<uint8_t*>(cma_map_[pending_index_]);
    } else if (use_mailbox_) {
        return static_cast<uint8_t*>(mailbox_map_[pending_index_]);
    } else {
        return cpu_buffers_[pending_index_].data();
    }
}

void ILI9488Framebuffer::swapBuffers() {
    const int temp = front_index_;
    front_index_ = back_index_;
    back_index_ = temp;
}

void ILI9488Framebuffer::rotateBuffers() {
    const int old_front = front_index_;
    front_index_ = pending_index_;
    pending_index_ = back_index_;
    back_index_ = old_front;
}

size_t ILI9488Framebuffer::bufferSize() const {
    return buffer_size_;
}

bool ILI9488Framebuffer::usingMailbox() const {
    return use_mailbox_;
}

uint32_t ILI9488Framebuffer::backBufferBusAddr() const {
    return use_mailbox_ ? mailbox_bus_addr_[back_index_] : 0;
}

uint32_t ILI9488Framebuffer::frontBufferBusAddr() const {
    return use_mailbox_ ? mailbox_bus_addr_[front_index_] : 0;
}

uint32_t ILI9488Framebuffer::pendingBufferBusAddr() const {
    return use_mailbox_ ? mailbox_bus_addr_[pending_index_] : 0;
}

bool ILI9488Framebuffer::openMailboxDevice() {
    if (mailbox_fd_ >= 0) {
        return true;
    }
    mailbox_fd_ = open("/dev/vcio", O_RDWR | O_CLOEXEC);
    if (mailbox_fd_ < 0) {
        LogMailboxError("open /dev/vcio");
        return false;
    }
    if (mem_fd_ < 0) {
        mem_fd_ = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (mem_fd_ < 0) {
            close(mailbox_fd_);
            mailbox_fd_ = -1;
            return false;
        }
    }
    return true;
}

uint32_t ILI9488Framebuffer::mailboxAllocate(size_t size, uint32_t align, uint32_t flags) {
    if (mailbox_fd_ < 0) {
        return 0;
    }
    MailboxBuffer msg {};
    msg.size = sizeof(msg);
    msg.request = kMboxProperty;
    msg.tags[0] = kMboxTagAllocateMemory;
    msg.tags[1] = 12;
    msg.tags[2] = 12;
    msg.tags[3] = static_cast<uint32_t>(size);
    msg.tags[4] = align;
    msg.tags[5] = flags;
    msg.tags[6] = kMboxTagLast;

    if (ioctl(mailbox_fd_, kMailboxIoctl, &msg) < 0) {
        LogMailboxError("mailbox allocate");
        return 0;
    }

    const uint32_t handle = msg.tags[3];
    return handle;
}

uint32_t ILI9488Framebuffer::mailboxLock(uint32_t handle) {
    if (mailbox_fd_ < 0 || handle == 0) {
        return 0;
    }
    MailboxBuffer msg {};
    msg.size = sizeof(msg);
    msg.request = kMboxProperty;
    msg.tags[0] = kMboxTagLockMemory;
    msg.tags[1] = 4;
    msg.tags[2] = 4;
    msg.tags[3] = handle;
    msg.tags[4] = kMboxTagLast;

    if (ioctl(mailbox_fd_, kMailboxIoctl, &msg) < 0) {
        LogMailboxError("mailbox lock");
        return 0;
    }
    return msg.tags[3];
}

bool ILI9488Framebuffer::mailboxUnlock(uint32_t handle) {
    if (mailbox_fd_ < 0 || handle == 0) {
        return false;
    }
    MailboxBuffer msg {};
    msg.size = sizeof(msg);
    msg.request = kMboxProperty;
    msg.tags[0] = kMboxTagUnlockMemory;
    msg.tags[1] = 4;
    msg.tags[2] = 4;
    msg.tags[3] = handle;
    msg.tags[4] = kMboxTagLast;

    if (ioctl(mailbox_fd_, kMailboxIoctl, &msg) < 0) {
        LogMailboxError("mailbox unlock");
        return false;
    }
    return true;
}

bool ILI9488Framebuffer::mailboxRelease(uint32_t handle) {
    if (mailbox_fd_ < 0 || handle == 0) {
        return false;
    }
    MailboxBuffer msg {};
    msg.size = sizeof(msg);
    msg.request = kMboxProperty;
    msg.tags[0] = kMboxTagReleaseMemory;
    msg.tags[1] = 4;
    msg.tags[2] = 4;
    msg.tags[3] = handle;
    msg.tags[4] = kMboxTagLast;

    if (ioctl(mailbox_fd_, kMailboxIoctl, &msg) < 0) {
        LogMailboxError("mailbox release");
        return false;
    }
    return true;
}

void* ILI9488Framebuffer::mapBusAddress(uint32_t bus_addr, size_t size) {
    if (mem_fd_ < 0 || bus_addr == 0) {
        return nullptr;
    }
    const uint32_t phys_addr = bus_addr & kBusAddressMask;

    const uint32_t page_offset = phys_addr & (kPageAlign - 1);
    const uint32_t aligned_addr = phys_addr & ~(kPageAlign - 1);
    const size_t aligned_size = (size + page_offset + kPageAlign - 1) & ~(kPageAlign - 1);

    void* map = mmap(nullptr, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd_, aligned_addr);
    if (map == MAP_FAILED) {
        LogMailboxError("mmap bus address");
        return nullptr;
    }
    return static_cast<uint8_t*>(map) + page_offset;
}

bool ILI9488Framebuffer::allocateDmaBuffer(size_t size, DmaBuffer& out_buffer) {
    if (!openMailboxDevice()) {
        return false;
    }

    const size_t aligned_size = (size + kPageAlign - 1) & ~(kPageAlign - 1);
    const uint32_t flag_options[] = {
        kMboxMemFlagCoherent | kMboxMemFlagDirect | kMboxMemFlagZero,
        kMboxMemFlagCoherent | kMboxMemFlagDirect,
        kMboxMemFlagCoherent
    };

    uint32_t handle = 0;
    uint32_t flags_used = 0;
    for (uint32_t flags : flag_options) {
        handle = mailboxAllocate(aligned_size, kPageAlign, flags);
        if (handle != 0) {
            flags_used = flags;
            break;
        }
    }

    if (handle == 0) {
        return false;
    }

    const uint32_t bus_addr = mailboxLock(handle);
    if (bus_addr == 0) {
        mailboxRelease(handle);
        return false;
    }

    void* user_ptr = mapBusAddress(bus_addr, aligned_size);
    if (user_ptr == nullptr) {
        mailboxUnlock(handle);
        mailboxRelease(handle);
        return false;
    }

    out_buffer.user_ptr = user_ptr;
    out_buffer.bus_addr = bus_addr;
    out_buffer.handle = handle;
    out_buffer.size = aligned_size;
    return true;
}

void ILI9488Framebuffer::freeDmaBuffer(DmaBuffer& buffer) {
    if (buffer.user_ptr != nullptr && buffer.size > 0) {
        const uint32_t phys_addr = buffer.bus_addr & kBusAddressMask;
        const uint32_t page_offset = phys_addr & (kPageAlign - 1);
        const size_t aligned_size = (buffer.size + page_offset + kPageAlign - 1) & ~(kPageAlign - 1);
        void* map_base = static_cast<uint8_t*>(buffer.user_ptr) - page_offset;
        munmap(map_base, aligned_size);
    }
    if (buffer.handle != 0) {
        mailboxUnlock(buffer.handle);
        mailboxRelease(buffer.handle);
    }
    buffer = DmaBuffer{};
}

bool ILI9488Framebuffer::createDmaSharedMemory(const std::string& shm_name, size_t size,
                                            DmaBuffer& out_buffer, int& out_shm_fd) {
    if (!allocateDmaBuffer(size, out_buffer)) {
        return false;
    }

    std::string name = shm_name;
    if (!name.empty() && name[0] != '/') {
        name.insert(name.begin(), '/');
    }
    if (name.empty()) {
        name = "/ili9488_dma_shm";
    }

    shm_unlink(name.c_str());

    int memfd = memfd_create("ili9488_dma_buffer", MFD_ALLOW_SEALING);
    if (memfd < 0) {
        umask(0);
        out_shm_fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        if (out_shm_fd < 0 && errno == EEXIST) {
            shm_unlink(name.c_str());
            out_shm_fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
        }
        if (out_shm_fd < 0) {
            freeDmaBuffer(out_buffer);
            std::perror("Failed to create shared memory");
            return false;
        }

        struct DmaShmHeader {
            uint32_t magic;
            uint32_t version;
            uint32_t bus_addr;
            uint32_t size;
            uint32_t width;
            uint32_t height;
        };

        const size_t total_size = sizeof(DmaShmHeader) + size;
        if (ftruncate(out_shm_fd, static_cast<off_t>(total_size)) < 0) {
            close(out_shm_fd);
            shm_unlink(name.c_str());
            freeDmaBuffer(out_buffer);
            std::perror("Failed to size shared memory");
            return false;
        }

        fchmod(out_shm_fd, 0666);
        return true;
    }

    out_shm_fd = memfd;
    if (ftruncate(out_shm_fd, static_cast<off_t>(size)) < 0) {
        close(out_shm_fd);
        freeDmaBuffer(out_buffer);
        return false;
    }

    return true;
}

bool ILI9488Framebuffer::allocateMailboxBuffers() {
    if (!openMailboxDevice()) {
        std::fprintf(stderr, "ERROR: Failed to open mailbox device (/dev/vcio)\n");
        std::fprintf(stderr, "       This is required for GPU memory allocation\n");
        std::fprintf(stderr, "       Check: ls -la /dev/vcio\n");
        return false;
    }

    if (mem_fd_ < 0) {
        mem_fd_ = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (mem_fd_ < 0) {
            std::fprintf(stderr, "ERROR: Failed to open /dev/mem: %s\n", strerror(errno));
            return false;
        }
    }

    const uint32_t flag_options[] = {
        kMboxMemFlagCoherent | kMboxMemFlagDirect | kMboxMemFlagZero,
        kMboxMemFlagCoherent | kMboxMemFlagDirect,
        kMboxMemFlagCoherent
    };

    for (int i = 0; i < 3; ++i) {
        uint32_t handle = 0;
        uint32_t flags_used = 0;
        for (uint32_t flags : flag_options) {
            handle = mailboxAllocate(buffer_size_, kPageAlign, flags);
            if (handle != 0) {
                flags_used = flags;
                break;
            }
        }
        if (handle == 0) {
            std::fprintf(stderr, "ERROR: Failed to allocate mailbox buffer %d (%zu bytes)\n", i, buffer_size_);
            std::fprintf(stderr, "       GPU memory may be insufficient or reserved\n");
            std::fprintf(stderr, "       Check: vcgencmd get_mem gpu (should be 32M)\n");
            return false;
        }

        const uint32_t bus_addr = mailboxLock(handle);
        if (bus_addr == 0) {
            std::fprintf(stderr, "ERROR: Failed to lock mailbox buffer %d\n", i);
            mailboxRelease(handle);
            return false;
        }

        void* map = mapBusAddress(bus_addr, buffer_size_);
        if (map == nullptr) {
            std::fprintf(stderr, "ERROR: Failed to map mailbox buffer %d (bus_addr=0x%08x)\n", i, bus_addr);
            mailboxUnlock(handle);
            mailboxRelease(handle);
            return false;
        }

        mailbox_handle_[i] = handle;
        mailbox_bus_addr_[i] = bus_addr;
        mailbox_map_[i] = map;
    }

    return true;
}

bool ILI9488Framebuffer::allocateCmaBuffers() {
    dma_heap_fd_ = OpenAnyDmaHeap();
    if (dma_heap_fd_ < 0) {
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        dmabuf_fd_[i] = AllocateDmaHeapBuffer(dma_heap_fd_, buffer_size_);
        if (dmabuf_fd_[i] < 0) {
            for (int j = 0; j < i; ++j) {
                if (cma_map_[j] != nullptr) {
                    munmap(cma_map_[j], buffer_size_);
                    cma_map_[j] = nullptr;
                }
                if (dmabuf_fd_[j] >= 0) {
                    close(dmabuf_fd_[j]);
                    dmabuf_fd_[j] = -1;
                }
            }
            close(dma_heap_fd_);
            dma_heap_fd_ = -1;
            return false;
        }

        void* map = mmap(nullptr, buffer_size_, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd_[i], 0);
        if (map == MAP_FAILED) {
            close(dmabuf_fd_[i]);
            dmabuf_fd_[i] = -1;
            for (int j = 0; j < i; ++j) {
                if (cma_map_[j] != nullptr) {
                    munmap(cma_map_[j], buffer_size_);
                    cma_map_[j] = nullptr;
                }
                if (dmabuf_fd_[j] >= 0) {
                    close(dmabuf_fd_[j]);
                    dmabuf_fd_[j] = -1;
                }
            }
            close(dma_heap_fd_);
            dma_heap_fd_ = -1;
            return false;
        }

        cma_map_[i] = map;
        mailbox_bus_addr_[i] = 0;
    }

    discoverCmaBusAddresses();

    return true;
}

void ILI9488Framebuffer::releaseCmaBuffers() {
    for (int i = 0; i < 3; ++i) {
        vcsm_handle_[i] = 0;
        if (cma_map_[i] != nullptr && cma_map_[i] != MAP_FAILED) {
            munmap(cma_map_[i], buffer_size_);
            cma_map_[i] = nullptr;
        }
        if (dmabuf_fd_[i] >= 0) {
            close(dmabuf_fd_[i]);
            dmabuf_fd_[i] = -1;
        }
    }
    if (vcsm_fd_ >= 0) {
        close(vcsm_fd_);
        vcsm_fd_ = -1;
    }
    if (dma_heap_fd_ >= 0) {
        close(dma_heap_fd_);
        dma_heap_fd_ = -1;
    }
    using_cma_ = false;
}

bool ILI9488Framebuffer::discoverCmaBusAddresses() {
    vcsm_fd_ = open("/dev/vcsm-cma", O_RDWR | O_CLOEXEC);
    if (vcsm_fd_ < 0) {
        std::fprintf(stderr, "  VCSM-CMA: /dev/vcsm-cma not available: %s\n", strerror(errno));
        return false;
    }

    bool all_ok = true;
    for (int i = 0; i < 3; ++i) {
        if (dmabuf_fd_[i] < 0) {
            all_ok = false;
            continue;
        }

        VcsmCmaIoctlImportDmabuf import_data {};
        import_data.dmabuf_fd = dmabuf_fd_[i];
        import_data.cached = 0;
        std::strncpy(reinterpret_cast<char*>(import_data.name), "ili9488_fb", kVcsmCmaResourceName);

        if (ioctl(vcsm_fd_, VCSM_CMA_IOCTL_MEM_IMPORT_DMABUF, &import_data) < 0) {
            std::fprintf(stderr, "  VCSM-CMA: import buffer %d failed: %s\n", i, strerror(errno));
            all_ok = false;
            continue;
        }

        vcsm_handle_[i] = static_cast<uint32_t>(import_data.handle);
        const uint64_t dma_addr = import_data.dma_addr;

        if (dma_addr != 0) {
            mailbox_bus_addr_[i] = static_cast<uint32_t>(dma_addr);
        } else {
            all_ok = false;
        }
    }

    return all_ok;
}

bool ILI9488Framebuffer::allocateCpuBuffers() {
    for (int i = 0; i < 3; ++i) {
        cpu_buffers_[i].resize(buffer_size_);
        std::memset(cpu_buffers_[i].data(), 0, buffer_size_);
    }
    return true;
}

void ILI9488Framebuffer::releaseMailboxBuffers() {
    for (int i = 0; i < 3; ++i) {
        if (mailbox_map_[i] != nullptr && mailbox_map_[i] != MAP_FAILED) {
            const uint32_t phys_addr = mailbox_bus_addr_[i] & kBusAddressMask;
            const uint32_t page_offset = phys_addr & (kPageAlign - 1);
            const size_t aligned_size = (buffer_size_ + page_offset + kPageAlign - 1) & ~(kPageAlign - 1);
            void* map_base = static_cast<uint8_t*>(mailbox_map_[i]) - page_offset;
            munmap(map_base, aligned_size);
        }
        if (mailbox_handle_[i] != 0) {
            mailboxUnlock(mailbox_handle_[i]);
            mailboxRelease(mailbox_handle_[i]);
        }

        mailbox_map_[i] = nullptr;
        mailbox_handle_[i] = 0;
        mailbox_bus_addr_[i] = 0;
    }

    if (mailbox_fd_ >= 0) {
        close(mailbox_fd_);
        mailbox_fd_ = -1;
    }
    if (mem_fd_ >= 0) {
        close(mem_fd_);
        mem_fd_ = -1;
    }
}

bool ILI9488Framebuffer::createTripleBufferSharedMemory(
    const std::string& shm_name,
    uint32_t width, uint32_t height,
    TripleBufferShmHeader** out_header,
    int& out_shm_fd) {

    if (out_header == nullptr) {
        return false;
    }

    if (buffer_size_ == 0) {
        width_ = width;
        height_ = height;
        buffer_size_ = static_cast<size_t>(width_) * height_ * 3;
    }

    const size_t header_size = sizeof(TripleBufferShmHeader);
    triple_buffer_total_size_ = header_size + (3 * buffer_size_);

    bool buffers_ready = false;
    if (using_cma_ && cma_map_[0] != nullptr && cma_map_[1] != nullptr && cma_map_[2] != nullptr) {
        buffers_ready = true;
    } else if (use_mailbox_ && mailbox_map_[0] != nullptr && mailbox_map_[1] != nullptr && mailbox_map_[2] != nullptr) {
        buffers_ready = true;
    }

    if (!buffers_ready) {
        std::fprintf(stderr, "ERROR: No DMA buffers available.\n");
        std::fprintf(stderr, "       Ensure driver.initialize() was called first.\n");
        return false;
    }

    std::string name = shm_name;
    if (!name.empty() && name[0] != '/') {
        name.insert(name.begin(), '/');
    }
    if (name.empty()) {
        name = "/ili9488_triple_buffer";
    }

    shm_unlink(name.c_str());

    const size_t shm_size = header_size + (3 * buffer_size_);
    umask(0);
    int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd < 0 && errno == EEXIST) {
        shm_unlink(name.c_str());
        fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
    }
    if (fd < 0) {
        std::perror("Failed to create shared memory");
        return false;
    }

    if (ftruncate(fd, static_cast<off_t>(shm_size)) < 0) {
        std::perror("Failed to size shared memory");
        close(fd);
        shm_unlink(name.c_str());
        return false;
    }

    if (fchmod(fd, 0666) < 0) {
        std::perror("Failed to chmod shared memory");
    }

    void* header_map = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (header_map == MAP_FAILED) {
        std::perror("Failed to mmap header");
        close(fd);
        shm_unlink(name.c_str());
        return false;
    }

    triple_buffer_header_ = static_cast<TripleBufferShmHeader*>(header_map);
    triple_buffer_base_ = static_cast<uint8_t*>(header_map) + header_size;
    triple_buffer_shm_fd_ = fd;
    shm_name_ = name;

    triple_buffer_header_->magic = 0x49494C39;
    triple_buffer_header_->version = 1;
    triple_buffer_header_->width = width;
    triple_buffer_header_->height = height;
    triple_buffer_header_->bytes_per_pixel = 3;

    triple_buffer_header_->buffer_a_bus_addr = mailbox_bus_addr_[0];
    triple_buffer_header_->buffer_b_bus_addr = mailbox_bus_addr_[1];
    triple_buffer_header_->buffer_c_bus_addr = mailbox_bus_addr_[2];

    std::fprintf(stderr, "  Buffer bus addresses: A=0x%08x B=0x%08x C=0x%08x%s\n",
                 mailbox_bus_addr_[0], mailbox_bus_addr_[1], mailbox_bus_addr_[2],
                 (mailbox_bus_addr_[0] != 0) ? " (DMA-capable)" : " (no bus addr)");

    front_index_ = 0;
    back_index_ = 1;
    pending_index_ = 2;

    triple_buffer_header_->front_index = front_index_;
    triple_buffer_header_->back_index = back_index_;
    triple_buffer_header_->pending_index = pending_index_;

    if (sem_init(&triple_buffer_header_->pending_sem, 1, 1) != 0) {
        std::perror("Failed to initialize semaphore");
        munmap(header_map, shm_size);
        close(fd);
        shm_unlink(name.c_str());
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        void* buf = using_cma_ ? cma_map_[i] : mailbox_map_[i];
        if (buf != nullptr) {
            std::memset(buf, 0x00, buffer_size_);
        }
        std::memset(triple_buffer_base_ + i * buffer_size_, 0x00, buffer_size_);
    }

    triple_buffer_header_->frame_counter = 0;
    triple_buffer_header_->rotation_degrees = 0;
    triple_buffer_header_->daemon_ready = 0;
    triple_buffer_header_->app_connected = 0;

    std::memset(triple_buffer_header_->padding, 0, sizeof(triple_buffer_header_->padding));

    *out_header = triple_buffer_header_;
    out_shm_fd = fd;

    return true;
}

void ILI9488Framebuffer::rotateBufferIndices() {
    int temp = pending_index_;
    pending_index_ = back_index_;
    back_index_ = front_index_;
    front_index_ = temp;

    if (triple_buffer_header_ != nullptr) {
        triple_buffer_header_->front_index = front_index_;
        triple_buffer_header_->back_index = back_index_;
        triple_buffer_header_->pending_index = pending_index_;

        triple_buffer_header_->buffer_a_bus_addr = mailbox_bus_addr_[front_index_];
        triple_buffer_header_->buffer_b_bus_addr = mailbox_bus_addr_[back_index_];
        triple_buffer_header_->buffer_c_bus_addr = mailbox_bus_addr_[pending_index_];
    }
}

uint8_t* ILI9488Framebuffer::getPendingBuffer() {
    if (using_cma_) {
        return static_cast<uint8_t*>(cma_map_[pending_index_]);
    }
    return static_cast<uint8_t*>(mailbox_map_[pending_index_]);
}

uint8_t* ILI9488Framebuffer::getBackBuffer() {
    if (using_cma_) {
        return static_cast<uint8_t*>(cma_map_[back_index_]);
    }
    return static_cast<uint8_t*>(mailbox_map_[back_index_]);
}

uint8_t* ILI9488Framebuffer::getFrontBuffer() {
    if (using_cma_) {
        return static_cast<uint8_t*>(cma_map_[front_index_]);
    }
    return static_cast<uint8_t*>(mailbox_map_[front_index_]);
}

uint8_t* ILI9488Framebuffer::getShmPendingBuffer() {
    if (triple_buffer_base_ == nullptr) {
        return nullptr;
    }
    return triple_buffer_base_ + pending_index_ * buffer_size_;
}

void ILI9488Framebuffer::cleanupSharedMemory() {
    if (triple_buffer_header_ != nullptr) {
        sem_destroy(&triple_buffer_header_->pending_sem);
        munmap(triple_buffer_header_,
               sizeof(TripleBufferShmHeader) + 3 * buffer_size_);
        triple_buffer_header_ = nullptr;
        triple_buffer_base_ = nullptr;
    }
    if (triple_buffer_shm_fd_ >= 0) {
        close(triple_buffer_shm_fd_);
        triple_buffer_shm_fd_ = -1;
    }
    shm_name_.clear();
}

void ILI9488Framebuffer::swapBackAndFront() {
    std::swap(back_index_, front_index_);

    if (triple_buffer_header_ != nullptr) {
        triple_buffer_header_->back_index = back_index_;
        triple_buffer_header_->front_index = front_index_;

        triple_buffer_header_->buffer_a_bus_addr = mailbox_bus_addr_[front_index_];
        triple_buffer_header_->buffer_b_bus_addr = mailbox_bus_addr_[back_index_];
    }
}

}

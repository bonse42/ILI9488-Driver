/* Simple frame generator for benchmarking ili9488-daemon.
   Continuously writes frames to shared memory to simulate app input. */

#include <fcntl.h>
#include <math.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Header structure must match ili9488_mailbox.h TripleBufferShmHeader
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

int main(int argc, char *argv[]) {
    int duration = 15;
    if (argc > 1) {
        duration = atoi(argv[1]);
    }

    const char *shm_name = "/ili9488_rgb666";
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        return 1;
    }

    struct stat sb;
    if (fstat(shm_fd, &sb) < 0) {
        perror("fstat");
        return 1;
    }

    void *map = mmap(NULL, sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    struct TripleBufferShmHeader *header = (struct TripleBufferShmHeader *)map;

    // Verify magic
    if (header->magic != 0x49494C39) {
        fprintf(stderr, "Invalid shared memory header\n");
        return 1;
    }

    uint32_t buffer_size = header->width * header->height * header->bytes_per_pixel;
    time_t start = time(NULL);

    // Generate frames continuously with animated colors
    unsigned int frame_num = 0;
    while (time(NULL) - start < duration) {
        // Try to acquire semaphore (non-blocking)
        if (sem_trywait(&header->pending_sem) == 0) {
            // Write animated color pattern to pending buffer
            uint8_t *pending_buf =
                (uint8_t *)map + sizeof(struct TripleBufferShmHeader) +
                header->pending_index * buffer_size;

            // Generate rainbow gradient animation
            for (uint32_t y = 0; y < header->height; y++) {
                for (uint32_t x = 0; x < header->width; x++) {
                    uint32_t pixel_idx = (y * header->width + x) * 3;

                    // Create moving rainbow effect
                    // HSV to RGB conversion for smooth color transitions
                    float hue = ((float)((x + y + frame_num * 2) % 360)) / 360.0f;
                    float s = 1.0f;
                    float v = 1.0f;

                    float c = v * s;
                    float x_val = c * (1.0f - fabsf(fmodf(hue * 6.0f, 2.0f) - 1.0f));
                    float m = v - c;

                    float r, g, b;
                    if (hue < 1.0f/6.0f) {
                        r = c; g = x_val; b = 0;
                    } else if (hue < 2.0f/6.0f) {
                        r = x_val; g = c; b = 0;
                    } else if (hue < 3.0f/6.0f) {
                        r = 0; g = c; b = x_val;
                    } else if (hue < 4.0f/6.0f) {
                        r = 0; g = x_val; b = c;
                    } else if (hue < 5.0f/6.0f) {
                        r = x_val; g = 0; b = c;
                    } else {
                        r = c; g = 0; b = x_val;
                    }

                    pending_buf[pixel_idx] = (uint8_t)((r + m) * 252.0f);     // R (max 0xFC)
                    pending_buf[pixel_idx + 1] = (uint8_t)((g + m) * 252.0f); // G
                    pending_buf[pixel_idx + 2] = (uint8_t)((b + m) * 252.0f); // B
                }
            }

            header->frame_counter++;
            frame_num++;

            // Release for daemon
            sem_post(&header->pending_sem);
        }

        // Sleep a bit to allow daemon to process
        usleep(10000);  // 10ms = ~100 FPS max
    }

    munmap(map, sb.st_size);
    close(shm_fd);
    return 0;
}

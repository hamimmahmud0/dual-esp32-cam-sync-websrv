

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"   // <- provides esp_log_timestamp()

#include "sd_bench.h"

#define MOUNT_POINT "/sdcard"
static const char *TAG = "sdmmc_bench";

static uint32_t fill_pattern(uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t x = seed ? seed : 0x12345678;
    for (size_t i = 0; i < len; i++) {
        x = (1103515245u * x + 12345u);
        buf[i] = (uint8_t)(x >> 24);
    }
    return x;
}

static void print_result(const char *label, size_t buf_sz, size_t total_bytes, uint32_t ms)
{
    double sec = (double)ms / 1000.0;
    double mb  = (double)total_bytes / (1024.0 * 1024.0);
    double mbps = (sec > 0.0) ? (mb / sec) : 0.0;

    ESP_LOGI(TAG, "%s | buf=%7u bytes | total=%.2f MB | time=%.3f s | %.2f MB/s",
             label, (unsigned)buf_sz, mb, sec, mbps);
}

static esp_err_t bench_one_size(size_t buf_sz, size_t total_bytes)
{
    const char *path = MOUNT_POINT "/speed_test.bin";

    uint8_t *buf = (uint8_t *)malloc(buf_sz);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed for buf_sz=%u", (unsigned)buf_sz);
        return ESP_ERR_NO_MEM;
    }

    // ---------- WRITE ----------
    unlink(path);

    FILE *f = fopen(path, "wb");
    if (!f) {
    int e = errno;
    ESP_LOGE(TAG, "Failed to open for write: %s | errno=%d (%s)",
             path, e, strerror(e));
    free(buf);
    return ESP_FAIL;
}

    uint32_t seed = 0xA5A5A5A5;
    size_t remaining = total_bytes;

    uint32_t t0 = esp_log_timestamp(); // ms
    while (remaining > 0) {
        size_t chunk = (remaining > buf_sz) ? buf_sz : remaining;
        seed = fill_pattern(buf, chunk, seed);

        size_t w = fwrite(buf, 1, chunk, f);
        if (w != chunk) {
            ESP_LOGE(TAG, "fwrite failed: wrote %u/%u", (unsigned)w, (unsigned)chunk);
            fclose(f);
            free(buf);
            return ESP_FAIL;
        }
        remaining -= chunk;
    }

    fflush(f);
    // If your libc supports it, you can also do:
    // fsync(fileno(f));
    fclose(f);

    uint32_t t1 = esp_log_timestamp();
    print_result("WRITE", buf_sz, total_bytes, (t1 - t0));

    // ---------- READ ----------
    f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open for read: %s", path);
        free(buf);
        return ESP_FAIL;
    }

    remaining = total_bytes;
    volatile uint32_t sink = 0;

    uint32_t r0 = esp_log_timestamp();
    while (remaining > 0) {
        size_t chunk = (remaining > buf_sz) ? buf_sz : remaining;

        size_t r = fread(buf, 1, chunk, f);
        if (r != chunk) {
            ESP_LOGE(TAG, "fread failed: read %u/%u", (unsigned)r, (unsigned)chunk);
            fclose(f);
            free(buf);
            return ESP_FAIL;
        }

        sink ^= buf[0];
        remaining -= chunk;
    }
    fclose(f);
    uint32_t r1 = esp_log_timestamp();
    (void)sink;

    print_result("READ ", buf_sz, total_bytes, (r1 - r0));

    free(buf);
    return ESP_OK;
}

void run_sdmmc_buffer_benchmark(void)
{
    const size_t total_bytes = 16 * 1024 * 1024; // 16MB

    ESP_LOGI(TAG, "Benchmark start: mount=%s, file size=%u bytes (%.2f MB)",
             MOUNT_POINT, (unsigned)total_bytes,
             (double)total_bytes / (1024.0 * 1024.0));

    for (size_t buf_sz = 1024*16; buf_sz <= (64 * 1024); buf_sz <<= 1) {
        ESP_LOGI(TAG, "----------------------------------------");
        esp_err_t err = bench_one_size(buf_sz, total_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed at buf=%u: %s", (unsigned)buf_sz, esp_err_to_name(err));
            break;
        }
    }

    ESP_LOGI(TAG, "Benchmark finished");
}

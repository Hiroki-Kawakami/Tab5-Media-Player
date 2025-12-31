#include "storage_benchmark.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>

void storage_benchmark(const char *file, int block_size, void (*output)(const char *str, void *user_info), void *user_info) {
    int fd = open(file, O_RDONLY);
    if (fd < 0) {
        output("Failed to open file", user_info);
        return;
    }

    // バッファを確保
    uint8_t *buffer = (uint8_t *)malloc(131072);
    if (buffer == NULL) {
        output("Failed to allocate buffer", user_info);
        close(fd);
        return;
    }

    // ベンチマーク開始ログ
    char start_msg[256];
    snprintf(start_msg, sizeof(start_msg),
             "Starting benchmark: file=%s, block_size=%d",
             file, block_size);
    output(start_msg, user_info);

    // 開始時刻を記録
    struct timeval start, end;
    gettimeofday(&start, NULL);

    // ファイル全体を読み取り
    size_t total_bytes = 0;
    ssize_t bytes_read;
    const size_t log_interval = 10 * 1024 * 1024; // 10MB
    size_t next_log_threshold = log_interval;

    while ((bytes_read = read(fd, buffer, block_size)) > 0) {
        total_bytes += bytes_read;

        // 10MB読み取るごとにログ出力
        if (total_bytes >= next_log_threshold) {
            char progress_msg[128];
            snprintf(progress_msg, sizeof(progress_msg),
                     "Progress: %zu MB read",
                     total_bytes / (1024 * 1024));
            output(progress_msg, user_info);
            next_log_threshold += log_interval;
        }
    }

    // 終了時刻を記録
    gettimeofday(&end, NULL);

    // 経過時間を計算（マイクロ秒単位）
    long elapsed_usec = (end.tv_sec - start.tv_sec) * 1000000L + (end.tv_usec - start.tv_usec);
    double elapsed_sec = elapsed_usec / 1000000.0;

    // 読み取り速度を計算（MB/s）
    double speed_mbps = (total_bytes / (1024.0 * 1024.0)) / elapsed_sec;

    // 結果を文字列として出力
    char result[256];
    snprintf(result, sizeof(result),
             "Read %zu bytes in %.3f sec (%.2f MB/s, block_size=%d)",
             total_bytes, elapsed_sec, speed_mbps, block_size);
    output(result, user_info);

    // クリーンアップ
    free(buffer);
    close(fd);
}

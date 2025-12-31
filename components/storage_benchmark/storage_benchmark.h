#pragma once
void storage_benchmark(const char *file, int block_size, void (*output)(const char *str, void *user_info), void *user_info);

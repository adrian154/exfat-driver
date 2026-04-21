#ifndef __EXFAT_H
#define __EXFAT_H

#include "sd-interface.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint32_t cur_cluster;
    uint32_t cur_sector_offset;
    uint32_t cluster_sector_offset;
    uint32_t bytes_remaining;
    bool no_chain;
    bool no_length;
    bool finished;
    bool is_directory;
} exfat_stream_t;

// error codes
#define ERR_EXFAT_PARTITION_NOT_FOUND   1
#define ERR_EXFAT_VERSION_UNKNOWN       2
#define ERR_EXFAT_WRONG_SECTOR_SIZE     3
#define ERR_EXFAT_BAD_NUM_FATS          4
#define ERR_NOT_DIRECTORY               5
#define ERR_NOT_FOUND                   6

int init_exfat();
uint32_t read_stream(exfat_stream_t *stream, void *dst);
int open_from_directory(exfat_stream_t *dir_stream, char *filename, exfat_stream_t *dst_stream);

#endif
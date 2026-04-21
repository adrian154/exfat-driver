#include "exfat.h"
#include "sd-interface.h"
#include <stdio.h>
#include <string.h>

FILE *file;

// simulate reading block from SD card
int read_block(uint32_t addr, void *dst) {
    if(fseek(file, addr << BLOCK_SIZE_SHIFT, SEEK_SET)) {
        perror("fseek");
        return 1;
    }
    if(fread(dst, BLOCK_SIZE, 1, file) != 1) {
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    
    file = fopen("test/disk-image", "r");
    if(file == NULL) {
        perror("fopen");
        return 1;
    }

    if(init_exfat()) {
        fprintf(stderr, "failed to read filesystem\n");
        return 1;        
    }

    if(argc != 2) {
        fprintf(stderr, "invalid arguments\n");
        return 1;
    }

    char *path = argv[1];
    char *path_cur = path;
    char filename[256];
    exfat_stream_t parent;
    exfat_stream_t child;
    bool parent_ready = false;
    while(1) {

        // extract next term in path to `filename`
        char *filename_write = filename;
        while(*path_cur != '\0' && *path_cur != '/') {
            *filename_write = *path_cur;
            *path_cur++;
            *filename_write++;
        }
        *filename_write = '\0';

        if(open_from_directory(parent_ready ? &parent : NULL, filename, &child)) {
            fprintf(stderr, "file '%s' could not be opened\n", filename);
            return 1;
        }

        fprintf(stderr, "opened '%s'\n", filename);

        // are we at the end of the path?
        if(*path_cur == '\0') {
            if(child.is_directory) {
                fprintf(stderr, "this is a directory!\n");
            } else {
                while(1) {
                    char buf[BLOCK_SIZE];
                    uint32_t count = read_stream(&child, buf);
                    fwrite(buf, 1, count, stdout);
                    if(child.finished) {
                        break;
                    }
                }
            }
            break;
        } else {
            parent = child;
            parent_ready = true;
            path_cur++;
        }

    }

    return 0;

}
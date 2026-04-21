/* 
 * LIMITATIONS -
 *    Files are assumed to be smaller than 4 GiB.
 *    The filesystem is assumed to be intact and not corrupted.
 *    This code is rather permissive and will accept things that aren't up to ExFAT spec.
 *    File uninitialized space is read out from disk rather than being zeroed (this is not compliant).
 *    We only support ASCII names, and filenames are not automatically uppercased (this is also not compliant).
 */
#include "exfat.h"

// MBR partition status flags
#define PART_STATUS_ACTIVE              0x80

// MBR partition type for exFAT
#define PART_TYPE_EXFAT                 0x07

// VolumeFlags in main boot region
#define VOLUME_FLAGS_ACTIVE_FAT         0x01

// Special FAT values
#define FAT_BAD                         0xFFFFFFF7
#define FAT_END                         0xFFFFFFFF

// Directory entry type flags
#define DIRENT_TYPE_FILE                0x85
#define DIRENT_TYPE_STREAMEXT           0xc0
#define DIRENT_TYPE_FILENAME            0xc1

// FileAttributes flags
#define FILE_ATTRIBUTE_DIRECTORY        0x10

// Directory entry secondary flags
#define SECONDARY_FLAGS_NOCHAIN         0x01

// Directory entry-reading state machine
#define STATE_READING_FILE              0
#define STATE_READING_STREAM_EXT        1
#define STATE_READING_FILENAME          2

// general filesystem parameters
uint32_t active_FAT_start_sector = 0;
uint32_t cluster_heap_start_sector = 0;
uint32_t sectors_per_cluster = 0;
uint32_t sectors_per_cluster_shift = 0;
uint32_t root_directory_cluster = 0;

// utility - read little-endian u32 from buffer
uint32_t read_u32(uint8_t *buf, int offset) {
    return buf[offset] | buf[offset+1]<<8 | buf[offset+2]<<16 | buf[offset+3]<<24;
}

uint16_t read_u16(uint8_t *buf, int offset) {
    return buf[offset] | buf[offset+1]<<8;
}

// read MBR to determine start of exFAT volume
// returns start sector address if found, 0 if not
uint32_t read_mbr() {

    uint8_t buf[BLOCK_SIZE];
    read_block(0, buf);

    // loop over partition entries
    for(int i = 0; i < 4; i++) {

        int offset = 446 + i*16;
        uint8_t status = buf[offset];
        if(!(status & PART_STATUS_ACTIVE)) {
            continue; // skip inactive partitions
        }
        
        uint8_t type = buf[offset + 4];
        if(!(type == PART_TYPE_EXFAT)) {
            continue; // skip partitions 
        }

        // identified exFAT partition
        return read_u32(buf, offset+8);
       
    }

    // no exFAT partition found
    return 0;

}

// try to initialize exFAT filesystem
// returns 0 on success, error code on failure 
int init_exfat() {

    uint32_t volume_start_sector = read_mbr();
    if(volume_start_sector == 0) {
        return ERR_EXFAT_PARTITION_NOT_FOUND;
    }

    // read main boot region
    uint8_t buf[BLOCK_SIZE]; 
    read_block(volume_start_sector, buf);

    // sanity check: fs version must be 1.0
    if(buf[104] != 0 || buf[105] != 1) {
        return ERR_EXFAT_VERSION_UNKNOWN;
    }

    // sanity check: sector size must be 512
    if(buf[108] != BLOCK_SIZE_SHIFT) {
        return ERR_EXFAT_WRONG_SECTOR_SIZE;
    }

    uint32_t FAT_start_sector = read_u32(buf, 80) + volume_start_sector;
    uint32_t FAT_length = read_u32(buf, 88);
    cluster_heap_start_sector = read_u32(buf, 88) + volume_start_sector;
    sectors_per_cluster_shift = buf[109];
    sectors_per_cluster = 1 << sectors_per_cluster_shift;
    root_directory_cluster = read_u32(buf, 96);

    uint8_t num_FATs = buf[110];
    if(num_FATs == 1) {
        active_FAT_start_sector = FAT_start_sector; // only one FAT
    } else if(num_FATs == 2) {
        if(buf[106] & VOLUME_FLAGS_ACTIVE_FAT) {
            active_FAT_start_sector = FAT_start_sector + FAT_length; // second FAT is active
        } else {
            active_FAT_start_sector = FAT_start_sector; // first FAT is active
        }
    } else {
        return ERR_EXFAT_BAD_NUM_FATS;
    }

    return 0;

}

// lookup next cluster within a chain from FAT
uint32_t FAT_lookup(uint32_t cluster_index) {
    
    // we define this buffer as static under the reasoning that since we don't call FAT_lookup() from within itself, this code need not be re-entrant
    // this way, we don't have to allocate additional storage on the stack
    static uint8_t buf[BLOCK_SIZE]; 

    uint32_t byte_offset = cluster_index*4;
    read_block((byte_offset >> BLOCK_SIZE_SHIFT) + active_FAT_start_sector, buf);
    return read_u32(buf, byte_offset & BLOCK_SIZE_MASK);

}

// convert cluster index to sector
uint32_t cluster_to_sector(uint32_t cluster_index) {
    return ((cluster_index - 2) << sectors_per_cluster_shift) + cluster_heap_start_sector;
}

// hash filename according to spec
// ExFAT operates on UTF-16.. disgusting.. since we only accept ASCII we do the equivalent of appending a null byte to each character
uint16_t compute_name_hash(char *filename) {
    uint16_t hash = 0;
    while(*filename != '\0') {
        hash = ((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (uint8_t)(*filename);
        hash = ((hash & 1) ? 0x8000 : 0) + (hash >> 1);
        filename++;
    }
    return hash;
}

// initialize stream state, see read_stream()
void init_stream(exfat_stream_t *stream, uint32_t start_cluster, uint32_t length, bool no_chain, bool no_length, bool is_directory) {
    stream->cur_cluster = start_cluster;
    stream->cur_sector_offset = 0;
    stream->cluster_sector_offset = cluster_to_sector(start_cluster);
    stream->bytes_remaining = length;
    stream->no_chain = no_chain;
    stream->no_length = no_length;
    stream->is_directory = is_directory;
    stream->finished = false;
}

// read data from a cluster chain, one sector at a time
// dst must be able to accomodate BLOCK_SIZE bytes; the actual number of bytes written is returned
uint32_t read_stream(exfat_stream_t *stream, void *dst) {

    read_block(stream->cluster_sector_offset + stream->cur_sector_offset, dst);
    
    // if remaining bytes to be read is less than or equal to BLOCK_SIZE, we are finished
    if(!stream->no_length && stream->bytes_remaining <= BLOCK_SIZE) {
        stream->finished = true;
        return stream->bytes_remaining;
    }

    // otherwise, advance the stream to the next sector
    if(!stream->no_length) {
        stream->bytes_remaining -= BLOCK_SIZE;
    }
    stream->cur_sector_offset++;

    // if finished with the cluster, move to next cluster
    if(stream->cur_sector_offset == sectors_per_cluster) {
        stream->cur_sector_offset = 0;
        if(stream->no_chain) {
            stream->cur_cluster++;
        } else {
            uint32_t next_cluster = FAT_lookup(stream->cur_cluster);
            if(next_cluster == FAT_BAD || next_cluster == FAT_END) {
                if(stream->no_length) {
                    stream->finished = true;
                } else {
                    // TODO... handle this error condition
                }
            }
        }
    }

    // indicate that we read a full sector
    return BLOCK_SIZE;

}

bool compare_filename(char *name1, char *name2) {
    while(1) {
        
        // any discrepancy - including one string terminating before the other - means the filenames are not equal
        if(*name1 != *name2) {
            return false;
        }

        // if both terminate at the same time, they are equal
        if(*name1 == '\0' && *name2 == '\0') {
            return true;
        }

        // otherwise, continue
        name1++;
        name2++;

    }
}

// open a file from directory; if dir_stream is NULL, attemts to open from root directory.
// returns 0 on success, 1 on fail
int open_from_directory(exfat_stream_t *dir_stream, char *target_filename, exfat_stream_t *dst_stream) {

    exfat_stream_t root_stream;
    if(dir_stream == NULL) {
        dir_stream = &root_stream;
        init_stream(dir_stream, root_directory_cluster, 0, false, true, true);
    } else if(!dir_stream->is_directory) {
        return 1;
    }

    static uint8_t buf[BLOCK_SIZE];
    
    // keep track of current file parameters
    uint32_t file_first_cluster = 0;
    uint32_t file_data_length = 0;
    uint8_t file_name_length = 0;
    static char filename[256];
    int filename_write_idx = 0;
    bool is_directory = 0;
    bool no_chain = false;
    int state = STATE_READING_FILE;

    // stream directory entries
    while(1) {

        uint32_t num_read = read_stream(dir_stream, buf);
        for(uint32_t offset = 0; offset < num_read; offset += 32) {

            uint8_t type = buf[offset];
            if(type == 0) {
                return 1; // entry type 0 indicates end of directory; file was not found
            }

            // file directory entry is followed by stream extension entry
            if(type == DIRENT_TYPE_FILE) {
                uint8_t attributes = buf[offset + 4];
                is_directory = attributes & FILE_ATTRIBUTE_DIRECTORY;
                state = STATE_READING_STREAM_EXT;
            }

            // stream extension contains info on filename length as well as physical location of file data
            // stream extension is followed by filename entry/entries
            if(type == DIRENT_TYPE_STREAMEXT) {

                // ignore spurious stream extension entry
                if(state != STATE_READING_STREAM_EXT) {
                    continue;
                }

                file_first_cluster = read_u32(buf, offset + 20);
                file_data_length = read_u32(buf, offset + 24);
                file_name_length = buf[offset + 3];
                filename_write_idx = 0;
                no_chain = buf[offset + 1] & SECONDARY_FLAGS_NOCHAIN;
                state = STATE_READING_FILENAME;

            }

            // each filename entry can contain 15 UTF-16 chars; these characters are appended in order to produce the filename 
            if(type == DIRENT_TYPE_FILENAME) {
                
                // ignore spurious filename entry
                if(state != STATE_READING_FILENAME) {
                    continue;
                }

                for(int i = 0; i < 15; i++) {
                    char c = buf[offset + i*2 + 2];
                    filename[filename_write_idx] = c;
                    filename_write_idx++;
                    if(filename_write_idx == file_name_length) {
                        
                        filename[filename_write_idx] = '\0'; // null-terminate filename
                        if(compare_filename(filename, target_filename)) {
                            init_stream(dst_stream, file_first_cluster, file_data_length, no_chain, false, is_directory);
                            return 0;
                        }
                        state = STATE_READING_FILE;

                    }
                }

            }

        }

        if(dir_stream->finished) {
            break;
        }

    }

    return 1;

}
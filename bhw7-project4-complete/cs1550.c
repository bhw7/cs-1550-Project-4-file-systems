/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.

    Edited by Betsalel "Saul" Williamson
    saul.williamson@pitt.edu
    Last edited: Aug 3, 2016 10:44 PM

*/

#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>

#ifndef _DEBUG
#define _DEBUG
#endif

#ifdef _DEBUG
#define print_debug(s) printf s
#else
#define print_debug(s) do {} while (0)
#endif

static int dirty = false;

//size of a disk block
#define    BLOCK_SIZE 512

//we'll use 8.3 filenames
#define    MAX_FILENAME 8
#define    MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

/**
 *
 * @param bitmap pointer to a bitmap
 * @return the first location of a free pointer
 */
long get_free_block(char *bitmap);

/**
 *
 * @param offset the offset from the start of the bitmap
 * @param length the length of the bits to set
 * @param value the value to set. usually 1.
 * @param bitmap pointer to a bitmap
 */
void set_bit_map(long offset, long length, char value, char *bitmap);

void print_bit_map(int offset, int length, char *bitmap);

/**
 *
 * @param src string
 * @param dest string
 * @param start_position index to start
 * @param length total length to copy from src to dest
 */
void substring(const char *src, char **dest, int start_position, int length);

/**
 *
 * @param path the full path information
 * @param dir_name a pointer to a pointer
 * @param full_file_name a pointer to a pointer
 * @param file_name a pointer to a pointer
 * @param extension_name a pointer to a pointer
 * @return:     0 on success
 *      -ENAMETOOLONG if the name is beyond 8.3 chars
 *      -EPERM if the file is trying to be created in the root dir
 *      -EEXIST if the file already exists
 */
int get_path_info_for_mknod(const char *path, char **dir_name, char **full_file_name, char **file_name,
                            char **extension_name);

/**
 *
 * @param path the full path information
 * @param dir_name a pointer to a pointer
 * @param full_file_name a pointer to a pointer, is empty string if no file
 * @param file_name a pointer to a pointer, is empty string if no file
 * @param extension_name a pointer to a pointer, is empty string if no file
 */
void get_path_info(const char *path, char **dir_name, char **full_file_name, char **file_name,
                   char **extension_name);


//The attribute packed means to not align these things
struct cs1550_directory_entry {
    int nFiles;    //How many files are in this directory.
    //Needs to be less than MAX_FILES_IN_DIR

    struct cs1550_file_directory {
        char fname[MAX_FILENAME + 1];    //filename (plus space for nul)
        char fext[MAX_EXTENSION + 1];    //extension (plus space for nul)
        size_t fsize;                    //file size
        long nStartBlock;                //where the first block is on disk
    } __attribute__((packed)) files[MAX_FILES_IN_DIR];    //There is an array of these

    //This is some space to get this to be exactly the size of the disk block.
    //Don't use it for anything.
    char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
};

typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory {
    int nDirectories;    //How many subdirectories are in the root
    //Needs to be less than MAX_DIRS_IN_ROOT
    struct cs1550_directory {
        char dname[MAX_FILENAME + 1];    //directory name (plus space for nul)
        long nStartBlock;                //where the directory block is on disk
    } __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];    //There is an array of these

    //This is some space to get this to be exactly the size of the disk block.
    //Don't use it for anything.
    char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
};


typedef struct cs1550_directory_entry cs1550_directory_entry;

//How much data can one block hold?
#define    MAX_DATA_IN_BLOCK (BLOCK_SIZE)

struct cs1550_disk_block {
    //All of the space in the block can be used for actual data
    //storage.
    char data[MAX_DATA_IN_BLOCK]; // 512 * 1 byte
};

typedef struct cs1550_disk_block cs1550_disk_block;

// we need to keep track of the disk by using a bitmap
// size is 5*2^20 for 5 mb or 5242880 bytes
#define SIZE_OF_DISK 5242880
#define BIT_MAP_SIZE 655360
#define NUMBER_OF_BLOCKS ((SIZE_OF_DISK - (BIT_MAP_SIZE)) / BLOCK_SIZE)
// need to get disk size in bytes on init
// information
struct cs1550_disk {
    cs1550_disk_block blocks[NUMBER_OF_BLOCKS];
    // reserve 5242880 bytes - (5242880 bits or 655360 bytes)
    // or 4587520 bytes / 512 bytes per block = 8960 blocks

    char bitmap[BIT_MAP_SIZE]; // 655360 bytes for 8960 blocks this is 1 byte == 8 bits
};

typedef struct cs1550_disk cs1550_disk;

/**
 *
 * @param disk a pointer to the disk
 * @return EXIT_SUCCESS
 *      -EBADF if ".disk" can't be opened
 */
int write_to_disk(cs1550_disk *disk);

struct Singleton {
    cs1550_disk *d;
};

typedef struct Singleton *singleton;

struct Singleton *get_instance(void);

/**
 *
 * By using a singleton to wrap our disk access we can be sure that we are accessing the most up-to-date information.
 *
 * Every time we read we check first to see that we don't need to write by using a global 'dirty' flag.
 *
 * Upon the init the disk is asserted to not be dirty.
 *
 * @return pointer to singleton
 */
struct Singleton *get_instance(void) {
    static singleton instance = NULL;

//    pthread_mutex_lock(&instance_mutex);

    if (instance == NULL) {

        // get map for struct
        instance = (singleton) calloc(1, sizeof(struct Singleton));

        print_debug(("Calloc instance\n"));
        instance->d = (cs1550_disk *) calloc(1, sizeof(struct cs1550_disk));

        assert(dirty == false);

        print_debug(("Opening disk for read\n"));
        FILE *filePtr = fopen(".disk", "rb");
        if (filePtr == NULL) {
            exit(-EBADF);
        }

        fread(instance->d, sizeof(struct cs1550_disk), 1, filePtr);
        print_debug(("Closed disk for read in get_instance\n"));
        fclose(filePtr);

        // todo: implement variable size disk
//        // get disk size
        struct stat st;
        stat(".disk", &st);
        off_t size = st.st_size;
        print_debug(("disk size: %ld\n", (long) size));
        print_debug(("size of struct cs1550_disk: %ld\n", sizeof(struct cs1550_disk)));
        print_debug(("max directories = %ld\n", MAX_DIRS_IN_ROOT));
        print_debug(("max files in dir = %ld\n", MAX_FILES_IN_DIR));

//        // get map for disk
//        instance->d = (cs1550_disk *) calloc(1, (size_t) size);
//
//        print_debug(("size of blocks: %ld\n", (long) (size - (size >> 3)) >> 9));
//        print_debug(("size of bitmap: %ld\n", (long) size >> 3));
//
//        // Example is size is 5MB or 5242880 bytes
//        // I need 5242880 bits or 5242880 >> 3
//        // This leaves (5242880 bytes - 5242880 bites) bytes left
//
//        // To convert from bytes to bits
//        size_t bit_map_size = (size_t) (size >> 3);
//        instance->d->bitmap = (char *) calloc(1, bit_map_size);
//
//        // To convert from bytes to blocks >> 9
//        size_t block_size = (size_t) (size - bit_map_size) >> 9;
//        instance->d->blocks = (cs1550_disk_block *) calloc(1, block_size);

    } else {
        print_debug(("Accessed non-null instance\n"));

        if (dirty == true) {
            print_debug(("!! ** Disk is dirty ** !!\nWriting out before read.\n"));
            write_to_disk(instance->d);
            dirty = false;
        }

        print_debug(("Opening disk for read\n"));
        FILE *filePtr = fopen(".disk", "rb");
        if (filePtr == NULL) {
            exit(-EBADF);
        }

        fread(instance->d, sizeof(struct cs1550_disk), 1, filePtr);
        print_debug(("Closed disk for read in get_instance\n"));
        fclose(filePtr);
    }

    return instance;
}


// that means we store a 0 when the block is empty and 1 when the block is using information
void set_bit_map(long offset, long length, char value, char *bitmap) {
    long i;
//    print_debug(("offset: %ld\tlength: %ld\tvalue: %d\n", offset, length, value));

    for (i = offset; i < offset + length; ++i) {

        if ((bitmap[i / 8] & (value << (i % 8))) == 1) {
            print_debug(("Index %ld has value %8.8x\n", i / 8, bitmap[i / 8]));
            print_debug(("Overwrote index %ld with %8.8x\n", i / 8, value << (i % 8)));
        }

//        if (0x3f & 0x40){
//            print_debug(("i've got issues with math\n"));
//        } else {
//            print_debug(("i understand things\n"));
//        }

        bitmap[i / 8] |= value << (i % 8);
    }
}


void print_bit_map(int offset, int length, char *bitmap) {

    int j;
    for (j = offset; j < length / 8; ++j) {
        print_debug(("Index %d with %x\n", j, bitmap[j]));
    }
}


int write_to_disk(cs1550_disk *disk) {

    print_debug(("Opening disk for write\n"));
    FILE *filePtr = fopen(".disk", "rb+");
    if (filePtr == NULL) {
        return -EBADF;
    }

    fwrite(disk, sizeof(struct cs1550_disk), 1, filePtr); //write struct to file
    print_debug(("Closed disk after write\n"));
    fclose(filePtr);

    return EXIT_SUCCESS;
}

// could cache results and return things if I update this when I write out information
long get_free_block(char *bitmap) {

    print_debug(("Getting free block starting index at: %ld\n", sizeof(struct cs1550_root_directory) - 1));
    int i = sizeof(struct cs1550_root_directory) - 1;

    // seek until I find the first free bit
    // reserve the first block for root
    for (; i < BIT_MAP_SIZE; ++i) {

        if ((bitmap[i / 8] & (1 << (i % 8))) == 0) {
            print_debug(("Free block at: %d\n", i));
            break;
        }
    }

    return i;
}


int get_path_info_for_mknod(const char *path, char **dir_name, char **full_file_name, char **file_name,
                            char **extension_name) {

    int result = 0;

    print_debug(("In get_path_info_for_mknod\n"));

    get_path_info(path, dir_name, full_file_name, file_name, extension_name);

    print_debug(("dir_name: %s\n", *dir_name));
    print_debug(("full_file_name: %s\n", *full_file_name));
    print_debug(("file_name: %s\n", *file_name));
    print_debug(("extension_name: %s\n", *extension_name));

    int file_letter_count = (int) strlen(*file_name);
    int extension_letter_count = (int) strlen(*extension_name);
    int dir_letter_count = (int) strlen(*dir_name);

    if (file_letter_count > 8 || extension_letter_count > 3) {
        result = -ENAMETOOLONG;
    } else if (dir_letter_count > 8) {
        result = -ENAMETOOLONG;
    }

    int i, slash_count = 0;
    for (i = 0; i < strlen(path); ++i) {
        if (path[i] == '/') {
            slash_count++;
        }
    }
    // we have tried to create a file in a subdirectory of a subdirectory
    if (slash_count > 2) {
        result = -EPERM;
    }

    print_debug(("Done get_path_info_for_mknod\n"));

//    free(dir_name);
//    free(file_name);
//    free(full_file_name);
//    free(extension_name);

    return result;
}


void get_path_info(const char *path, char **dir_name, char **full_file_name, char **file_name,
                   char **extension_name) {
    print_debug(("In get_path_info\n"));

    (*dir_name) = (char *) malloc(FILENAME_MAX);
    (*file_name) = (char *) malloc(FILENAME_MAX);
    (*extension_name) = (char *) malloc(FILENAME_MAX);
    (*full_file_name) = (char *) malloc(FILENAME_MAX);

    int path_index = 0;
    int dot_index = 0;
    int num_slash = 0;
    int in_dir = false;
    int in_file = false;

    int i;
    for (i = 0; i < strlen(path); i++) {
        if (path[i] == '/') {
            in_dir = true;
            in_file = false;

            num_slash++;
            path_index = i;

            if (num_slash == 2) {
                // got directory
                substring(path, dir_name, 1, i - 1);
                print_debug(("dirname : %s\n", *dir_name));
            } else if (num_slash > 2) {
                // error with file name
            }
        } else if (path[i] == '.') {
            dot_index = i;
            in_dir = false;
            in_file = true;
            if (num_slash == 1) {
                // error with file name
            }

        }

        if (i == strlen(path) - 1) { // at the end of the string
            if (in_dir) {
                substring(path, dir_name, path_index + 1, (int) ((strlen(path) - 1) - path_index));
                strcpy(*full_file_name, "");
                strcpy(*file_name, "");
                strcpy(*extension_name, "");
            } else if (in_file) {
                substring(path, full_file_name, path_index + 1, (int) ((strlen(path) - 1) - path_index));
                substring(path, file_name, path_index + 1, (dot_index - 1) - path_index);
                substring(path, extension_name, dot_index + 1, (int) ((strlen(path) - 1) - dot_index));
            } else {
                //error
            }
        }
    }
}


void substring(const char *src, char **dest, int start_position, int length) {
    memcpy((*dest), &src[start_position], length);
    (*dest)[length] = '\0';
    print_debug(("Src %s length %d\n", src, (int) strlen(src)));
    print_debug(("Dest %s length %d\n", *dest, (int) strlen(*dest)));
}

/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not.
 *
 * man -s 2 stat will show the fields of a stat structure
 *
 * @return: 0 on success, with a correctly set structure
 *      -ENOENT if the file is not found
 */
static int cs1550_getattr(const char *path, struct stat *stbuf) {
    print_debug(("Inside cs1550_getattr = %s\n", path));

    //default return that path doesn't exist
    int result = -ENOENT;

    char *dir_name;
    char *full_file_name;
    char *file_name;
    char *extension_name;

    get_path_info(path, &dir_name, &full_file_name, &file_name, &extension_name);

    print_debug(("dir_name: %s\n", dir_name));
    print_debug(("full_file_name: %s\n", full_file_name));
    print_debug(("file_name: %s\n", file_name));
    print_debug(("extension_name: %s\n", extension_name));

    memset(stbuf, 0, sizeof(struct stat));

    cs1550_disk *disk = get_instance()->d;
    struct cs1550_root_directory *bitmapFileHeader = (struct cs1550_root_directory *) &disk->blocks[0];
//    print_debug(("\n\nnDirectories %d\n\n", bitmapFileHeader->nDirectories));

    // this will contain all of the information about the disk
    //our bitmap file header

    //is path the root dir?
    if (strcmp(path, "/") == 0) {
        print_debug(("In get_path_info for root\n"));
        print_debug(("\n\nnDirectories %d\n\n", bitmapFileHeader->nDirectories));

        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        result = 0;
    } else {

        // if in double or + sub dir resturn not found
        int j, slash_count = 0;
        for (j = 0; j < strlen(path); ++j) {
            if (path[j] == '/') {
                slash_count++;
            }
        }

        /*
         * todo: this business logic is convoluted would do better to have a switch statement with 1) root, 2) subdir 3) subdir-subdir
         */
        // we have tried to create a file in a subdirectory of a subdirectory
        if ((strlen(file_name) == 0) && slash_count >= 2) {
            // this is used if the user tries to create a directory in a subdirectory
            // result does not exist
        } else if (strlen(full_file_name) == 0) {  // if in single sub dir
            print_debug(("In get_path_info for dir\n"));
            //Check if name is subdirectory
            // if the directory exists
            int i;
            for (i = 0; i < bitmapFileHeader->nDirectories; ++i) {

                assert(bitmapFileHeader->directories[i].nStartBlock < sizeof(cs1550_disk_block) * NUMBER_OF_BLOCKS);
                cs1550_directory_entry *entry = (cs1550_directory_entry *) &disk->blocks[bitmapFileHeader->directories[i].nStartBlock];
                print_debug(("\n\nNumber of files %d\n\n", entry->nFiles));
                print_debug(
                        ("bitmap %s dir_name %s result %d\n", bitmapFileHeader->directories[i].dname, dir_name, strcmp(
                                bitmapFileHeader->directories[i].dname, dir_name)));

                if (strcmp(bitmapFileHeader->directories[i].dname, dir_name) == 0) {

                    //Might want to return a structure with these fields
                    stbuf->st_mode = S_IFDIR | 0755;
                    stbuf->st_nlink = 2;
                    result = 0; //no error

                    break;
                }
            }
        } else { // reading file
            print_debug(("In get_path_info for file\n"));

            int i;
            for (i = 0; i < bitmapFileHeader->nDirectories; ++i) {
                //    print_debug(("\n\nnDirectories %d\n\n", bitmapFileHeader->nDirectories));
                print_debug(
                        ("bitmap %s dir_name %s result %d\n", bitmapFileHeader->directories[i].dname, dir_name, strcmp(
                                bitmapFileHeader->directories[i].dname, dir_name)));

                if (strcmp(bitmapFileHeader->directories[i].dname, dir_name) == 0) {

                    // get the cs1550_directory_entry
                    assert(bitmapFileHeader->directories[i].nStartBlock < sizeof(cs1550_disk_block) * NUMBER_OF_BLOCKS);
                    cs1550_directory_entry *entry = (cs1550_directory_entry *) &disk->blocks[bitmapFileHeader->directories[i].nStartBlock];

                    print_debug(("entry->nFiles : %d\n", entry->nFiles));

                    int m;
                    for (m = 0; m < entry->nFiles; ++m) {
                        print_debug(
                                ("entry %s file_name %s result %d\n", entry->files[m].fname, file_name,
                                        strcmp(entry->files[m].fname, file_name)));
                        print_debug(
                                ("entry %s extension_name %s result %d\n", entry->files[m].fext, extension_name,
                                        strcmp(entry->files[m].fext, extension_name)));

                        if (strcmp(entry->files[m].fname, file_name) == 0 &&
                            strcmp(entry->files[m].fext, extension_name) == 0) {
                            //Check if name is a regular file


                            //regular file, probably want to be read and write
                            stbuf->st_mode = S_IFREG | 0666;
                            stbuf->st_nlink = 1; //file links
                            stbuf->st_size = 0; //file size - make sure you replace with real size!
                            result = 0; // no error
                            break;
                        }
                    }
                    break;

                }
            }
        }
    }

    free(dir_name);
    free(file_name);
    free(full_file_name);
    free(extension_name);

    return result;
}


/*
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 *
 * This function should look up the input path, ensuring that it is a directory, and then list the contents.
 *
 * To list the contents, you need to use the filler() function.  For example: filler(buf, ".", NULL, 0); adds the
 * current directory to the listing generated by ls -a
 *
 * In general, you will only need to change the second parameter to be the name of the file or directory you want to add
 * to the listing.
 *
 * @return: 0 on success
 *      -ENOENT if the directory is not valid or found
 */
static int cs1550_readdir(const char *path,
                          void *buf,
                          fuse_fill_dir_t filler,
                          off_t offset,
                          struct fuse_file_info *fi) {

    print_debug(("Inside read directory path = %s\n", path));

    // list the contents of the directory at path
    // that means I need to navigate to the path first

    //Since we're building with -Wall (all warnings reported) we need
    //to "use" every parameter, so let's just cast them to void to
    //satisfy the compiler

    (void) offset;
    (void) fi;

    char *dir_name;
    char *full_file_name;
    char *file_name;
    char *extension_name;

    get_path_info(path, &dir_name, &full_file_name, &file_name, &extension_name);

    cs1550_disk *disk = get_instance()->d;
    struct cs1550_root_directory *bitmapFileHeader = (struct cs1550_root_directory *) &disk->blocks[0];
//    print_debug(("\n\nnDirectories %d\n\n", bitmapFileHeader->nDirectories));

    // this will contain all of the information about the disk

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    if (strcmp(path, "/") == 0) {
        // todo: ineffeicient way to read. need to work on this because it is reading all directories. I just want the current directory
        int i;
        for (i = 0; i < bitmapFileHeader->nDirectories; ++i) {
            filler(buf, bitmapFileHeader->directories[i].dname, NULL, 0);
        }
    } else {

        int i;
        for (i = 0; i < bitmapFileHeader->nDirectories; ++i) {

            if (strcmp(bitmapFileHeader->directories[i].dname, dir_name) == 0) {
                print_debug(("I'm in this directory %s\n", dir_name));

                // get the cs1550_directory_entry
                assert(bitmapFileHeader->directories[i].nStartBlock < sizeof(cs1550_disk_block) * NUMBER_OF_BLOCKS);
                cs1550_directory_entry *entry = (cs1550_directory_entry *) &disk->blocks[bitmapFileHeader->directories[i].nStartBlock];
                print_debug(("Number of entries directory %d\n", entry->nFiles));

                int j;
                for (j = 0; j < entry->nFiles; ++j) {

                    char buff_full_file_name[MAX_FILENAME + MAX_EXTENSION + 1] = "";

                    print_debug(("Before strcpy: buff_full_file_name %s\n", buff_full_file_name));

                    strcat(buff_full_file_name, entry->files[j].fname);
                    print_debug(("file_name: %s\n", entry->files[j].fname));

                    strcat(buff_full_file_name, ".");
                    strcat(buff_full_file_name, entry->files[j].fext);
                    print_debug(("extension_name: %s\n", entry->files[j].fext));

                    print_debug(("After strcpy: buff_full_file_name %s\n", buff_full_file_name));

                    filler(buf, buff_full_file_name, NULL, 0);
                }
            }
        }
    }

    return 0;
}

/**
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 *
 * @return: 0 on success
 *      -ENAMETOOLONG if the name is beyond 8 chars
 *      -EPERM if the directory is not under the root dir only
 *      -EEXIST if the directory already exists
 */
static int cs1550_mkdir(const char *path, mode_t mode) {
    print_debug(("Inside make directory path = %s\n", path));

    (void) path;
    (void) mode;

    // get name
    int result = 0;
    int slash_count = 0;
    int letter_count = 0;
    const size_t str_length = strlen(path);

    // start at one before the '\0'
    int i;
    for (i = (int) str_length - 1; i >= 0; --i) {

        // if name is too long
        if (letter_count > 8) {
            result = -ENAMETOOLONG;
            break;
        }

        if (path[i] == '/') {
            slash_count++;
            letter_count = 0; // reset letter count
        } else {
            letter_count++;
        }

        if (slash_count > 1) {
            result = -EPERM;
            break;
        }
    }

    // if directory is not under the root directory
    // this means that the path information is off

    if (result == 0) {

        // minus 1 for '\0' minus 1 for '/' path character
        print_debug(("Length %d\n", (int) str_length));
        assert((str_length - 1) <= 8);
        char file_name[str_length - 1];
        memcpy(file_name, &path[1], str_length);
        file_name[str_length - 1] = '\0';
        print_debug(("file_name %s\n", file_name));

        cs1550_disk *disk = get_instance()->d;
        struct cs1550_root_directory *bitmapFileHeader = (struct cs1550_root_directory *) &disk->blocks[0];

        // if the directory exists
        int j;
        for (j = 0; j < bitmapFileHeader->nDirectories; ++j) {

            if (strcmp(bitmapFileHeader->directories[j].dname, file_name) == 0) {
                result = -EEXIST;
                break;
            }
        }

        if (bitmapFileHeader->nDirectories == MAX_DIRS_IN_ROOT) {
            result = -EPERM;
        }

        // else create directory
        if (result == 0) {
            dirty = true;

            // loop through array
            // we are adding a new directory therefor we can write directly to the end
            strcpy(bitmapFileHeader->directories[bitmapFileHeader->nDirectories].dname, file_name);

            long start_block = get_free_block(disk->bitmap);

            bitmapFileHeader->directories[bitmapFileHeader->nDirectories].nStartBlock = start_block;
            set_bit_map((int) start_block, sizeof(struct cs1550_directory_entry), 1, disk->bitmap);

            long address = bitmapFileHeader->directories[bitmapFileHeader->nDirectories].nStartBlock;
            assert(address < sizeof(cs1550_disk_block) * NUMBER_OF_BLOCKS);

            cs1550_directory_entry *new_entry = (cs1550_directory_entry *) &disk->blocks[address];

            new_entry->nFiles = 0;

            bitmapFileHeader->nDirectories++;

            write_to_disk(disk);
            dirty = false;
        }
    }

    return result;
}

/*
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path) {
    (void) path;
    return 0;
}


/**
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 * This function should add a new file to a subdirectory, and should update the .disk file appropriately with the
 * modified directory entry structure.
 *
 * @return:     0 on success
 *      -ENAMETOOLONG if the name is beyond 8.3 chars
 *      -EPERM if the file is trying to be created in the root dir
 *      -EEXIST if the file already exists
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev) {

    print_debug(("I'm in mknod path = %s\n", path));

    (void) mode;
    (void) dev;

    // get name
    int result = 0;

    char *dir_name;
    char *full_file_name;
    char *file_name;
    char *extension_name;

    result = get_path_info_for_mknod(path, &dir_name, &full_file_name, &file_name, &extension_name);

    print_debug(("dir_name = %s\n", dir_name));
    print_debug(("full_file_name = %s\n", full_file_name));
    print_debug(("file_name = %s\n", file_name));
    print_debug(("extension_name = %s\n", extension_name));

    cs1550_directory_entry *entry = NULL;
    cs1550_disk *disk = get_instance()->d;
    struct cs1550_root_directory *bitmapFileHeader = (struct cs1550_root_directory *) &disk->blocks[0];

    int m = 0;

    if (strcmp(path, "/") == 0) {

        result = -EPERM;
    }

    if (result == 0) {
        // go to the directory
        int found_dir = false;
        int l;
        for (l = 0; l < bitmapFileHeader->nDirectories; ++l) {

            if (strcmp(bitmapFileHeader->directories[l].dname, dir_name) == 0) {
                // i found the directory
                found_dir = true;

                // get the cs1550_directory_entry
                assert(bitmapFileHeader->directories[l].nStartBlock < sizeof(cs1550_disk_block) * NUMBER_OF_BLOCKS);
                entry = (cs1550_directory_entry *) &disk->blocks[bitmapFileHeader->directories[l].nStartBlock];

                if (entry->nFiles == MAX_FILES_IN_DIR) {
                    result = -EPERM;
                    break;
                }

                for (m = 0; m < entry->nFiles; ++m) {
                    if (strcmp(entry->files[m].fname, file_name) == 0 &&
                        strcmp(entry->files[m].fext, extension_name) == 0) {
                        result = -EEXIST;
                        break;
                    }
                }
                break;
            }
        }

        if (!found_dir) {
            result = -EPERM;

            // todo: allow user to create directory and file in the same operation
//            int mkdir_result = cs1550_mkdir(path, (mode_t) NULL);
//            print_debug(("Result of mkdir: %d\n", mkdir_result));
//
//            if (mkdir_result != 0) {
//                result = -EPERM;
//            }
        }
    }

    // create file;
    if (result == 0) {

        dirty = true;
        assert(entry != NULL);

        print_debug(("Creating file entry\n"));
        // m is the current position in the directory structure for me to store the file
        // entry is a pointer to the subdirectory

        entry->nFiles++;

        strcpy(entry->files[m].fname, file_name);
        print_debug(("file_name: %s\n", entry->files[m].fname));

        strcpy(entry->files[m].fext, extension_name);
        print_debug(("extension_name: %s\n", entry->files[m].fext));

        // get proper file size; note that this seems to be given to the write call
        // will try to do this stuff when I write to the file for the first time.

        entry->files[m].fsize = 0;
        entry->files[m].nStartBlock = 0;

        write_to_disk(disk);
        dirty = false;
    }

    free(dir_name);
    free(file_name);
    free(full_file_name);
    free(extension_name);

    return result;
}

/*
 * Deletes a file
 *
 * This function should not be modified.
 */
static int cs1550_unlink(const char *path) {
    (void) path;

    return 0;
}

/*
 * Read size bytes from file into buf starting from offset
 *
 * @return: size read on success
 *      -EISDIR if the path is a directory
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi) {

    print_debug(
            ("I'm in cs1550_read: size = %ld offset = %ld\npath = %s\nbuffer = %s\n", size, (long) offset, path, buf));

    ////    This function should read the data in the file denoted by path into buf, starting at offset.
//    (void) buf;
//    (void) offset;
    (void) size;
    (void) fi;
//    (void) path;
//
//    //check to make sure path exists
//    //check that size is > 0
//    //check that offset is <= to the file size
//    //read in data
//    //set size and return, or error
//
//    size = 0;
//
//    return (int) size;

    int result = 0;
    char *dir_name;
    char *full_file_name;
    char *file_name;
    char *extension_name;

    get_path_info(path, &dir_name, &full_file_name, &file_name, &extension_name);

    if (strlen(full_file_name) == 0) {
        result = -EISDIR;
    }

    if (result == 0) {

        cs1550_directory_entry *entry = NULL;
        cs1550_disk *disk = get_instance()->d;
        struct cs1550_root_directory *bitmapFileHeader = (struct cs1550_root_directory *) &disk->blocks[0];

        print_debug(("In cs1550_read for file\n"));

        int i;
        for (i = 0; i < bitmapFileHeader->nDirectories; ++i) {

            if (strcmp(bitmapFileHeader->directories[i].dname, dir_name) == 0) {

                // entry is a pointer to the subdirectory

                assert(bitmapFileHeader->directories[i].nStartBlock < sizeof(cs1550_disk_block) * NUMBER_OF_BLOCKS);
                entry = (cs1550_directory_entry *) &disk->blocks[bitmapFileHeader->directories[i].nStartBlock];

                print_debug(
                        ("bitmap %s dir_name %s result %d\n", bitmapFileHeader->directories[i].dname, dir_name, strcmp(
                                bitmapFileHeader->directories[i].dname, dir_name)));
                print_debug(("entry->nFiles : %d\n", entry->nFiles));

                int m;
                for (m = 0; m < entry->nFiles; ++m) {

                    if (strcmp(entry->files[m].fname, file_name) == 0 &&
                        strcmp(entry->files[m].fext, extension_name) == 0) {

                        print_debug(("Reading file\n"));
                        print_debug(("file_name: %s\n", entry->files[m].fname));
                        print_debug(("extension_name: %s\n", entry->files[m].fext));


                        int fd;

                        // need to read from disk
                        fd = open(".disk", O_RDONLY);
                        if (fd == -1) {
                            result = -EBADF;
                        }

                        if (result == 0) {
                            result = (int) pread(fd, buf, entry->files[m].fsize, entry->files[m].nStartBlock);
                            print_debug(("result after pread = %d\n", result));
                            print_debug(("result after buf = %s\n", buf));
                        }

                        if (result >= 0) {
                            result = (int) entry->files[m].fsize;
                            print_debug(("size = %d\n", result));
                        }

                        close(fd);

                        break;
                    }
                }
                break;
            }
        }
    }

    return result;
}

/*
 * Write size bytes from buf into file starting from offset
 *
 * @return: size on success
 *      -EFBIG if the offset is beyond the file size (but handle appends)
 *          // it was ambiguous on how to handle appends.
 *          // assume that append means writing to a location within the bounds of the initial file size
 */
static int cs1550_write(const char *path, const char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi) {
    print_debug(
            ("I'm in cs1550_write: size = %ld offset = %ld\npath = %s\nbuffer = %s\n", size, (long) offset, path, buf));

    ////    This function should write the data in buf into the file denoted by path, starting at offset.
//    (void) buf;
//    (void) offset;
    (void) fi;
//    (void) path;
//
//    //check to make sure path exists
//    //check that size is > 0
//    //check that offset is <= to the file size
//    //write data
//    //set size (should be same as input) and return, or error
//
//    return (int) size;

    int result = 0;

    char *dir_name;
    char *full_file_name;
    char *file_name;
    char *extension_name;

    get_path_info(path, &dir_name, &full_file_name, &file_name, &extension_name);

    cs1550_directory_entry *entry = NULL;
    cs1550_disk *disk = get_instance()->d;
    struct cs1550_root_directory *bitmapFileHeader = (struct cs1550_root_directory *) &disk->blocks[0];

    print_debug(("In cs1550_write for file\n"));

    int i;
    for (i = 0; i < bitmapFileHeader->nDirectories; ++i) {
        //    print_debug(("\n\nnDirectories %d\n\n", bitmapFileHeader->nDirectories));


        if (strcmp(bitmapFileHeader->directories[i].dname, dir_name) == 0) {

            // entry is a pointer to the subdirectory
            assert(bitmapFileHeader->directories[i].nStartBlock < sizeof(cs1550_disk_block) * NUMBER_OF_BLOCKS);
            entry = (cs1550_directory_entry *) &disk->blocks[bitmapFileHeader->directories[i].nStartBlock];

            print_debug(
                    ("bitmap %s dir_name %s result %d\n", bitmapFileHeader->directories[i].dname, dir_name, strcmp(
                            bitmapFileHeader->directories[i].dname, dir_name)));
            print_debug(("entry->nFiles : %d\n", entry->nFiles));

            int m;
            for (m = 0; m < entry->nFiles; ++m) {

                // m is the current position in the directory structure for me to store the file
                if (strcmp(entry->files[m].fname, file_name) == 0 &&
                    strcmp(entry->files[m].fext, extension_name) == 0) {


                    print_debug(("Writing to file\n"));
                    print_debug(("file_name: %s\n", entry->files[m].fname));
                    print_debug(("extension_name: %s\n", entry->files[m].fext));

                    if (entry->files[m].nStartBlock == 0) {

                        print_debug(("First time writing to file\n"));

                        // check to see that there is room left on the disk
                        if (get_free_block(disk->bitmap) + size > SIZE_OF_DISK) {
                            result = -EFBIG;
                        } else {
                            dirty = true;
                            entry->files[m].nStartBlock = get_free_block(disk->bitmap);
                            entry->files[m].fsize = size;
                            set_bit_map((int) entry->files[m].nStartBlock, (int) entry->files[m].fsize, 1,
                                        disk->bitmap);
                            write_to_disk(disk);
                            dirty = false;
                        }
                    }

                    /* Finished with dealing with writing to information blocks */

                    if (offset + size > entry->files[m].fsize) {
                        result = -EFBIG;
                    } else {
                        int fd;

                        // need to write to .disk
                        fd = open(".disk", O_WRONLY);
                        if (fd == -1) {
                            result = -EBADF;
                        }

                        if (result == 0) {
                            result = (int) pwrite(fd, buf, size, entry->files[m].nStartBlock + offset);
                            print_debug(("result after pwrite = %d\n", result));
                            print_debug(("result after buf = %s\n", buf));
                        }

                        if (result >= 0) {
                            result = (int) size;
                            print_debug(("size = %d\n", result));
                        }

                        close(fd);
                    }
                    break;
                }
            }
            break;
        }
    }

    return result;
}

/******************************************************************************
 *
 *  DO NOT MODIFY ANYTHING BELOW THIS LINE
 *
 *****************************************************************************/

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 * This function should not be modified.
 *
 */
static int cs1550_truncate(const char *path, off_t size) {
    (void) path;
    (void) size;

    return 0;
}


/*
 * Called when we open a file
 *
 * This function should not be modified, as you get the full path every time any of the other functions are called.
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi) {
    (void) path;
    (void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but
       if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 *
 * This function should not be modified.
 */
static int cs1550_flush(const char *path, struct fuse_file_info *fi) {
    (void) path;
    (void) fi;

    return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
        .getattr    = cs1550_getattr,
        .readdir    = cs1550_readdir,
        .mkdir    = cs1550_mkdir,
        .rmdir = cs1550_rmdir,
        .read    = cs1550_read,
        .write    = cs1550_write,
        .mknod    = cs1550_mknod,
        .unlink = cs1550_unlink,
        .truncate = cs1550_truncate,
        .flush = cs1550_flush,
        .open    = cs1550_open,
};

//Don't change this.
int main(int argc, char *argv[]) {
    return fuse_main(argc, argv, &hello_oper, NULL);
}

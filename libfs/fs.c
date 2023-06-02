#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include "disk.h"
#include "fs.h"

struct SuperBlock {
	char signature[8];
	uint16_t total_blocks;
	uint16_t root_index;
	uint16_t data_index;
	uint16_t data_blocks;
	uint8_t fat_blocks;
	uint8_t unused_padding[4079];
} __attribute__((packed));

struct FAT {
	uint16_t *flat;
} __attribute__((packed));

struct Entry {
	uint8_t filename[FS_FILENAME_LEN];
	uint32_t file_size;
	uint16_t data_index;
	uint8_t unused_padding[10];
} __attribute__((packed));

struct RootDirectory {
	struct Entry entry[FS_FILE_MAX_COUNT];
} __attribute__((packed));

struct File {
	uint8_t filename[FS_FILENAME_LEN];
	size_t offset;
};

struct Files {
	int open;
	struct File file[FS_OPEN_MAX_COUNT];
};

struct SuperBlock super;
struct FAT fat;
struct RootDirectory root;
struct Files files;

int fs_mount(const char *diskname)
{
	// return -1 if virtual disk file does not open
	if (block_disk_open(diskname) == -1) {
		return -1;
	}

	// read super block
	block_read(0, &super);

	// minimum capacity for fat
	uint8_t fat_min = (super.data_blocks * 2 + BLOCK_SIZE - 1) / BLOCK_SIZE;

	// return -1 if signature is not ECS150FS
	if (memcmp("ECS150FS", super.signature, 8) != 0) {
		return -1;
	}
	// return -1 if number of blocks in super does not match total blocks
	if (super.fat_blocks + super.data_blocks != super.total_blocks - 2) {
		return -1;
	}
	// return -1 if total blocks in super block does not match block disk count
	if (super.total_blocks != block_disk_count()) {
		return -1;
	}
	// return -1 if super block is not in the correct order
	if (super.fat_blocks + 1 != super.root_index || super.root_index + 1 != super.data_index) {
		return -1;
	}
	// return -1 if number of fat blocks is not equal to the min capacity
	if (super.fat_blocks != fat_min) {
		return -1;
	}

	// allocate fat
	fat.flat = (uint16_t *)malloc(super.data_blocks * sizeof(uint16_t));
	// allocate a buffer
	void *buffer = (void *)malloc(BLOCK_SIZE);

	// read from buffer, transfer to fat
	for (size_t i = 0; i < super.fat_blocks; i++) {
		// return -1 if the block read returns -1
		if (block_read(i + 1, buffer) == -1) {
			return -1;
		}
		// send buffer to fat
		memcpy(fat.flat + (i * BLOCK_SIZE / 2), buffer, BLOCK_SIZE);
	}

	// return -1 if first entry is not invalid entry (FFFF)
	if (fat.flat[0] != 0xFFFF) {
		return -1;
	}

	// read root
	block_read(super.root_index, &root);

	return 0;
}

int fs_umount(void)
{
	// write super block, return -1 if no mounted FS
	if (block_write(0,&super) == -1){
		return -1;
	}

	// allocate a buffer
	void *buffer = (void *)malloc(BLOCK_SIZE);

	//read from fat, transfer to buffer
	for (size_t i = 0; i < super.fat_blocks; i++) {
		// send fat to buffer
		memcpy(buffer, fat.flat + (i * BLOCK_SIZE / 2), BLOCK_SIZE);

		// writes the buffer into fat block, return -1 if issue with writing
		if (block_write(i + 1, buffer) == -1) {
			return -1;
		}
	}

	// return -1 if issue when writing to super block
	if (block_write(super.root_index, &root) == -1) {
		return -1;
	}

	// close virtual disk, return value returned by block_disk_close function
	return block_disk_close();
}

int fs_info(void)
{
	// print FS Info as follows
	printf("FS Info:\n");
	printf("total_blk_count=%" PRIu16 "\n", super.total_blocks);
	printf("fat_blk_count=%" PRIu8 "\n", super.fat_blocks);
	printf("rdir_blk=%" PRIu16 "\n", super.root_index);
	printf("data_blk=%" PRIu16 "\n", super.data_index);
	printf("data_blk_count=%" PRIu16 "\n", super.data_blocks);

	// set free blocks to 0
	int free_blocks = 0;

	// if fat array at index i is 0, there is a free block
	for (size_t i = 0; i < super.data_blocks; i++) {
		if (fat.flat[i] == 0) {
			free_blocks++;
		}
	}
	// print rest of FS Info
	printf("fat_free_ratio=%d/%" PRIu16 "\n", free_blocks, super.data_blocks);

	// do the same for last FS Info
	free_blocks = 0;
	for(size_t i = 0; i < FS_FILE_MAX_COUNT; i++){
		if (root.entry[i].filename[0] == '\0') {
			free_blocks++;
		}
	}
	
	printf("rdir_free_ratio=%d/%d\n", free_blocks, FS_FILE_MAX_COUNT);

	return 0;
}

int fs_create(const char *filename)
{
	// return -1 if filename is not correct length
	if (strlen(filename) >= FS_FILENAME_LEN) {
		return -1;
	}

	// return -1 if at capacity of open files
	if (files.open == FS_FILE_MAX_COUNT) {
		return -1;
	}

	for (size_t i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (memcmp(root.entry[i].filename, filename, FS_FILENAME_LEN) == 0) {
			return -1;
		}
	}

	struct Entry *new_entry = NULL;

	for (size_t i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (root.entry[i].filename[0] == '\0') {
			new_entry = &root.entry[i];
			break;
		}
	}

	if (new_entry == NULL) {
		return -1;
	}

	strncpy((char*)new_entry->filename, filename, FS_FILENAME_LEN);
	new_entry->file_size = 0;

	for (size_t i = 0; i < super.data_blocks; i++) {
		if (fat.flat[i] == 0) {
			fat.flat[i] = 0xFFFF;
			new_entry->data_index = 0xFFFF;
			break;
		}
	}

	files.open++;

	return 0;
}

int fs_delete(const char *filename)
{
	for (size_t i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (memcmp(root.entry[i].filename, filename, FS_FILENAME_LEN) == 0) {
			if (files.open == 0) {
				fat.flat[root.entry[i].data_index] = 0x0000;
				memset(&root.entry[i], 0, sizeof(struct Entry));
				return 0;
			} else {
				return -1;
			}
		}
	}

	return -1;
}

int fs_ls(void)
{
	printf("FS Ls:\n");

	for (size_t i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (root.entry[i].filename[0] != '\0') {
			printf("file: %s, size: %" PRIu32 ", data_blk: %" PRIu16 "\n", root.entry[i].filename, root.entry[i].file_size, root.entry[i].data_index);
		}
	}

	return 0;
}

int fs_open(const char *filename)
{
	// return -1 if number of files open is max, full
	if (files.open == FS_OPEN_MAX_COUNT) {
		return -1;
	}
	// return -1 if invalid file
	if (filename == NULL) {
		return -1;
	}

	// iterate through files, look for a place for new file to be created
	for (size_t i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (memcmp(root.entry[i].filename, filename, FS_FILENAME_LEN) == 0) {
			//create new file to open
			struct File *new_file = NULL;

			// if the filename first character is \0, have new file map there
			for (size_t j = 0; j < FS_OPEN_MAX_COUNT; j++) {
				if (files.file[j].filename[0] == '\0') {
					new_file = &files.file[j];
					break;
				}
			}

			// return -1 if the new file is empty (opened nothing)
			if (new_file == NULL) {
				return -1;
			}

			// update new file's offset and filename to match target
			strncpy((char *)new_file->filename, filename, FS_FILENAME_LEN);
			new_file->offset = 0;

			// file successfully opened, increment number of files open
			files.open++;

			return 0;
		}
	}

	return -1;
}

int fs_close(int fd)
{
	// return -1 if file descriptor invalid (out of bounds)
	if (fd < 0 || fd >= FS_OPEN_MAX_COUNT) {
		return -1;
	}
	// return -1 if file descriptor invalid (not open)
	if (files.file[fd].filename[0] == '\0') {
		return -1;
	}

	// close the file
	memset(&files.file[fd], 0, sizeof(struct File));

	// file successfully closed, decrement number of files open
	files.open--;

	return 0;
}

int fs_stat(int fd)
{
	// return -1 if file descriptor invalid (out of bounds)
	if (fd < 0 || fd >= FS_OPEN_MAX_COUNT) {
		return -1;
	}
	// return -1 if file descriptor invalid (not open)
	if (files.file[fd].filename[0] == '\0') {
		return -1;
	}

	// return current file size of fd
	for (size_t i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (memcmp(root.entry[i].filename, files.file[fd].filename, FS_FILENAME_LEN) == 0) {
			return root.entry[i].file_size;
		}
	}

	// return -1 if invalid file descriptor, no match found
	return -1;
}

int fs_lseek(int fd, size_t offset)
{
	// return -1 if file descriptor invalid (out of bounds)
	if (fd < 0 || fd >= FS_OPEN_MAX_COUNT) {
		return -1;
	}
	// return -1 if file descriptor invalid (not open)
	if (files.file[fd].filename[0] == '\0') {
		return -1;
	}
	// return -1 if offset is larger than current file size
	if (offset > root.entry[fd].file_size) {
		return -1;
	}

	// set offset of file to argument given by function
	files.file[fd].offset = offset;

	return 0;
}

int fs_write(int fd, void *buf, size_t count)
{
	// return -1 if file descriptor invalid (out of bounds)
	if (fd < 0 || fd >= FS_OPEN_MAX_COUNT) {
		return -1;
	}
	// return -1 if file descriptor invalid (not open)
	if (files.file[fd].filename[0] == '\0') {
		return -1;
	}

	// create a new empty entry
	struct Entry *entry = NULL;

	// set entry to be the specified entry through the function argument
	for (size_t i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (memcmp(root.entry[i].filename, files.file[fd].filename, FS_FILENAME_LEN) == 0) {
			entry = &root.entry[i];
			break;
		}
	}

	// return -1 if no file could be found with argument identifier
	if (entry == NULL) {
		return -1;
	}

	// initialize writing, representing the number of bytes to be written to the file
	size_t writing = count;
	// store offset of argument file
	size_t offset = files.file[fd].offset;

	// write to file until no more bytes can be written
	while (writing > 0) {
		// set index and offset of block
		size_t block_index = entry->data_index + offset / BLOCK_SIZE;
		size_t block_offset = offset % BLOCK_SIZE;

		// write_size used to determine amount of bytes to write to current block
		size_t write_size = BLOCK_SIZE - block_offset;
		if (write_size > writing) {
			write_size = writing;
		}

		// allocate a buffer to read
		void *buffer = (void*)malloc(BLOCK_SIZE);

		// return -1 if block_read returns -1, issue with the read
		if (block_read(super.data_index + block_index, buffer) == -1) {
			return -1;
		}

		// copy the data from the allocated buffer to the argument buffer
		memcpy(buffer + block_offset, buf, write_size);

		// write the buffer to the file system, return -1 if unable to do so
		if (block_write(super.data_index + block_index, buffer) == -1) {
			return -1;
		}

		// increment the offset and buffer by how many bytes were written, decrement writing by that amount (those bytes have been written, no longer need to be written)
		offset += write_size;
		buf += write_size;
		writing -= write_size;
	}

	// update offset of file to match the new offset position
	files.file[fd].offset = offset;

	// if new offset is bigger than the file size, file size needs to be set to the new offset
	if (offset > entry->file_size) {
		entry->file_size = offset;
	}

	// return number of bytes written to file
	return count;
}

int fs_read(int fd, void *buf, size_t count)
{
	// return -1 if file descriptor invalid (out of bounds)
	if (fd < 0 || fd >= FS_OPEN_MAX_COUNT) {
		return -1;
	}
	// return -1 if file descriptor invalid (not open)
	if (files.file[fd].filename[0] == '\0') {
		return -1;
	}

	// create a new empty entry
	struct Entry *entry = NULL;

	// set entry to be the specified entry through the function argument
	for (size_t i = 0; i < FS_FILE_MAX_COUNT; i++) {
		if (memcmp(root.entry[i].filename, files.file[fd].filename, FS_FILENAME_LEN) == 0) {
			entry = &root.entry[i];
			break;
		}
	}

	// return -1 if no file could be found with argument identifier
	if (entry == NULL) {
		return -1;
	}

	// initialize reading, representing the number of bytes to be read from the file
	size_t reading = count;
	// store offset of argument file
	size_t offset = files.file[fd].offset;

	// check to see if offset is greater than the file size, indicating the end of the file (no more bytes to read, return 0)
	if (offset >= entry->file_size) {
		return 0;
	}

	// update how many bytes we will read based on the offset, we can't read count bytes if the file is not large enough with the given offset
	if (offset + count > entry->file_size) {
		reading = entry->file_size - offset;
	}

	// read through the file until no bytes left to read
	while (reading > 0) {
		// set index and offset of block
		size_t block_index = entry->data_index + offset / BLOCK_SIZE;
		size_t block_offset = offset % BLOCK_SIZE;

		// read_size used to determine amount of bytes to read from current block
		size_t read_size = BLOCK_SIZE - block_offset;

		if (read_size > reading) {
			read_size = reading;
		}

		// allocate a buffer to read
		void *buffer = (void*)malloc(BLOCK_SIZE);

		// return -1 if block_read returns -1, issue with the read
		if (block_read(super.data_index + block_index, buffer) == -1) {
			return -1;
		}

		// copy the data from the allocated buffer to the argument buffer
		memcpy(buf, buffer + block_offset, read_size);

		// increment the offset and buffer by how many bytes were read, decrement reading by that amount (those bytes were read, no longer need to be read)
		offset += read_size;
		buf += read_size;
		reading -= read_size;
	}

	// update offset of file to match the new offset position
	files.file[fd].offset = offset;

	// return number of bytes read from file
	return count - reading;
}


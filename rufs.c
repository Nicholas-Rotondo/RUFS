/*
 *  Copyright (C) 2023 CS416 Rutgers CS
 *	Tiny File System
 *	File:	rufs.c
 *
 */

#define FUSE_USE_VERSION 26
#define SUPERBLOCK 0
#define ROOT_DIRECTORY 0

#define FILE 0
#define DIRECTORY 1

#define INODE_PER_BLOCK (((sizeof(struct inode)) * (MAX_INUM)) / (BLOCK_SIZE))
#define DIRENT_PER_BLOCK ((sizeof(struct dirent)) / (BLOCK_SIZE))

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <libgen.h>
#include <limits.h>

#include "block.h"
#include "rufs.h"

char diskfile_path[PATH_MAX];

// Declare your in-memory data structures here

unsigned char block_buf[BLOCK_SIZE], i_bitmap_buf[BLOCK_SIZE], d_bitmap_buf[BLOCK_SIZE], superblock_buf[BLOCK_SIZE];

struct superblock *superblock_ptr = superblock_buf;

char cur_dir[] = ".", par_dir[] = "..";

/* 
 * Get available inode number from bitmap
 */
int get_avail_ino() {

	// Step 1: Read inode bitmap from disk
	bio_read(superblock_ptr->i_bitmap_blk, i_bitmap_buf);
	
	// Step 2: Traverse inode bitmap to find an available slot
	int inode = 0, found_flag = 0;
	for ( int i = 0; i < (MAX_INUM / 8); i++ ) {
		if ( i_bitmap_buf[i] == 255 ) inode += 8;
		else {
			unsigned char map = 1;
			for ( int j = 0; j < 8; j++ ) {
				if ( i_bitmap_buf[i] & map == 0 ) {
					found_flag = 1;
					break;
				} else {
					map <<= 1;
					inode++;
				}
			}
			if ( found_flag ) break;
		}
	}

	if ( ! found_flag ) return -1;

	// Step 3: Update inode bitmap and write to disk 

	set_bitmap(i_bitmap_buf, inode);

	bio_write(superblock_ptr->i_bitmap_blk, i_bitmap_buf);

	return inode;
}

/* 
 * Get available data block number from bitmap
 */
int get_avail_blkno() {

	// Step 1: Read inode bitmap from disk
	bio_read(superblock_ptr->d_bitmap_blk, d_bitmap_buf);
	
	// Step 2: Traverse inode bitmap to find an available slot
	int block = 0, found_flag = 0;
	for ( int i = 0; i < (MAX_INUM / 8); i++ ) {
		if ( d_bitmap_buf[i] == 255 ) block += 8;
		else {
			unsigned char map = 1;
			for ( int j = 0; j < 8; j++ ) {
				if ( d_bitmap_buf[i] & map == 0 ) {
					found_flag = 1;
					break;
				} else {
					map <<= 1;
					block++;
				}
			}
			if ( found_flag ) break;
		}
	}

	if ( ! found_flag ) return -1;

	// Step 3: Update inode bitmap and write to disk 

	set_bitmap(d_bitmap_buf, block);

	bio_write(superblock_ptr->d_bitmap_blk, d_bitmap_buf);

	return block;
}

/* 
 * inode operations
 */
int readi(uint16_t ino, struct inode *inode) {

	// Step 1: Get the inode's on-disk block number
	int block_num = (ino / INODE_PER_BLOCK) + superblock_ptr->i_start_blk; 

	// Step 2: Get offset of the inode in the inode on-disk block
	int offset = ino % INODE_PER_BLOCK;

	// Step 3: Read the block from disk and then copy into inode structure
	bio_read(block_num, block_buf);

	struct inode *inode_ptr = block_buf;
	inode_ptr += offset;
	*(inode) = *(inode_ptr);

	return 0;
}

int writei(uint16_t ino, struct inode *inode) {

	// Step 1: Get the block number where this inode resides on disk
	int block_num = (ino / INODE_PER_BLOCK) + superblock_ptr->i_start_blk; 
	
	// Step 2: Get the offset in the block where this inode resides on disk
	int offset = ino % INODE_PER_BLOCK;

	// Step 3: Write inode to disk 
	bio_read(block_num, block_buf);

	struct inode *inode_ptr = block_buf;
	inode_ptr += offset;
	*(inode_ptr) = *(inode);

	bio_write(block_num, block_buf);

	return 0;
}


/* 
 * directory operations
 */
int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {

	// Step 1: Call readi() to get the inode using ino (inode number of current directory)
	struct inode dir_inode;
	readi(ino, &dir_inode);

  // Step 2: Get data block of current directory from inode
	for ( int i = 0; i < dir_inode.size; i++ ) {
		bio_read(dir_inode.direct_ptr[i], block_buf);
		struct dirent *dirent_ptr = block_buf;
		for ( int j = 0; j < DIRENT_PER_BLOCK; j++ ) {
			if ( ! (dirent_ptr->valid && (dirent_ptr->len == name_len))) continue;
			else if ( memcmp(fname, dirent_ptr->name, name_len) == 0 ) {
				*(dirent) = *(dirent_ptr);
				return 0;
			}
		}

	}

  // Step 3: Read directory's data block and check each directory entry.
  //If the name matches, then copy directory entry to dirent structure

	return -1;
}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and check each directory entry of dir_inode
	char invalid_flag = 0;

	for ( int i = 0; i < dir_inode.size; i++ ) {
		bio_read(dir_inode.direct_ptr[i], block_buf);
		struct dirent *dirent_ptr = block_buf;
		for ( int j = 0; j < DIRENT_PER_BLOCK; j++ ) {
			if ( ! dirent_ptr->valid ) {
				invalid_flag = 1;
				continue;
			}

			if ( dirent_ptr->len != name_len ) continue;

			if ( memcmp(fname, dirent_ptr->name, name_len) == 0 ) return -1;
			else continue;
		}
	}
	// Step 3: Add directory entry in dir_inode's data block and write to disk

	if ( invalid_flag ) {

		for ( int i = 0; i < dir_inode.size; i++ ) {
			bio_read(dir_inode.direct_ptr[i], block_buf);
			struct dirent *dirent_ptr = block_buf;
			for ( int j = 0; j < DIRENT_PER_BLOCK; j++ ) {
				if ( dirent_ptr[j].valid ) continue;
				else {
					dirent_ptr[j].ino = f_ino;
					memcpy(dirent_ptr[j].name, fname, name_len);
					dirent_ptr[j].valid = 1;
					bio_write(dir_inode.direct_ptr[i], block_buf);
					return 0;
				}
			}
		}

	} else {

		int new_block = get_avail_blkno();
		bio_read(new_block, block_buf);
		struct dirent *dirent_ptr = block_buf;

		for ( int i = 0; i < DIRENT_PER_BLOCK; i++ ) dirent_ptr[i].valid = 0;

		dirent_ptr->ino = f_ino;
		memcpy(dirent_ptr->name, fname, name_len);
		dirent_ptr->valid = 1;

		bio_write(new_block, block_buf);

		dir_inode.direct_ptr[dir_inode.size++] = new_block;
		writei(dir_inode.ino, &dir_inode);

		return 0;		

	}

	// Allocate a new data block for this directory if it does not exist

	// Update directory inode

	// Write directory entry

	return 0;
}

int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {

	// Step 1: Read dir_inode's data block and checks each directory entry of dir_inode
	for ( int i = 0; i < dir_inode.size; i++ ) {
		bio_read(dir_inode.direct_ptr[i], block_buf);
		struct dirent *dirent_ptr = block_buf;
		for ( int j = 0; j < DIRENT_PER_BLOCK; j++ ) {
			if ( ! ( dirent_ptr->valid && dirent_ptr->len == name_len ) ) continue;
			if ( memcmp(fname, dirent_ptr->name, name_len) == 0 ) {
				dirent_ptr->valid = 0;
				bio_write(dir_inode.direct_ptr[i], block_buf);
				return 0;
			}
			else continue;
		}
	}
	
	// Step 2: Check if fname exist

	// Step 3: If exist, then remove it from dir_inode's data block and write to disk

	return -1;
}

/* 
 * namei operation
 */
int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {
	
	// Step 1: Resolve the path name, walk through path, and finally, find its inode.
	// Note: You could either implement it in a iterative way or recursive way

	struct inode dir_inode;
	readi(ino, &dir_inode);

	char *s1, *s2, *subdir_name;
	int s1_length, s2_length, subdir_length;

	s1 = strchr(path, '/');
	if ( s1 ==  NULL ) return -1;
	s1++;
	s1_length = strlen(s1);
	if ( s1_length == 0 ) {
		*(inode) = dir_inode;
		return 0;
	}

	s2 = strchr(s1, '/');
	if ( s2 == NULL ) {
		
		struct dirent dirent;
		if ( dir_find(ino, s1, s1_length, &dirent) == -1 ) return -1;
		readi(dirent.ino, inode);

	} else {
		
		s2_length = strlen(s2);

		subdir_length = s1_length - s2_length;
		if ( subdir_length == 0 ) return -1;

		char subdir_name[subdir_length];
		memcpy(subdir_name, s1, subdir_length);

		struct dirent dirent;
		if ( dir_find(ino, subdir_name, subdir_length, &dirent) == -1 ) return -1;
		return get_node_by_path(s1, dirent.ino, inode);

	}

	return 0;
}

/* 
 * Make file system
 */

int rufs_mkfs() {

	// Call dev_init() to initialize (Create) Diskfile
	dev_init(diskfile_path);

	// write superblock information
	superblock_ptr->magic_num = MAGIC_NUM;
	superblock_ptr->max_inum = MAX_INUM;
	superblock_ptr->max_dnum = MAX_DNUM;

	int blocks_for_inode_bitmap = ((MAX_INUM / 8) + BLOCK_SIZE - 1) / BLOCK_SIZE;
	int blocks_for_dblock_bitmap = ((MAX_DNUM / 8) + BLOCK_SIZE - 1) / BLOCK_SIZE;
	int blocks_for_inodes = (sizeof(struct inode) * MAX_INUM) / BLOCK_SIZE;

	superblock_ptr->i_bitmap_blk = 1;
	superblock_ptr->d_bitmap_blk = superblock_ptr->i_bitmap_blk + blocks_for_inode_bitmap;
	superblock_ptr->i_start_blk = superblock_ptr->d_bitmap_blk + blocks_for_dblock_bitmap;
	superblock_ptr->d_start_blk = superblock_ptr->i_start_blk + blocks_for_inodes;

	bio_write(SUPERBLOCK, superblock_buf);

	// initialize inode bitmap
	memset(i_bitmap_buf, 0, BLOCK_SIZE);
	
	// initialize data block bitmap
	memset(d_bitmap_buf, 0, BLOCK_SIZE);

	// update bitmap information for root directory
	bio_write(ROOT_DIRECTORY, d_bitmap_buf);

	// update inode for root directory
	bio_write(ROOT_DIRECTORY, i_bitmap_buf);

	return 0;
}


/* 
 * FUSE file operations
 */
static void *rufs_init(struct fuse_conn_info *conn) {

	// Step 1a: If disk file is not found, call mkfs

	if ( dev_open(diskfile_path) == -1 ) rufs_mkfs();

	// Step 1b: If disk file is found, just initialize in-memory data structures
	// and read superblock from disk

	bio_read(SUPERBLOCK, superblock_buf);
	bio_read(superblock->i_bitmap_blk, i_bitmap_buf);
	bio_read(superblock->d_bitmap_blk, d_bitmap_buf);

	return NULL;
}

static void rufs_destroy(void *userdata) {

	// Step 1: De-allocate in-memory data structures

	// Step 2: Close diskfile

}

static int rufs_getattr(const char *path, struct stat *stbuf) {
	struct inode *temp;
	// Step 1: call get_node_by_path() to get inode from path
	if(get_node_by_path(path, ROOT_DIRECTORY, &temp) == 0) {
		stbuf->st_ino;
		stbuf->st_size;
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_mode;
		stbuf->st_nlink = 2;
		time(&stbuf->st_mtime);
		temp->vstat = stbuf;
	} else return -1;
	// Step 2: fill attribute of file into stbuf from inode
	return 0;
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {


	struct inode temp;
	// Step 1: Call get_node_by_path() to get inode from path
	if ( get_node_by_path(path, ROOT_DIRECTORY, &temp) == 0 ) return 0;
	else return -1;

	// Step 2: If not find, return -1

    return 0;
}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path
	struct inode inode;
	get_node_by_path(path, ROOT_DIRECTORY, &inode);
	if ( inode.type != DIRECTORY ) return -1;

	// Step 2: Read directory entries from its data blocks, and copy them to filler
	for ( int i = 0; i < inode.size; i++ ) {
		bio_read(inode.direct_ptr[i], block_buf);
		struct dirent *dirent_ptr = block_buf;
		for ( int j = 0; j < DIRENT_PER_BLOCK; j++ ) {
			if ( ! dirent_ptr->valid ) continue;
			else filler(buffer, dirent_ptr->name, NULL, 0);
		}
	}

	return 0;
}


static int rufs_mkdir(const char *path, mode_t mode) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name
	char directory_path[] = dirname(path);
	char directory_name[] = basename(path);

	// Step 2: Call get_node_by_path() to get inode of parent directory
	struct inode parent_inode;
	get_node_by_path(directory_path, ROOT_DIRECTORY, &parent_inode);

	// Step 3: Call get_avail_ino() to get an available inode number
	ino_t inode = get_avail_ino();

	// Step 4: Call dir_add() to add directory entry of target directory to parent directory
	dir_add(parent_inode, inode, directory_name, strlen(directory_name));

	// Step 5: Update inode for target directory
	struct inode base_inode;
	readi(inode, &base_inode);

	base_inode.ino = inode;
	
	int init_block = get_avail_blkno();
	bio_read(init_block, block_buf);
	struct dirent *dirent_ptr = block_buf;

	dirent_ptr->ino = inode;
	dirent_ptr->valid = 1;
	memcpy(dirent_ptr->name, cur_dir, 2);
	dirent_ptr->len = 1;
	dirent_ptr++;

	dirent_ptr->ino = parent_inode.ino;
	dirent_ptr->valid = 1;
	memcpy(dirent_ptr->name, par_dir, 3);
	dirent_ptr->len = 2;

	bio_write(init_block, block_buf);

	base_inode.direct_ptr[0] = init_block;
	base_inode.size = 1;
	base_inode.type = DIRECTORY;
	base_inode.valid = 1;
	base_inode.link++;

	// Step 6: Call writei() to write inode to disk
	writei(inode, &base_inode);

	return 0;
}

static int rufs_rmdir(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target directory name

	// Step 2: Call get_node_by_path() to get inode of target directory
	struct inode target_inode;
	get_node_by_path(path, ROOT_DIRECTORY, &target_inode);

	// Step 3: Clear data block bitmap of target directory
	for ( int i = 0; i < parent_inode.size; i++ ) {

	}

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target directory in its parent directory

	return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of parent directory

	// Step 3: Call get_avail_ino() to get an available inode number

	// Step 4: Call dir_add() to add directory entry of target file to parent directory

	// Step 5: Update inode for target file

	// Step 6: Call writei() to write inode to disk

	return 0; 
}

static int rufs_open(const char *path, struct fuse_file_info *fi) {

	// Step 1: Call get_node_by_path() to get inode from path

	// Step 2: If not find, return -1

	return 0;
}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: copy the correct amount of data from offset to buffer

	// Note: this function should return the amount of bytes you copied to buffer
	return 0;
}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
	// Step 1: You could call get_node_by_path() to get inode from path

	// Step 2: Based on size and offset, read its data blocks from disk

	// Step 3: Write the correct amount of data from offset to disk

	// Step 4: Update the inode info and write it to disk

	// Note: this function should return the amount of bytes you write to disk
	return size;
}

static int rufs_unlink(const char *path) {

	// Step 1: Use dirname() and basename() to separate parent directory path and target file name

	// Step 2: Call get_node_by_path() to get inode of target file

	// Step 3: Clear data block bitmap of target file

	// Step 4: Clear inode bitmap and its data block

	// Step 5: Call get_node_by_path() to get inode of parent directory

	// Step 6: Call dir_remove() to remove directory entry of target file in its parent directory

	return 0;
}

static int rufs_truncate(const char *path, off_t size) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_release(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_flush(const char * path, struct fuse_file_info * fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_utimens(const char *path, const struct timespec tv[2]) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}


static struct fuse_operations rufs_ope = {
	.init		= rufs_init,
	.destroy	= rufs_destroy,

	.getattr	= rufs_getattr,
	.readdir	= rufs_readdir,
	.opendir	= rufs_opendir,
	.releasedir	= rufs_releasedir,
	.mkdir		= rufs_mkdir,
	.rmdir		= rufs_rmdir,

	.create		= rufs_create,
	.open		= rufs_open,
	.read 		= rufs_read,
	.write		= rufs_write,
	.unlink		= rufs_unlink,

	.truncate   = rufs_truncate,
	.flush      = rufs_flush,
	.utimens    = rufs_utimens,
	.release	= rufs_release
};


int main(int argc, char *argv[]) {
	int fuse_stat;

	getcwd(diskfile_path, PATH_MAX);
	strcat(diskfile_path, "/DISKFILE");

	fuse_stat = fuse_main(argc, argv, &rufs_ope, NULL);

	return fuse_stat;
}


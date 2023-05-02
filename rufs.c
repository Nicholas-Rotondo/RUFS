/*
 *  Copyright (C) 2023 CS416 Rutgers CS
 *	Tiny File System
 *	File:	rufs.c
 *
 */

#define FUSE_USE_VERSION 26
#define SUPERBLOCK_BLKNO 0
#define ROOT_DIRECTORY_INO 0

#define IS_FILE 0
#define IS_DIRECTORY 1

#define INVALID 0
#define VALID 1

#define INODE_PER_BLOCK ((BLOCK_SIZE) / (sizeof(struct inode)))
#define DIRENT_PER_BLOCK ((BLOCK_SIZE) / (sizeof(struct dirent)))

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <libgen.h>
#include <limits.h>

#include "block.h"
#include "rufs.h"

char diskfile_path[PATH_MAX];

unsigned char block_buf[BLOCK_SIZE], i_bitmap_buf[BLOCK_SIZE], d_bitmap_buf[BLOCK_SIZE], superblock_buf[BLOCK_SIZE];

struct superblock *superblock_ptr = (struct superblock *) superblock_buf;

char cur_dir[] = ".", par_dir[] = "..";

int get_avail_ino() {

	int ino = 0, found_flag = 0;

	for ( int i = 0; i < (MAX_INUM / 8); i++ ) {

		if ( i_bitmap_buf[i] == 255 ) ino += 8;

		else {

			unsigned char map = 1;

			for ( int j = 0; j < 8; j++ ) {

				if ( i_bitmap_buf[i] & map ) {
					map <<= 1;
					ino++;
				} else {
					found_flag = 1;
					break;
				}

			}

			if ( found_flag ) break;

		}

	}

	if ( ! found_flag ) return -1;

	set_bitmap(i_bitmap_buf, ino);

	bio_write(superblock_ptr->i_bitmap_blk, i_bitmap_buf);

	return ino;

}

int get_avail_blkno() {

	int blkno = 0, found_flag = 0;

	for ( int i = 0; i < (MAX_INUM / 8); i++ ) {

		if ( d_bitmap_buf[i] == 255 ) blkno += 8;

		else {

			unsigned char map = 1;

			for ( int j = 0; j < 8; j++ ) {

				if ( d_bitmap_buf[i] & map ) {
					map <<= 1;
					blkno++;
				} else {
					found_flag = 1;
					break;
				}

			}

			if ( found_flag ) break;

		}

	}

	if ( ! found_flag ) return -1;

	set_bitmap(d_bitmap_buf, blkno);

	bio_write(superblock_ptr->d_bitmap_blk, d_bitmap_buf);

	return blkno + superblock_ptr->d_start_blk;
}

int readi(uint16_t ino, struct inode *inode) {

	int blkno = (ino / INODE_PER_BLOCK) + superblock_ptr->i_start_blk; 
	int offset = ino % INODE_PER_BLOCK;

	bio_read(blkno, block_buf);

	struct inode *inode_ptr = (struct inode *) block_buf;
	inode_ptr += offset;

	*(inode) = *(inode_ptr);

	return 0;
}

int writei(uint16_t ino, struct inode *inode) {

	int blkno = (ino / INODE_PER_BLOCK) + superblock_ptr->i_start_blk; 
	int offset = ino % INODE_PER_BLOCK;

	bio_read(blkno, block_buf);

	struct inode *inode_ptr = (struct inode *) block_buf;
	inode_ptr += offset;

	*(inode_ptr) = *(inode);

	bio_write(blkno, block_buf);

	return 0;
}

int dir_find(uint16_t ino, const char *fname, size_t name_len, struct dirent *dirent) {

	struct inode dir_ino;
	readi(ino, &dir_ino);

	if ( dir_ino.type != IS_DIRECTORY ) return -ENOTDIR;

	for ( int i = 0; i < dir_ino.size; i++ ) {

		bio_read(dir_ino.direct_ptr[i], block_buf);

		struct dirent *dirent_ptr = (struct dirent *) block_buf;

		for ( int j = 0; j < DIRENT_PER_BLOCK; j++ ) {

			if ( (dirent_ptr[j].valid) && (dirent_ptr[j].len == name_len) && (memcmp(fname, dirent_ptr[j].name, name_len) == 0) ) {
				*(dirent) = dirent_ptr[j];
				return 0;
			}

		}

	}

	return -ENOENT;

}

int dir_add(struct inode dir_inode, uint16_t f_ino, const char *fname, size_t name_len) {

	char invalid_flag = 0;

	for ( int i = 0; i < dir_inode.size; i++ ) {

		bio_read(dir_inode.direct_ptr[i], block_buf);

		struct dirent *dirent_ptr = (struct dirent *) block_buf;

		for ( int j = 0; j < DIRENT_PER_BLOCK; j++ ) {
			
			if ( ! dirent_ptr[j].valid ) {
				invalid_flag = 1;
			} else if ( (dirent_ptr[j].len == name_len) && (memcmp(fname, dirent_ptr[j].name, name_len) == 0) ) return -EEXIST;

		}

	}

	if ( invalid_flag ) {

		for ( int i = 0; i < dir_inode.size; i++ ) {

			bio_read(dir_inode.direct_ptr[i], block_buf);

			struct dirent *dirent_ptr = (struct dirent *) block_buf;

			for ( int j = 0; j < DIRENT_PER_BLOCK; j++ ) {

				if ( ! dirent_ptr[j].valid ) {

					dirent_ptr[j].valid = VALID;
					dirent_ptr[j].ino = f_ino;
					dirent_ptr[j].len = name_len;
					memcpy(dirent_ptr[j].name, fname, name_len);
					bio_write(dir_inode.direct_ptr[i], block_buf);

					dir_inode.vstat.st_size += sizeof(struct dirent);
					dir_inode.vstat.st_atime = time(NULL);
					dir_inode.vstat.st_mtime = time(NULL);
					writei(dir_inode.ino, &dir_inode);

					return 0;

				}

			}

		}

	} else {

		if ( dir_inode.size == 16 ) return -EFBIG;

		int blkno = get_avail_blkno();
		if ( blkno == -1 ) return -ENOMEM;

		bio_read(blkno, block_buf);

		struct dirent *dirent_ptr = (struct dirent *) block_buf;

		for ( int i = 0; i < DIRENT_PER_BLOCK; i++ ) dirent_ptr[i].valid = INVALID;

		dirent_ptr->valid = VALID;
		dirent_ptr->ino = f_ino;
		dirent_ptr->len = name_len;
		memcpy(dirent_ptr->name, fname, name_len);

		bio_write(blkno, block_buf);

		dir_inode.direct_ptr[dir_inode.size++] = blkno;
		dir_inode.vstat.st_size += sizeof(struct dirent);
		dir_inode.vstat.st_blksize++;
		dir_inode.vstat.st_blocks++;
		dir_inode.vstat.st_atime = time(NULL);
		dir_inode.vstat.st_mtime = time(NULL);

		writei(dir_inode.ino, &dir_inode);

		return 0;

	}

	return -ENOMEM;
}

int dir_remove(struct inode dir_inode, const char *fname, size_t name_len) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

int get_node_by_path(const char *path, uint16_t ino, struct inode *inode) {

	struct inode dir_inode;
	readi(ino, &dir_inode);

	char *s1 = strchr(path, '/');
	if ( s1 == NULL ) return -ENOENT;
	s1++;
	int s1_length = strlen(s1);
	if ( s1_length == 0 ) {
		*(inode) = dir_inode;
		return 0;
	}


	char *s2 = strchr(s1, '/');
	if ( s2 == NULL ) {
		
		struct dirent dirent;
		int retval = dir_find(ino, s1, s1_length, &dirent);
		if ( retval != 0 ) return retval;

		readi(dirent.ino, inode);

	} else {

		int s2_length = strlen(s2);

		int subdir_length = s1_length - s2_length;
		if ( subdir_length == 0 ) return -ENOENT;

		char subdir_name[subdir_length];
		memcpy(subdir_name, s1, subdir_length);

		struct dirent dirent;
		int retval = dir_find(ino, subdir_name, subdir_length, &dirent);
		if ( retval != 0 ) return retval;

		return get_node_by_path(s2, dirent.ino, inode);

	}

	return 0;
}

int rufs_mkfs() {

	fprintf(stderr, "rufs_mkfs() called\n");

	dev_init(diskfile_path);

	memset(superblock_buf, 0, BLOCK_SIZE);

	superblock_ptr->magic_num = MAGIC_NUM;
	superblock_ptr->max_inum = MAX_INUM;
	superblock_ptr->max_dnum = MAX_DNUM;

	int blocks_for_i_bitmap = ((MAX_INUM / 8) + BLOCK_SIZE - 1) / BLOCK_SIZE;
	int blocks_for_d_bitmap = ((MAX_DNUM / 8) + BLOCK_SIZE - 1) / BLOCK_SIZE;
	int blocks_for_inodes = (sizeof(struct inode) * MAX_INUM) / BLOCK_SIZE;

	superblock_ptr->i_bitmap_blk = 1;
	superblock_ptr->d_bitmap_blk = superblock_ptr->i_bitmap_blk + blocks_for_i_bitmap;
	superblock_ptr->i_start_blk = superblock_ptr->d_bitmap_blk + blocks_for_d_bitmap;
	superblock_ptr->d_start_blk = superblock_ptr->i_start_blk + blocks_for_inodes;

	bio_write(SUPERBLOCK_BLKNO, superblock_buf);

	memset(i_bitmap_buf, 0, BLOCK_SIZE);
	memset(d_bitmap_buf, 0, BLOCK_SIZE);

	for ( int i = 0; i < blocks_for_inodes; i++ ) bio_write(i + superblock_ptr->i_start_blk, i_bitmap_buf);

	struct inode root_ino;
	root_ino.ino = get_avail_ino();
	if ( root_ino.ino == -1 ) return -ENOMEM;

	root_ino.type = IS_DIRECTORY;
	root_ino.valid = VALID;

	int dir_blkno = get_avail_blkno();
	if ( dir_blkno == -1 ) return -ENOMEM;

	bio_read(dir_blkno, block_buf);
	struct dirent *dirent_ptr = (struct dirent *) block_buf;

	dirent_ptr->ino = root_ino.ino;
	dirent_ptr->valid = VALID;
	memcpy(dirent_ptr->name, cur_dir, 1);
	dirent_ptr->len = 1;
	dirent_ptr++;

	dirent_ptr->ino = root_ino.ino;
	dirent_ptr->valid = VALID;
	memcpy(dirent_ptr->name, par_dir, 2);
	dirent_ptr->len = 2;

	bio_write(dir_blkno, block_buf);

	root_ino.direct_ptr[0] = dir_blkno;
	root_ino.size = 1;

	root_ino.vstat.st_ctime = time(NULL);
	root_ino.vstat.st_atime = time(NULL);
	root_ino.vstat.st_mtime = time(NULL);
	root_ino.vstat.st_uid = getuid();
	root_ino.vstat.st_gid = getgid();
	root_ino.vstat.st_blksize = root_ino.size;
	root_ino.vstat.st_blocks = root_ino.size;
	root_ino.vstat.st_ino = root_ino.ino;
	root_ino.vstat.st_mode = __S_IFDIR | 0755;
	root_ino.vstat.st_nlink = 2;
	root_ino.vstat.st_size = sizeof(struct dirent) * 2;

	writei(root_ino.ino, &root_ino);

	bio_write(superblock_ptr->i_bitmap_blk, i_bitmap_buf);
	bio_write(superblock_ptr->d_bitmap_blk, d_bitmap_buf);
	
	return 0;
}

static void *rufs_init(struct fuse_conn_info *conn) {

	fprintf(stderr, "rufs_init() called\n");

	if ( dev_open(diskfile_path) == -1 ) rufs_mkfs();

	bio_read(SUPERBLOCK_BLKNO, superblock_buf);
	bio_read(superblock_ptr->i_bitmap_blk, i_bitmap_buf);
	bio_read(superblock_ptr->d_bitmap_blk, d_bitmap_buf);

	return NULL;
}

static void rufs_destroy(void *userdata) {

	dev_close();

}

static int rufs_getattr(const char *path, struct stat *stbuf) {

	struct inode temp;
	int retval = get_node_by_path(path, ROOT_DIRECTORY_INO, &temp);
	if ( retval == 0 ) {
		*(stbuf) = temp.vstat;
		return 0;
	} else return retval;
	
}

static int rufs_opendir(const char *path, struct fuse_file_info *fi) {

	fprintf(stderr, "rufs_opendir() called\n");

	struct inode ino;
	int retval = get_node_by_path(path, ROOT_DIRECTORY_INO, &ino);
	if ( retval != 0 ) return retval;
	else return 0;

}

static int rufs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	struct inode inode;
	int retval = get_node_by_path(path, ROOT_DIRECTORY_INO, &inode);
	if ( retval != 0 ) return retval;
	if ( inode.type != IS_DIRECTORY ) return -ENOTDIR;

	for ( int i = 0; i < inode.size; i++ ) {

		bio_read(inode.direct_ptr[i], block_buf);

		struct dirent *dirent_ptr = (struct dirent *) block_buf;

		for ( int j = 0; j < DIRENT_PER_BLOCK; j++ ) {

			if ( dirent_ptr[j].valid ) filler(buffer, dirent_ptr[j].name, NULL, 0);

		}

	}

	inode.vstat.st_atime = time(NULL);

	writei(inode.ino, &inode);

	return 0;
}


static int rufs_mkdir(const char *path, mode_t mode) {

	fprintf(stderr, "rufs_mkdir() called\n");

	char path_cpy1[strlen(path)], path_cpy2[strlen(path)];
	strcpy(path_cpy1, path);
	strcpy(path_cpy2, path);

	char *directory_path = dirname(path_cpy1);
	char *directory_name = basename(path_cpy2);

	struct inode parent_inode;
	int retval = get_node_by_path(directory_path, ROOT_DIRECTORY_INO, &parent_inode);
	if ( retval != 0 ) return retval;
	if ( parent_inode.type != IS_DIRECTORY ) return -ENOTDIR;

	ino_t inode = get_avail_ino();
	if ( inode == -1 ) return -ENOMEM;

	retval = dir_add(parent_inode, inode, directory_name, strlen(directory_name));
	if ( retval != 0 ) return retval;
	
	struct inode base_inode;
	readi(inode, &base_inode);

	base_inode.ino = inode;
	
	int blkno = get_avail_blkno();
	if ( blkno == -1 ) return -ENOMEM;

	bio_read(blkno, block_buf);

	struct dirent *dirent_ptr = (struct dirent *) block_buf;

	dirent_ptr->ino = inode;
	dirent_ptr->valid = VALID;
	memcpy(dirent_ptr->name, cur_dir, 1);
	dirent_ptr->len = 1;
	dirent_ptr++;

	dirent_ptr->ino = parent_inode.ino;
	dirent_ptr->valid = VALID;
	memcpy(dirent_ptr->name, par_dir, 2);
	dirent_ptr->len = 2;

	bio_write(blkno, block_buf);

	base_inode.direct_ptr[0] = blkno;
	base_inode.size = 1;
	base_inode.type = IS_DIRECTORY;
	base_inode.valid = VALID;
	base_inode.link = 2;

	base_inode.vstat.st_atime = time(NULL);
	base_inode.vstat.st_ctime = time(NULL);
	base_inode.vstat.st_mtime = time(NULL);
	base_inode.vstat.st_uid = getuid();
	base_inode.vstat.st_gid = getgid();
	base_inode.vstat.st_blksize = base_inode.size;
	base_inode.vstat.st_blocks = base_inode.size;
	base_inode.vstat.st_ino = base_inode.ino;
	base_inode.vstat.st_mode = __S_IFDIR | mode;
	base_inode.vstat.st_nlink = 2;
	base_inode.vstat.st_size = sizeof(struct dirent) * 2;

	writei(inode, &base_inode);

	return 0;
}

static int rufs_rmdir(const char *path) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
	return 0;
}

static int rufs_releasedir(const char *path, struct fuse_file_info *fi) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
    return 0;
}

static int rufs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {

	char path_cpy1[strlen(path)], path_cpy2[strlen(path)];
	strcpy(path_cpy1, path);
	strcpy(path_cpy2, path);

	char *directory_path = dirname(path_cpy1);
	char *file_name = basename(path_cpy2);

	struct inode par_inode; 
	int retval = get_node_by_path(directory_path, ROOT_DIRECTORY_INO, &par_inode);
	if ( retval != 0 ) return retval;

	ino_t ino_num = get_avail_ino();
	if ( ino_num == -1 ) return -ENOMEM;

	retval = dir_add(par_inode, ino_num, file_name, strlen(file_name));
	if ( retval != 0 ) return retval;

	struct inode file_inode;

	file_inode.ino = ino_num;
	file_inode.type = IS_FILE;
	file_inode.valid = VALID;
	file_inode.link = 1;
	file_inode.size = 0;

	file_inode.vstat.st_atime = time(NULL);
	file_inode.vstat.st_ctime = time(NULL);
	file_inode.vstat.st_mtime = time(NULL);
	file_inode.vstat.st_uid = getuid();
	file_inode.vstat.st_gid = getgid();
	file_inode.vstat.st_blksize = file_inode.size;
	file_inode.vstat.st_blocks = file_inode.size;
	file_inode.vstat.st_ino = file_inode.ino;
	file_inode.vstat.st_mode = __S_IFREG | mode;
	file_inode.vstat.st_nlink = 1;
	file_inode.vstat.st_size = 0;
	
	writei(ino_num, &file_inode);

	return 0; 
}

static int rufs_open(const char *path, struct fuse_file_info *fi) {

	fprintf(stderr, "rufs_open() called\n");

	struct inode open_inode;
	int retval = get_node_by_path(path, ROOT_DIRECTORY_INO, &open_inode);
	return retval;

}

static int rufs_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	fprintf(stderr, "rufs_read() called\n");

	struct inode read_inode;
	int retval = get_node_by_path(path, ROOT_DIRECTORY_INO, &read_inode);
	if ( retval != 0 ) return retval;
	if ( read_inode.type != IS_FILE ) return -EISDIR;

	int blocks_to_read = (size + offset + BLOCK_SIZE - 1) / BLOCK_SIZE;
	if ( blocks_to_read > read_inode.size ) return -1;

	int curr_block = offset / BLOCK_SIZE;
	int bytes_read = 0;

	while ( bytes_read < size ) {

		int bytes_left_to_read = size - bytes_read;
		int bytes_left_in_block = BLOCK_SIZE - (offset % BLOCK_SIZE);
		int bytes_to_read_from_block = (bytes_left_to_read > bytes_left_in_block) ? bytes_left_in_block : bytes_left_to_read;
		
		bio_read(read_inode.direct_ptr[curr_block], block_buf);

		void *read_ptr = block_buf + (offset % BLOCK_SIZE);
		memcpy(read_ptr, buffer, bytes_to_read_from_block);
		
		bytes_read += bytes_to_read_from_block;
		buffer += bytes_to_read_from_block;		
		offset = 0;
		curr_block++;

	}

	read_inode.vstat.st_atime = time(NULL);
	writei(read_inode.ino, &read_inode);

	return size;

}

static int rufs_write(const char *path, const char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {

	fprintf(stderr, "rufs_write() called\n");

	struct inode inode;
	int retval = get_node_by_path(path, ROOT_DIRECTORY_INO, &inode);
	if ( retval != 0 ) return retval;
	if ( inode.type != IS_FILE ) return -EISDIR;

	int blocks_for_write = (size + offset + BLOCK_SIZE - 1) / BLOCK_SIZE;
	if ( blocks_for_write > 16 ) return -EFBIG;

	int blocks_to_allocate = blocks_for_write - inode.size;

	int curr_block = offset / BLOCK_SIZE;
	int bytes_written = 0;
	
	while ( bytes_written < size ) {

		int bytes_left_to_write = size - bytes_written;
		int bytes_left_in_block = BLOCK_SIZE - (offset % BLOCK_SIZE);
		int bytes_to_write_from_block = (bytes_left_to_write > bytes_left_in_block) ? bytes_left_in_block : bytes_left_to_write;

		if ( curr_block == inode.size ) {

			int allocated_block = get_avail_blkno();
			bio_read(allocated_block, block_buf);

			void *write_ptr = block_buf;
			write_ptr += (offset % BLOCK_SIZE);

			memcpy(write_ptr, buffer, bytes_to_write_from_block);

			bio_write(allocated_block, block_buf);

			inode.direct_ptr[inode.size++] = allocated_block;

		} else {

			bio_read(inode.direct_ptr[curr_block], block_buf);

			void *write_ptr = block_buf;
			write_ptr += (offset % BLOCK_SIZE);

			memcpy(write_ptr, buffer, bytes_to_write_from_block);

			bio_write(inode.direct_ptr[curr_block], block_buf);

		}

		bytes_written += bytes_to_write_from_block;
		buffer += bytes_to_write_from_block;
		offset = 0;
		curr_block++;

	}

	inode.vstat.st_atime = time(NULL);
	inode.vstat.st_mtime = time(NULL);
	inode.vstat.st_size += size;
	inode.vstat.st_blksize += blocks_to_allocate;
	inode.vstat.st_blocks += blocks_to_allocate;

	writei(inode.ino, &inode);

	return size;
}

static int rufs_unlink(const char *path) {
	// For this project, you don't need to fill this function
	// But DO NOT DELETE IT!
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


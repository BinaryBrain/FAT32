	// vim: noet:ts=8:sts=8
#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <sys/mman.h>
#include <assert.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vfat.h"

#define MAX_LONGNAME_LENGTH 512

// A kitchen sink for all important data about filesystem
struct vfat_data {
	const char	*dev;
	int		fs;
	struct	fat_boot fb;
	uint32_t fats_offset;
	uint32_t clusters_offset;
	uint32_t cluster_size;
};

struct vfat_data vfat_info;
iconv_t iconv_utf16;

uid_t mount_uid;
gid_t mount_gid;
time_t mount_time;

// Used by vfat_search_entry()
struct vfat_search_data {
	const char	*name;
	int		found;
	off_t first_cluster;
	struct stat	*st;
};

static void vfat_init(const char *dev)
{
	printf("Mounting filesystem...\n");
	iconv_utf16 = iconv_open("utf-8", "utf-16"); // from utf-16 to utf-8
	// These are useful so that we can setup correct permissions in the mounted directories
	mount_uid = getuid();
	mount_gid = getgid();

	// Use mount time as mtime and ctime for the filesystem root entry (e.g. "/")
	mount_time = time(NULL);

	vfat_info.fs = open(dev, O_RDONLY);
	if (vfat_info.fs < 0) {
		err(1, "open(%s)", dev);
	}
	
	read(vfat_info.fs, &(vfat_info.fb), 91);
	
	if(!isFAT32(vfat_info.fb)) {
		err(404, "%s is not a FAT32 system\n", dev);
	}
	
	// Helpers
	vfat_info.fats_offset = vfat_info.fb.reserved_sectors * vfat_info.fb.bytes_per_sector;
	vfat_info.clusters_offset = vfat_info.fats_offset + (vfat_info.fb.fat32.sectors_per_fat * vfat_info.fb.bytes_per_sector * vfat_info.fb.fat_count);
	vfat_info.cluster_size = vfat_info.fb.sectors_per_cluster * vfat_info.fb.bytes_per_sector;
	
	struct vfat_search_data sd;
	sd.name = "sd.name";
	sd.found = 0;
	sd.first_cluster = 0;
	
	struct stat st;
}

bool isFAT32(struct fat_boot fb) {
	int root_dir_sectors = (fb.root_max_entries*32 + (fb.bytes_per_sector - 1)) / fb.bytes_per_sector;
	
	if(root_dir_sectors != 0) {
		return false;
	}
	
	uint32_t FATSz;
	uint32_t TotSec;
	uint32_t DataSec;
	uint32_t CountofClusters;
	
	if(fb.sectors_per_fat_small != 0) {
		FATSz = fb.sectors_per_fat_small;
	} else {
		FATSz = fb.fat32.sectors_per_fat;
	}
	
	if(fb.total_sectors_small != 0) {
		TotSec = fb.total_sectors_small;
	} else {
		TotSec = fb.total_sectors;
	}
	
	DataSec = TotSec - (fb.reserved_sectors + (fb.fat_count * FATSz) + root_dir_sectors);
	
	CountofClusters = DataSec / fb.sectors_per_cluster;
	
	return CountofClusters >= 65525;
}

static int vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t filler, void *fillerdata, bool searching)
{
	struct stat st; // we can reuse same stat entry over and over again
	void *buf = NULL;
	struct vfat_direntry *e;
	char *name;
	
	memset(&st, 0, sizeof(st));
	st.st_uid = mount_uid;
	st.st_gid = mount_gid;
	st.st_nlink = 1;
	
	// Goes through the directory table and calls the filler function on the
	// filler data for each entry (usually the filler is vfat_search_entry)
	
	u_int32_t entry_per_cluster = vfat_info.cluster_size/32;
	
	//struct fat32_direntry dir_entry;
	u_int8_t buffer[32];
	u_int32_t cur_cluster = first_cluster;
	bool cont = true;
	while(cont){
		u_int32_t offset = (cur_cluster-2) * vfat_info.cluster_size + vfat_info.clusters_offset;
		lseek(vfat_info.fs, offset, SEEK_SET);
		
		char longname[MAX_LONGNAME_LENGTH];
		longname[0] = 0;
		
		int i;
		for(i = 0; i < entry_per_cluster; ++i){
			read(vfat_info.fs, &buffer, 32);
			if (buffer[0] != 0xE5 && buffer[0] != 0 && buffer[0] != 0x2E && (!(buffer[11] & 0x08) && !(buffer[11] & 0x02) && !(buffer[11] & 0x80) || (buffer[11] == 0x0F))){//ignores . and ..
				struct fat32_direntry* dir_entry = &buffer;
				
				// long name
				if(buffer[11] == 0x0F) {
					struct fat32_direntry_long* dir_entry = &buffer;
					
					char longname_chunk[MAX_LONGNAME_LENGTH];
					size_t index = get_longname_chunck(dir_entry, longname_chunk);
					longname_chunk[index] = 0;
					
					char tmp[MAX_LONGNAME_LENGTH];
					strcpy(tmp, longname_chunk);
					strcat(tmp, longname);
					
					strcpy(longname, tmp);
				}
				// shortname
				else {
					struct fat32_direntry* dir_entry = &buffer;
					
					char name[MAX_LONGNAME_LENGTH];
					
					if(longname[0] == 0) {
						char nameext[12];
						
						if(buffer[0] == 0x05){
							nameext[0] = 0xE5;
						} else {
							nameext[0] = dir_entry->name[0];
						}
						
						int i;
						for(i = 1; i < 8; ++i){
							nameext[i] = dir_entry->name[i];
						}
						if(dir_entry->ext[0] != (char) 32 || dir_entry->ext[0] != (char) 32 || dir_entry->ext[0] != (char) 32){
							i=0;
							while(i < 8 && nameext[i] != (char) 32){
								i++;
							}
							nameext[i] = '.';
							int ext_offs = i+1;
							int pos_first_32 = 0;
							for(i=0; i<3; ++i){
								if (dir_entry->ext[i] != (char) 32){
									nameext[i+ext_offs] = dir_entry->ext[i];
								} else {
									pos_first_32 = i;
									i=3;
								}
							}
							int pos_null_char = pos_first_32 == 0 ? i : pos_first_32;
							nameext[pos_null_char+ext_offs] = 0;
						} else {
							i=0;
							while(i < 8 && nameext[i] != (char) 32){
								i++;
							}
							nameext[i] = 0;
						}
						strcpy(name, nameext);
						name[11] = 0;
					} else {
						strcpy(name, longname);
					}
					
					off_t cluster_tot = (searching) ? (dir_entry->cluster_hi << 16) + dir_entry->cluster_lo : 0;
					
					set_fuse_attr(dir_entry, &st);
					
					filler(fillerdata, name, &st, cluster_tot);
					
					longname[0] = 0;
				}
			}
		}
		
		u_int32_t fat_entry_offset = vfat_info.fats_offset + cur_cluster * 4;
		lseek(vfat_info.fs, fat_entry_offset, SEEK_SET);
		u_int32_t next_cluster;
		read(vfat_info.fs, &next_cluster, 4);
		
		if(0x0FFFFFF8 <= next_cluster && next_cluster <= 0x0FFFFFFF){
			cont = false;
		} else {
			cur_cluster = next_cluster;
		}
	}
	
	return 0;
}

size_t get_longname_chunck(struct fat32_direntry_long* dir_entry, char* name) {
	size_t size1 = 10;
	size_t size2 = 12;
	size_t size3 = 4;
	
	size_t max = MAX_LONGNAME_LENGTH;
	
	char* str1 = (char*) &(dir_entry->name1);
	char* str2 = (char*) &(dir_entry->name2);
	char* str3 = (char*) &(dir_entry->name3);
	
	iconv(iconv_utf16, &str1, &size1, &name, &max);
	iconv(iconv_utf16, &str2, &size2, &name, &max);
	iconv(iconv_utf16, &str3, &size3, &name, &max);
	
	return MAX_LONGNAME_LENGTH-max;
}

// You can use this in vfat_resolve as a filler function for vfat_readdir
static int vfat_search_entry(void *data, const char *name, const struct stat *st, off_t offs)
{
	struct vfat_search_data *sd = data;

	if (strcmp(sd->name, name) != 0)
		return (0);
	
	sd->found = 1;
	sd->first_cluster = offs;
	*(sd->st) = *st;
	
	return (1);
}

// Recursively find correct file/directory node given the path
static int vfat_resolve(const char *path, struct stat *st)
{
	struct vfat_search_data sd;
	sd.st = st;
	uint32_t cur_cluster = vfat_info.fb.fat32.root_cluster;
	// Calls vfat_readdir with vfat_search_entry as a filler
	// and a struct vfat_search_data as fillerdata in order
	// to find each node of the path recursively
	/* XXX add your code here */
	
	if (strcmp(path, "/") == 0) {
		st->st_dev = 0; // Ignored by FUSE
		st->st_ino = 0; // Ignored by FUSE unless overridden
		st->st_mode = S_IRUSR | S_IRGRP | S_IROTH | S_IFDIR;
		st->st_nlink = 1;
		st->st_uid = mount_uid;
		st->st_gid = mount_gid;
		st->st_rdev = 0;
		st->st_size = 0;
		st->st_blksize = 0; // Ignored by FUSE
		st->st_blocks = 1;
		return cur_cluster;
	} else {
		
		const char sep[2] = "/";
		char p[MAX_LONGNAME_LENGTH];
		
		strcpy(p, path);
		
		char* token;
		token = strtok(p, sep);
		
		while(token != NULL) {
			sd.first_cluster = 0;
			sd.found = 0;
			sd.name = token;
			
			vfat_readdir(cur_cluster, vfat_search_entry, &sd, true);
			
			cur_cluster = sd.first_cluster;
			token = strtok(NULL, sep);
		}
	}
	
	return cur_cluster;
}

static int set_fuse_attr(struct fat32_direntry* dir_entry, struct stat* st) {
	bool isDir = false;
	bool isFile = false;
	
	st->st_mode = 0;
	
	if(!(dir_entry->attr & 0x01)) {
		// Not Read Only
		st->st_mode = st->st_mode | S_IWUSR;
	}
	
	st->st_mode = st->st_mode | S_IRUSR | S_IRGRP | S_IROTH;
	st->st_mode = st->st_mode | S_IXUSR | S_IXGRP | S_IXOTH;
	
	if(dir_entry->attr & 0x02) {
		// Hidden File. Should not show in dir listening.
	}
	if(dir_entry->attr & 0x04) {
		// System. File is Operating system
	}
	if(dir_entry->attr & 0x08) {
		// Volume ID.
	}
	if(dir_entry->attr & 0x10) {
		// Directory
		st->st_mode = st->st_mode | S_IFDIR;
	} else {
		st->st_mode = st->st_mode | S_IFREG; // Doesn't handle special files
	}
	if(dir_entry->attr & 0x20) {
		// Archive
	}
	
	struct tm c_timeinfo;
	struct tm a_timeinfo;
	struct tm m_timeinfo;
	
	m_timeinfo.tm_hour = (dir_entry->mtime_time	& 0b1111100000000000) >> 11;
	m_timeinfo.tm_min = (dir_entry->mtime_time	& 0b0000011111100000) >> 5;
	m_timeinfo.tm_sec = (dir_entry->mtime_time	& 0b0000000000011111)*2;
	
	m_timeinfo.tm_year = ((dir_entry->mtime_date	& 0b1111111000000000) >> 9) + 1980 - 1900;
	m_timeinfo.tm_mon = ((dir_entry->mtime_date		& 0b0000000111100000) >> 5) - 1;
	m_timeinfo.tm_mday = dir_entry->mtime_date		& 0b0000000000011111;
	
	a_timeinfo.tm_year = ((dir_entry->atime_date	& 0b1111111000000000) >> 9) + 1980 - 1900;
	a_timeinfo.tm_mon = ((dir_entry->atime_date		& 0b0000000111100000) >> 5) - 1;
	a_timeinfo.tm_mday = dir_entry->atime_date		& 0b0000000000011111;
	
	c_timeinfo.tm_hour = (dir_entry->ctime_time	& 0b1111100000000000) >> 11;
	c_timeinfo.tm_min = (dir_entry->ctime_time	& 0b0000011111100000) >> 5;
	c_timeinfo.tm_sec = (dir_entry->ctime_time	& 0b0000000000011111) * 2;
	
	c_timeinfo.tm_year = ((dir_entry->ctime_date	& 0b1111111000000000) >> 9) + 1980;
	c_timeinfo.tm_mon = (dir_entry->ctime_date		& 0b0000000111100000) >> 5;
	c_timeinfo.tm_mday = dir_entry->ctime_date		& 0b0000000000011111;

	time_t c_t = mktime(&c_timeinfo);
	time_t a_t = mktime(&a_timeinfo);
	time_t m_t = mktime(&m_timeinfo);
	
	st->st_dev = 0; // Ignored by FUSE
	st->st_ino = 0; // Ignored by FUSE unless overridden
	st->st_nlink = 1;
	st->st_uid = mount_uid;
	st->st_gid = mount_gid;
	st->st_rdev = 0;
	st->st_size = dir_entry->size;
	st->st_blksize = 0; // Ignored by FUSE
	st->st_blocks = 1;
	st->st_atime = a_t; //time of last access
	st->st_mtime = m_t; //time of last modification
	st->st_ctime = c_t; //time of last status change 
	return 0;
}

// Get file attributes
static int vfat_fuse_getattr(const char *path, struct stat *st)
{
	/* XXX: This is example code, replace with your own implementation */
	vfat_resolve(path, st);
	
	return 0;
	
	// TODO
	//return -ENOENT;
}

static int vfat_fuse_readdir(const char *path, void *buf,
		  fuse_fill_dir_t filler, off_t offs, struct fuse_file_info *fi)
{
	struct stat st;
	int cluster_offset = vfat_resolve(path, &st);
	
	vfat_readdir(cluster_offset, filler, buf, false);
	
	// TODO
	//assert(offs == 0);
	
	// Calls vfat_resolve to find the first cluster of the directory
	// we wish to read then uses the filler function on all the files
	// in the directory table
	return 0;
}

static int vfat_fuse_read(const char *path, char *buf, size_t size, off_t offs,
	       struct fuse_file_info *fi)
{
	/* XXX: This is example code, replace with your own implementation */
	
	// TODO
	assert(size > 1);
	
	struct stat st;
	int file_cluster = vfat_resolve(path, &st);
	off_t tmp_offs = offs;
	while(tmp_offs >= vfat_info.cluster_size){
		tmp_offs -= vfat_info.cluster_size;
		uint32_t next_cluster_offset = vfat_info.fats_offset + file_cluster * 4;
		lseek(vfat_info.fs, next_cluster_offset, SEEK_SET);
		read(vfat_info.fs, &file_cluster, 4);
		if (0x0FFFFFF8 <= file_cluster && file_cluster <= 0x0FFFFFFF){
			return 0;
		}
	}
	
	u_int32_t read_begin = (file_cluster - 2) * vfat_info.cluster_size + vfat_info.clusters_offset + offs;
	lseek(vfat_info.fs, read_begin, SEEK_SET);
	size_t read_bytes  = 0;
	if ( size > (vfat_info.cluster_size - offs) ){
		read(vfat_info.fs, buf, vfat_info.cluster_size - offs);
		size_t rem_size = size - (vfat_info.cluster_size - offs);
		read_bytes = vfat_info.cluster_size - offs;
		while(rem_size >= vfat_info.cluster_size){
			rem_size -= vfat_info.cluster_size;
			uint32_t next_cluster_offset = vfat_info.fats_offset + file_cluster * 4;
			lseek(vfat_info.fs, next_cluster_offset, SEEK_SET);
			read(vfat_info.fs, &file_cluster, 4);
			if (0x0FFFFFF8 <= file_cluster && file_cluster <= 0x0FFFFFFF){
				return read_bytes;
			}
			read_begin = (file_cluster - 2) * vfat_info.cluster_size + vfat_info.clusters_offset;
			lseek(vfat_info.fs, read_begin, SEEK_SET);
			char tmp_buf[size];
			read(vfat_info.fs, tmp_buf, vfat_info.cluster_size);
			int i;
			for(i = 0; i < vfat_info.cluster_size; ++i){
				buf[i + read_bytes] = tmp_buf[i];
			}
			read_bytes += vfat_info.cluster_size;
		}
		uint32_t next_cluster_offset = vfat_info.fats_offset + file_cluster * 4;
		lseek(vfat_info.fs, next_cluster_offset, SEEK_SET);
		read(vfat_info.fs, &file_cluster, 4);
		if (0x0FFFFFF8 <= file_cluster && file_cluster <= 0x0FFFFFFF){
			return -1;
		}
		read_begin = (file_cluster - 2) * vfat_info.cluster_size + vfat_info.clusters_offset;
		lseek(vfat_info.fs, read_begin, SEEK_SET);
		char tmp_buf[size];
		read(vfat_info.fs, tmp_buf, rem_size);
		int i;
		for(i = 0; i < rem_size; ++i){
			buf[i + read_bytes] = tmp_buf[i];
		}
		read_bytes += vfat_info.cluster_size;
	} else {
		read(vfat_info.fs, buf, size);
	}
	
	/* XXX add your code here */
	return size; // number of bytes read from the file
				 // must be size unless EOF reached, negative for an error 
}

////////////// No need to modify anything below this point
static int vfat_opt_args(void *data, const char *arg, int key, struct fuse_args *oargs)
{
	if (key == FUSE_OPT_KEY_NONOPT && !vfat_info.dev) {
		vfat_info.dev = strdup(arg);
		return (0);
	}
	return (1);
}

static struct fuse_operations vfat_available_ops = {
	.getattr = vfat_fuse_getattr,
	.readdir = vfat_fuse_readdir,
	.read = vfat_fuse_read,
};

int main(int argc, char **argv)
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);
	
	if (!vfat_info.dev)
		errx(1, "missing file system parameter");

	vfat_init(vfat_info.dev);
	return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}

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
	printf("reserved sectors: %d\n", vfat_info.fats_offset);
	vfat_info.clusters_offset = vfat_info.fats_offset + (vfat_info.fb.fat32.sectors_per_fat * vfat_info.fb.bytes_per_sector * vfat_info.fb.fat_count);
	vfat_info.cluster_size = vfat_info.fb.sectors_per_cluster * vfat_info.fb.bytes_per_sector;
	
	struct vfat_search_data sd;
	sd.name = "sd.name";
	sd.found = 0;
	sd.first_cluster = 0;
	
	struct stat st;

	// Tests
	//vfat_readdir(vfat_info.fb.fat32.root_cluster, vfat_search_entry, &sd);
	//int found = vfat_resolve("/hi", &st);
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
	printf("vfat_readdir\n");
	
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
	/* XXX add your code here */
	
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
			if (buffer[0] != 0xE5 && buffer[0] != 0 && buffer[0] != 0x2E){//ignores . and ..
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
						char nameext[11];
						
						if(buffer[0] == 0x05){
							nameext[0] = 0xE5;
						} else {
							nameext[0] = dir_entry->nameext[0];
						}
						
						int i;
						for(i = 1; i < 11; ++i){
							nameext[i] = dir_entry->nameext[i];
						}
						
						strcpy(name, nameext);
						name[11] = 0;
					} else {
						strcpy(name, longname);
					}
					
					
					/*printf("\n---\n");
					printf("Name : %s\n", name);
					//printf("Longname chunk : %s\n", longname_chunk);
					printf("Attribute : %d\n", dir_entry->attr);
					//printf("Type  : %d\n", dir_entry->type);*/
					
					off_t cluster_tot = (searching) ? (dir_entry->cluster_hi << 16) + dir_entry->cluster_lo : 0;
					
					//printf("CLUSTER: %d, %d, %d, %d\n\n", dir_entry->cluster_hi, dir_entry->cluster_hi << 16, dir_entry->cluster_lo, cluster_tot); 
					
					set_fuse_attr(dir_entry, &st);
					
					filler(fillerdata, name, &st, cluster_tot);
					
					longname[0] = 0;
				}
			}
		}
		u_int32_t fat_entry_offset = vfat_info.fats_offset + cur_cluster * 4;
		lseek(vfat_info.fs, fat_entry_offset, SEEK_SET);
		u_int32_t next_cluster;
		if(0x0FFFFFF8 <= next_cluster <= 0x0FFFFFFF){
			cont = false;
		} else {
			cur_cluster = next_cluster;
		}
	}
	/*u_int32_t fat_entry_offset = vfat_info.fats_offset + first_cluster * 4;
	lseek(vfat_info.fs, fat_entry_offset, SEEK_SET);
	uint32_t test;
	read(vfat_info.fs, &test, 4);
	printf("\n --- \n%d\n --- \n", entry_per_cluster);
	printf("\n --- \n%d\n --- \n", 0x0FFFFFF8<=test<=0x0FFFFFFF);*/
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
	printf("---------\n");
	
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
			printf("token: %s\n", token);
			
			sd.first_cluster = 0;
			sd.found = 0;
			sd.name = token;
			
			vfat_readdir(cur_cluster, vfat_search_entry, &sd, true);
			
			cur_cluster = sd.first_cluster;
			token = strtok(NULL, sep);
			
			//sd.name = token;
			//sd.found = 0;
			/*Find first cluster*/
			//vfat_readdir(cluster, vfat_search_entry, sd);
		}
	}
	
	return cur_cluster;
}

static int set_fuse_attr(struct fat32_direntry* dir_entry, struct stat* st) {
	bool isDir = false;
	bool isFile = false;
	
	// st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFDIR;
	st->st_mode = 0;
	
	if(dir_entry->attr & 0x01) {
		// Read Only
		st->st_mode = st->st_mode | S_IRUSR | S_IRGRP | S_IROTH;
	} else {
		// Commented because we mount the FAT32 in read only
		// st->st_mode = st->st_mode | S_IWUSR | S_IWGRP | S_IWOTH;
		st->st_mode = st->st_mode | S_IRUSR | S_IRGRP | S_IROTH;
	}
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

	st->st_dev = 0; // Ignored by FUSE
	st->st_ino = 0; // Ignored by FUSE unless overridden
	st->st_nlink = 1;
	st->st_uid = mount_uid;
	st->st_gid = mount_gid;
	st->st_rdev = 0;
	st->st_size = dir_entry->size;
	st->st_blksize = 0; // Ignored by FUSE
	st->st_blocks = 1;
	return 0;
}

// Get file attributes
static int vfat_fuse_getattr(const char *path, struct stat *st)
{
	/* XXX: This is example code, replace with your own implementation */
	DEBUG_PRINT("fuse getattr %s\n", path);
	
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
	
	/*if(cluster_offset == 0) {
		printf("Error: %s file not found", path);
		return;
	}*/
	
	vfat_readdir(cluster_offset, filler, buf, false);
	
	/* XXX: This is example code, replace with your own implementation */
	DEBUG_PRINT("fuse readdir %s\n", path);
	//assert(offs == 0);
	/* XXX add your code here */
	//filler(buf, "a.txt", NULL, 0);
	//filler(buf, "b.txt", NULL, 0);
	// Calls vfat_resolve to find the first cluster of the directory
	// we wish to read then uses the filler function on all the files
	// in the directory table
	return 0;
}

static int vfat_fuse_read(const char *path, char *buf, size_t size, off_t offs,
	       struct fuse_file_info *fi)
{
	/* XXX: This is example code, replace with your own implementation */
	DEBUG_PRINT("fuse read %s\n", path);
	assert(size > 1);
	buf[0] = 'X';
	buf[1] = 'Y';
	/* XXX add your code here */
	return 2; // number of bytes read from the file
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

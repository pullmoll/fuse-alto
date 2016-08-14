/*******************************************************************************************
 *
 * Alto file system operations
 *
 * Copyright (c) 2016 Jürgen Buchmüller <pullmoll@t-online.de>
 *
 * This is based on L. Stewart's aar.c dated 1/18/93
 *
 *******************************************************************************************/
#if !defined(_ALTOFS_H_)
#define _ALTOFS_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define NCYLS   203                  //!< Number of cylinders
#define NHEADS  2                    //!< Number of heads
#define NSECS   12                   //!< Number of sectors per track
#define NPAGES  (NCYLS*NHEADS*NSECS) //!< Number of pages on one disk image
#define PAGESZ  (256*2)              //!< Number of bytes in one page (data is actually words)
#define FNLEN   40                   //!< Maximum length of a file name

typedef uint16_t word;               //!< Storage type of Alto file system (big endian words)
typedef uint8_t byte;                //!< Well known type...
typedef ssize_t page_t;              //!< Page number type

/**
 * @brief Endian test union
 * Initialize little.e = 1;
 * If little.l == 1 the machine is little endian
 * If little.l == 0 the machine is big endian
 */
typedef union {
    uint16_t e;
    uint8_t  l, h;
} endian_t;

typedef struct {
    word        pagenum;        //!< page number (think LBA)
    word        header[2];      //!< Header words
    word        label[8];       //!< Label words
    word        data[256];      //!< Data words
}   afs_page_t;

typedef struct {
    word        next_rda;       //!< Next raw disk address
    word        prev_rda;       //!< Previous raw disk address
    word        unused1;        //!< always 0 ?
    word        nbytes;         //!< Number of bytes in page (up to 512)
    word        filepage;       //!< File relative page (zero based)
    word        fid_file;       //!< 1 for all used files, ffff for free pages
    word        fid_dir;        //!< 8000 for a directory, 0 for regular, ffff for free
    word        fid_id;         //!< file identifier, ffff for free
}   afs_label_t;

typedef struct {
    word        time[2];        //!< 32 bit time in seconds (based on what epoch?)
}   afs_time_t;

typedef struct {
    word        sn[2];          //!< Serial number 32 bit (really?)
}   afs_sn_t;

/**
 * @brief Structure of the file pointer
 */
typedef struct {
    word        fid_dir;
    word        serialno;
    word        version;
    word        blank;
    word        leader_vda;
}   afs_fp_t;

/**
 * @brief Structure of the file appendix
 */
typedef struct {
    word        vda;                //!< Virtual disk address
    word        page_number;        //!< page number (0 to NPAGES-1)
    word        char_pos;           //!< Offset into the page (character position)
}   afs_fa_t;

/**
 * @brief Structure of the leader page of each file
 * This is exactly 256 words (or 512 bytes) and defines
 * the structure of the first sector in each file.
 */
typedef struct {
    afs_time_t  created;            //!< Time when created (ctime)
    afs_time_t  written;            //!< Time when last written to (mtime)
    afs_time_t  read;               //!< Time when last read from (atime)
    char        filename[FNLEN];    //!< Pascal style string; all filenames have a trailing dot (.)
    word        leader_props[210];  //!< Property space (?)
    word        spare[10];          //!< Spare words for 256 word pages
    byte        proplength;         //!< 1 word
    byte        propbegin;          //!< offset into leader_props (?)
    byte        change_SN;          //!< 1 word (meaning is unknown)
    byte        consecutive;        //!< flag for consecutive images (?)
    afs_fp_t    dir_fp_hint;        //!< 5 words
    afs_fa_t    last_page_hint;     //!< 3 words
}   afs_leader_t;

/**
 * @brief Structure of a directory entry (vector)
 */
typedef struct {
    word        typelength;         //!< type part == 4 for used files, == 0 for deleted; length (of what?)
    afs_fp_t    fileptr;            //!< see %afs_fp_t
    char        filename[FNLEN];    //!< not all used; filename[0] is the number of characters allocated
}   afs_dv_t;

/**
 * @brief Header for the DiskDescriptor file
 */
typedef struct {
    word        nDisks;             //!< How many disks in the file system
    word        nTracks;            //!< How big is each disk
    word        nHeads;             //!< How many heads
    word        nSectors;           //!< How many sectors
    afs_sn_t    last_sn;            //!< Last SN used on disk
    word        blank;              //!< Formerly bitTableChanged
    word        disk_bt_size;       //!< Number valid words in the bit table
    word        def_versions_kept;  //!< 0 implies no multiple versions
    word        free_pages;         //!< Free pages left on the file system
    word        blank1[6];          //!< unused (0) space
}   afs_khd_t;

/**
 * @brief Structure to keep information about a file or directory
 */
typedef struct afs_fileinfo_s {
    struct afs_fileinfo_s* parent;  //!< Parent directory
    char name[FNLEN+2];             //!< File name
    struct stat st;                 //!< Status
    page_t leader_page_vda;         //!< Leader page of this file
    size_t nchildren;               //!< Number of child nodes
    struct afs_fileinfo_s** child;  //!< Array of pointers to the child nodes
}   afs_fileinfo_t;

/**
 * @brief The union's little.e is initialized to 1
 * The little.l can be used for xor of words which need
 * a byte swap on little endian machines.
 */
extern endian_t little;

/**
 * @brief The root directory entry (/)
 */
extern afs_fileinfo_t* root_dir;

/**
 * @brief Be verbose if non zero.
 */
extern int vflag;

extern page_t find_file(const char *name);
extern int free_sysdir_array(afs_dv_t* pdv, size_t count);
extern int make_sysdir_array(afs_dv_t*& array, size_t& count);
extern int save_sysdir_array(afs_dv_t* array, size_t count);
extern int remove_sysdir_entry(const char *name);
extern int rename_sysdir_entry(const char *name, const char* newname);

extern int verify_headers();
extern int validate_disk_descriptor();
extern void fix_disk_descriptor();

extern page_t rda_to_vda(word rda);
extern word vda_to_rda(page_t vda);

extern page_t alloc_page(afs_fileinfo_t* info, page_t prev_vda);

extern void filename_to_string(char *dst, const char *src);
extern void string_to_filename(char *dst, const char *src);
extern void swabit(char *data, int count);
extern word getword(afs_fa_t *fa);

extern int getBT(page_t page);
extern void setBT(page_t page, int val);

extern int delete_file(afs_fileinfo_t* info);
extern int rename_file(afs_fileinfo_t* info, const char* newname);
extern int truncate_file(afs_fileinfo_t* info, off_t offset);
extern int statvfs_kdh(struct statvfs* vfs);

extern void freeinfo(afs_fileinfo_t* node);
extern int makeinfo_all();
extern int makeinfo_file(afs_fileinfo_t* parent, int leader_page_vda);
extern afs_fileinfo_t* find_fileinfo(const char* path);

extern void read_page(page_t filepage, char* data, size_t size = PAGESZ);
extern size_t read_file(page_t leader_page_vda, char* data, size_t size, off_t offs = 0);

extern void write_page(page_t filepage, const char* data, size_t size);
extern size_t write_file(page_t leader_page_vda, const char* data, size_t size, off_t offs = 0);

extern int alto_time_to_time(afs_time_t at, time_t* ptime);
extern int alto_time_to_tm(afs_time_t at, struct tm* ptm);

extern int read_disk_file(const char* name);
extern int read_single_disk(const char *name, afs_page_t* diskp);

extern void dump_memory(char* data, size_t nwords);
extern void dump_disk_block(page_t page);
extern int file_length(page_t leader_page_vda);

/* general utilities */
extern void swabit(char *data, size_t size);
extern int  my_assert(int flag, const char *errmsg, ...);	/* printf style */
extern void my_assert_or_die(int flag, const char *errmsg,...);	/* printf style */

#endif // !defined(_ALTOFS_H_)

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
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <vector>

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

/**
 * @brief Structure of the disk image pages.
 * The pagenum field is not physically stored on the Diablo disks,
 * but an extension of the software which created the *.dsk files.
 *
 * The other fields are are on each disk, separated by gaps and
 * sync patterns.
 */
typedef struct afs_page_s {
    word        pagenum;        //!< page number (think LBA)
    word        header[2];      //!< Header words
    word        label[8];       //!< Label words
    word        data[256];      //!< Data words
}   afs_page_t;

/**
 * @brief Structure of a disk label.
 * This is the internal structure of afs_page_t::label
 */
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
 *
 * The actual length of an entry is defined by the size of
 * typelength (1 word), fileptr (5 words) and the length
 * of the filename (filename[0] in big endian) in words,
 * i.e. words = ((filename[0] | 1) + 1) / 2;
 *
 * It seems that the criteria for the end of a directory
 * is that the filename length is 0 or greater than FNLEN.
 *
 * The typelength word is actually two bytes:
 * The type value is 4 for regular files, 0 for deleted entries.
 * The length value is most of the times somewhere close to the
 * filename length, but not the same.
 */
typedef struct {
    byte        typelength[2];      //!< type and length
    afs_fp_t    fileptr;            //!< 5 words
    char        filename[FNLEN];    //!< not all used; filename[0] is the number of characters allocated
}   afs_dv_t;

class afs_dv {
public:
    afs_dv() : data() { memset(&data, 0, sizeof(data)); }
    afs_dv(const afs_dv_t& _dv) : data() { data = _dv; }
    afs_dv(const afs_dv& other) : data() { data = other.data; }
    afs_dv_t data;
};

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
    word        disk_bt_size;       //!< Number of valid words in the bit table
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


class AltoFS
{
public:

    AltoFS();
    AltoFS(const char* filename);
    ~AltoFS();

    afs_fileinfo_t* find_fileinfo(const char* path);

    int delete_file(afs_fileinfo_t* info);
    int rename_file(afs_fileinfo_t* info, const char* newname);
    int truncate_file(afs_fileinfo_t* info, off_t offset);
    int create_file(const char* path);

    size_t read_file(page_t leader_page_vda, char* data, size_t size, off_t offs = 0);
    size_t write_file(page_t leader_page_vda, const char* data, size_t size, off_t offs = 0);
    int statvfs(struct statvfs* vfs);

private:
    afs_leader_t* page_leader(page_t vda);
    afs_label_t* page_label(page_t vda);

    int read_disk_file(const char* name);
    int read_single_disk(const char *name, afs_page_t* diskp);

    void dump_memory(char* data, size_t nwords);
    void dump_disk_block(page_t page);

    int file_length(page_t leader_page_vda);

    page_t rda_to_vda(word rda);
    word vda_to_rda(page_t vda);

    page_t alloc_page(page_t page);
    page_t find_file(const char *name);

    int make_sysdir_array(std::vector<afs_dv>& array);
    int save_sysdir_array(const std::vector<afs_dv>& array);
    int remove_sysdir_entry(const char *name);
    int rename_sysdir_entry(const char *name, const char* newname);

    void free_fileinfo(afs_fileinfo_t* node);
    int make_fileinfo();
    int make_fileinfo_file(afs_fileinfo_t* parent, int leader_page_vda);

    void read_page(page_t filepage, char* data, size_t size = PAGESZ);
    void write_page(page_t filepage, const char* data, size_t size = PAGESZ);

    void altotime_to_time(afs_time_t at, time_t* ptime);
    void time_to_altotime(time_t time, afs_time_t* at);
    void altotime_to_tm(afs_time_t at, struct tm* ptm);

    word getword(afs_fa_t *fa);

    int getBT(page_t page);
    void setBT(page_t page, int val);

    void free_page(page_t page, word id);
    int is_page_free(page_t page);

    int verify_headers();
    int validate_disk_descriptor();
    void fix_disk_descriptor();
    void cleanup_afs();

    int  my_assert(int flag, const char *errmsg, ...);	/* printf style */
    void my_assert_or_die(int flag, const char *errmsg,...);	/* printf style */

    void swabit(char *data, size_t size);

    const char* filename_to_string(const char *src);
    void string_to_filename(char *dst, const char *src);

    /**
     * @brief The union's little.e is initialized to 1
     * The little.l can be used for xor of words which need
     * a byte swap on little endian machines.
     */
    endian_t little;

    /**
     * @brief storage for disk allocation datastructures: disk descriptor
     */
    afs_khd_t khd;

    /**
     * @brief bitmap for pages allocated
     */
    word *bit_table;

    /**
     * @brief number of bits in bit_table
     */
    page_t bit_count;

    /**
     * @brief storage for the disk image for dp0 and dp1
     */
    afs_page_t* disk;

    /**
     * @brief doubledisk is true, if both of dp0 and dp1 are loaded
     */
    bool doubledisk;

    /**
     * @brief dp0name is the name of the first disk image
     */
    char *dp0name;

    /**
     * @brief dp1name is the name of the second disk image, if any
     */
    char *dp1name;

    /**
     * @brief verbosity flag
     */
    int vflag;

    /**
     * @brief The root directory file info
     */
    afs_fileinfo_t* root_dir;
};

#endif // !defined(_ALTOFS_H_)

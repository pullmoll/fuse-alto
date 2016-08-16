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

#include "fileinfo.h"

class AltoFS
{
public:

    AltoFS();
    AltoFS(const char* filename);
    ~AltoFS();

    int verbosity() const;
    void setVerbosity(int verbosity);

    afs_fileinfo* find_fileinfo(std::string path) const;

    int unlink_file(afs_fileinfo* info);
    int rename_file(afs_fileinfo* info, std::string newname);
    int truncate_file(afs_fileinfo* info, off_t offset);
    int create_file(std::string path);

    size_t read_file(page_t leader_page_vda, char* data, size_t size,
        off_t offset = 0, bool dbg = true);
    size_t write_file(page_t leader_page_vda, const char* data, size_t size,
        off_t offset = 0, bool dbg = true);

    int statvfs(struct statvfs* vfs);

private:
    void log(int verbosity, const char* format, ...);

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

    int read_sysdir();
    int save_sysdir();
    int remove_sysdir_entry(std::string name);
    int rename_sysdir_entry(std::string name, std::string newname);

    int make_fileinfo();
    int make_fileinfo_file(afs_fileinfo* parent, int leader_page_vda);

    void read_page(page_t filepage, char* data, size_t size = PAGESZ);
    void write_page(page_t filepage, const char* data, size_t size = PAGESZ);

    void altotime_to_time(afs_time_t at, time_t* ptime);
    void time_to_altotime(time_t time, afs_time_t* at);
    void altotime_to_tm(afs_time_t at, struct tm& tm);
    std::string altotime_to_str(afs_time_t at);

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

    std::string filename_to_string(const char* src);
    void string_to_filename(char *dst, std::string src);

    /**
     * @brief The union's little.e is initialized to 1
     * The little.l can be used for xor of words which need
     * a byte swap on little endian machines.
     */
    endian_t little;
    afs_khd_t khd;                      //!< Storage for disk allocation datastructures: disk descriptor
    std::vector<word> bit_table;        //!< bitmap for pages allocated
    page_t bit_count;                   //!< Number of bits in bit_table
    std::vector<char> m_sysdir;         //!< A copy of the on-disk SysDir file
    std::vector<afs_dv> m_files;        //!< The contents of SysDir as vector of files
    std::vector<afs_page_t> disk;       //!< Storage for the disk image for dp0 and (optionally) dp1
    bool doubledisk;                    //!< If doubledisk is true, then both of dp0 and dp1 are loaded
    char *dp0name;                      //!< the name of the first disk image
    char *dp1name;                      //!< the name of the second disk image, if any
    int verbose;                        //!< verbosity value
    afs_fileinfo* root_dir;             //!< The root directory file info node
};

#endif // !defined(_ALTOFS_H_)

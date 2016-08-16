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
        off_t offset = 0, bool update = true);
    size_t write_file(page_t leader_page_vda, const char* data, size_t size,
        off_t offset = 0, bool update = true);

    int statvfs(struct statvfs* vfs);

private:
    void log(int verbosity, const char* format, ...);

    afs_leader_t* page_leader(page_t vda);
    afs_label_t* page_label(page_t vda);

    int read_disk_file(std::string name);
    int read_single_disk(std::string , afs_page_t* diskp);

    int save_disk_file();
    int save_single_disk(std::string name, afs_page_t* diskp);

    void dump_memory(char* data, size_t nwords);
    void dump_disk_block(page_t page);

    size_t file_length(page_t leader_page_vda);

    page_t rda_to_vda(word rda);
    word vda_to_rda(page_t vda);

    page_t alloc_page(page_t page);
    page_t find_file(const char *name);

    int read_sysdir();
    int save_sysdir();
    int save_disk_descriptor();

    int remove_sysdir_entry(std::string name);
    int rename_sysdir_entry(std::string name, std::string newname);

    int make_fileinfo();
    int make_fileinfo_file(afs_fileinfo* parent, int leader_page_vda);

    void read_page(page_t filepage, char* data, size_t size = PAGESZ);
    void write_page(page_t filepage, const char* data, size_t size = PAGESZ);
    void zero_page(page_t filepage);

    void altotime_to_time(afs_time_t at, time_t* ptime);
    void time_to_altotime(time_t time, afs_time_t* at);
    void altotime_to_tm(afs_time_t at, struct tm& tm);
    std::string altotime_to_str(afs_time_t at);

    word getword(afs_fa_t *fa);
    int putword(afs_fa_t *fa, word w);

    int getBT(page_t page);
    void setBT(page_t page, int val);

    void free_page(page_t page, word id);
    int is_page_free(page_t page);

    int verify_headers();
    int validate_disk_descriptor();
    void fix_disk_descriptor();

    int  my_assert(int flag, const char *errmsg, ...);
    void my_assert_or_die(bool flag, const char *errmsg,...);

    void swabit(char *data, size_t size);

    std::string filename_to_string(const char* src);
    void string_to_filename(char *dst, std::string src);

    endian_t m_little;                  //!< The little vs. big endian test flag
    afs_kdh_t m_kdh;                    //!< Storage for disk allocation datastructures: disk descriptor
    page_t m_bit_count;                 //!< Number of bits in bit_table
    std::vector<word> m_bit_table;      //!< bitmap for pages allocated
    bool m_disk_descriptor_dirty;             //!< Flag to tell when the bit_table was written to
    std::vector<char> m_sysdir;         //!< A copy of the on-disk SysDir file
    bool m_sysdir_dirty;                //!< Flag to tell when the sysdir was written to
    std::vector<afs_dv> m_files;        //!< The contents of SysDir as vector of files
    std::vector<afs_page_t> m_disk;     //!< Storage for the disk image for dp0 and (optionally) dp1
    bool m_doubledisk;                  //!< If doubledisk is true, then both of dp0 and dp1 are loaded
    std::string m_dp0name;              //!< the name of the first disk image
    std::string m_dp1name;              //!< the name of the second disk image, if any
    int m_verbose;                      //!< verbosity value
    afs_fileinfo* m_root_dir;           //!< The root directory file info node
};

#endif // !defined(_ALTOFS_H_)

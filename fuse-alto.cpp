/*******************************************************************************************
 *
 * Alto file system FUSE interface
 *
 * Copyright (c) 2016 Jürgen Buchmüller <pullmoll@t-online.de>
 *
 *******************************************************************************************/
#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include "altofs.h"

const char* filenames = NULL;

static int getattr_callback(const char *path, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(struct stat));
    afs_fileinfo_t* info = get_fileinfo(path);

    if (!info)
        return -ENOENT;

    memcpy(stbuf, &info->st, sizeof(*stbuf));
    return 0;
}

static int readdir_callback(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{

    afs_fileinfo_t* info = get_fileinfo(path);
    if (!info)
        return -ENOENT;

    filler(buf, ".", &info->st, 0);
    filler(buf, "..", NULL, 0);

    for (size_t i = 0; i < info->nchildren; i++) {
        afs_fileinfo_t* child = info->children[i];
        if (!child)
            continue;
        if (filler(buf, child->name, &child->st, 0))
            break;
    }

    return 0;
}

static int open_callback(const char *path, struct fuse_file_info *fi)
{
    afs_fileinfo_t* info = get_fileinfo(path);
    if (!info)
        return -ENOENT;
    return 0;
}

static int read_callback(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    static __thread char data[PAGESZ];
    afs_fileinfo_t* info = get_fileinfo(path);
    if (!info)
        return -ENOENT;

    if (offset >= info->st.st_size)
        return 0;

    size_t copied = (off_t)(offset + size) > (off_t)info->st.st_size ?
        info->st.st_size - offset : size;

    size_t filepage = offset / PAGESZ;

    // sanity check the page number
    if (filepage >= info->npages)
        return -EBADF;

    size_t done = 0;
    while (done < copied) {
        read_page(info->pages[filepage++], data);
        size_t offs = offset % PAGESZ;
        size_t chunk = copied > PAGESZ ? PAGESZ : copied;
        if (offs && offs + chunk > PAGESZ) {
            // clip read chunk to page boundary
            chunk = PAGESZ - offset;
            memcpy(buf, data + offs, PAGESZ - offset);
        } else {
            memcpy(buf, data + offs, chunk);
        }
        done += chunk;
        offset += chunk;
        buf += chunk;
    }
    return copied;
}

static int unlink_callback(const char *path)
{
    afs_fileinfo_t* info = get_fileinfo(path);
    if (!info)
        return -ENOENT;
    return delete_file(info);
}

static int rename_callback(const char *path, const char* newname)
{
    afs_fileinfo_t* info = get_fileinfo(path);
    if (!info)
        return -ENOENT;
    return rename_file(info, newname);
}

#if defined(DEBUG)
static const char* fuse_cap(unsigned flags)
{
    static char buff[512];
    char* dst = buff;
    memset(buff, 0, sizeof(buff));

    if (flags & FUSE_CAP_ASYNC_READ)
        dst += snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", ASYNC_READ");
    if (flags & FUSE_CAP_POSIX_LOCKS)
        dst += snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", POSIX_LOCKS");
    if (flags & FUSE_CAP_ATOMIC_O_TRUNC)
        dst += snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", ATOMIC_O_TRUNC");
    if (flags & FUSE_CAP_EXPORT_SUPPORT)
        dst += snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", EXPORT_SUPPORT");
    if (flags & FUSE_CAP_BIG_WRITES)
        dst += snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", BIG_WRITES");
    if (flags & FUSE_CAP_DONT_MASK)
        dst += snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", DONT_MASK");
    if (flags & FUSE_CAP_SPLICE_WRITE)
        dst += snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", SPLICE_WRITE");
    if (flags & FUSE_CAP_SPLICE_MOVE)
        dst += snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", SPLICE_MOVE");
    if (flags & FUSE_CAP_SPLICE_READ)
        dst += snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", SPLICE_READ");
    if (flags & FUSE_CAP_FLOCK_LOCKS)
        dst += snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", FLOCK_LOCKS");
    if (flags & FUSE_CAP_IOCTL_DIR)
        dst += snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", IOCTL_DIR");
    return buff + 2;
}
#endif

void* init_callback(fuse_conn_info* info)
{
    (void)info;
    little.e = 1;

#if defined(DEBUG)
    fprintf(stdout, "%s: fuse_conn_info* = %p\n", __func__, info);
    fprintf(stdout, "%s:   proto_major             : %u\n", __func__, info->proto_major);
    fprintf(stdout, "%s:   proto_minor             : %u\n", __func__, info->proto_minor);
    fprintf(stdout, "%s:   async_read              : %u\n", __func__, info->async_read);
    fprintf(stdout, "%s:   max_write               : %u\n", __func__, info->max_write);
    fprintf(stdout, "%s:   max_readahead           : %u\n", __func__, info->max_readahead);
    fprintf(stdout, "%s:   capable                 : %#x\n", __func__, info->capable);
    fprintf(stdout, "%s:   capable.flags           : %s\n", __func__, fuse_cap(info->capable));
    fprintf(stdout, "%s:   want                    : %#x\n", __func__, info->want);
    fprintf(stdout, "%s:   want.flags              : %s\n", __func__, fuse_cap(info->want));
    fprintf(stdout, "%s:   max_background          : %u\n", __func__, info->max_background);
    fprintf(stdout, "%s:   congestion_threshold    : %u\n", __func__, info->congestion_threshold);
#endif

    // FIXME: Where do I really get the "device" to mount?
    // Handling it on my own by using the last argv[] can't be right.
    read_disk_file(filenames);
    if (!validate_disk_descriptor())
        fix_disk_descriptor();
    makeinfo_all();

#if defined(DEBUG)
    printf("%s: root_dir.nchildren=%u\n", __func__, root_dir->nchildren);
    for (size_t i = 0; i < root_dir->nchildren; i++) {
        afs_fileinfo_t* node = root_dir->children[i];
        if (!node)
            continue;
        struct tm tm_ctime;
        memcpy(&tm_ctime, localtime(&node->st.st_ctime), sizeof(tm_ctime));
        char ctime[40];
        strftime(ctime, sizeof(ctime), "%Y-%m-%d %H:%M:%S", &tm_ctime);
        printf("%-40s %08o %5u %9lu %s\n", node->name, node->st.st_mode,
            node->st.st_ino, node->st.st_size, ctime);
    }
#endif

    return root_dir;
}

static struct fuse_operations fuse_ops;

int usage(char* program)
{
    fprintf(stderr, "usage: %s <mountpoint> [options] <disk image file(s)>\n", program);
    fprintf(stderr, "The options are the FUSE options listed below.\n");
    fprintf(stderr, "The last parameter is an Alto disk image file, or two\n");
    fprintf(stderr, "of them separated by a colon (:) for double disk images.\n");
    char help[] = "-h";
    char* argv[3];
    int argc = 0;
    argv[argc++] = program;
    argv[argc++] = help;
    argv[argc] = NULL;
    return fuse_main(argc, argv, &fuse_ops, NULL);
}

int main(int argc, char *argv[])
{
    memset(&fuse_ops, 0, sizeof(fuse_ops));
    if (argc < 3) {
        return usage(argv[0]);
    }
    filenames = argv[--argc];
    fuse_ops.getattr = getattr_callback;
    fuse_ops.unlink = unlink_callback;
    fuse_ops.rename = rename_callback;
    fuse_ops.open = open_callback;
    fuse_ops.read = read_callback;
    fuse_ops.readdir = readdir_callback;
    fuse_ops.init = init_callback;
    return fuse_main(argc, argv, &fuse_ops, NULL);
}

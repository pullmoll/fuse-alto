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

static const char* mountpoint = NULL;
static const char* filenames = NULL;
static struct fuse_chan* chan = NULL;
static struct fuse* fuse = NULL;
static struct fuse_operations fuse_ops;
static int foreground = 0;
static int multithreaded = 1;

/**
 * @brief Yet unused list of options
 * I should switch from using fuse_opt_parse() with
 * a callback to a list of my own options...
 */
static struct fuse_opt opts_alto[] = {
    FUSE_OPT_END
};

static int getattr_alto(const char *path, struct stat *stbuf)
{
    memset(stbuf, 0, sizeof(struct stat));
    afs_fileinfo_t* info = find_fileinfo(path);

    if (!info)
        return -ENOENT;
    struct fuse_context* ctx = fuse_get_context();
    info->st.st_uid = ctx->uid;
    info->st.st_gid = ctx->gid;
    info->st.st_mode &= ~ctx->umask;

    memcpy(stbuf, &info->st, sizeof(*stbuf));
    return 0;
}

static int readdir_alto(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{

    afs_fileinfo_t* info = find_fileinfo(path);
    if (!info)
        return -ENOENT;

    struct fuse_context* ctx = fuse_get_context();
    info->st.st_uid = ctx->uid;
    info->st.st_gid = ctx->gid;
    // info->st.st_mode &= ctx->umask;

    filler(buf, ".", &info->st, 0);
    filler(buf, "..", NULL, 0);

    for (size_t i = 0; i < info->nchildren; i++) {
        afs_fileinfo_t* child = info->child[i];
        if (!child)
            continue;
        child->st.st_uid = ctx->uid;
        child->st.st_gid = ctx->gid;
        child->st.st_mode &= ~ctx->umask;
        if (filler(buf, child->name, &child->st, 0))
            break;
    }

    return 0;
}

static int open_alto(const char *path, struct fuse_file_info *fi)
{
    afs_fileinfo_t* info = find_fileinfo(path);
    if (!info)
        return -ENOENT;
    return 0;
}

static int read_alto(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info*)
{
    afs_fileinfo_t* info = find_fileinfo(path);
    if (!info)
        return -ENOENT;

    if (offset >= info->st.st_size)
        return 0;

    size_t done = read_file(info->leader_page_vda, buf, size, offset);
    return done;
}

static int write_alto(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info*)
{
    afs_fileinfo_t* info = find_fileinfo(path);
    if (!info)
        return -ENOENT;

    size_t done = write_file(info->leader_page_vda, buf, size, offset);
    return done;
}

static int truncate_alto(const char* path, off_t offset)
{
    afs_fileinfo_t* info = find_fileinfo(path);
    if (!info) {
        // TODO: create new file entry
        return -ENOENT;
    }
    int res = truncate_file(info, offset);
    return res;
}

static int unlink_alto(const char *path)
{
    afs_fileinfo_t* info = find_fileinfo(path);
    if (!info)
        return -ENOENT;
    return delete_file(info);
}

static int rename_alto(const char *path, const char* newname)
{
    afs_fileinfo_t* info = find_fileinfo(path);
    if (!info)
        return -ENOENT;
    return rename_file(info, newname);
}

static int statfs_alto(const char *path, struct statvfs* vfs)
{
    // We have but a single root directory
    if (strcmp(path, "/"))
        return -EINVAL;
    return statvfs_kdh(vfs);
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

void* init_alto(fuse_conn_info* info)
{
    (void)info;
    little.e = 1;

#if defined(DEBUG)
    if (vflag > 2) {
      printf("%s: fuse_conn_info* = %p\n", __func__, (void*)info);
      printf("%s:   proto_major             : %u\n", __func__, info->proto_major);
      printf("%s:   proto_minor             : %u\n", __func__, info->proto_minor);
      printf("%s:   async_read              : %u\n", __func__, info->async_read);
      printf("%s:   max_write               : %u\n", __func__, info->max_write);
      printf("%s:   max_readahead           : %u\n", __func__, info->max_readahead);
      printf("%s:   capable                 : %#x\n", __func__, info->capable);
      printf("%s:   capable.flags           : %s\n", __func__, fuse_cap(info->capable));
      printf("%s:   want                    : %#x\n", __func__, info->want);
      printf("%s:   want.flags              : %s\n", __func__, fuse_cap(info->want));
      printf("%s:   max_background          : %u\n", __func__, info->max_background);
      printf("%s:   congestion_threshold    : %u\n", __func__, info->congestion_threshold);
    }
#endif

    // FIXME: Where do I really get the "device" to mount?
    // Handling it on my own by using the last argv[] can't be right.
    read_disk_file(filenames);
    // verify_headers();
    if (!validate_disk_descriptor())
        fix_disk_descriptor();
    make_fileinfo();

    return root_dir;
}

int usage(const char* program)
{
    const char* prog = strrchr(program, '/');
    prog = prog ? prog + 1 : program;
    fprintf(stderr, "usage: %s <mountpoint> [options] <disk image file(s)>\n", prog);
    fprintf(stderr, "Where [options] can be one or more of\n");
    fprintf(stderr, "    -v|--verbose           set verbose mode (can be repeated)\n");
    char name[] = "fuse-alto";
    char help[] = "-ho";
    char* argv[3];
    int argc = 0;
    argv[argc++] = name;
    argv[argc++] = help;
    argv[argc] = NULL;
    fuse_main(argc, argv, &fuse_ops, NULL);
    fprintf(stderr, "\n");
    fprintf(stderr, "The last parameter is an Alto disk image file, or two\n");
    fprintf(stderr, "of them separated by a comma for double disk images.\n");
    return 0;
}

int fuse_opt_alto(void* data, const char* arg, int key, struct fuse_args* outargs)
{
#if defined(DEBUG)
    printf("%s: key=%d arg=%s\n", __func__, key, arg);
#endif
    switch (key) {
    case FUSE_OPT_KEY_OPT:
        if (!strcmp(arg, "-v"))
            vflag += 1;
        if (!strcmp(arg, "-f"))
            foreground = 1;
        if (!strcmp(arg, "-s"))
            multithreaded = 0;
        break;
    case FUSE_OPT_KEY_NONOPT:
        if (NULL == mountpoint) {
            mountpoint = arg;
            break;
        }
        if (NULL == filenames) {
            filenames = arg;
            break;
        }
        return 1;
    }
    return 0;
}

void shutdown_fuse()
{
    if (fuse) {
        if (vflag)
            printf("%s: removing signal handlers\n", __func__);
        fuse_remove_signal_handlers(fuse_get_session(fuse));
    }
    if (mountpoint && chan) {
        if (vflag)
            printf("%s: unmounting %s\n", __func__, mountpoint);
        fuse_unmount(mountpoint, chan);
        mountpoint = 0;
        chan = 0;
    }
    if (fuse) {
        if (vflag)
            printf("%s: destroying fuse (%p)\n", __func__, (void*)fuse);
        fuse_destroy(fuse);
        fuse = 0;
    }
    cleanup_afs();
}

int main(int argc, char *argv[])
{
    memset(&fuse_ops, 0, sizeof(fuse_ops));
    if (argc < 3)
        return usage(argv[0]);

    fuse_ops.getattr = getattr_alto;
    fuse_ops.unlink = unlink_alto;
    fuse_ops.rename = rename_alto;
    fuse_ops.open = open_alto;
    fuse_ops.read = read_alto;
    fuse_ops.write = write_alto;
    fuse_ops.truncate = truncate_alto;
    fuse_ops.readdir = readdir_alto;
    fuse_ops.statfs = statfs_alto;
    fuse_ops.init = init_alto;

    atexit(shutdown_fuse);

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    int res = fuse_opt_parse(&args, NULL, opts_alto, fuse_opt_alto);

    chan = fuse_mount(mountpoint, &args);
    if (!chan) {
        perror("fuse_mount()");
        exit(1);
    }

    fuse = fuse_new(chan, &args, &fuse_ops, sizeof(fuse_ops), NULL);
    if (!fuse) {
        perror("fuse_new()");
        exit(2);
    }

    res = fuse_daemonize(foreground);
    if (res != -1)
        res = fuse_set_signal_handlers(fuse_get_session(fuse));

    if (res != -1) {
        if (multithreaded)
            res = fuse_loop_mt(fuse);
        else
            res = fuse_loop(fuse);
    }

    shutdown_fuse();

    return res;
}

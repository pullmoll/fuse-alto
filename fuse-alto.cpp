/*******************************************************************************************
 *
 * Alto file system FUSE interface
 *
 * Copyright (c) 2016 Jürgen Buchmüller <pullmoll@t-online.de>
 *
 *******************************************************************************************/
#define FUSE_USE_VERSION 26

#include "config.h"
#include <fuse.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <assert.h>
#include "altofs.h"

static struct fuse_args fuse_args;
static int verbose = 0;
static int nonopt_seen = 0;
static char* mountpoint = NULL;
static char* filenames = NULL;
static struct fuse_chan* chan = NULL;
static struct fuse* fuse = NULL;
static struct fuse_operations* fuse_ops = NULL;
static int foreground = 0;
static int multithreaded = 1;
static AltoFS* afs = 0;

enum {
    KEY_HELP,
    KEY_FOREGROUND,
    KEY_SINGLE_THREADED,
    KEY_VERBOSE,
    KEY_VERSION
};

/**
 * @brief List of options
 */
static struct fuse_opt alto_opts[] = {
    FUSE_OPT_KEY("-h",           KEY_HELP),
    FUSE_OPT_KEY("--help",       KEY_HELP),
    FUSE_OPT_KEY("-f",           KEY_FOREGROUND),
    FUSE_OPT_KEY("--foreground", KEY_FOREGROUND),
    FUSE_OPT_KEY("-s",           KEY_SINGLE_THREADED),
    FUSE_OPT_KEY("--single",     KEY_SINGLE_THREADED),
    FUSE_OPT_KEY("-v",           KEY_VERBOSE),
    FUSE_OPT_KEY("--verbose",    KEY_VERBOSE),
    FUSE_OPT_KEY("-V",           KEY_VERSION),
    FUSE_OPT_KEY("--version",    KEY_VERSION),
    FUSE_OPT_END
};

static int create_alto(const char* path, mode_t mode, dev_t dev)
{
    struct fuse_context* ctx = fuse_get_context();
    AltoFS* afs = reinterpret_cast<AltoFS*>(ctx->private_data);
    int res;

    afs_fileinfo* info = afs->find_fileinfo(path);
    if (info) {
        res = afs->unlink_file(path);
        if (res < 0) {
            printf("%s: unlink_file(\"%s\") returned %d\n",
                __func__, path, res);
            return res;
        }
    }

    res = afs->create_file(path);
    if (res < 0) {
        printf("%s: create_file(\"%s\") returned %d\n",
            __func__, path, res);
        return res;
    }

    info = afs->find_fileinfo(path);
    // Something went really, really wrong
    if (!info) {
        printf("%s: file not found after create_file()\n",
            __func__);
        return -ENOSPC;
    }

    return 0;
}

static int getattr_alto(const char *path, struct stat *stbuf)
{
    struct fuse_context* ctx = fuse_get_context();
    AltoFS* afs = reinterpret_cast<AltoFS*>(ctx->private_data);

    memset(stbuf, 0, sizeof(struct stat));
    afs_fileinfo* info = afs->find_fileinfo(path);
    if (!info)
        return -ENOENT;

    info->setStatUid(ctx->uid);
    info->setStatGid(ctx->gid);
    info->setStatMode(info->statMode() & ~ctx->umask);

    memcpy(stbuf, info->st(), sizeof(*stbuf));
    return 0;
}

static int readdir_alto(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
    struct fuse_context* ctx = fuse_get_context();
    AltoFS* afs = reinterpret_cast<AltoFS*>(ctx->private_data);

    afs_fileinfo* info = afs->find_fileinfo("/");
    if (!info)
        return -ENOENT;

    info->setStatUid(ctx->uid);
    info->setStatGid(ctx->gid);
    info->setStatMode(info->statMode() & ~ctx->umask);
    filler(buf, ".", info->st(), 0);
    filler(buf, "..", NULL, 0);

    for (int i = 0; i < info->size(); i++) {
        afs_fileinfo* child = info->child(i);
        if (!child)
            continue;
        if (child->deleted())
            continue;

        child->setStatUid(ctx->uid);
        child->setStatGid(ctx->gid);
        child->setStatMode(child->statMode() & ~ctx->umask);

        if (filler(buf, child->name().c_str(), child->st(), 0))
            break;
    }

    return 0;
}

static int open_alto(const char *path, struct fuse_file_info *fi)
{
    struct fuse_context* ctx = fuse_get_context();
    AltoFS* afs = reinterpret_cast<AltoFS*>(ctx->private_data);

    afs_fileinfo* info = afs->find_fileinfo(path);
    if (!info)
        return -ENOENT;

    fi->fh = (uint64_t)info;
    return 0;
}

static int read_alto(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info*)
{
    struct fuse_context* ctx = fuse_get_context();
    AltoFS* afs = reinterpret_cast<AltoFS*>(ctx->private_data);

    afs_fileinfo* info = afs->find_fileinfo(path);
    if (!info)
        return -ENOENT;
    if (offset >= info->st()->st_size)
        return 0;
    size_t done = afs->read_file(info->leader_page_vda(), buf, size, offset);
    return done;
}

static int write_alto(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info*)
{
    struct fuse_context* ctx = fuse_get_context();
    AltoFS* afs = reinterpret_cast<AltoFS*>(ctx->private_data);

    afs_fileinfo* info = afs->find_fileinfo(path);
    if (!info)
        return -ENOENT;
    size_t done = afs->write_file(info->leader_page_vda(), buf, size, offset);
    return done;
}

static int truncate_alto(const char* path, off_t offset)
{
    struct fuse_context* ctx = fuse_get_context();
    AltoFS* afs = reinterpret_cast<AltoFS*>(ctx->private_data);
    return afs->truncate_file(path, offset);
}

static int unlink_alto(const char *path)
{
    struct fuse_context* ctx = fuse_get_context();
    AltoFS* afs = reinterpret_cast<AltoFS*>(ctx->private_data);
    return afs->unlink_file(path);
}

static int rename_alto(const char *path, const char* newname)
{
    struct fuse_context* ctx = fuse_get_context();
    AltoFS* afs = reinterpret_cast<AltoFS*>(ctx->private_data);
    return afs->rename_file(path, newname);
}

static int utimens_alto(const char* path, const struct timespec tv[2])
{
    struct fuse_context* ctx = fuse_get_context();
    AltoFS* afs = reinterpret_cast<AltoFS*>(ctx->private_data);
    return afs->set_times(path, tv);
}

static int statfs_alto(const char *path, struct statvfs* vfs)
{
    struct fuse_context* ctx = fuse_get_context();
    AltoFS* afs = reinterpret_cast<AltoFS*>(ctx->private_data);

    // We have but a single root directory
    if (strcmp(path, "/"))
        return -EINVAL;
    return afs->statvfs(vfs);
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
        snprintf(dst, sizeof(buff) - (size_t)(dst - buff), "%s", ", IOCTL_DIR");
    return buff + 2;
}
#endif

void* init_alto(fuse_conn_info* info)
{
    (void)info;

    afs = new AltoFS(filenames, verbose);

#if defined(DEBUG)
    if (verbose > 2) {
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
    return afs;
}

static int usage(const char* program)
{
    const char* prog = strrchr(program, '/');
    prog = prog ? prog + 1 : program;
    fprintf(stderr, "%s Version %s\n", prog, FUSE_ALTO_VERSION);
    fprintf(stderr, "usage: %s <mountpoint> [options] <disk image file(s)>\n", prog);
    fprintf(stderr, "Where [options] can be one or more of\n");
    fprintf(stderr, "    -h|--help              print this help\n");
    fprintf(stderr, "    -f|--foreground        run fuse-alto in the foreground\n");
    fprintf(stderr, "    -s|--single            run fuse-alto single threaded\n");
    fprintf(stderr, "    -v|--verbose           set verbose mode (can be repeated)\n");
    return 0;
}

static int is_alto_opt(const char* arg)
{
    // no "-o option" yet
    return 0;
}

static int alto_fuse_main(struct fuse_args *args)
{
    return fuse_main(args->argc, args->argv, fuse_ops, NULL);
}

static int fuse_opt_proc(void* data, const char* arg, int key, struct fuse_args* outargs)
{
    char* arg0 = reinterpret_cast<char *>(data);

    switch (key) {
    case FUSE_OPT_KEY_OPT:
        if (is_alto_opt(arg)) {
            size_t size = strlen(arg) + 3;
            char* tmp = new char[size];
            snprintf(tmp, size, "-o%s", arg);
            // Not yet
            // fuse_opt_add_arg(atlto_args, arg, -1);
            delete[] tmp;
            return 0;
        }
        break;

    case FUSE_OPT_KEY_NONOPT:
        if (0 == nonopt_seen++) {
            return 1;
        }
        if (NULL == filenames) {
            size_t size = strlen(arg) + 1;
            filenames = new char[size];
            snprintf(filenames, size, "%s", arg);
            return 0;
        } else {
            size_t size = strlen(filenames) + 1 + strlen(arg) + 1;
            char* tmp = new char[size];
            snprintf(tmp, size, "%s,%s", filenames, arg);
            delete[] filenames;
            filenames = tmp;
            return 0;
        }
        break;

    case KEY_HELP:
        usage(arg0);
        fuse_opt_add_arg(outargs, "-ho");
        alto_fuse_main(outargs);
        exit(1);

    case KEY_FOREGROUND:
        foreground = 1;
        return 1;

    case KEY_SINGLE_THREADED:
        multithreaded = 0;
        return 1;

    case KEY_VERBOSE:
        verbose++;
        return 0;

    case KEY_VERSION:
        printf("fuse-alto version %s\n", FUSE_ALTO_VERSION);
        fuse_opt_add_arg(outargs, "--version");
        alto_fuse_main(outargs);
        exit(0);

    default:
        fprintf(stderr, "internal error\n");
        exit(2);
    }
    return 0;
}

static void shutdown_fuse()
{
    delete afs;
    afs = 0;
    if (fuse) {
        if (verbose)
            printf("%s: removing signal handlers\n", __func__);
        fuse_remove_signal_handlers(fuse_get_session(fuse));
    }
    if (mountpoint && chan) {
        if (verbose)
            printf("%s: unmounting %s\n", __func__, mountpoint);
        fuse_unmount(mountpoint, chan);
        mountpoint = 0;
        chan = 0;
    }
    if (fuse) {
        if (verbose)
            printf("%s: shutting down fuse\n", __func__);
        fuse_destroy(fuse);
        fuse = 0;
    }
    if (fuse_ops) {
        if (verbose)
            printf("%s: releasing fuse ops\n", __func__);
        free(fuse_ops);
        fuse_ops = 0;
    }
    if (verbose)
            printf("%s: releasing fuse args\n", __func__);
    fuse_opt_free_args(&fuse_args);
}

int main(int argc, char *argv[])
{
    int res;
    assert(sizeof(afs_kdh_t) == 32);
    assert(sizeof(afs_leader_t) == PAGESZ);

    fuse_ops = reinterpret_cast<struct fuse_operations *>(calloc(1, sizeof(*fuse_ops)));
    fuse_ops->getattr = getattr_alto;
    fuse_ops->unlink = unlink_alto;
    fuse_ops->rename = rename_alto;
    fuse_ops->open = open_alto;
    fuse_ops->read = read_alto;
    fuse_ops->write = write_alto;
    fuse_ops->mknod = create_alto;
    fuse_ops->truncate = truncate_alto;
    fuse_ops->readdir = readdir_alto;
    fuse_ops->utimens = utimens_alto;
    fuse_ops->statfs = statfs_alto;
    fuse_ops->init = init_alto;

    atexit(shutdown_fuse);

    struct fuse_args a = FUSE_ARGS_INIT(argc, argv);
    memcpy(&fuse_args, &a, sizeof(fuse_args));

    res = fuse_opt_parse(&fuse_args, argv[0], alto_opts, fuse_opt_proc);
    if (res == -1) {
        perror("fuse_opt_parse()");
        exit(1);
    }

    res = fuse_parse_cmdline(&fuse_args, &mountpoint, &multithreaded, &foreground);
    if (res == -1) {
        perror("fuse_parse_cmdline()");
        exit(1);
    }

    struct stat st;
    res = stat(mountpoint, &st);
    if (res == -1) {
        perror(mountpoint);
        exit(1);
    }

    if (!S_ISDIR(st.st_mode)) {
        perror(mountpoint);
        exit(1);
    }

    chan = fuse_mount(mountpoint, &fuse_args);
    if (0 == chan) {
        perror("fuse_mount()");
        exit(1);
    }

    fuse = fuse_new(chan, &fuse_args, fuse_ops, sizeof(*fuse_ops), NULL);
    if (0 == fuse) {
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

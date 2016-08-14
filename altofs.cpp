/*******************************************************************************************
 *
 * Alto file system operations
 *
 * Copyright (c) 2016 Jürgen Buchmüller <pullmoll@t-online.de>
 *
 * This is based on L. Stewart's aar.c dated 1/18/93
 *
 *******************************************************************************************/
#include "altofs.h"

endian_t little;

/**
 * @brief storage for disk allocation datastructures: disk descriptor
 */
static afs_khd_t khd;

/**
 * @brief bitmap for pages allocated
 */
static word *bit_table;

/**
 * @brief storage for the disk image for dp0 and dp1
 */
static afs_page_t disk[NPAGES * 2];

/**
 * @brief doubledisk is true, if both of dp0 and dp1 are loaded
 */
static bool doubledisk = false;

/**
 * @brief dp0name is the name of the first disk image
 */
static char *dp0name = NULL;

/**
 * @brief dp1name is the name of the second disk image, if any
 */
static char *dp1name = NULL;

/**
 * @brief verbosity flag
 */
int vflag = 0;

/**
 * @brief The root directory file info
 */
afs_fileinfo_t* root_dir = NULL;

/**
 * @brief Return a pointer to the afs_leader_t for page vda.
 * @param vda page number
 * @return pointer to afs_leader_t
 */
afs_leader_t* page_leader(int vda)
{
    return (afs_leader_t *)&disk[vda].data[0];
}

/**
 * @brief Return a pointer to the afs_label_t for page vda.
 * @param vda page number
 * @return pointer to afs_label_t
 */
afs_label_t* page_label(int vda)
{
    return (afs_label_t *)&disk[vda].label[0];
}

/**
 * @brief Read a disk file or two of them separated by comma
 * @param name filename of the disk image(s)
 * @return 0 on succes, or -ENOSYS, -EBADF etc. on error
 */
int read_disk_file(const char* name)
{
    int ok;

    ok = my_assert(sizeof(afs_leader_t) == PAGESZ,
        "%s: sizeof(afs_leader_t) is not %d",
        __func__, PAGESZ);
    if (!ok)
        return -ENOSYS;

    dp0name = strdup(name);
    dp1name = strchr(dp0name, ',');
    if (dp1name)
        *dp1name++ = '\0';
    ok = read_single_disk(dp0name, &disk[0]);
    if (!ok)
        return -EBADF;
    doubledisk = dp1name != NULL;
    if (doubledisk) {
        ok = read_single_disk(dp1name, &disk[NPAGES]);
        if (!ok)
            return -EBADF;
    }
    return 0;
}

/**
 * @brief Read a single file to the in-memory disk space
 * @param name file name
 * @param diskp pointer to the starting disk page
 */
int read_single_disk(const char *name, afs_page_t* diskp)
{
    FILE *infile;
    int bytes;
    int totalbytes = 0;
    int total = NPAGES * sizeof (afs_page_t);
    char *dp = (char *)diskp;
    int ok = 1;
    int use_pclose = 0;

    // We conclude the disk image is compressed if the name ends with .Z
    if (strstr(name, ".Z") == name + strlen(name) - 2) {
        char* cmd = new char[strlen (name) + 10];
        sprintf(cmd, "zcat %s", name);
        infile = popen(cmd, "r");
        delete[] cmd;
        my_assert_or_die(infile != NULL,
            "%s: popen failed on zcat %s\n",
            __func__, name);
        use_pclose = 1;
    } else {
        infile = fopen (name, "rb");
        my_assert_or_die(infile != NULL,
            "%s: fopen failed on Alto disk image file %s\n",
            __func__, name);
    }

    while (totalbytes < total) {
        bytes = fread(dp, sizeof (char), total - totalbytes, infile);
        dp += bytes;
        totalbytes += bytes;
        ok = my_assert(!ferror(infile) && !feof(infile),
            "%s: Disk read failed: %d bytes read instead of %d\n",
            __func__, totalbytes, total);
        if (!ok)
            break;
    }
    if (use_pclose)
        pclose(infile);
    else
        fclose(infile);
    return ok;
}

void dump_memory(char* data, size_t nbytes)
{
    const size_t nwords = nbytes / 2;
    char str[17];

    for (size_t row = 0; row < (nwords+7)/8; row++) {
        printf("%04lx:", row * 8);
        for (size_t col = 0; col < 8; col++) {
            size_t offs = row * 8 + col;
            if (offs < nwords) {
                unsigned char h = data[(2*offs+0) ^ little.l];
                unsigned char l = data[(2*offs+1) ^ little.l];
                printf(" %02x%02x", h, l);
            } else {
                printf("     ");
            }
        }

        for (size_t col = 0; col < 8; col++) {
            size_t offs = row * 8 + col;
            if (offs < nwords) {
                unsigned char h = data[(2*offs+0) ^ little.l];
                unsigned char l = data[(2*offs+1) ^ little.l];
                str[(col * 2) + 0] = (isprint(h)) ? h : '.';
                str[(col * 2) + 1] = (isprint(l)) ? l : '.';
            } else {
                str[(col * 2) + 0] = ' ';
                str[(col * 2) + 1] = ' ';
            }
        }
        str[16] = '\0';
        printf("  %16s\n", str);
    }
}

void dump_disk_block(page_t pageno)
{
    char page[PAGESZ];
    read_page(pageno, page);
    dump_memory(page, PAGESZ);
}

int file_length(page_t leader_page_vda)
{
    int length = 0;
    page_t filepage = leader_page_vda;
    afs_label_t* label;

    while (filepage != 0) {
        label = page_label(filepage);
        length = length + label->nbytes;
        if (label->nbytes < PAGESZ)
            break;
        filepage = rda_to_vda(label->next_rda);
    }
    return length;
}

/**
 * @brief Convert a raw disk address to a virtual disk address.
 * @param rda raw disk address (from the image)
 * @return virtual disk address (think LBA)
 */
page_t rda_to_vda(word rda)
{
    const word dp1flag = (rda >> 1) & 1;
    const word head = (rda >> 2) & 1;
    const word cylinder = (rda >> 3) & 0x1ff;
    const word sector = (rda >> 12) & 0xf;
    const page_t vda = (dp1flag * NPAGES) + (cylinder * NHEADS * NSECS) + (head * NSECS) + sector;
    return vda;
}

/**
 * @brief Convert a virtual disk address to a raw disk address.
 * @param vda virtual disk address (LBA)
 * @return raw disk address
 */
word vda_to_rda(page_t vda)
{
    const word dp1flag = vda >= NPAGES ? 1 : 0;
    const word cylinder = ((vda % NPAGES) / (NHEADS * NSECS)) & 0x1ff;
    const word head = ((vda % NPAGES) / NSECS) & 1;
    const word sector = vda % NSECS;
    const word rda = (dp1flag << 1) | (head << 2) | (cylinder << 3) | (sector << 12);
    return rda;
}

/**
 * @brief Allocate a new page from the free pages.
 *
 * First scan for an unused page following the previous page.
 * Then scan from the start up to the previous page.
 *
 * @param filepage previous page VDA where this page is chained to
 * @return new page RDA, or 0 if no free page is found
 */
page_t alloc_page(afs_fileinfo_t* info, page_t filepage)
{
    // Won't find a free page anyway
    if (0 == khd.free_pages)
        return 0;

    const page_t maxpage = khd.disk_bt_size * sizeof(word);
    const page_t prev_vda = filepage;

    afs_label_t* lprev = page_label(filepage);
    my_assert_or_die(lprev != NULL,
        "%s: No previous page label (%ld)\n", __func__, filepage);

    // Search a free page close to the current filepage
    page_t dist = 1;
    while (dist < maxpage) {
        if (filepage + dist < maxpage && 0 == getBT(filepage + dist)) {
            filepage += dist;
            break;
        }
        if (filepage - dist > 1 && 0 == getBT(filepage - dist)) {
            filepage -= dist;
            break;
        }
        dist++;
    }

    if (getBT(filepage)) {
        // No free page found
#if defined(DEBUG)
        printf("%s: no free page found\n", __func__);
#endif
        return 0;
    }

    info->pages = (word*)realloc(info->pages, (info->npages + 1) * sizeof(word));
    info->pages[info->npages] = filepage;
    khd.free_pages -= 1;
    setBT(filepage, 1);
    afs_label_t* lthis = page_label(filepage);
    memset(lthis, 0, sizeof(*lthis));
    // link pages
    lprev->next_rda = vda_to_rda(filepage);
    lthis->prev_rda = vda_to_rda(prev_vda);
    lthis->nbytes = PAGESZ;
    lthis->filepage = lprev->filepage + 1;
    lthis->fid_file = lprev->fid_file;
    lthis->fid_dir = lprev->fid_dir;
    lthis->fid_id = lprev->fid_id;

#if defined(DEBUG)
    printf("%s: prev page label (%ld)\n", __func__, prev_vda);
    printf("%s:   next_rda    : 0x%04x (vda=%ld)\n", __func__,
        lprev->next_rda, rda_to_vda(lprev->next_rda));
    printf("%s:   prev_rda    : 0x%04x (vda=%ld)\n", __func__,
        lprev->prev_rda, rda_to_vda(lprev->prev_rda));
    printf("%s:   unused1     : %u\n", __func__, lprev->unused1);
    printf("%s:   nbytes      : %u\n", __func__, lprev->nbytes);
    printf("%s:   filepage    : %u\n", __func__, lprev->filepage);
    printf("%s:   fid_file    : %#x\n", __func__, lprev->fid_file);
    printf("%s:   fid_dir     : %#x\n", __func__, lprev->fid_dir);
    printf("%s:   fid_id      : %#x\n", __func__, lprev->fid_id);
    printf("%s: next page label (%ld)\n", __func__, filepage);
    printf("%s:   next_rda    : 0x%04x (vda=%ld)\n", __func__,
        lthis->next_rda, rda_to_vda(lthis->next_rda));
    printf("%s:   prev_rda    : 0x%04x (vda=%ld)\n", __func__,
        lthis->prev_rda, rda_to_vda(lthis->prev_rda));
    printf("%s:   unused1     : %u\n", __func__, lthis->unused1);
    printf("%s:   nbytes      : %u\n", __func__, lthis->nbytes);
    printf("%s:   filepage    : %u\n", __func__, lthis->filepage);
    printf("%s:   fid_file    : %#x\n", __func__, lthis->fid_file);
    printf("%s:   fid_dir     : %#x\n", __func__, lthis->fid_dir);
    printf("%s:   fid_id      : %#x\n", __func__, lthis->fid_id);
#endif
    return filepage;
}

/**
 * @brief Search directory for file %name and return leader page VDA.
 * @param name filename (without the trailing dot)
 * @return page number of the
 */
page_t find_file(const char *name)
{
    int i, last;
    char fn[FNLEN+2];
    afs_label_t* l;
    afs_leader_t* lp;

    /* use linear search ! */
    last = doubledisk ? NPAGES * 2 : NPAGES;
    for (i = 0; i < last; i += 1) {
        l = page_label(i);
        lp = page_leader (i);
        // First file page and actually a file?
        if (l->filepage == 0 && l->fid_file == 1) {
            filename_to_string(fn, lp->filename);
            if (strcasecmp(name, &fn[1]) == 0)
                return i;
        }
    }
    my_assert(0,
        "%s: File %s not found\n",
        __func__, name);
    return -1;
}

/**
 * @brief Scan the SysDir file and build an array of afs_dv_t entries.
 * @param ppdv pointer to a pointer to afs_dv_t
 * @param pcount pointer to a size_t to receive the number of entries
 */
int make_sysdir_array(afs_dv_t*& array, size_t& count)
{
    afs_fileinfo_t* info = get_fileinfo("SysDir");

    if (!info)
        return -ENOENT;

    size_t sdsize = info->st.st_size;
    char* sysdir = (char *) malloc(sdsize);

    my_assert(sysdir !=  NULL,
        "%s: sysdir = malloc(%d) failed\n",
        __func__, sdsize);
    if (!sysdir)
        return -ENOMEM;

    read_file(info->leader_page_vda, sysdir, sdsize);
    if (little.l)
        swabit((char *)sysdir, sdsize);

    const afs_dv_t* end = (afs_dv_t *)(sysdir + sdsize);
    afs_dv_t* pdv = (afs_dv_t *)sysdir;
    size_t nalloc = 0;
    count = 0;
    while (pdv < end) {
        // End of directory if fnlen is zero (?)
        byte fnlen = pdv->filename[little.l];
        if (fnlen == 0 || fnlen > FNLEN)
            break;

        // length is always word aligned
        size_t nsize = (fnlen | 1) + 1;
        size_t esize = sizeof(*pdv) - sizeof(pdv->filename) + nsize;
        if (count >= nalloc) {
            // Reallocate the array in increasing steps
            nalloc = nalloc ? nalloc * 2 : 32;
            array = (afs_dv_t *) realloc(array, nalloc * sizeof(afs_dv_t));
        }
        afs_dv_t* dv = &array[count];
        memset(dv, 0, sizeof(*dv));
        memcpy(dv, pdv, esize);
        count++;

        byte length = dv->typelength % 0400;
        byte type = dv->typelength / 0400;
        if (vflag > 1) {
            printf("%s:* directory entry    : @%u\n", __func__, (word)((char *)pdv - sysdir));
            printf("%s:  length             : %u\n", __func__, length);
            printf("%s:  type               : %u\n", __func__, type);
            printf("%s:  fileptr.fid_dir    : %#x\n", __func__, dv->fileptr.fid_dir);
            printf("%s:  fileptr.serialno   : %#x\n", __func__, dv->fileptr.serialno);
            printf("%s:  fileptr.version    : %#x\n", __func__, dv->fileptr.version);
            printf("%s:  fileptr.blank      : %#x\n", __func__, dv->fileptr.blank);
            printf("%s:  fileptr.leader_vda : %u\n", __func__, dv->fileptr.leader_vda);
            printf("%s:  filename length    : %u\n", __func__, dv->filename[little.l]);
        }
        char fn[FNLEN+2];
        filename_to_string(fn, dv->filename);
        if (vflag > 1)
            printf("%s:  filename           : %s.\n", __func__, &fn[1]);

        pdv = (afs_dv_t*)((char *)pdv + esize);
    }
    if (vflag) {
        size_t eod = (size_t)((char *)pdv - sysdir);
        printf("%s: SysDir usage is %lu/%lu bytes\n", __func__, eod, sdsize);
    }

    free(sysdir);
    return 0;
}

/**
 * @brief Save an array of afs_dv_t entries to the SysDir pages.
 * @param array pointer to array of afs_dv_t entries
 * @param count number of entries
 * @return 0 on success, or -ENOENT if SysDir is not found
 */
int save_sysdir_array(afs_dv_t* array, size_t count)
{
    if (!array)
        return -ENOENT;

    afs_fileinfo_t* info = get_fileinfo("SysDir");
    if (!info)
        return -ENOENT;

    size_t sdsize = info->st.st_size;

    // Allocate sysdir with slack for one extra afs_dv_t
    char* sysdir = (char *) calloc(1, sdsize + sizeof(afs_dv_t));
    my_assert(sysdir !=  NULL,
        "%s: sysdir = calloc(1,%d) failed\n",
        __func__, sdsize + sizeof(afs_dv_t));
    if (!sysdir)
        return -ENOMEM;

    read_file(info->leader_page_vda, sysdir, sdsize);
    if (little.l)
        swabit((char *)sysdir, sdsize);

    const afs_dv_t* end = (afs_dv_t *)(sysdir + sdsize);
    afs_dv_t* pdv = (afs_dv_t *)sysdir;
    size_t idx = 0;
    while (idx < count && pdv < end) {
        afs_dv_t* dv = &array[idx];
        byte fnlen = dv->filename[little.l];
        // length is always word aligned
        size_t nsize = (fnlen | 1) + 1;
        size_t esize = sizeof(*pdv) - sizeof(pdv->filename) + nsize;
        memcpy(pdv, dv, esize);
        pdv = (afs_dv_t*)((char *)pdv + esize);
        idx++;
    }

    if (idx < count) {
        // We need to increase sdsize and write the last entry
        afs_dv_t* dv = &array[idx];
        byte fnlen = dv->filename[little.l];
        // length is always word aligned
        size_t nsize = (fnlen | 1) + 1;
        size_t esize = sizeof(*pdv) - sizeof(pdv->filename) + nsize;
        memcpy(pdv, dv, esize);
        pdv = (afs_dv_t*)((char *)pdv + esize);
        // Terminate the directory
        if (pdv + 1 <= end)
            pdv->filename[little.l] = 0;
        sdsize += esize;
    }

    if (vflag) {
        size_t eod = (size_t)((char *)pdv - sysdir);
        printf("%s: SysDir usage is %lu/%lu bytes\n", __func__, eod, sdsize);
    }

#if defined(DEBUG)
    dump_memory(sysdir, sdsize);
#endif
    if (little.l)
        swabit(sysdir, sdsize);
    write_file(info->leader_page_vda, sysdir, sdsize);
    free(sysdir);
    return 0;
}

/**
 * @brief Remove a file name in the SysDir file and write it back
 * @param name file name to look for (without trailing dot)
 * @return 0 on success, or -ENOENT on error
 */
int remove_sysdir_entry(const char* name)
{
    if (vflag)
        printf("%s: searching for %s\n", __func__, name);

    afs_dv_t* array = NULL;
    size_t count = 0;
    int res = make_sysdir_array(array, count);
    if (0 != res)
        return res;

    // Assume failure
    res = -ENOENT;
    for (size_t idx = 0; idx < count; idx++) {
        afs_dv_t* dv = &array[idx];
        char fn[FNLEN+2];
        filename_to_string(fn, dv->filename);
        if (strcmp(fn+1, name))
            continue;
        // Just mark this entry as unused
        dv->typelength = 0;
        res = save_sysdir_array(array, count);
        break;
    }

    free(array);
    return res;
}

/**
 * @brief Rename a file name in the SysDir file and write it back
 * @param name file name to look for (without trailing dot)
 * @param newname new file name (without trailing dot)
 * @return 0 on success, or -ENOENT on error
 */
int rename_sysdir_entry(const char* name, const char* newname)
{
    if (vflag)
        printf("%s: searching for %s\n", __func__, name);

    afs_dv_t* array = NULL;
    size_t count = 0;
    int res = make_sysdir_array(array, count);
    if (0 != res)
        return res;

    // Assume failure
    res = -ENOENT;
    for (size_t idx = 0; idx < count; idx++) {
        afs_dv_t* dv = &array[idx];
        char fn[FNLEN+2];
        filename_to_string(fn, dv->filename);
        if (strcmp(fn+1, name))
            continue;
        // Change the name of this array entry
        string_to_filename(dv->filename, newname);
        filename_to_string(fn, dv->filename);
        if (vflag)
            printf("%s:  new filename       : %s.\n", __func__, &fn[1]);
        // FIXME: do we need to sort the array by name?
        res = save_sysdir_array(array, count);
        break;
    }

    free(array);
    return res;
}

/**
 * @brief Delete a file from the tree and free its chain's bits in the BT
 * @param info pointer to afs_fileinfo_t describing the file
 * @return 0 on success, or -EPERM, -ENOMEM etc. on error
 */
int delete_file(afs_fileinfo_t* info)
{
    // FIXME: Do we need to modify the leader page?
    afs_leader_t* lp = page_leader(info->leader_page_vda);

    char fn[FNLEN+2];
    filename_to_string(fn, lp->filename);

    // Never allow deleting SysDir or DiskDescriptor
    if (0 == strcmp(fn+1, "SysDir") || 0 == strcmp(fn+1, "DiskDescriptor"))
        return -EPERM;

    memset(lp->filename, 0, sizeof(lp->filename));
    memset(&lp->dir_fp_hint, 0, sizeof(lp->dir_fp_hint));
    memset(&lp->last_page_hint, 0, sizeof(lp->last_page_hint));

    page_t filepage = info->leader_page_vda;
    afs_label_t* l;
    // mark the file pages as unused
    int freed = 0;
    while (filepage != 0) {
        l = page_label(filepage);
        l->fid_file = 0xffff;
        l->fid_dir = 0xffff;
        l->fid_id = 0xffff;
        freed++;
        if (l->nbytes < PAGESZ)
            break;
        filepage = rda_to_vda(l->next_rda);
    }
    khd.free_pages += freed;

    // Remove this node from the file info hiearchy
    afs_fileinfo_t* parent = info->parent;
    freeinfo(info);
    for (size_t i = 0; i < parent->nchildren; i++) {
        if (parent->children[i] != info)
            continue;
        for (size_t j = i; j < parent->nchildren - 1; j++)
            parent->children[j] = parent->children[j+1];
        parent->nchildren -= 1;
        break;
    }

    return remove_sysdir_entry(fn+1);
}

int rename_file(afs_fileinfo_t* info, const char* newname)
{
    afs_leader_t* lp = page_leader(info->leader_page_vda);
    char fn[FNLEN+2];
    filename_to_string(fn, lp->filename);

    // Skip leading directory (we have only root)
    if (newname[0] == '/')
        newname++;

    int ok = my_assert(strlen(newname) < FNLEN-2,
        "%s: newname too long for '%s' -> '%s'\n",
        __func__, info->name, newname);
    if (!ok)
        return -EINVAL;

    snprintf(info->name, sizeof(info->name), "%s", newname);
    // Set new name in the leader page
    string_to_filename(lp->filename, newname);

    return rename_sysdir_entry(fn+1, newname);
}

void freeinfo(afs_fileinfo_t* node)
{
    if (!node)
        return;
    for (size_t i = 0; i < node->nchildren; i++)
        freeinfo(node->children[i]);
    if (node->children)
        free(node->children);
    if (node->pages)
        free(node->pages);
    delete node;
}

int makeinfo_all()
{
    afs_label_t* l;

    if (root_dir) {
        delete root_dir;
        root_dir = 0;
    }

    root_dir = new afs_fileinfo_t;
    my_assert(root_dir != 0,
        "%s: new root_dir failed\n", __func__);
    if (!root_dir)
        return -ENOMEM;

    memset(root_dir, 0, sizeof(*root_dir));
    snprintf(root_dir->name, sizeof(root_dir->name), "/");
    root_dir->st.st_mode = S_IFDIR | 0755;
    // Set number of links = 2 for "." and ".."
    root_dir->st.st_nlink = 2;
    root_dir->st.st_blksize = PAGESZ;
    root_dir->st.st_blocks = 0;

    const int last = doubledisk ? NPAGES * 2 : NPAGES;
    for (int i = 0; i < last; i++) {
        l = (afs_label_t*) disk[i].label;
        // First page of a file?
        if (l->filepage == 0 && l->fid_file == 1) {
            const int res = makeinfo_file(root_dir, i);
            if (res < 0)
                return res;
        }
    }
    return 0;
}

int makeinfo_file(afs_fileinfo_t* parent, int leader_page_vda)
{
    char fn[FNLEN+2];
    afs_label_t* l = page_label(leader_page_vda);
    afs_leader_t* lp = page_leader(leader_page_vda);

    my_assert_or_die (l->filepage == 0,
        "%s: page %d is not a leader page!\n",
        __func__, leader_page_vda);

    filename_to_string(fn, lp->filename);

    afs_fileinfo_t* info = new afs_fileinfo_t;
    my_assert(info != 0,
        "%s: new afs_fileinfo_t failed for %s\n",
        __func__, &fn[1]);
    if (!info)
        return -ENOMEM;

    memset(info, 0, sizeof(*info));

    info->parent = parent;
    info->leader_page_vda = leader_page_vda;
    strcpy(info->name, &fn[1]);
    // Use the file identifier as inode number
    info->st.st_ino = leader_page_vda;
    if (l->fid_dir == 0x8000) {
        // A directory (SysDir) is a file which can't be modified
        info->st.st_mode = S_IFREG | 0400;
    } else if (0 == strcmp(fn+1, "DiskDescriptor")) {
        // Don't allow DiskDescriptor to be written to
        info->st.st_mode = S_IFREG | 0400;
    } else {
        info->st.st_mode = S_IFREG | 0666;
    }
    info->st.st_blksize = PAGESZ;

    // Count the file size and pages
    int npages = 0;
    while (l->next_rda != 0) {
        const page_t filepage = rda_to_vda(l->next_rda);
        l = page_label(filepage);
        info->st.st_size += l->nbytes;
        npages++;
    }
    info->st.st_blocks = (info->st.st_size + PAGESZ - 1) / PAGESZ;

    // Create the list of file pages for random access
    info->npages = npages;
    info->pages = (word *)malloc(sizeof(word) * npages);
    my_assert(info->pages != 0,
        "%s: malloc(%u) failed for %s\n",
        __func__, sizeof(word) * npages, &fn[1]);
    if (!info->pages) {
        delete info;
        return -ENOMEM;
    }

    l = page_label(leader_page_vda);
    npages = 0;
    while (l->next_rda != 0) {
        const page_t filepage = rda_to_vda(l->next_rda);
        info->pages[npages++] = filepage;
        l = page_label(filepage);
    }

    // Fill more struct stat info
    alto_time_to_time(lp->created, &info->st.st_ctime);
    alto_time_to_time(lp->written, &info->st.st_mtime);
    alto_time_to_time(lp->read, &info->st.st_atime);
    info->st.st_nlink = 0;

    // Make a new entry in the parent's list of children
    size_t size = (parent->nchildren + 1) * sizeof(*parent->children);
    parent->children = (afs_fileinfo_t **) realloc(parent->children, size);
    my_assert(parent->children != 0,
        "%s: realloc(...,%u) failed for %s\n",
        __func__, size, &fn[1]);
    if (!parent->children) {
        free(info->pages);
        delete info;
        return -ENOMEM;
    }

    parent->children[parent->nchildren] = info;
    parent->nchildren += 1;
    parent->st.st_nlink++;
    return 0;
}

/**
 * @brief Get a fileinfo entry for the given path
 * @param path file name with leading path (i.e. "/" prepended)
 * @return pointer to afs_fileinfo_t for the entry, or NULL on error
 */
afs_fileinfo_t* get_fileinfo(const char* path)
{
    if (!root_dir)
        return NULL;

    if (!strcmp(path, "/"))
        return root_dir;

    if (path[0] == '/')
        path++;

    for (size_t i = 0; i < root_dir->nchildren; i++) {
        afs_fileinfo_t* node = root_dir->children[i];
        if (!node)
            continue;
        if (!strcmp(node->name, path))
            return node;
    }
    return NULL;
}

/**
 * @brief Read the page filepage into the buffer at data
 * @param filepage page number
 * @param data buffer of PAGESZ bytes
 */
void read_page(page_t filepage, char* data, size_t size)
{
    const char *src = (char *)&disk[filepage].data;
    for (size_t i = 0; i < size; i++)
        data[i] = src[i ^ little.l];
}

/**
 * @brief Write the page filepage to the disk image
 * @param filepage page number
 * @param data buffer of PAGESZ bytes
 */
void write_page(page_t filepage, const char* data, size_t size)
{
    char *dst = (char *)&disk[filepage].data;
    for (size_t i = 0; i < size; i++)
        dst[i ^ little.l] = data[i];
}

/**
 * @brief Read a file starting ad leader_page_vda into the buffer at data
 * @param leader_page_vda page number of leader VDA
 * @param data buffer of size bytes
 * @param size number of bytes to read
 * @param offs start offset to read from
 * @return number of bytes actually read
 */
size_t read_file(page_t leader_page_vda, char* data, size_t size, off_t offs)
{
    afs_label_t* l = page_label(leader_page_vda);
    page_t filepage = rda_to_vda(l->next_rda);
    size_t done = 0;
    off_t pos = 0;
    while (filepage && size > 0) {
        l = page_label(filepage);
        size_t nbytes = size < l->nbytes ? size : l->nbytes;
        if (pos >= offs) {
            // aligned page read
#if defined(DEBUG)
            printf("%s: pos=0x%06lx filepage=%-5ld nbytes=0x%03lx\n",
                __func__, pos, filepage, nbytes);
#endif
            read_page(filepage, data, nbytes);
            filepage = rda_to_vda(l->next_rda);
            data += nbytes;
            done += nbytes;
            size -= nbytes;
        } else if ((off_t)(pos + nbytes) > offs) {
            // partial page read
            off_t from = offs - pos;
            nbytes -= from;
#if defined(DEBUG)
            printf("%s: pos=0x%06lx filepage=%-5ld nbytes=0x%03lx from=0x%03lx\n",
                __func__, pos, filepage, nbytes, from);
#endif
            char buff[PAGESZ];
            read_page(filepage, buff, PAGESZ);
            memcpy(data, buff + from, nbytes);
            data += nbytes;
            done += nbytes;
            size -= nbytes;
        } else {
#if defined(DEBUG)
            printf("%s: pos=0x%06lx filepage=%-5ld (seeking to 0x%06lx)\n",
                __func__, pos, filepage, offs);
#endif
        }
        pos += nbytes;
    }
    return done;
}

/**
 * @brief Write a file starting ad leader_page_vda from the buffer at data
 * @param leader_page_vda page number of leader VDA
 * @param data buffer of PAGESZ bytes
 * @param size number of bytes to write
 * @param offs start offset to write to
 */
size_t write_file(page_t leader_page_vda, const char* data, size_t size, off_t offs)
{
    afs_label_t* l = page_label(leader_page_vda);

    // TODO: modify lp->written date/time
    afs_leader_t* lp = page_leader(leader_page_vda);

    char fn[FNLEN+2];
    filename_to_string(fn, lp->filename);
    afs_fileinfo_t* info = get_fileinfo(fn+1);
    my_assert_or_die(info != NULL,
        "%s: The get_fileinfo(\"%s\") call returned NULL\n",
        __func__, fn+1);

    page_t filepage = rda_to_vda(l->next_rda);
    size_t done = 0;
    off_t pos = 0;
    while (filepage && size > 0) {
        l = page_label(filepage);
        size_t nbytes = size < PAGESZ ? size : PAGESZ;
        if (pos >= offs && l->nbytes == PAGESZ) {
            // aligned page write
            l->nbytes = nbytes;
#if defined(DEBUG)
            printf("%s: pos=0x%06lx filepage=%-5ld nbytes=0x%03lx size=0x%06lx\n",
                __func__, offs, filepage, nbytes, size);
#endif
            write_page(filepage, data, nbytes);
            data += nbytes;
            done += nbytes;
            size -= nbytes;
        } else if (l->nbytes < PAGESZ) {
            // partial page write to fill it up to PAGESZ
            off_t to = l->nbytes;
            nbytes = size < (size_t)(PAGESZ - to) ? size : (size_t)(PAGESZ - to);
            char buff[PAGESZ];
            read_page(filepage, buff, PAGESZ);  // get the current page
#if defined(DEBUG)
            printf("%s: pos=0x%06lx filepage=%-5ld nbytes=0x%03lx size=0x%06lx to=0x%03lx\n",
                __func__, pos, filepage, nbytes, size, to);
#endif
            memcpy(buff + to, data, nbytes);
            write_page(filepage, buff, PAGESZ); // write the modified page
            l->nbytes = to + nbytes;
            data += nbytes;
            done += nbytes;
            size -= nbytes;
        } else {
#if defined(DEBUG)
            printf("%s: pos=0x%06lx filepage=%-5ld (seeking to 0x%06lx)\n",
                __func__, pos, filepage, offs);
#endif
        }
        pos += PAGESZ;
        if (size > 0 && 0 == l->next_rda) {
            // Need to allocate a new page
#if defined(DEBUG)
            printf("%s: allocate new page after %ld\n",
                __func__, filepage);
#endif
            filepage = alloc_page(info, filepage);
        }
        filepage = rda_to_vda(l->next_rda);
    }
    info->st.st_size = offs + done;
    return done;
}



int alto_time_to_time(afs_time_t at, time_t* ptm)
{
    time_t time;
    time = at.time[0] * 65536 + at.time[1];
    time += 2117503696;	// magic offset to Unix epoch
    if (ptm)
        *ptm = time;
    return 0;
}

int alto_time_to_tm(afs_time_t at, struct tm* ptm)
{
    time_t time;
    alto_time_to_time(at, &time);
    memcpy(ptm, localtime(&time), sizeof(*ptm));
    return 0;
}

word getword(afs_fa_t *fa)
{
    afs_label_t* l = page_label(fa->vda);
    word w;

    my_assert_or_die((fa->char_pos & 1) == 0,
        "%s: Called on odd byte boundary (%u)\n",
        __func__, fa->char_pos);

    if (fa->char_pos >= l->nbytes) {
        if (l->next_rda == 0 || l->nbytes < PAGESZ)
            return -1;
        fa->vda = rda_to_vda(l->next_rda);
        l = page_label(fa->vda);
        fa->page_number += 1;
        fa->char_pos = 0;
    }
    my_assert_or_die(fa->page_number == l->filepage,
        "%s: disk corruption - expected vda %d to be filepage %d\n",
        __func__, fa->vda, l->filepage);

    w = disk[fa->vda].data[fa->char_pos >> 1];
    if (little.l)
        w = (w >> 8) | (w << 8);

    fa->char_pos += 2;
    return w;
}

/* don't think we need this routine anyway */
void putword(afs_fa_t *fa, word w)
{
    afs_label_t* l = page_label(fa->vda);
    my_assert_or_die((fa->char_pos & 1) == 0,
        "%s: Called on odd byte boundary (%d)\n",
        __func__, fa->char_pos);
    /*
     * case 1: writing in the middle of an existing file, on a page with more
     * bytes than the one we're at.
     * case 2: extending the last page of a file, changing nbytes as we go
     * case 3: extending past the last page, need to allocate a new one
     */

    /* case 1, existing page, in the middle */
    if (fa->char_pos < l->nbytes) {

    }
}


/**********************************************/
/* Disk page allocation, DiskDescriptor, etc. */
/**********************************************/

/**
 * @brief Get bit from free page bit table
 * The bit table is big endian, so page 0 is in bit 15,
 * page 1 is in bit 14, and page 15 is in bit 0
 * @param page page number
 * @return bit value 0 or 1
 */
int getBT(page_t page)
{
    int bit;
    bit = 15 - (page % 16);
    return (bit_table[page / 16] >> bit) & 1;
}

/**
 * @brief Set bit in the free page bit table
 * @param page page number
 * @param bit value
 */
void setBT(page_t page, int val)
{
    int w, bit;
    w = page / 16;
    bit = 15 - (page % 16);
    bit_table[w] = (bit_table[w] & ~(1 << bit)) | ((val & 1) << bit);
}

int is_free_page(int page)
{
    afs_label_t* l;
    l = page_label(page);
    return (l->fid_file == 0xffff) && (l->fid_dir == 0xffff) && (l->fid_id == 0xffff);
}

/* Sanity Checking */

/**
 * @brief Make sure that each page header refers to itself
 */
int verify_headers()
{
    int ok = 1;

    const int last = doubledisk ? NPAGES * 2 : NPAGES;
    for (int i = 0; i < last; i += 1)
        ok &= my_assert(disk[i].pagenum == rda_to_vda(disk[i].header[1]),
            "%s: page %04x header doesn't match: %04x %04x\n",
            __func__, disk[i].pagenum, disk[i].header[0], disk[i].header[1]);
    return ok;
}

/**
 * @brief Verify the disk descript file DiskDescriptor
 * Check single or double disks
 */
int validate_disk_descriptor()
{
    int ddlp, i, nfree, ok;
    afs_leader_t* lp;
    afs_label_t* l;
    afs_fa_t fa;

    /* Locate DiskDescriptor and copy it into the global data structure */
    ddlp = find_file("DiskDescriptor");
    ok = my_assert(ddlp != -1,
        "%s: Can't find DiskDescriptor\n",
        __func__);
    if (!ok)
        return ok;

    lp = page_leader(ddlp);
    (void)lp; // yet unused
    l = page_label(ddlp);

    fa.vda = rda_to_vda (l->next_rda);
    memcpy(&khd, &disk[fa.vda].data[0], sizeof(khd));
    bit_table = (word *) malloc (khd.disk_bt_size * sizeof(word));

    /* Now copy the bit table from the disk into bit_table */
    fa.page_number = 1;
    fa.char_pos = sizeof(khd);
    for (i = 0; i < khd.disk_bt_size; i += 1)
        bit_table[i] = getword(&fa);

    if (doubledisk) {
        /* for double disk systems */
        ok &= my_assert (khd.nDisks == 2, "%s: Expect double disk system\n", __func__);
    } else {
        /* for single disk systems */
        ok &= my_assert (khd.nDisks == 1, "%s: Expect single disk system\n", __func__);
    }
    ok &= my_assert (khd.nTracks == NCYLS, "%s: KDH tracks != %d\n", __func__, NCYLS);
    ok &= my_assert (khd.nHeads == NHEADS, "%s: KDH heads != %d\n", __func__, NHEADS);
    ok &= my_assert (khd.nSectors == NSECS, "%s: KDH sectors != %d\n", __func__, NSECS);
    ok &= my_assert (khd.def_versions_kept == 0, "%s: defaultVersions != 0\n", __func__);

    // Count free pages in bit table
    nfree = 0;
    const page_t last = doubledisk ? NPAGES * 2 : NPAGES;
    for (page_t page = 0; page < last; page++)
        nfree += getBT(page) ^ 1;
    ok &= my_assert (nfree == khd.free_pages,
        "%s: Bit table count %d doesn't match KDH free pages %d\n",
        __func__, nfree, khd.free_pages);

    // Count free pages in actual image
    nfree = 0;
    for (page_t page = 0; page < last; page++)
        nfree += is_free_page(page);

    ok &= my_assert (nfree == khd.free_pages,
        "%s: Actual free page count %d doesn't match KDH value %d\n",
        __func__, nfree, khd.free_pages);

    return ok;
}

/**
 * @brief Rebuild bit table and free page count from labels.
 */
void fix_disk_descriptor()
{
    int nfree, t, last;

    nfree = 0;
    last = doubledisk ? NPAGES * 2 : NPAGES;
    for (int page = 0; page < last; page++) {
        t = is_free_page(page);
        nfree += t;
        setBT(page, t ^ 1);
    }
    khd.free_pages = nfree;
}

/**
 * @brief An assert() like function
 *
 * As opposed to assert(), this function is in debug and release
 * builds and does not break in a debug build.
 *
 * @param flag if zero, print the assert message
 * @param errmsg message format (printf style)
 * @return the flag
 */
int my_assert(int flag, const char *errmsg, ...)
{
    va_list ap;
    if (!flag) {
        va_start (ap, errmsg);
        vprintf(errmsg, ap);
        va_end (ap);
    }
    return flag;
}

/**
 * @brief An assert() like function which exits
 *
 * As opposed to assert(), this function is in debug and release
 * builds. This version exits if the assertion fails.
 *
 * @param flag if zero, print the assert message
 * @param errmsg message format (printf style)
 * @return the flag
 */
void my_assert_or_die(int flag, const char *errmsg, ...)
{
    va_list ap;
    if (!flag) {
        va_start (ap, errmsg);
        vprintf(errmsg, ap);
        va_end (ap);
        exit (1);
    }
}

/**
 * @brief Swap an array of words
 * @param data pointer to array
 * @param count number of bytes in array
 */
void swabit(char *data, size_t count)
{
    my_assert_or_die((count & 1) == 0 && ((size_t)data & 1) == 0,
        "%s: Called with unaligned size (%u)\n",
        __func__, count);

    my_assert_or_die(((size_t)data & 1) == 0,
        "%s: Called with unaligned data (%p)\n",
        __func__, (void*)data);

    count /= 2;
    word* d = (word *) data;
    while (count--) {
        word w = *d;
        w = (w >> 8) | (w << 8);
        *d++ = w;
    }
}

/**
 * @brief Copy an Alto file system filename from src to a C string at dst.
 *
 * In src the first byte contains the length of the string (Pascal style).
 * Every filename ends with a dot (.) which we remove here.
 *
 * @param dst destination buffer of FNLEN characters
 * @param src pointer to an Alto file system filename
 */
void filename_to_string(char* dst, const char* src)
{
    size_t length = src[little.l];
    if (length < 0 || length >= FNLEN)
        length = FNLEN - 1;
    for (size_t i = 0; i < length; i++)
        dst[i] = src[i ^ little.l];
    /* erase closing '.' */
    length--;
    if ('.' == dst[length])
        dst[length] = '\0';
    else
        dst[++length] = '\0';
}

/**
 * @brief Copy a C string from src to an Alto file system filename at dst.
 *
 * @param dst pointer to an Alto file system filename
 * @param src source buffer of at mose FNLEN-1 characters
 */
void string_to_filename(char* dst, const char*src)
{
    size_t length = strlen(src);
    if (length >= FNLEN)
        length = FNLEN - 1;
    dst[little.l] = length + 1;
    for (size_t i = 0; i < length; i++)
        dst[(i+1) ^ little.l] = src[i];
    // Append a dot
    dst[length ^ little.l] = '.';
}

/**
 * @brief Fill a struct statvfs pointer with info about the file system.
 * @param vfs pointer to a struct statvfs
 * @return 0 on success, -EBADF on error (root_dir is NULL)
 */
int statvfs_kdh(struct statvfs* vfs)
{
    memset(vfs, 0, sizeof(*vfs));
    if (NULL == root_dir)
        return -EBADF;

    vfs->f_bsize = PAGESZ;
    vfs->f_blocks = NPAGES;
    if (doubledisk) {
        vfs->f_frsize *= 2;
        vfs->f_blocks *= 2;
    }
    vfs->f_bfree = khd.free_pages;
    vfs->f_bavail = khd.free_pages;
    vfs->f_files = root_dir->nchildren;
    // FIXME: Is there a way to determine the number of possible files?
    vfs->f_ffree = 0;
    // FIXME: this is without length byte and trailing dot, thus -2. Is this correct?
    vfs->f_namemax = FNLEN-2;
    return 0;
}

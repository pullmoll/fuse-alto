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

AltoFS::AltoFS() :
    little(),
    khd(),
    bit_table(0),
    bit_count(0),
    disk(0),
    doubledisk(false),
    dp0name(0),
    dp1name(0),
    vflag(0),
    root_dir(0)
{
    little.e = 1;
}

AltoFS::AltoFS(const char* filename) :
    little(),
    khd(),
    bit_table(0),
    bit_count(0),
    disk(0),
    doubledisk(false),
    dp0name(0),
    dp1name(0),
    vflag(0),
    root_dir(0)
{
    little.e = 1;
    read_disk_file(filename);
    // verify_headers();
    if (!validate_disk_descriptor())
        fix_disk_descriptor();
    make_fileinfo();
}

AltoFS::~AltoFS()
{
    cleanup_afs();
}

/**
 * @brief Return a pointer to the afs_leader_t for page vda.
 * @param vda page number
 * @return pointer to afs_leader_t
 */
afs_leader_t* AltoFS::page_leader(page_t vda)
{
    afs_leader_t* lp = (afs_leader_t *)&disk[vda].data[0];

#if defined(DEBUG)
    if (vflag > 3 && lp->proplength > 0) {
        if (is_page_free(vda))
            return lp;
        if (lp->filename[little.l] == 0 || lp->filename[little.l] > FNLEN)
            return lp;
        if (lp->dir_fp_hint.blank != 0)
            return lp;

        static afs_leader_t empty = {0,};
        byte size = lp->proplength;
        if (lp->propbegin + size > sizeof(lp->leader_props))
            size = sizeof(lp->leader_props) - lp->propbegin;
        if (memcmp(lp->leader_props, empty.leader_props, sizeof(lp->leader_props))) {
            const char* fn = filename_to_string(lp->filename);
            printf("%s: leader props for page %ld (%s)\n", __func__, vda, fn);
            printf("%s:   propbegin      : %u\n", __func__, lp->propbegin);
            printf("%s:   proplength     : %u (%u)\n", __func__, size, lp->proplength);
            printf("%s:   non-zero data found:\n", __func__);
            dump_memory((char *)lp->leader_props, size);
            fflush(stdout);
        }
    }
#endif
    return lp;
}

/**
 * @brief Return a pointer to the afs_label_t for page vda.
 * @param vda page number
 * @return pointer to afs_label_t
 */
afs_label_t* AltoFS::page_label(page_t vda)
{
    return (afs_label_t *)&disk[vda].label[0];
}

/**
 * @brief Read a disk file or two of them separated by comma
 * @param name filename of the disk image(s)
 * @return 0 on succes, or -ENOSYS, -EBADF etc. on error
 */
int AltoFS::read_disk_file(const char* name)
{
    int ok;

    ok = my_assert(sizeof(afs_leader_t) == PAGESZ,
        "%s: sizeof(afs_leader_t) is not %d",
        __func__, PAGESZ);
    if (!ok)
        return -ENOENT;

    dp0name = strdup(name);
    dp1name = strchr(dp0name, ',');
    if (dp1name)
        *dp1name++ = '\0';
    doubledisk = dp1name != NULL;

    disk = (afs_page_t*) calloc(2*NPAGES, sizeof(afs_page_t));
    my_assert_or_die(disk != NULL,
        "%s: disk calloc(%d,%d) failed",
        __func__, 2*NPAGES, sizeof(afs_page_t));

    ok = read_single_disk(dp0name, &disk[0]);
    if (ok && doubledisk) {
        ok = read_single_disk(dp1name, &disk[NPAGES]);
    }
    return ok ? 0 : -ENOENT;
}

/**
 * @brief Read a single file to the in-memory disk space
 * @param name file name
 * @param diskp pointer to the starting disk page
 */
int AltoFS::read_single_disk(const char *name, afs_page_t* diskp)
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

/**
 * @brief Dump a memory block as words and ASCII data
 * @param data pointer to data to dump
 * @param nbytes number of bytes to dump
 */
void AltoFS::dump_memory(char* data, size_t nbytes)
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

/**
 * @brief Dump a disk block as words and ASCII data
 * @param pageno page number
 */
void AltoFS::dump_disk_block(page_t pageno)
{
    char page[PAGESZ];
    read_page(pageno, page);
    dump_memory(page, PAGESZ);
}

int AltoFS::file_length(page_t leader_page_vda)
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
page_t AltoFS::rda_to_vda(word rda)
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
word AltoFS::vda_to_rda(page_t vda)
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
page_t AltoFS::alloc_page(page_t page)
{
    // Won't find a free page anyway
    if (0 == khd.free_pages)
        return 0;

    const page_t maxpage = khd.disk_bt_size * sizeof(word);
    const page_t prev_vda = page;

    afs_label_t* lprev = page ? page_label(page) : NULL;

    // Search a free page close to the current filepage
    page_t dist = 1;
    while (dist < maxpage) {
        if (page + dist < maxpage && !getBT(page + dist)) {
            page += dist;
            break;
        }
        if (page - dist > 1 && !getBT(page - dist)) {
            page -= dist;
            break;
        }
        dist++;
    }

    if (getBT(page)) {
        // No free page found
#if defined(DEBUG)
        printf("%s: no free page found\n", __func__);
#endif
        return 0;
    }

    khd.free_pages -= 1;
    setBT(page, 1);
    afs_label_t* lthis = page_label(page);
    memset(lthis, 0, sizeof(*lthis));
    // link pages
    if (lprev)
        lprev->next_rda = vda_to_rda(page);
    lthis->prev_rda = vda_to_rda(prev_vda);
    lthis->nbytes = PAGESZ;

    if (lprev) {
        // copy from the previous page
        lthis->filepage = lprev->filepage + 1;
        lthis->fid_file = lprev->fid_file;
        lthis->fid_dir = lprev->fid_dir;
        lthis->fid_id = lprev->fid_id;
    } else {
        // set new values
        lthis->filepage = 0;
        lthis->fid_file = 1;
        lthis->fid_dir = 0;
        lthis->fid_id = khd.last_sn.sn[little.l];
        khd.last_sn.sn[little.l] += 1;
    }

#if defined(DEBUG)
    if (lprev) {
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
    }
    printf("%s: next page label (%ld)\n", __func__, page);
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
    fflush(stdout);
#endif
    return page;
}

/**
 * @brief Search disk for file %name and return leader page VDA.
 * @param name filename (without the trailing dot)
 * @return page number of the
 */
page_t AltoFS::find_file(const char *name)
{
    int i, last;
    afs_label_t* l;
    afs_leader_t* lp;

    // Use linear search !
    last = doubledisk ? NPAGES * 2 : NPAGES;
    for (i = 0; i < last; i += 1) {
        l = page_label(i);
        lp = page_leader (i);
        // First file page and actually a file?
        if (l->filepage == 0 && l->fid_file == 1) {
            const char* fn = filename_to_string(lp->filename);
            if (strcasecmp(name, fn) == 0)
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
 * @return 0 on success, or -ENOENT etc. otherwise
 */
int AltoFS::make_sysdir_array(std::vector<afs_dv>& array)
{
    array.resize(2);
    afs_fileinfo_t* info = find_fileinfo("SysDir");
    if (!info)
        return -ENOENT;

    size_t sdsize = info->st.st_size;
    // Allocate sysdir with slack for one extra afs_dv_t
    char* sysdir = new char[sdsize + sizeof(afs_dv_t)];

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
    size_t count = 0;
    while (pdv < end) {
        byte type = pdv->typelength[1];     // FIXME: little.l?
        byte length = pdv->typelength[0];   // FIXME: little.h?

        // End of directory if length is 0?
        if (0 == length)
            break;

        // End of directory if fnlen is zero (?)
        byte fnlen = pdv->filename[little.l];
        if (fnlen == 0 || fnlen > FNLEN)
            break;

        // length is always word aligned
        size_t nsize = (fnlen | 1) + 1;
        size_t esize = sizeof(*pdv) - sizeof(pdv->filename) + nsize;

        // Skip if deleted entry (?)
        if (4 == type) {

            afs_dv dv(*pdv);
            if (vflag > 3) {
                printf("%s:* directory entry    : @%u\n", __func__, (word)((char *)pdv - sysdir));
                printf("%s:  type               : %u\n", __func__, dv.data.typelength[1]); // FIXME: little.l?
                printf("%s:  length             : %u\n", __func__, dv.data.typelength[0]); // FIXME: little.h?
                printf("%s:  fileptr.fid_dir    : %#x\n", __func__, dv.data.fileptr.fid_dir);
                printf("%s:  fileptr.serialno   : %#x\n", __func__, dv.data.fileptr.serialno);
                printf("%s:  fileptr.version    : %#x\n", __func__, dv.data.fileptr.version);
                printf("%s:  fileptr.blank      : %#x\n", __func__, dv.data.fileptr.blank);
                printf("%s:  fileptr.leader_vda : %u\n", __func__, dv.data.fileptr.leader_vda);
                printf("%s:  filename length    : %u\n", __func__, dv.data.filename[little.l]);
            }
            const char* fn = filename_to_string(dv.data.filename);
            if (vflag > 2)
                printf("%s:  filename           : %s.\n", __func__, fn);
            fflush(stdout);
            array.resize(count+1);
            array[count++] = dv;
        }
        pdv = (afs_dv_t*)((char *)pdv + esize);
    }

    size_t eod = (size_t)((char *)pdv - sysdir);
    if (vflag > 1)
        printf("%s: SysDir usage is %lu/%lu bytes\n", __func__, eod, sdsize);

#if defined(DEBUG)
    if (vflag > 3)
        dump_memory(sysdir, eod);
#endif
    delete[] sysdir;
    return 0;
}

/**
 * @brief Save an array of afs_dv_t entries to the SysDir pages.
 * @param array pointer to array of afs_dv_t entries
 * @param count number of entries
 * @return 0 on success, or -ENOENT if SysDir is not found
 */
int AltoFS::save_sysdir_array(const std::vector<afs_dv>& array)
{
    afs_fileinfo_t* info = find_fileinfo("SysDir");
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
    while (idx < array.size() && pdv < end) {
        const afs_dv* dv = &array[idx];
        byte fnlen = dv->data.filename[little.l];
        // length is always word aligned
        size_t nsize = (fnlen | 1) + 1;
        size_t esize = sizeof(*pdv) - sizeof(pdv->filename) + nsize;
        memcpy(pdv, &dv->data, esize);
        pdv = (afs_dv_t*)((char *)pdv + esize);
        idx++;
    }

    if (idx < array.size()) {
        // We need to increase sdsize and write the last entry
        const afs_dv* dv = &array[idx];
        byte fnlen = dv->data.filename[little.l];
        // length is always word aligned
        size_t nsize = (fnlen | 1) + 1;
        size_t esize = sizeof(*pdv) - sizeof(pdv->filename) + nsize;
        memcpy(pdv, &dv->data, esize);
        pdv = (afs_dv_t*)((char *)pdv + esize);
    }

    size_t eod = (size_t)((char *)pdv - sysdir);
    if (vflag > 1)
        printf("%s: SysDir usage is %lu/%lu bytes\n", __func__, eod, sdsize);
    if (eod > sdsize)
        sdsize = eod;

#if defined(DEBUG)
    if (vflag > 3)
        dump_memory(sysdir, eod);
#endif
    if (little.l)
        swabit(sysdir, sdsize);
    write_file(info->leader_page_vda, sysdir, eod);
    free(sysdir);
    return 0;
}

/**
 * @brief Remove a file name in the SysDir file and write it back
 * @param name file name to look for (without trailing dot)
 * @return 0 on success, or -ENOENT on error
 */
int AltoFS::remove_sysdir_entry(const char* name)
{
    if (vflag)
        printf("%s: searching for %s\n", __func__, name);

    std::vector<afs_dv> array;
    int res = make_sysdir_array(array);

    my_assert(res >= 0,
        "%s: Could not read SysDir array.",
        __func__);
    if (res < 0)
        return res;

    // Assume failure
    res = -ENOENT;
    for (size_t idx = 0; idx < array.size(); idx++) {
        afs_dv* dv = &array[idx];
        const char* fn = filename_to_string(dv->data.filename);
        if (strcmp(fn, name))
            continue;
        // Just mark this entry as unused
        dv->data.typelength[1] = 0;
        res = save_sysdir_array(array);
        break;
    }

    return res;
}

/**
 * @brief Rename a file name in the SysDir file and write it back
 * @param name file name to look for (without trailing dot)
 * @param newname new file name (without trailing dot)
 * @return 0 on success, or -ENOENT on error
 */
int AltoFS::rename_sysdir_entry(const char* name, const char* newname)
{
    if (vflag)
        printf("%s: searching for %s\n", __func__, name);

    std::vector<afs_dv> array;
    int res = make_sysdir_array(array);
    my_assert(res >= 0,
        "%s: Could not read SysDir array.",
        __func__);
    if (res < 0)
        return res;

    // Assume failure
    res = -ENOENT;
    for (size_t idx = 0; idx < array.size(); idx++) {
        afs_dv* dv = &array[idx];
        const char* fn = filename_to_string(dv->data.filename);
        if (strcmp(fn, name))
            continue;
        // Change the name of this array entry
        string_to_filename(dv->data.filename, newname);
        fn = filename_to_string(dv->data.filename);
        if (vflag)
            printf("%s:  new filename       : %s.\n", __func__, fn);
        // FIXME: do we need to sort the array by name?
        res = save_sysdir_array(array);
        break;
    }

    return res;
}

/**
 * @brief Delete a file from the tree and free its chain's bits in the BT
 * @param info pointer to afs_fileinfo_t describing the file
 * @return 0 on success, or -EPERM, -ENOMEM etc. on error
 */
int AltoFS::delete_file(afs_fileinfo_t* info)
{
    // FIXME: Do we need to modify the leader page?
    afs_leader_t* lp = page_leader(info->leader_page_vda);
    const char* fn = filename_to_string(lp->filename);

    // Never allow deleting SysDir or DiskDescriptor
    if (0 == strcmp(fn, "SysDir") || 0 == strcmp(fn, "DiskDescriptor"))
        return -EPERM;

    memset(lp->filename, 0, sizeof(lp->filename));
    memset(&lp->dir_fp_hint, 0, sizeof(lp->dir_fp_hint));
    memset(&lp->last_page_hint, 0, sizeof(lp->last_page_hint));

    page_t page = info->leader_page_vda;
    afs_label_t* l = page_label(page);
    const word id = l->fid_id;

    // mark the file pages as unused
    while (page != 0) {
        l = page_label(page);
        free_page(page, id);
        if (l->nbytes < PAGESZ)
            break;
        page = rda_to_vda(l->next_rda);
    }

    // Remove this node from the file info hiearchy
    afs_fileinfo_t* parent = info->parent;
    for (size_t i = 0; i < parent->nchildren; i++) {
        if (parent->child[i] != info)
            continue;
        parent->nchildren -= 1;
        for (size_t j = i; j < parent->nchildren; j++)
            parent->child[j] = parent->child[j+1];
        free_fileinfo(info);
        break;
    }

    return remove_sysdir_entry(fn);
}

/**
 * @brief Rename file in the tree and in SysDir
 * @param info pointer to afs_fileinfo_t describing the file
 * @param newname new filename
 * @return 0 on success, or -ENOENT on error
 */
int AltoFS::rename_file(afs_fileinfo_t* info, const char* newname)
{
    afs_leader_t* lp = page_leader(info->leader_page_vda);
    const char* fn = filename_to_string(lp->filename);

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

    return rename_sysdir_entry(fn, newname);
}

/**
 * @brief Truncate an (existing) file at the given offset
 * @param info pointer to afs_fileinfo_t describing the file
 * @param offset new size of the file
 * @return 0 on success, or -ENOENT on error
 */
int AltoFS::truncate_file(afs_fileinfo_t* info, off_t offset)
{
    afs_leader_t* lp = page_leader(info->leader_page_vda);
    afs_label_t* l = page_label(info->leader_page_vda);
    const word id = l->fid_id;

    size_t size = info->st.st_size - offset;
    page_t page = rda_to_vda(l->next_rda);
    page_t last_page = -1;
    word last_bytes = 0;

    off_t pos = 0;
    while (page && size > 0) {
        l = page_label(page);
        if (pos >= offset) {

            // free this page
#if defined(DEBUG)
            printf("%s: pos=0x%06lx page=%-5ld (free page)\n",
                __func__, pos, page);
#endif
            free_page(page, id);
            size -= l->nbytes;
            pos += PAGESZ;
            if (-1 == last_page) {
                last_page = page;
                last_bytes = 0;
            }

        } else if (pos + PAGESZ > offset) {

            // shrink this page to the remaining bytes
            l->nbytes = offset - pos;
#if defined(DEBUG)
            printf("%s: pos=0x%06lx page=%-5ld (shrink to 0x%03x bytes)\n",
                __func__, pos, page, l->nbytes);
#endif
            size -= l->nbytes;
            pos += l->nbytes;
            last_page = page;
            last_bytes = l->nbytes;
            break;  // we're done

        } else if (0 == l->next_rda) {

            // allocate a new page
            page = alloc_page(page);
            if (0 == page) {
                // No free page found
                info->st.st_size = pos;
                return -ENOSPC;
            }
#if defined(DEBUG)
            printf("%s: pos=0x%06lx page=%-5ld (allocated new page)\n",
                __func__, pos, page);
#endif
            if (pos + PAGESZ < offset) {
                l->nbytes = PAGESZ;
            } else {
                l->nbytes = (offset - pos) % PAGESZ;
            }
            last_page = page;
            last_bytes = l->nbytes;
            // FIXME: should we fill it with zeroes?

            if (l->nbytes < PAGESZ)
                break;

        } else {
#if defined(DEBUG)
            printf("%s: pos=0x%06lx page=%-5ld (seeking to 0x%06lx)\n",
                __func__, pos, page, offset);
#endif
            size -= PAGESZ;
            pos += PAGESZ;
        }
        page = rda_to_vda(l->next_rda);
    }
    lp->last_page_hint.vda = last_page;
    lp->last_page_hint.page_number = last_page % NPAGES;
    lp->last_page_hint.char_pos = last_bytes;

    return 0;
}

/**
 * @brief Create a new file with SysDir entry, leader page and zero length first page
 * @param path filename including the path (/)
 * @return 0 on success, or -EBADF, -ENOSPC etc. otherwise
 */
int AltoFS::create_file(const char* path)
{
    if (vflag > 1)
        printf("%s: path=%s\n", __func__, path);

    // Skip leading directory (we have only root)
    if (path[0] == '/')
        path++;

    afs_fileinfo_t* info = find_fileinfo("SysDir");
    my_assert(info != NULL,
        "%s: SysDir not found?\n",
        __func__);
    if (!info)
        return -EBADF;

    // Allocate a free page
    page_t page = alloc_page(0);
    my_assert(page != 0,
        "%s: Found no free page\n",
        __func__);
    if (0 == page)
        return -ENOSPC;

    // Get the SysDir info
    afs_leader_t* lp = page_leader(page);
    afs_label_t* l = page_label(page);

    struct timeval tv;
    gettimeofday(&tv, NULL);

    // Set all times to "now"
    time_to_altotime(tv.tv_sec, &lp->created);
    time_to_altotime(tv.tv_sec, &lp->written);
    time_to_altotime(tv.tv_sec, &lp->read);

    string_to_filename(lp->filename, path);

    lp->dir_fp_hint.fid_dir = 0;            // or 0x8000?
    lp->dir_fp_hint.serialno = l->fid_id;   // FIXME: serialno == id?
    lp->dir_fp_hint.version = 1;            // version must be 1
    lp->dir_fp_hint.blank = 0;
    lp->dir_fp_hint.leader_vda = page;      // let's assume this means our own leader page

    lp->last_page_hint.vda = page;
    lp->last_page_hint.page_number = page % NPAGES;
    lp->last_page_hint.char_pos = 0;

    page_t page0 = alloc_page(page);
    my_assert(page0 != 0,
        "%s: Disk full when allocating first filepage of %s\n",
        __func__, path);
    if (page0 == 0)
        return -ENOSPC;

    // Set the page0 nbytes to zero
    l = page_label(page0);
    l->nbytes = 0;

    // Insert a new entry into the SysDir
    std::vector<afs_dv> array;
    int res = make_sysdir_array(array);

    my_assert(res >= 0,
        "%s: Could not read SysDir array.",
        __func__);
    if (res < 0)
        return -ENOENT;

    // Find position in the SysDir array where to insert
    std::vector<afs_dv>::iterator idx;
    int pos = 0;
    for (idx = array.begin(); idx != array.end(); idx++) {
        afs_dv* dv = &array[pos];
        const char* fn = filename_to_string(dv->data.filename);
        if (strcmp(fn, path) > 0)
            break;
        pos++;
    }

    afs_dv* dv = &array[pos];
    if (idx == array.end()) {
        // Not the last entry, so make room
        if (vflag > 1)
            printf("%s: insert entry at SysDir idx=%d/%ld\n", __func__, pos, array.size());
        // Insert a new entry at idx
        array.insert(idx, *dv);
    } else {
        if (vflag > 1)
            printf("%s: insert entry after SysDir\n", __func__);
        array[pos] = *dv;
    }
    dv = &array[pos];
    dv->data.typelength[0] = strlen(path);   // whatever "length" this is
    dv->data.typelength[1] = 4;              // this is an existing file
    dv->data.fileptr.fid_dir = 0x0000;       // this is not a directory;
    dv->data.fileptr.serialno = l->fid_id;   // FIXME: serialno == id?
    dv->data.fileptr.version = 1;
    dv->data.fileptr.blank = 0x0000;
    dv->data.fileptr.leader_vda = page;      // store the leader page
    string_to_filename(dv->data.filename, path);

    res = save_sysdir_array(array);

    my_assert(res >= 0,
        "%s: Could not save the SysDir array.",
        __func__);

    res = make_fileinfo_file(root_dir, page);

    return res;
}

void AltoFS::free_fileinfo(afs_fileinfo_t* node)
{
    if (!node)
        return;
    for (size_t i = 0; i < node->nchildren; i++)
        free_fileinfo(node->child[i]);
    if (node->child)
        free(node->child);
    delete node;
}

int AltoFS::make_fileinfo()
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
        l = (afs_label_t*)disk[i].label;
        // First page of a file?
        if (l->filepage == 0 && l->fid_file == 1) {
            const int res = make_fileinfo_file(root_dir, i);
            if (res < 0)
                return res;
        }
    }
    return 0;
}

int AltoFS::make_fileinfo_file(afs_fileinfo_t* parent, int leader_page_vda)
{
    afs_label_t* l = page_label(leader_page_vda);
    afs_leader_t* lp = page_leader(leader_page_vda);

    my_assert_or_die (l->filepage == 0,
        "%s: page %d is not a leader page!\n",
        __func__, leader_page_vda);

    const char* fn = filename_to_string(lp->filename);

    afs_fileinfo_t* info = new afs_fileinfo_t;
    my_assert(info != 0,
        "%s: new afs_fileinfo_t failed for %s\n",
        __func__, fn);
    if (!info)
        return -ENOMEM;

    memset(info, 0, sizeof(*info));

    info->parent = parent;
    info->leader_page_vda = leader_page_vda;
    strcpy(info->name, fn);
    // Use the file identifier as inode number
    info->st.st_ino = leader_page_vda;
    if (l->fid_dir == 0x8000) {
        // A directory (SysDir) is a file which can't be modified
        info->st.st_mode = S_IFREG | 0400;
    } else if (0 == strcmp(fn, "DiskDescriptor")) {
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

    // Fill more struct stat info
    altotime_to_time(lp->created, &info->st.st_ctime);
    altotime_to_time(lp->written, &info->st.st_mtime);
    altotime_to_time(lp->read, &info->st.st_atime);
#if defined(DEBUG)
    if (vflag > 2) {
        struct tm tm_ctime;
        altotime_to_tm(lp->created, &tm_ctime);
        char ctime[40];
        strftime(ctime, sizeof(ctime), "%Y-%m-%d %H:%M:%S", &tm_ctime);
        printf("%-40s %08o %5lu %9lu %s [%04x%04x]\n", info->name, info->st.st_mode,
            info->st.st_ino, info->st.st_size,
            ctime, lp->created.time[0], lp->created.time[1]);
    }
#endif
    info->st.st_nlink = 0;

    // Make a new entry in the parent's list of children
    size_t size = (parent->nchildren + 1) * sizeof(*parent->child);
    parent->child = (afs_fileinfo_t**) realloc(parent->child, size);
    my_assert(parent->child != 0,
        "%s: realloc(...,%u) failed for %s\n",
        __func__, size, fn);
    if (!parent->child) {
        delete info;
        return -ENOMEM;
    }

    parent->child[parent->nchildren] = info;
    parent->nchildren += 1;
    parent->st.st_nlink++;
    return 0;
}

/**
 * @brief Get a fileinfo entry for the given path
 * @param path file name with leading path (i.e. "/" prepended)
 * @return pointer to afs_fileinfo_t for the entry, or NULL on error
 */
afs_fileinfo_t* AltoFS::find_fileinfo(const char* path)
{
    if (!root_dir)
        return NULL;

    if (!strcmp(path, "/"))
        return root_dir;

    if (path[0] == '/')
        path++;

    for (size_t i = 0; i < root_dir->nchildren; i++) {
        afs_fileinfo_t* node = root_dir->child[i];
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
void AltoFS::read_page(page_t filepage, char* data, size_t size)
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
void AltoFS::write_page(page_t filepage, const char* data, size_t size)
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
size_t AltoFS::read_file(page_t leader_page_vda, char* data, size_t size, off_t offs)
{
    afs_label_t* l = page_label(leader_page_vda);
    page_t page = rda_to_vda(l->next_rda);
    size_t done = 0;
    off_t pos = 0;
    while (page && size > 0) {
        l = page_label(page);
        size_t nbytes = size < l->nbytes ? size : l->nbytes;
        if (pos >= offs) {
            // aligned page read
#if defined(DEBUG)
            printf("%s: pos=0x%06lx page=%-5ld nbytes=0x%03lx\n",
                __func__, pos, page, nbytes);
#endif
            read_page(page, data, nbytes);
            page = rda_to_vda(l->next_rda);
            data += nbytes;
            done += nbytes;
            size -= nbytes;
        } else if ((off_t)(pos + nbytes) > offs) {
            // partial page read
            off_t from = offs - pos;
            nbytes -= from;
#if defined(DEBUG)
            printf("%s: pos=0x%06lx page=%-5ld nbytes=0x%03lx from=0x%03lx\n",
                __func__, pos, page, nbytes, from);
#endif
            char buff[PAGESZ];
            read_page(page, buff, PAGESZ);
            memcpy(data, buff + from, nbytes);
            data += nbytes;
            done += nbytes;
            size -= nbytes;
        } else {
#if defined(DEBUG)
            printf("%s: pos=0x%06lx page=%-5ld (seeking to 0x%06lx)\n",
                __func__, pos, page, offs);
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
size_t AltoFS::write_file(page_t leader_page_vda, const char* data, size_t size, off_t offs)
{
    afs_label_t* l = page_label(leader_page_vda);

    // TODO: modify lp->written date/time
    afs_leader_t* lp = page_leader(leader_page_vda);

    const char* fn = filename_to_string(lp->filename);
    afs_fileinfo_t* info = find_fileinfo(fn);
    my_assert_or_die(info != NULL,
        "%s: The find_fileinfo(\"%s\") call returned NULL\n",
        __func__, fn);

    off_t pos = 0;
    page_t page = rda_to_vda(l->next_rda);

    // See if offs is at or beyond last page
    if (offs / PAGESZ >= info->st.st_size / PAGESZ) {
        // In this case use the leader page's last page hint
        page = lp->last_page_hint.vda;
        pos = (offs / PAGESZ) * PAGESZ;
    }

    size_t done = 0;
    while (page && size > 0) {
        l = page_label(page);
        size_t nbytes = size < PAGESZ ? size : PAGESZ;
        if (pos >= offs && l->nbytes == PAGESZ) {
            // aligned page write
            l->nbytes = nbytes;
#if defined(DEBUG)
            printf("%s: pos=0x%06lx page=%-5ld nbytes=0x%03lx size=0x%06lx\n",
                __func__, offs, page, nbytes, size);
#endif
            write_page(page, data, nbytes);
            data += nbytes;
            done += nbytes;
            size -= nbytes;
        } else if (l->nbytes < PAGESZ) {
            // partial page write to fill it up to PAGESZ
            off_t to = l->nbytes;
            nbytes = size < (size_t)(PAGESZ - to) ? size : (size_t)(PAGESZ - to);
            char buff[PAGESZ];
            read_page(page, buff, PAGESZ);  // get the current page
#if defined(DEBUG)
            printf("%s: pos=0x%06lx page=%-5ld nbytes=0x%03lx size=0x%06lx to=0x%03lx\n",
                __func__, pos, page, nbytes, size, to);
#endif
            memcpy(buff + to, data, nbytes);
            write_page(page, buff, PAGESZ); // write the modified page
            l->nbytes = to + nbytes;
            data += nbytes;
            done += nbytes;
            size -= nbytes;
        } else {
#if defined(DEBUG)
            printf("%s: pos=0x%06lx page=%-5ld (seeking to 0x%06lx)\n",
                __func__, pos, page, offs);
#endif
        }
        pos += PAGESZ;
        if (size > 0 && 0 == l->next_rda) {
            // Need to allocate a new page
            page = alloc_page(page);
        }
        page = rda_to_vda(l->next_rda);
    }
    info->st.st_size = offs + done;
    return done;
}

/**
 * @brief convert an Alto 32-bit date/time value to *nix
 *
 * The magic offset to Unix epoch is 2117503696.
 * Adding this value seems to reliy on 32bit wrap-around?
 *
 *   $ date -u --date @2117503696
 *   Fri Feb  6 03:28:16 UTC 2037
 *
 *   $ date -u --date @-2117503696
 *   Tue Nov 25 20:31:44 UTC 1902
 */
#define ALTOTIME_MAGIC 2117503696ul
void AltoFS::altotime_to_time(afs_time_t at, time_t* ptime)
{
    time_t time = 65536 * at.time[0] + at.time[1];
    if (UINT32_MAX == (uint32_t)time)
        time = 1;
    else
        time += ALTOTIME_MAGIC;
    *ptime = time;
}

void AltoFS::time_to_altotime(time_t time, afs_time_t* at)
{
    time -= ALTOTIME_MAGIC;
    at->time[0] = time / 65536;
    at->time[1] = time % 65536;
}

void AltoFS::altotime_to_tm(afs_time_t at, struct tm* ptm)
{
    time_t time;
    altotime_to_time(at, &time);
    memcpy(ptm, localtime(&time), sizeof(*ptm));
}

word AltoFS::getword(afs_fa_t *fa)
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

/**
 * @brief Get bit from free page bit table
 * The bit table is big endian, so page 0 is in bit 15,
 * page 1 is in bit 14, and page 15 is in bit 0
 * @param page page number
 * @return bit value 0 or 1
 */
int AltoFS::getBT(page_t page)
{
    if (!my_assert(page >= 0 && page < bit_count,
        "%s: page out of bounds (%d)\n",
        __func__, page))
        return 1;
    int bit;
    bit = 15 - (page % 16);
    return (bit_table[page / 16] >> bit) & 1;
}

/**
 * @brief Set bit in the free page bit table
 * @param page page number
 * @param bit value
 */
void AltoFS::setBT(page_t page, int val)
{
    if (!my_assert(page >= 0 && page < bit_count,
        "%s: page out of bounds (%d)\n",
        __func__, page))
        return;
    int offs, bit;
    offs = page / 16;
    bit = 15 - (page % 16);
    bit_table[offs] = (bit_table[offs] & ~(1 << bit)) | ((val & 1) << bit);
}

/**
 * @brief Free a page by filling its label fields
 *
 * Setting all three fid fields to 0xffff marks a page as unused.
 * Count up khd.free_pages, if this page was marked as in use,
 * then mark it as free.
 *
 * @param page page number
 * @param id file id
 * @return
 */
void AltoFS::free_page(page_t page, word id)
{
    afs_label_t* l;
    l = page_label(page);

    my_assert_or_die(l->fid_id == id,
        "%s: Fatal: the label id 0x%04x does not match the leader id 0x%04x\n",
        __func__, l->fid_id, id);
    l->fid_file = 0xffff;
    l->fid_dir = 0xffff;
    l->fid_id = 0xffff;
    // count as freed if marked as in use
    khd.free_pages += getBT(page);
    // mark as freed
    setBT(page, 0);
}

/**
 * @brief Return 1, if a page is marked as free
 * @param page page number
 * @return 1 if free, or 0 otherwise
 */
int AltoFS::is_page_free(page_t page)
{
    afs_label_t* l;
    l = page_label(page);
    return (l->fid_file == 0xffff) && (l->fid_dir == 0xffff) && (l->fid_id == 0xffff);
}

/**
 * @brief Make sure that each page header refers to itself
 */
int AltoFS::verify_headers()
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
int AltoFS::validate_disk_descriptor()
{
    int ddlp, i, nfree, ok;
    afs_leader_t* lp;
    afs_label_t* l;
    afs_fa_t fa;

    // Locate DiskDescriptor and copy it into the global data structure
    ddlp = find_file("DiskDescriptor");
    ok = my_assert(ddlp != -1,
        "%s: Can't find DiskDescriptor\n",
        __func__);
    if (!ok)
        return ok;

    lp = page_leader(ddlp);
    (void)lp; // yet unused
    l = page_label(ddlp);

    fa.vda = rda_to_vda(l->next_rda);
    memcpy(&khd, &disk[fa.vda].data[0], sizeof(khd));
    bit_count = khd.disk_bt_size * sizeof(word) * 16;
    bit_table = (word *) malloc (bit_count / 16);

    // Now copy the bit table from the disk into bit_table
    fa.page_number = 1;
    fa.char_pos = sizeof(khd);
    for (i = 0; i < khd.disk_bt_size; i++)
        bit_table[i] = getword(&fa);

    if (doubledisk) {
        // For double disk systems
        ok &= my_assert (khd.nDisks == 2, "%s: Expect double disk system\n", __func__);
    } else {
        // for single disk systems
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
        nfree += is_page_free(page);

    ok &= my_assert (nfree == khd.free_pages,
        "%s: Actual free page count %d doesn't match KDH value %d\n",
        __func__, nfree, khd.free_pages);

    return ok;
}

/**
 * @brief Rebuild bit table and free page count from labels.
 */
void AltoFS::fix_disk_descriptor()
{
    int nfree, t, last;

    nfree = 0;
    last = doubledisk ? NPAGES * 2 : NPAGES;
    for (int page = 0; page < last; page++) {
        t = is_page_free(page);
        nfree += t;
        setBT(page, t ^ 1);
    }
    khd.free_pages = nfree;
}

/**
 * @brief Clean up resources at exit.
 */
void AltoFS::cleanup_afs()
{
    if (root_dir) {
        free_fileinfo(root_dir);
        root_dir = NULL;
    }
    if (bit_table) {
        free(bit_table);
        bit_table = NULL;
        bit_count = 0;
    }
    if (disk) {
        free(disk);
        disk = NULL;
    }
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
int AltoFS::my_assert(int flag, const char *errmsg, ...)
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
void AltoFS::my_assert_or_die(int flag, const char *errmsg, ...)
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
void AltoFS::swabit(char *data, size_t count)
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
 * @param src pointer to an Alto file system filename
 * @param returns a temporary pointer to the C style filename string
 */
const char* AltoFS::filename_to_string(const char* src)
{
    static char buff[16][FNLEN+2];
    static int which = 0;
    char* dst;
    which = (which + 1) % 16;
    dst = buff[which];

    size_t length = src[little.l];
    if (length < 0 || length >= FNLEN)
        length = FNLEN - 1;
    for (size_t i = 0; i < length; i++)
        dst[i] = src[i ^ little.l];
    // Replace invalid (non printing) characters
    for (size_t i = 1; i < length; i++)
        dst[i] = isprint((unsigned char)dst[i]) ? dst[i] : '#';
    // Erase the closing '.'
    length--;
    if ('.' == dst[length])
        dst[length] = '\0';
    else
        dst[++length] = '\0';
    // Return the string after the length byte
    return dst+1;
}

/**
 * @brief Copy a C string from src to an Alto file system filename at dst.
 *
 * @param dst pointer to an Alto file system filename
 * @param src source buffer of at mose FNLEN-1 characters
 */
void AltoFS::string_to_filename(char* dst, const char*src)
{
    size_t length = strlen(src) + 1;
    if (length >= FNLEN)
        length = FNLEN - 1;
    dst[little.l] = length;
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
int AltoFS::statvfs(struct statvfs* vfs)
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

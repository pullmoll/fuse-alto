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

static char *dp0name = NULL;
static char *dp1name = NULL;

int vflag = 1;

afs_fileinfo_t* root_dir = NULL;

/* actual procedures */

afs_leader_t* page_leader(int vda)
{
    return (afs_leader_t *)&disk[vda].data[0];
}

afs_label_t* page_label(int vda)
{
    return (afs_label_t *)&disk[vda].label[0];
}


/******************************/
/* Main work-doing procedures */
/******************************/

int read_disk_file(const char* name)
{
    int ok;

    ok = my_assert(sizeof(afs_leader_t) == PAGESZ,
        "%s: sizeof(afs_leader_t) is not %d", __func__, PAGESZ);
    if (!ok)
        return -ENOSYS;

    dp0name = strdup(name);
    dp1name = strchr(dp0name, ':');
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

int read_single_disk(const char *name, afs_page_t* diskp)
{
    FILE *infile;
    int bytes;
    int totalbytes = 0;
    int total = NPAGES * sizeof (afs_page_t);
    char *dp = (char *)diskp;
    int ok = 1;

    /*
     * We conclude the disk image is compressed if the name ends with .Z
     */
    if (strstr(name, ".Z") == name + strlen(name) - 2) {
        char* cmd = new char[strlen (name) + 10];
        sprintf(cmd, "zcat %s", name);
        infile = popen(cmd, "r");
        my_assert_or_die(infile != NULL,
            "%s: popen failed on zcat %s\n", __func__, name);
        delete[] cmd;
    } else {
        infile = fopen (name, "rb");
        my_assert_or_die(infile != NULL,
            "%s: open failed on Alto disk image file %s\n", __func__, name);
    }

    while (totalbytes < total) {
        bytes = fread(dp, sizeof (char), total - totalbytes, infile);
        dp += bytes;
        totalbytes += bytes;
        ok = my_assert(!ferror(infile) && !feof(infile),
            "% Disk read failed: %d bytes read instead of %d\n", __func__, totalbytes, total);
        if (!ok)
            break;
    }
    return ok;
}

void dump_disk_block(int page)
{
    int row, col;
    word d;
    char str[17], c;
    for (row = 0; row < 16; row++) {
        printf("%04x:", row * 8);
        for (col = 0; col < 8; col++) {
            printf(" %04x", disk[page].data[(row * 8) + col]);
        }

        for (col = 0; col < 8; col++) {
            d = disk[page].data[(row * 8) + col];
            c = (d >> 8) & 0x7f;
            str[(col * 2)] = (isprint ((unsigned char)c)) ? c : '.';
            c = (d) & 0x7f;
            str[(col * 2) + 1] = (isprint ((unsigned char)c)) ? c : '.';
        }
        str[16] = 0;
        printf("  %16s\n", str);
    }
}

int file_length(int leader_page_VDA)
{
    int length = 0;
    int filepage = leader_page_VDA;
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

/**********************************/
/* general disk untility routines */
/**********************************/

word rda_to_vda(word rda)
{
    word sector = (rda >> 12) & 0xf;
    word head = (rda >> 2) & 1;
    word cylinder = (rda >> 3) & 0x1ff;
    word vda = (cylinder * 24) + (head * 12) + sector;
    if (rda & 2)
        vda += NPAGES;
    return vda;
}

word vda_to_rda(word vda)
{
    word sector = vda % 12;
    word head = (vda / 12) & 1;
    word cylinder = (vda / 24) & 0x1ff;
    word rda = (cylinder << 3) + (head << 2) + (sector << 12);
    if (vda >= NPAGES)
        rda |= 2;
    return rda;
}

/**
 * @brief Search directory for file <name> and return leader page VDA
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
        if (l->filepage == 0 && l->fid_file == 1) {
            copyfilename(fn, lp->filename);
            if (strcasecmp(name, &fn[1]) == 0)
                return i;
        }
    }
    my_assert(0, "%s: File %s not found\n", __func__, name);
    return -1;
}


int delete_file(afs_fileinfo_t* info)
{
    afs_label_t* l = page_label(info->leader_page_vda);
    afs_leader_t* lp = page_leader(info->leader_page_vda);
    int freed = 0;

    // FIXME: Do we need to modify the leader page?
    memset(lp->filename, 0, sizeof(lp->filename));
    memset(&lp->dir_fp_hint, 0, sizeof(lp->dir_fp_hint));
    memset(&lp->last_page_hint, 0, sizeof(lp->last_page_hint));

    // mark the leader page label as unused
    setBT(info->leader_page_vda, 1);
    l->fid_file = 0xffff;
    l->fid_dir = 0xffff;
    l->fid_id = 0xffff;
    freed++;

    while (l->next_rda != 0) {
        page_t pageno = rda_to_vda(l->next_rda);
        setBT(pageno, 1);   // mark as free
        l = page_label(pageno);
        l->fid_file = 0xffff;
        l->fid_dir = 0xffff;
        l->fid_id = 0xffff;
        freed++;
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

    // Modify the SysDir file
    info = get_fileinfo("SysDir");
    if (!info)
        return -EBADF;

    char* sysdir = new char[info->st.st_size];
    if (!sysdir)
        return -ENOMEM;
    ssize_t offs = 0;
    while (offs < info->st.st_size) {

    }
    delete[] sysdir;

    return 0;
}

int rename_file(afs_fileinfo_t* info, const char* newname)
{
    afs_leader_t* lp = page_leader(info->leader_page_vda);

    int ok = my_assert(strlen(newname) < FNLEN-2, "%s: newname too long for '%s' -> '%s'\n", __func__, info->name, newname);
    if (!ok)
        return -EINVAL;

    snprintf(info->name, sizeof(info->name), "%s", newname);
    lp->filename[0] = strlen(newname) + 1;
    snprintf(lp->filename + 1, sizeof(lp->filename) - 1, "%s.", newname);

    // TODO: modify SysDir
    return 0;
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
    my_assert(root_dir != 0, "%s: new root_dir failed\n", __func__);
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

    copyfilename(fn, lp->filename);

    afs_fileinfo_t* info = new afs_fileinfo_t;
    my_assert(info != 0, "%s: new afs_fileinfo_t failed for %s\n", __func__, &fn[1]);
    if (!info)
        return -ENOMEM;
    memset(info, 0, sizeof(*info));

    info->parent = parent;
    info->leader_page_vda = leader_page_vda;
    strcpy(info->name, &fn[1]);
    // Use the file identifier as inode number
    info->st.st_ino = leader_page_vda;
    // A directory (SysDir) is a file which can't be modified
    if (l->fid_dir == 0x8000) {
        info->st.st_mode = S_IFREG | 0400;
    } else {
        info->st.st_mode = S_IFREG | 0666;
    }
    info->st.st_blksize = PAGESZ;

    // Count the file size and pages
    int npages = 0;
    while (l->next_rda != 0) {
        const int filepage = rda_to_vda(l->next_rda);
        l = page_label(filepage);
        info->st.st_size += l->nbytes;
        npages++;
    }
    info->st.st_blocks = (info->st.st_size + PAGESZ - 1) / PAGESZ;

    // Create the list of file pages for random access
    info->npages = npages;
    info->pages = (word *)malloc(sizeof(word) * npages);
    my_assert(info->pages != 0, "%s: malloc(%u) failed for %s\n", __func__, sizeof(word) * npages, &fn[1]);
    if (!info->pages) {
        delete info;
        return -ENOMEM;
    }

    l = page_label(leader_page_vda);
    npages = 0;
    while (l->next_rda != 0) {
        const int filepage = rda_to_vda(l->next_rda);
        info->pages[npages++] = filepage;
        l = page_label(filepage);
    }

    // Fill more struct stat info
    alto_time_to_time(lp->created, &info->st.st_ctime);
    alto_time_to_time(lp->written, &info->st.st_mtime);
    alto_time_to_time(lp->read, &info->st.st_atime);
    info->st.st_nlink = 1;

    // Make a new entry in the parent's list of children
    size_t size = (parent->nchildren + 1) * sizeof(*parent->children);
    parent->children = (afs_fileinfo_t **) realloc(parent->children, size);
    my_assert(parent->children != 0, "%s: realloc(...,%u) failed for %s\n", __func__, size, &fn[1]);
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
 * @brief Read the page filepage into the the buffer at data
 * @param filepage page number
 * @param data buffer of PAGESZ bytes
 */
void read_page(page_t filepage, char* data)
{
    char *src = (char *)&disk[filepage].data;
    for (int i = 0; i < PAGESZ; i++)
        data[i] = src[i ^ little.l];
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

    my_assert_or_die ((fa->char_pos & 1) == 0, "getword called on odd byte boundary\n");

    if (fa->char_pos >= l->nbytes) {
        if (l->next_rda == 0 || l->nbytes < PAGESZ)
            return -1;
        fa->vda = rda_to_vda(l->next_rda);
        l = page_label(fa->vda);
        fa->page_number += 1;
        fa->char_pos = 0;
    }
    my_assert_or_die (fa->page_number == l->filepage,
		"disk corruption - expected vda %d to be filepage %d\n",
		fa->vda, l->filepage);
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
    my_assert_or_die ((fa->char_pos & 1) == 0, "putword called on odd byte boundary\n");
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

/* make sure that each page header refers to itself */
int verify_headers()
{
    int i, ok, last;
    ok = 1;

    return ok;

    last = doubledisk ? NPAGES * 2 : NPAGES;
    for (i = 0; i < last; i += 1)
        ok &= my_assert(disk[i].pagenum == rda_to_vda (disk[i].header[1]),
			"page %04x header doesn't match: %04x %04x\n",
			disk[i].pagenum, disk[i].header[0], disk[i].header[1]);
    return ok;
}

/**
 * @brief Verify the disk descript file DiskDescriptor
 * Check single or double disks
 */
int validate_disk_descriptor()
{
    int ddlp, i, page, nfree, ok, last;
    afs_leader_t* lp;
    afs_label_t* l;
    afs_fa_t fa;

    /* Locate DiskDescriptor and copy it into the global data structure */
    ddlp = find_file("DiskDescriptor");
    ok = my_assert(ddlp != -1, "Can't find DiskDescriptor\n");
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
        ok &= my_assert (khd.nDisks == 2, "Expect double disk system\n");
        ok &= my_assert (khd.nTracks == 203, "KDH tracks != 203\n");
        ok &= my_assert (khd.nHeads == 2, "KDH heads != 2\n");
        ok &= my_assert (khd.nSectors == 12, "KDH sectors != 12\n");
        ok &= my_assert (khd.def_versions_kept == 0, "defaultVersions != 0\n");
    } else {
        /* for single disk systems */
        ok &= my_assert (khd.nDisks == 1, "Expect single disk system\n");
        ok &= my_assert (khd.nTracks == 203, "KDH tracks != 203\n");
        ok &= my_assert (khd.nHeads == 2, "KDH heads != 2\n");
        ok &= my_assert (khd.nSectors == 12, "KDH sectors != 12\n");
        ok &= my_assert (khd.def_versions_kept == 0, "defaultVersions != 0\n");
    }

    /* Count free pages in bit table */
    nfree = 0;
    last = doubledisk ? NPAGES * 2 : NPAGES;
    for (page = 0; page < last; page += 1)
        nfree += getBT(page) ^ 1;
    ok &= my_assert (nfree == khd.free_pages,
        "Bit table count %d doesn't match KDH free pages %d\n", nfree, khd.free_pages);

    /* Count free pages in actual image */
    nfree = 0;
    for (page = 0; page < last; page += 1)
        nfree += is_free_page(page);

    ok &= my_assert ((nfree == khd.free_pages),
        "Actual free page count %d doesn't match KDH value %d\n",
        nfree, khd.free_pages);
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

/****************************/
/* general support routines */
/****************************/
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

void swabit(char *data, int count)
{
    word junk, *d;
    my_assert_or_die ((count & 1) == 0 && ((size_t)data & 1) == 0,
		"%s: Called with unaligned values\n", __func__);
    count /= 2;
    d = (word *) data;
    while (count--) {
        junk = *d;
        junk = (junk >> 8) | (junk << 8);
        *d++ = junk;
    }
}

/**
 * @brief Copy a filename file src to dst
 * In src the first byte contains the length of the string (Pascal style).
 * Every filename ends with a dot (.) which we remove here.
 *
 * @param dst destination buffer of FNLEN characters
 * @param src pointer to a Alto file system filename
 */
void copyfilename(char* dst, const char* src)
{
    int length = src[little.l];
    if (length < 0 || length >= FNLEN)
        length = FNLEN - 1;
    for (int i = 0; i < length; i++)
        dst[i] = src[i ^ little.l];
    /* erase closing '.' */
    length--;
    if ('.' == dst[length])
        dst[length] = '\0';
    else
        dst[++length] = '\0';
}

int statvfs_kdh(struct statvfs* vfs)
{
	memset(vfs, 0, sizeof(*vfs));
	if (!root_dir)
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
	vfs->f_ffree = 0;
	vfs->f_namemax = FNLEN-2;
	return 0;
}

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
    m_little(),
    m_kdh(),
    m_bit_count(0),
    m_bit_table(),
    m_disk_descriptor_dirty(false),
    m_sysdir(),
    m_sysdir_dirty(false),
    m_files(),
    m_disk(),
    m_doubledisk(false),
    m_dp0name(),
    m_dp1name(),
    m_verbose(0),
    m_root_dir(0)
{
    /**
     * The union's little.e is initialized to 1
     * Then little.l can be used for xor of words which need
     * a byte swap on little endian machines.
     */
    m_little.e = 1;
}

AltoFS::AltoFS(const char* filename) :
    m_little(),
    m_kdh(),
    m_bit_count(0),
    m_bit_table(),
    m_disk_descriptor_dirty(false),
    m_sysdir(),
    m_sysdir_dirty(false),
    m_files(),
    m_disk(),
    m_doubledisk(false),
    m_dp0name(),
    m_dp1name(),
    m_verbose(0),
    m_root_dir(0)
{
    /**
     * The union's little.e is initialized to 1
     * Then little.l can be used for xor of words which need
     * a byte swap on little endian machines.
     */
    m_little.e = 1;
    read_disk_file(filename);
    // verify_headers();
    if (!validate_disk_descriptor())
        fix_disk_descriptor();
    make_fileinfo();
    read_sysdir();
}

AltoFS::~AltoFS()
{
    if (m_disk_descriptor_dirty) {
        int res = save_disk_descriptor();
        my_assert(res >= 0,
            "%s: Could not save the SysDir array.\n",
            __func__);
    }
    if (m_sysdir_dirty) {
        int res = save_sysdir();
        my_assert(res >= 0,
            "%s: Could not save the SysDir array.\n",
            __func__);
    }
    save_disk_file();
    delete m_root_dir;
    m_root_dir = 0;
}

void AltoFS::log(int verbosity, const char* format, ...)
{
    if (verbosity > m_verbose)
        return;
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
}

int AltoFS::verbosity() const
{
    return m_verbose;
}

void AltoFS::setVerbosity(int verbosity)
{
    m_verbose = verbosity;
}

/**
 * @brief Return a pointer to the afs_leader_t for page vda.
 * @param vda page number
 * @return pointer to afs_leader_t
 */
afs_leader_t* AltoFS::page_leader(page_t vda)
{
    afs_leader_t* lp = (afs_leader_t *)&m_disk[vda].data[0];

#if defined(DEBUG)
    if (m_verbose > 3 && lp->proplength > 0) {
        if (is_page_free(vda))
            return lp;
        if (lp->filename[m_little.l] == 0 || lp->filename[m_little.l] > FNLEN)
            return lp;
        if (lp->dir_fp_hint.blank != 0)
            return lp;
        const word offs0 = 3 * sizeof(afs_time_t) + FNLEN;
        if (lp->propbegin < offs0)
            return lp;

        static afs_leader_t empty = {0,};
        byte size = lp->proplength * sizeof(word);
        if (lp->propbegin + size > sizeof(lp->leader_props))
            size = sizeof(lp->leader_props) - lp->propbegin;
        if (memcmp(lp->leader_props, empty.leader_props, sizeof(lp->leader_props))) {
            std::string fn = filename_to_string(lp->filename);
            log(3,"%s: leader props for page %ld (%s)\n", __func__, vda, fn.c_str());
            log(3,"%s:   propbegin      : %u\n", __func__, lp->propbegin);
            log(3,"%s:   proplength     : %u (%u)\n", __func__, size, lp->proplength);
            log(3,"%s:   non-zero data found:\n", __func__);
            dump_memory((char *)lp->leader_props, size);
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
    return (afs_label_t *)&m_disk[vda].label[0];
}

/**
 * @brief Read a disk file or two of them separated by comma
 * @param name filename of the disk image(s)
 * @return 0 on succes, or -ENOSYS, -EBADF etc. on error
 */
int AltoFS::read_disk_file(std::string name)
{
    int ok;

    ok = my_assert(sizeof(afs_leader_t) == PAGESZ,
        "%s: sizeof(afs_leader_t) is not %d",
        __func__, PAGESZ);
    if (!ok)
        return -ENOENT;

    int pos = name.find(',');
    if (pos > 0) {
        m_dp0name = std::string(name, 0, pos);
        m_dp1name = std::string(name, pos + 1);
        m_doubledisk = true;
    } else {
        m_dp0name = name;
        m_dp1name.clear();
        m_doubledisk = false;
    }

    m_disk.resize(2*NPAGES);
    my_assert_or_die(m_disk.data() != NULL,
        "%s: disk resize(%d) failed",
        __func__, 2*NPAGES);

    ok = read_single_disk(m_dp0name, &m_disk[0]);
    if (ok && m_doubledisk) {
        ok = read_single_disk(m_dp1name, &m_disk[NPAGES]);
    }
    return ok ? 0 : -ENOENT;
}

/**
 * @brief Read a single file to the in-memory disk space
 * @param name file name
 * @param diskp pointer to the starting disk page
 */
int AltoFS::read_single_disk(std::string name, afs_page_t* diskp)
{
    FILE *infile;
    size_t bytes;
    size_t totalbytes = 0;
    size_t total = NPAGES * sizeof (afs_page_t);
    char *dp = (char *)diskp;
    int ok = 1;
    int use_pclose = 0;

    log(1,"%s: Reading disk image '%s'\n", __func__, name.c_str());
    // We conclude the disk image is compressed if the name ends with .Z
    int pos = name.find(".Z");
    if (pos > 0) {
        char* cmd = new char[name.size() + 10];
        sprintf(cmd, "zcat %s", name.c_str());
        infile = popen(cmd, "r");
        delete[] cmd;
        my_assert_or_die(infile != NULL,
            "%s: popen failed on zcat %s\n",
            __func__, name.c_str());
        use_pclose = 1;
    } else {
        infile = fopen (name.c_str(), "rb");
        my_assert_or_die(infile != NULL,
            "%s: fopen failed on Alto disk image file %s\n",
            __func__, name.c_str());
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

int AltoFS::save_disk_file()
{
    int res = save_single_disk(m_dp0name + "~", &m_disk[0]);
    if (m_doubledisk)
        res &= save_single_disk(m_dp1name + "~", &m_disk[NPAGES]);
    return res;
}

int AltoFS::save_single_disk(std::string name, afs_page_t* diskp)
{
    FILE *outfile;
    size_t bytes;
    size_t totalbytes = 0;
    size_t total = NPAGES * sizeof (afs_page_t);
    char *dp = (char *)diskp;
    int ok = 1;

    // We conclude the disk image is compressed if the name ends with .Z~ or .Z
    int pos = name.find(".Z~");
    if (pos < 0)
        pos = name.find(".Z");
    if (pos > 0) {
        // Remove the .Z~ or .Z extension, as we will save uncompressed
        name.erase(pos, pos);
    }
    log(1,"%s: Writing disk image '%s'\n", __func__, name.c_str());

    outfile = fopen (name.c_str(), "wb");
    my_assert_or_die(outfile != NULL,
        "%s: fopen failed on Alto disk image file %s\n",
        __func__, name.c_str());

    while (totalbytes < total) {
        bytes = fwrite(dp, sizeof (char), total - totalbytes, outfile);
        dp += bytes;
        totalbytes += bytes;
        ok = my_assert(!ferror(outfile),
            "%s: Disk read failed: %d bytes written instead of %d\n",
            __func__, totalbytes, total);
        if (!ok)
            break;
    }
    fclose(outfile);
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
        log(0,"%04lx:", row * 8);
        for (size_t col = 0; col < 8; col++) {
            size_t offs = row * 8 + col;
            if (offs < nwords) {
                unsigned char h = data[(2*offs+0) ^ m_little.l];
                unsigned char l = data[(2*offs+1) ^ m_little.l];
                log(0," %02x%02x", h, l);
            } else {
                log(0,"     ");
            }
        }

        for (size_t col = 0; col < 8; col++) {
            size_t offs = row * 8 + col;
            if (offs < nwords) {
                unsigned char h = data[(2*offs+0) ^ m_little.l];
                unsigned char l = data[(2*offs+1) ^ m_little.l];
                str[(col * 2) + 0] = (isprint(h)) ? h : '.';
                str[(col * 2) + 1] = (isprint(l)) ? l : '.';
            } else {
                str[(col * 2) + 0] = ' ';
                str[(col * 2) + 1] = ' ';
            }
        }
        str[16] = '\0';
        log(0,"  %16s\n", str);
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

/**
 * @brief Return the length of a file by scanning its pages
 * @param leader_page_vda page number of the leader page
 * @return size in bytes
 */
size_t AltoFS::file_length(page_t leader_page_vda)
{
    size_t length = 0;
    page_t page = leader_page_vda;
    afs_label_t* label;

    while (page != 0) {
        label = page_label(page);
        length += label->nbytes;
        if (label->nbytes < PAGESZ)
            break;
        page = rda_to_vda(label->next_rda);
    }
    // Subtract the leader page
    length -= PAGESZ;
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
 * Scan for pages near the given page, always alternatinly looking
 * at a page after and a page before the given page.
 *
 * @param filepage previous page VDA where this page is chained to
 * @return new page RDA, or 0 if no free page is found
 */
page_t AltoFS::alloc_page(page_t page)
{
    // Won't find a free page anyway
    if (0 == m_kdh.free_pages)
        return 0;

    const page_t maxpage = m_kdh.disk_bt_size * sizeof(word);
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
        log(0,"%s: no free page found\n", __func__);
#endif
        return 0;
    }

    m_kdh.free_pages -= 1;
    setBT(page, 1);
    zero_page(page);

    afs_label_t* lthis = page_label(page);
    memset(lthis, 0, sizeof(*lthis));
    // link pages
    if (lprev)
        lprev->next_rda = vda_to_rda(page);
    lthis->prev_rda = vda_to_rda(prev_vda);
    lthis->nbytes = 0;

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
        lthis->fid_id = m_kdh.last_sn.sn[m_little.l];
        m_kdh.last_sn.sn[m_little.l] += 1;
        lthis->nbytes = PAGESZ;
    }

#if defined(DEBUG)
    if (lprev) {
        log(2,"%s: prev page label (%ld)\n", __func__, prev_vda);
        log(2,"%s:   next_rda    : 0x%04x (vda=%ld)\n", __func__,
            lprev->next_rda, rda_to_vda(lprev->next_rda));
        log(2,"%s:   prev_rda    : 0x%04x (vda=%ld)\n", __func__,
            lprev->prev_rda, rda_to_vda(lprev->prev_rda));
        log(2,"%s:   unused1     : %u\n", __func__, lprev->unused1);
        log(2,"%s:   nbytes      : %u\n", __func__, lprev->nbytes);
        log(2,"%s:   filepage    : %u\n", __func__, lprev->filepage);
        log(2,"%s:   fid_file    : %#x\n", __func__, lprev->fid_file);
        log(2,"%s:   fid_dir     : %#x\n", __func__, lprev->fid_dir);
        log(2,"%s:   fid_id      : %#x\n", __func__, lprev->fid_id);
    }
    log(2,"%s: next page label (%ld)\n", __func__, page);
    log(2,"%s:   next_rda    : 0x%04x (vda=%ld)\n", __func__,
        lthis->next_rda, rda_to_vda(lthis->next_rda));
    log(2,"%s:   prev_rda    : 0x%04x (vda=%ld)\n", __func__,
        lthis->prev_rda, rda_to_vda(lthis->prev_rda));
    log(2,"%s:   unused1     : %u\n", __func__, lthis->unused1);
    log(2,"%s:   nbytes      : %u\n", __func__, lthis->nbytes);
    log(2,"%s:   filepage    : %u\n", __func__, lthis->filepage);
    log(2,"%s:   fid_file    : %#x\n", __func__, lthis->fid_file);
    log(2,"%s:   fid_dir     : %#x\n", __func__, lthis->fid_dir);
    log(2,"%s:   fid_id      : %#x\n", __func__, lthis->fid_id);
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
    last = m_doubledisk ? NPAGES * 2 : NPAGES;
    for (i = 0; i < last; i += 1) {
        l = page_label(i);
        lp = page_leader (i);
        // First file page and actually a file?
        if (l->filepage == 0 && l->fid_file == 1) {
            std::string fn = filename_to_string(lp->filename);
            if (fn.compare(name) == 0)
                return i;
        }
    }
    my_assert(0, "%s: File %s not found\n", __func__, name);
    return -1;
}

/**
 * @brief Scan the SysDir file and build an array of afs_dv_t entries.
 * @param ppdv pointer to a pointer to afs_dv_t
 * @param pcount pointer to a size_t to receive the number of entries
 * @return 0 on success, or -ENOENT etc. otherwise
 */
int AltoFS::read_sysdir()
{
    if (m_sysdir_dirty)
        save_sysdir();

    m_files.clear();
    afs_fileinfo* info = find_fileinfo("SysDir");
    my_assert_or_die(info != NULL, "%s: The file SysDir was not found!", __func__);
    if (info == NULL)
        return -ENOENT;

    size_t sdsize = info->statSize();
    // Allocate sysdir with slack for one extra afs_dv_t
    m_sysdir.resize(sdsize + sizeof(afs_dv_t));

    read_file(info->leader_page_vda(), m_sysdir.data(), sdsize, false);
    if (m_little.l)
        swabit((char *)m_sysdir.data(), sdsize);

    const afs_dv_t* end = (afs_dv_t *)(m_sysdir.data() + sdsize);
    afs_dv_t* pdv = (afs_dv_t *)m_sysdir.data();
    size_t count = 0;
    while (pdv < end) {
        byte type = pdv->typelength[1];     // FIXME: little.l?
        byte length = pdv->typelength[0];   // FIXME: little.h?

        // End of directory if length is 0?
        if (0 == length)
            break;

        // End of directory if fnlen is zero (?)
        byte fnlen = pdv->filename[m_little.l];
        if (fnlen == 0 || fnlen > FNLEN)
            break;

        // length is always word aligned
        size_t nsize = (fnlen | 1) + 1;
        size_t esize = sizeof(*pdv) - sizeof(pdv->filename) + nsize;

        // Skip if deleted entry (?)
        if (4 == type) {

            afs_dv dv(*pdv);
            log(3,"%s:* directory entry    : @%u\n", __func__, (word)((char *)pdv - m_sysdir.data()));
            log(3,"%s:  type               : %u\n", __func__, dv.data.typelength[1]); // FIXME: little.l?
            log(3,"%s:  length             : %u\n", __func__, dv.data.typelength[0]); // FIXME: little.h?
            log(3,"%s:  fileptr.fid_dir    : %#x\n", __func__, dv.data.fileptr.fid_dir);
            log(3,"%s:  fileptr.serialno   : %#x\n", __func__, dv.data.fileptr.serialno);
            log(3,"%s:  fileptr.version    : %#x\n", __func__, dv.data.fileptr.version);
            log(3,"%s:  fileptr.blank      : %#x\n", __func__, dv.data.fileptr.blank);
            log(3,"%s:  fileptr.leader_vda : %u\n", __func__, dv.data.fileptr.leader_vda);
            log(3,"%s:  filename length    : %u\n", __func__, dv.data.filename[m_little.l]);
            std::string fn = filename_to_string(dv.data.filename);
            log(2,"%s:  filename           : %s.\n", __func__, fn.c_str());
            m_files.resize(count+1);
            m_files[count++] = dv;
        }
        pdv = (afs_dv_t*)((char *)pdv + esize);
    }

    size_t eod = (size_t)((char *)pdv - m_sysdir.data());
    log(1,"%s: SysDir usage is %lu/%lu bytes\n", __func__, eod, sdsize);

#if defined(DEBUG)
    if (m_verbose > 3)
        dump_memory(m_sysdir.data(), eod);
#endif

#if 1
#endif
    return 0;
}

/**
 * @brief Save an array of afs_dv_t entries to the SysDir pages.
 * @param array pointer to array of afs_dv_t entries
 * @param count number of entries
 * @return 0 on success, or -ENOENT if SysDir is not found
 */
int AltoFS::save_sysdir()
{
    afs_fileinfo* info = find_fileinfo("SysDir");
    my_assert_or_die(info != NULL, "%s: The file SysDir was not found!", __func__);
    if (info == NULL)
        return -ENOENT;

    int res = 0;

    size_t sdsize = info->statSize();

    const afs_dv_t* end = (afs_dv_t *)(m_sysdir.data() + sdsize);
    afs_dv_t* pdv = (afs_dv_t *)m_sysdir.data();
    size_t idx = 0;
    while (idx < m_files.size() && pdv < end) {
        const afs_dv* dv = &m_files[idx];
        byte fnlen = dv->data.filename[m_little.l];
        // length is always word aligned
        size_t nsize = (fnlen | 1) + 1;
        size_t esize = sizeof(*pdv) - sizeof(pdv->filename) + nsize;
        memcpy(pdv, &dv->data, esize);
        pdv = (afs_dv_t*)((char *)pdv + esize);
        idx++;
    }

    if (idx < m_files.size()) {
        // We need to increase sdsize and write the last entry
        const afs_dv* dv = &m_files[idx];
        byte fnlen = dv->data.filename[m_little.l];
        // length is always word aligned
        size_t nsize = (fnlen | 1) + 1;
        size_t esize = sizeof(*pdv) - sizeof(pdv->filename) + nsize;
        size_t offs = (size_t)((char*)pdv - m_sysdir.data());
        if (offs + esize >= m_sysdir.size()) {
            // Resize m_sysdir
            m_sysdir.resize(offs + esize + 1);
            info->setStatSize(offs + esize + 1);
            m_sysdir[offs + esize] = '\0';
        }
        memcpy(pdv, &dv->data, esize);
        pdv = (afs_dv_t*)((char *)pdv + esize);
    }

    size_t eod = (size_t)((char *)pdv - m_sysdir.data());
    log(1,"%s: SysDir usage is %lu/%lu bytes\n", __func__, eod, sdsize);
    if (eod > sdsize) {
        sdsize = eod;
        m_sysdir.resize(sdsize+1);
        m_sysdir[sdsize] = '\0';
        info->setStatSize(sdsize);
    }

#if defined(DEBUG)
    if (m_verbose > 3)
        dump_memory(m_sysdir.data(), eod);
#endif
    if (m_little.l) {
        std::vector<char> sysdir(m_sysdir);
        swabit(sysdir.data(), sdsize);
        size_t written = write_file(info->leader_page_vda(), sysdir.data(), eod, false);
        if (written != eod)
            res = -ENOSPC;
    } else {
        size_t written = write_file(info->leader_page_vda(), m_sysdir.data(), eod, false);
        if (written != eod)
            res = -ENOSPC;
    }
    m_sysdir_dirty = 0 == res;
    return res;
}

int AltoFS::save_disk_descriptor()
{
    int ddlp;
    afs_label_t* l;
    afs_fa_t fa;

    // Locate DiskDescriptor and copy it into the global data structure
    ddlp = find_file("DiskDescriptor");
    my_assert_or_die(ddlp != -1, "%s: Can't find DiskDescriptor\n", __func__);

    l = page_label(ddlp);

    fa.vda = rda_to_vda(l->next_rda);
    memcpy(&m_disk[fa.vda].data[0], &m_kdh, sizeof(m_kdh));

    // Now copy the bit table from m_bit_table onto the disk
    fa.page_number = 1;
    fa.char_pos = sizeof(m_kdh);
    for (word i = 0; i < m_kdh.disk_bt_size; i++)
        putword(&fa, m_bit_table[i]);

    m_disk_descriptor_dirty = 0;
    return 0;
}

/**
 * @brief Remove a file name in the SysDir file and write it back
 * @param name file name to look for (without trailing dot)
 * @return 0 on success, or -ENOENT on error
 */
int AltoFS::remove_sysdir_entry(std::string name)
{
    log(1,"%s: searching for '%s'\n", __func__, name.c_str());

    for (size_t idx = 0; idx < m_files.size(); idx++) {
        afs_dv* dv = &m_files[idx];
        std::string fn = filename_to_string(dv->data.filename);
        if (fn != name)
            continue;
        // Just mark this entry as unused
        // FIXME: little.l instead of 1?
        dv->data.typelength[1] = 0;
        log(2,"%s: found '%s' at index %ld\n", __func__, name.c_str(), idx);
        m_sysdir_dirty = true;
        return 0;
    }

    log(1,"%s: Could not find '%s' in SysDir!\n", __func__, name.c_str());
    return -ENOENT;
}

/**
 * @brief Rename a file name in the SysDir file and write it back
 * @param name file name to look for (without trailing dot)
 * @param newname new file name (without trailing dot)
 * @return 0 on success, or -ENOENT on error
 */
int AltoFS::rename_sysdir_entry(std::string name, std::string newname)
{
    // Never allow renaming SysDir or DiskDescriptor
    if (0 == name.compare("SysDir") || 0 == name.compare("DiskDescriptor"))
        return -EPERM;
    if (name[0] == '/')
        name.erase(0, 1);
    if (newname[0] == '/')
        newname.erase(0, 1);

    log(1,"%s: renaming '%s' to '%s'\n", __func__, name.c_str(), newname.c_str());

    // Assume failure
    int res = -ENOENT;
    for (size_t idx = 0; idx < m_files.size(); idx++) {
        afs_dv* dv = &m_files[idx];
        std::string fn = filename_to_string(dv->data.filename);
        if (fn != name)
            continue;

        // Change the name of this array entry
        string_to_filename(dv->data.filename, newname);

        fn = filename_to_string(dv->data.filename);
        log(1,"%s:  new filename       : %s.\n", __func__, fn.c_str());

        // FIXME: do we need to sort the array by name?
        m_sysdir_dirty = true;
        break;
    }

    return res;
}

/**
 * @brief Delete a file from the tree and free its chain's bits in the BT
 * @param info pointer to afs_fileinfo_t describing the file
 * @return 0 on success, or -EPERM, -ENOMEM etc. on error
 */
int AltoFS::unlink_file(afs_fileinfo* info)
{
    afs_leader_t* lp = page_leader(info->leader_page_vda());
    std::string fn = filename_to_string(lp->filename);

    // Never allow deleting SysDir or DiskDescriptor
    if (0 == fn.compare("SysDir") || 0 == fn.compare("DiskDescriptor"))
        return -EPERM;

    // FIXME: What needs to be zapped?
    memset(lp->filename, 0, sizeof(lp->filename));
    memset(&lp->dir_fp_hint, 0, sizeof(lp->dir_fp_hint));
    memset(&lp->last_page_hint, 0, sizeof(lp->last_page_hint));

    page_t page = info->leader_page_vda();
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
    afs_fileinfo* parent = info->parent();
    if (!parent->remove(info)) {
        log(0, "%s: Could not remove child (%p) from parent (%p).\n",
            __func__, (void*)info, (void*)parent);
    }

    // Clean up the leader page label
    page = info->leader_page_vda();
    l = page_label(page);
    l->next_rda = 0;
    l->prev_rda = 0;
    l->unused1 = 0;
    l->fid_file = 0xffff;
    l->fid_dir = 0xffff;
    l->fid_id = 0xffff;

    return remove_sysdir_entry(fn);
}

/**
 * @brief Rename file in the tree and in SysDir
 * @param info pointer to afs_fileinfo_t describing the file
 * @param newname new filename
 * @return 0 on success, or -ENOENT on error
 */
int AltoFS::rename_file(afs_fileinfo* info, std::string newname)
{
    afs_leader_t* lp = page_leader(info->leader_page_vda());
    std::string fn = filename_to_string(lp->filename);

    // Skip leading directory (we have only root)
    if (newname[0] == '/')
        newname.erase(0,1);

    int ok = my_assert(newname.length() < FNLEN-2,
        "%s: newname too long for '%s' -> '%s'\n",
        __func__, info->name().c_str(), newname.c_str());
    if (!ok)
        return -EINVAL;

    info->rename(newname);

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
int AltoFS::truncate_file(afs_fileinfo* info, off_t offset)
{
    afs_leader_t* lp = page_leader(info->leader_page_vda());
    afs_label_t* l = page_label(info->leader_page_vda());
    const word id = l->fid_id;

    size_t size = info->statSize() - offset;
    page_t page = rda_to_vda(l->next_rda);
    page_t last_page = -1;
    word last_bytes = 0;

    off_t offs = 0;
    while (page && size > 0) {
        l = page_label(page);
        if (offs >= offset) {

            // free this page
#if defined(DEBUG)
            log(3,"%s: offs=0x%06lx page=%-5ld (free page)\n",
                __func__, offs, page);
#endif
            free_page(page, id);
            size -= l->nbytes;
            offs += PAGESZ;
            if (-1 == last_page) {
                last_page = page;
                last_bytes = 0;
            }

        } else if (offs + PAGESZ > offset) {

            // shrink this page to the remaining bytes
            l->nbytes = offset - offs;
#if defined(DEBUG)
            log(3,"%s: offs=0x%06lx page=%-5ld (shrink to 0x%03x bytes)\n",
                __func__, offs, page, l->nbytes);
#endif
            size -= l->nbytes;
            offs += l->nbytes;
            last_page = page;
            last_bytes = l->nbytes;
            break;  // we're done

        } else if (0 == l->next_rda) {

            // allocate a new page
            page = alloc_page(page);
            if (0 == page) {
                // No free page found
                info->setStatSize(static_cast<size_t>(offs));
                return -ENOSPC;
            }
#if defined(DEBUG)
            log(3,"%s: offs=0x%06lx page=%-5ld (allocated new page)\n",
                __func__, offs, page);
#endif
            if (offs + PAGESZ < offset) {
                l->nbytes = PAGESZ;
            } else {
                l->nbytes = (offset - offs) % PAGESZ;
            }
            last_page = page;
            last_bytes = l->nbytes;
            // FIXME: should we fill it with zeroes?

            if (l->nbytes < PAGESZ)
                break;

        } else {
#if defined(DEBUG)
            log(4,"%s: offs=0x%06lx page=%-5ld (seeking to 0x%06lx)\n",
                __func__, offs, page, offset);
#endif
            size -= PAGESZ;
            offs += PAGESZ;
        }
        page = rda_to_vda(l->next_rda);
    }
    lp->last_page_hint.vda = last_page;
    lp->last_page_hint.page_number = last_page % NPAGES;
    lp->last_page_hint.char_pos = last_bytes;
    info->setStatSize(offs);

    return 0;
}

/**
 * @brief Create a new file with SysDir entry, leader page and zero length first page
 * @param path filename including the path (/)
 * @return 0 on success, or -EBADF, -ENOSPC etc. otherwise
 */
int AltoFS::create_file(std::string path)
{
    log(1,"%s: path=%s\n", __func__, path.c_str());

    afs_fileinfo* info = find_fileinfo(path);
    if (info)
        return -EEXIST;

    // Skip leading directory (we have only root)
    if (path[0] == '/')
        path.erase(0, 1);

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

    page_t page0 = alloc_page(page);
    my_assert(page0 != 0,
        "%s: Disk full when allocating first filepage of %s\n",
        __func__, path.c_str());
    if (page0 == 0)
        return -ENOSPC;

    // Update the last page hint
    lp->last_page_hint.vda = page0;
    lp->last_page_hint.page_number = page0 % NPAGES;
    lp->last_page_hint.char_pos = 0;

#if 1
    log(0, "%s: created                    : %s\n", __func__, altotime_to_str(lp->created).c_str());
    log(0, "%s: written                    : %s\n", __func__, altotime_to_str(lp->written).c_str());
    log(0, "%s: read                       : %s\n", __func__, altotime_to_str(lp->read).c_str());
    log(0, "%s: filename                   : %s\n", __func__, filename_to_string(lp->filename).c_str());
    log(0, "%s: leader_props[]             : ...\n", __func__);
    log(0, "%s: spare[]                    : ...\n", __func__);
    log(0, "%s: proplength                 : %u\n", __func__, lp->proplength);
    log(0, "%s: propbegin                  : %u\n", __func__, lp->propbegin);
    log(0, "%s: change_SN                  : %u\n", __func__, lp->change_SN);
    log(0, "%s: consecutive                : %u\n", __func__, lp->consecutive);
    log(0, "%s: dir_fp_hint.fid_dir        : %u\n", __func__, lp->dir_fp_hint.fid_dir);
    log(0, "%s: dir_fp_hint.serialno       : %#x\n", __func__, lp->dir_fp_hint.serialno);
    log(0, "%s: dir_fp_hint.version        : %u\n", __func__, lp->dir_fp_hint.version);
    log(0, "%s: dir_fp_hint.blank          : %u\n", __func__, lp->dir_fp_hint.blank);
    log(0, "%s: dir_fp_hint.leader_vda     : %u\n", __func__, lp->dir_fp_hint.leader_vda);
    log(0, "%s: last_page_hint.vda         : %u\n", __func__, lp->last_page_hint.vda);
    log(0, "%s: last_page_hint.page_number : %u\n", __func__, lp->last_page_hint.page_number);
    log(0, "%s: last_page_hint.char_pos    : %u\n", __func__, lp->last_page_hint.char_pos);
#endif

    // Find position in the SysDir array where to insert
    std::vector<afs_dv>::iterator it;
    int idx = 0;
    for (it = m_files.begin(); it != m_files.end(); it++) {
        afs_dv* dv = &m_files[idx];
        std::string fn = filename_to_string(dv->data.filename);
        if (fn > path)
            break;
        idx++;
    }

    afs_dv* dv = &m_files[idx];
    if (it == m_files.end()) {
        // Not the last entry, so make room
        log(2,"%s: insert entry at SysDir pos=%d/%ld\n", __func__, idx, m_files.size());
        // Insert a new entry at idx
        m_files.insert(it, *dv);
    } else {
        log(2,"%s: insert entry after SysDir at pos=%ld\n", __func__, m_files.size());
        m_files[idx] = *dv;
    }

    dv = &m_files[idx];
    dv->data.typelength[0] = path.length();                 // whatever "length" this is
    dv->data.typelength[1] = 4;                             // this is an existing file
    dv->data.fileptr.fid_dir = 0x0000;                      // this is not a directory;
    dv->data.fileptr.serialno = lp->dir_fp_hint.serialno;   // FIXME: serialno == id?
    dv->data.fileptr.version = 1;                           // The version is always == 1
    dv->data.fileptr.blank = 0x0000;                        // And blank is, well, blank
    dv->data.fileptr.leader_vda = page;                     // store the leader page
    string_to_filename(dv->data.filename, path);
    m_sysdir_dirty = true;

    return make_fileinfo_file(m_root_dir, page);
}

int AltoFS::make_fileinfo()
{
    if (m_root_dir) {
        delete m_root_dir;
        m_root_dir = 0;
    }

    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_mode = S_IFDIR | 0755;
    st.st_nlink = 2;            // Set number of links = 2 for "." and ".."
    st.st_blksize = PAGESZ;
    st.st_blocks = 0;
    m_root_dir = new afs_fileinfo(0, "/", st, 0);
    my_assert(m_root_dir != 0, "%s: Allocating new root_dir failed\n", __func__);
    if (!m_root_dir)
        return -ENOMEM;

    const int last = m_doubledisk ? NPAGES * 2 : NPAGES;
    for (page_t page = 0; page < last; page++) {
        afs_label_t* l = page_label(page);
        // First page of a file?
        if (l->filepage != 0)
            continue;
        // Marked as a regular file?
        if (l->fid_file != 1)
            continue;
        // Previous RDA is 0?
        if (l->prev_rda != 0)
            continue;
        const int res = make_fileinfo_file(m_root_dir, page);
        if (res < 0) {
            log(0, "%s: make_fileinfo_file() for page %ld failed\n", __func__, page);
            return res;
        }
    }
    return 0;
}

int AltoFS::make_fileinfo_file(afs_fileinfo* parent, int leader_page_vda)
{
    afs_label_t* l = page_label(leader_page_vda);
    afs_leader_t* lp = page_leader(leader_page_vda);

    my_assert_or_die(l->filepage == 0, "%s: Page %d is not a leader page!\n", __func__, leader_page_vda);

    std::string fn = filename_to_string(lp->filename);
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = leader_page_vda;    // Use the leader page as inode
    if (l->fid_dir == 0x8000) {
        // A directory (SysDir) is a file which can't be modified
        st.st_mode = S_IFREG | 0400;
    } else if (0 == fn.compare("DiskDescriptor")) {
        // Don't allow DiskDescriptor to be written to
        st.st_mode = S_IFREG | 0400;
    } else {
        st.st_mode = S_IFREG | 0666;
    }
    st.st_blksize = PAGESZ;

    // Fill more struct stat info
    altotime_to_time(lp->created, &st.st_ctime);
    altotime_to_time(lp->written, &st.st_mtime);
    altotime_to_time(lp->read, &st.st_atime);
    st.st_nlink = 0;

    afs_fileinfo* info = new afs_fileinfo(parent, fn, st, leader_page_vda);
    my_assert(info != 0, "%s: new afs_fileinfo_t failed for %s\n", __func__, fn.c_str());
    if (!info)
        return -ENOMEM;

    // Count the file size and pages
    size_t npages = 0;
    size_t size = 0;
    while (l->next_rda != 0) {
        const page_t filepage = rda_to_vda(l->next_rda);
        l = page_label(filepage);
        size += l->nbytes;
        npages++;
    }
    info->setStatSize(size);
    info->setStatBlocks(npages);

#if defined(DEBUG)
    struct tm tm_ctime;
    altotime_to_tm(lp->created, tm_ctime);
    char ctime[40];
    strftime(ctime, sizeof(ctime), "%Y-%m-%d %H:%M:%S", &tm_ctime);
    log(3,"%-40s %08o %5lu %9lu %s [%04x%04x]\n",
        info->name().c_str(), info->statMode(),
        info->statIno(), info->statSize(),
        ctime, lp->created.time[0], lp->created.time[1]);
#endif

    // Make a new entry in the parent's list of children
    parent->append(info);

    return 0;
}

/**
 * @brief Get a fileinfo entry for the given path
 * @param path file name with leading path (i.e. "/" prepended)
 * @return pointer to afs_fileinfo_t for the entry, or NULL on error
 */
afs_fileinfo* AltoFS::find_fileinfo(std::string path) const
{
    if (!m_root_dir)
        return NULL;

    if (path == "/")
        return m_root_dir;

    if (path[0] == '/')
        path.erase(0,1);

    return m_root_dir->find(path);
}

/**
 * @brief Read the page filepage into the buffer at data
 * @param filepage page number
 * @param data buffer of PAGESZ bytes
 */
void AltoFS::read_page(page_t filepage, char* data, size_t size)
{
    const char *src = (char *)&m_disk[filepage].data;
    for (size_t i = 0; i < size; i++)
        data[i] = src[i ^ m_little.l];
}

/**
 * @brief Write the page filepage to the disk image
 * @param filepage page number
 * @param data buffer of PAGESZ bytes
 */
void AltoFS::write_page(page_t filepage, const char* data, size_t size)
{
    char *dst = (char *)&m_disk[filepage].data;
    for (size_t i = 0; i < size; i++)
        dst[i ^ m_little.l] = data[i];
}

/**
 * @brief Zero the page filepage in the disk image
 * @param filepage page number
 */
void AltoFS::zero_page(page_t filepage)
{
    char *dst = (char *)&m_disk[filepage].data;
    memset(dst, 0, PAGESZ);
}

/**
 * @brief Read a file starting ad leader_page_vda into the buffer at data
 * @param leader_page_vda page number of leader VDA
 * @param data buffer of size bytes
 * @param size number of bytes to read
 * @param offs start offset to read from
 * @return number of bytes actually read
 */
size_t AltoFS::read_file(page_t leader_page_vda, char* data, size_t size, off_t offset, bool update)
{
    afs_leader_t* lp = page_leader(leader_page_vda);
    afs_label_t* l = page_label(leader_page_vda);
    std::string fn = filename_to_string(lp->filename);
    afs_fileinfo* info = find_fileinfo(fn);
    my_assert_or_die(info != NULL, "%s: Could not find file info for %s\n", __func__, fn.c_str());
    if (info == NULL)
        return -1;

    page_t page = rda_to_vda(l->next_rda);
    size_t done = 0;
    off_t offs = 0;
    while (page && size > 0) {
        l = page_label(page);
        size_t nbytes = size < l->nbytes ? size : l->nbytes;
        if (offs >= offset) {
            // aligned page read
#if defined(DEBUG)
            log(3,"%s: offs=0x%06lx page=%-5ld nbytes=0x%03lx\n",
                __func__, offs, page, nbytes);
#endif
            read_page(page, data, nbytes);
            page = rda_to_vda(l->next_rda);
            data += nbytes;
            done += nbytes;
            size -= nbytes;
            if (nbytes < PAGESZ) {
               if (update) {
               }
               break;
            }
        } else if ((off_t)(offs + PAGESZ) > offset) {
            // partial page read
            off_t from = offset - offs;
            nbytes -= from;
#if defined(DEBUG)
            log(3,"%s: offs=0x%06lx page=%-5ld nbytes=0x%03lx from=0x%03lx\n",
                __func__, offs, page, nbytes, from);
#endif
            char buff[PAGESZ];
            read_page(page, buff, PAGESZ);
            memcpy(data, buff + from, nbytes);
            data += nbytes;
            done += nbytes;
            size -= nbytes;
        } else {
#if defined(DEBUG)
            log(4,"%s: offs=0x%06lx page=%-5ld (seeking to 0x%06lx)\n",
                __func__, offs, page, offs);
#endif
        }
        offs += nbytes;
    }

    if (update) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        info->setStatAtime(tv.tv_sec);
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
size_t AltoFS::write_file(page_t leader_page_vda, const char* data, size_t size, off_t offset, bool update)
{
    afs_leader_t* lp = page_leader(leader_page_vda);
    afs_label_t* l = page_label(leader_page_vda);
    std::string fn = filename_to_string(lp->filename);
    afs_fileinfo* info = find_fileinfo(fn);
    my_assert_or_die(info != NULL, "%s: Could not find file info for %s\n", __func__, fn.c_str());
    if (info == NULL)
        return -1;

    off_t offs = 0;
    page_t page = rda_to_vda(l->next_rda);
    size_t stsize = file_length(leader_page_vda);
    page_t last_page = -1;
    word last_bytes = 0;

    // See if offs is at or beyond last page
    if (static_cast<size_t>(offs) >= stsize) {
        // In this case use the leader page's last page hint
        page = lp->last_page_hint.vda;
        offs = (offset / PAGESZ) * PAGESZ;
    }

    size_t done = 0;
    while (page && size > 0) {
        l = page_label(page);
        size_t nbytes = size < PAGESZ ? size : PAGESZ;
        if (offs >= offset && l->nbytes == PAGESZ) {
            // aligned page write
            l->nbytes = nbytes;
#if defined(DEBUG)
            log(3,"%s: offs=0x%06lx page=%-5ld nbytes=0x%03lx size=0x%06lx\n",
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
            log(3,"%s: offs=0x%06lx page=%-5ld nbytes=0x%03lx size=0x%06lx to=0x%03lx\n",
                __func__, offs, page, nbytes, size, to);
#endif
            memcpy(buff + to, data, nbytes);
            l->nbytes = to + nbytes;
            write_page(page, buff, l->nbytes); // write the modified page
            data += nbytes;
            done += nbytes;
            size -= nbytes;
            if (l->nbytes < PAGESZ)
                break;
        } else {
#if defined(DEBUG)
            log(4,"%s: offs=0x%06lx page=%-5ld (seeking to 0x%06lx)\n",
                __func__, offs, page, offset);
#endif
        }
        offs += PAGESZ;
        if ((size_t)offs > stsize) {
            last_page = page;
            last_bytes = l->nbytes;
            info->setStatSize(offs);
        }
        if (size > 0 && 0 == l->next_rda) {
            // Need to allocate a new page
            page = alloc_page(page);
            if (page) {
                last_page = page;
                last_bytes = 0;
            }
        }
        page = rda_to_vda(l->next_rda);
    }

    if (-1 != last_page) {
        lp->last_page_hint.vda = last_page;
        lp->last_page_hint.page_number = last_page % NPAGES;
        lp->last_page_hint.char_pos = last_bytes;
    }

    if (update) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        info->setStatMtime(tv.tv_sec);
    }

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

void AltoFS::altotime_to_tm(afs_time_t at, struct tm& tm)
{
    time_t time;
    altotime_to_time(at, &time);
    memcpy(&tm, localtime(&time), sizeof(tm));
}

std::string AltoFS::altotime_to_str(afs_time_t at)
{
    char buff[64];
    struct tm tm;
    altotime_to_tm(at, tm);
    snprintf(buff, sizeof(buff), "%04d-%02d-%02d %02d:%02d:%02d",
             1900 + tm.tm_year, tm.tm_mon, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buff;
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

    w = m_disk[fa->vda].data[fa->char_pos >> 1];
    if (m_little.l)
        w = (w >> 8) | (w << 8);

    fa->char_pos += 2;
    return w;
}

int AltoFS::putword(afs_fa_t* fa, word w)
{
    afs_label_t* l = page_label(fa->vda);

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
    l->filepage = fa->page_number;

    if (m_little.l)
        w = (w >> 8) | (w << 8);
    m_disk[fa->vda].data[fa->char_pos >> 1] = w;

    fa->char_pos += 2;
    return 0;
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
    if (!my_assert(page >= 0 && page < m_bit_count,
        "%s: page out of bounds (%d)\n",
        __func__, page))
        return 1;
    int bit;
    bit = 15 - (page % 16);
    return (m_bit_table[page / 16] >> bit) & 1;
}

/**
 * @brief Set bit in the free page bit table
 * @param page page number
 * @param bit value
 */
void AltoFS::setBT(page_t page, int val)
{
    if (!my_assert(page >= 0 && page < m_bit_count,
        "%s: page out of bounds (%d)\n",
        __func__, page))
        return;
    int offs, bit;
    offs = page / 16;
    bit = 15 - (page % 16);
    if (val != ((m_bit_table[page / 16] >> bit) & 1)) {
        m_bit_table[offs] = (m_bit_table[offs] & ~(1 << bit)) | ((val & 1) << bit);
        m_disk_descriptor_dirty = true;
    }
}

/**
 * @brief Return the number of 1 bits in a word
 * @param val 16 bits
 * @return number of bits set
 */
word bit_count(word val)
{
    val -= ((val >> 1) & 0x5555);
    val = (((val >> 2) & 0x3333) + (val & 0x3333));
    val = (((val >> 4) + val) & 0x0f0f);
    val += (val >> 8);
    val &= 31;
    return val;
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
    m_kdh.free_pages += getBT(page);
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

    const int last = m_doubledisk ? NPAGES * 2 : NPAGES;
    for (int i = 0; i < last; i += 1)
        ok &= my_assert(m_disk[i].pagenum == rda_to_vda(m_disk[i].header[1]),
            "%s: page %04x header doesn't match: %04x %04x\n",
            __func__, m_disk[i].pagenum, m_disk[i].header[0], m_disk[i].header[1]);
    return ok;
}

/**
 * @brief Verify the disk descript file DiskDescriptor
 * Check single or double disks
 */
int AltoFS::validate_disk_descriptor()
{
    int ddlp, nfree, ok;
    afs_leader_t* lp;
    afs_label_t* l;
    afs_fa_t fa;

    // Locate DiskDescriptor and copy it into the global data structure
    ddlp = find_file("DiskDescriptor");
    my_assert_or_die(ddlp != -1, "%s: Can't find DiskDescriptor\n", __func__);

    lp = page_leader(ddlp);
    (void)lp; // yet unused
    l = page_label(ddlp);

    fa.vda = rda_to_vda(l->next_rda);
    memcpy(&m_kdh, &m_disk[fa.vda].data[0], sizeof(m_kdh));
    m_bit_count = m_kdh.disk_bt_size * 16;
    m_bit_table.resize(m_kdh.disk_bt_size);

    // Now copy the bit table from the disk into bit_table
    fa.page_number = 1;
    fa.char_pos = sizeof(m_kdh);
    for (word i = 0; i < m_kdh.disk_bt_size; i++)
        m_bit_table[i] = getword(&fa);
    m_disk_descriptor_dirty = 0;
    log(0, "%s: bit table size is %u words (%u bits)\n", __func__, m_kdh.disk_bt_size, m_bit_count);
    ok = 1;

    if (m_doubledisk) {
        // For double disk systems
        ok &= my_assert (m_kdh.nDisks == 2, "%s: Expect double disk system\n", __func__);
    } else {
        // for single disk systems
        ok &= my_assert (m_kdh.nDisks == 1, "%s: Expect single disk system\n", __func__);
    }
    ok &= my_assert (m_kdh.nTracks == NCYLS, "%s: KDH tracks != %d\n", __func__, NCYLS);
    ok &= my_assert (m_kdh.nHeads == NHEADS, "%s: KDH heads != %d\n", __func__, NHEADS);
    ok &= my_assert (m_kdh.nSectors == NSECS, "%s: KDH sectors != %d\n", __func__, NSECS);
    ok &= my_assert (m_kdh.def_versions_kept == 0, "%s: defaultVersions != 0\n", __func__);

    // Count free pages in bit table
    nfree = 0;
    for (word idx = 0; idx < m_kdh.disk_bt_size; idx++)
        nfree += 16 - bit_count(m_bit_table[idx]);
    ok &= my_assert (nfree == m_kdh.free_pages,
        "%s: Bit table count %d doesn't match KDH free pages %d\n",
        __func__, nfree, m_kdh.free_pages);

    // Count free pages in actual image
    nfree = 0;
    const page_t last = m_doubledisk ? NPAGES * 2 : NPAGES;
    for (page_t page = 0; page < last; page++)
        nfree += is_page_free(page);

    ok &= my_assert (nfree == m_kdh.free_pages,
        "%s: Actual free page count %d doesn't match KDH value %d\n",
        __func__, nfree, m_kdh.free_pages);

    return ok;
}

/**
 * @brief Rebuild bit table and free page count from labels.
 */
void AltoFS::fix_disk_descriptor()
{
    int nfree = 0;
    const page_t last = m_doubledisk ? NPAGES * 2 : NPAGES;
    for (int page = 0; page < last; page++) {
        int t = is_page_free(page);
        nfree += t;
        if (getBT(page) != (t ^ 1)) {
            // TODO: try to find filename which this page was allocted to?
            // Follow the chain of label->prev_rda until it is == 0,
            // then inspect the filename stored in the leader page.
            // Build a list of chains already reported to compact the output.
            setBT(page, t ^ 1);
        }
    }
    m_kdh.free_pages = nfree;

    // Count free pages in bit table - again
    nfree = 0;
    for (size_t offs = 0; offs < m_kdh.disk_bt_size; offs++)
        nfree += 16 - bit_count(m_bit_table[offs]);
    my_assert (nfree == m_kdh.free_pages,
        "%s: Bit table count %d doesn't match KDH free pages %d\n",
        __func__, nfree, m_kdh.free_pages);
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
    if (flag)
        return flag;
    va_list ap;
    va_start(ap, errmsg);
    vfprintf(stderr, errmsg, ap);
    va_end(ap);
    fflush(stderr);
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
void AltoFS::my_assert_or_die(bool flag, const char *errmsg, ...)
{
    if (flag)
        return;
    va_list ap;
    va_start(ap, errmsg);
    vfprintf(stderr, errmsg, ap);
    va_end(ap);
    fflush(stderr);
    exit(1);
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
std::string AltoFS::filename_to_string(const char* src)
{
    char buff[FNLEN+2];
    char* dst = buff;
    memset(buff, 0, sizeof(buff));

    size_t length = src[m_little.l];
    if (length < 0 || length >= FNLEN)
        length = FNLEN - 1;
    for (size_t i = 0; i < length; i++)
        dst[i] = src[i ^ m_little.l];

    // Replace invalid (non printing) characters with '#'
    for (size_t i = 1; i < length; i++)
        dst[i] = isprint((unsigned char)dst[i]) ? dst[i] : '#';

    // Erase the closing '.'
    length = strlen(buff);
    if ('.' == buff[--length])
        buff[length] = '\0';

    // Return the string after the length byte
    return dst+1;
}

/**
 * @brief Copy a C string from src to an Alto file system filename at dst.
 *
 * @param dst pointer to an Alto file system filename
 * @param src source buffer of at mose FNLEN-1 characters
 */
void AltoFS::string_to_filename(char* dst, std::string src)
{
    size_t length = src.length() + 1;
    if (length >= FNLEN - 2)
        length = FNLEN - 2;
    dst[m_little.l] = length;
    for (size_t i = 0; i < length; i++)
        dst[(i+1) ^ m_little.l] = src[i];
    // Append a dot
    dst[length ^ m_little.l] = '.';
}

/**
 * @brief Fill a struct statvfs pointer with info about the file system.
 * @param vfs pointer to a struct statvfs
 * @return 0 on success, -EBADF on error (root_dir is NULL)
 */
int AltoFS::statvfs(struct statvfs* vfs)
{
    memset(vfs, 0, sizeof(*vfs));
    if (NULL == m_root_dir)
        return -EBADF;

    vfs->f_bsize = PAGESZ;
    vfs->f_blocks = NPAGES;
    if (m_doubledisk) {
        vfs->f_frsize *= 2;
        vfs->f_blocks *= 2;
    }
    vfs->f_bfree = m_kdh.free_pages;
    vfs->f_bavail = m_kdh.free_pages;
    vfs->f_files = m_root_dir->size();
    // FIXME: Is there a way to determine the number of possible files?
    vfs->f_ffree = 0;
    // FIXME: this is without length byte and trailing dot, thus -2. Is this correct?
    vfs->f_namemax = FNLEN-2;
    return 0;
}

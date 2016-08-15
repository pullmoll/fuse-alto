#include "fileinfo.h"

afs_fileinfo* afs_fileinfo::parent() const
{
    return m_parent;
}

std::string afs_fileinfo::name() const
{
    return m_name;
}

struct stat* afs_fileinfo::st()
{
    return &m_st;
}

const struct stat* afs_fileinfo::st() const
{
    return &m_st;
}

page_t afs_fileinfo::leader_page_vda() const
{
    return m_leader_page_vda;
}

std::vector<afs_fileinfo> afs_fileinfo::children() const
{
    return m_children;
}

afs_fileinfo* afs_fileinfo::child(int idx)
{
    return &m_children.at(idx);
}

const afs_fileinfo* afs_fileinfo::child(int idx) const
{
    return &m_children.at(idx);
}

afs_fileinfo* afs_fileinfo::find(std::string name)
{
    std::vector<afs_fileinfo>::iterator it;
    for (it = m_children.begin(); it != m_children.end(); it++) {
        afs_fileinfo* node = it.base();
        if (!node)
            continue;
        if (name == node->name())
            return node;
    }
    return NULL;

}

ino_t afs_fileinfo::statIno() const
{
    return m_st.st_ino;
}

time_t afs_fileinfo::statCtime() const
{
    return m_st.st_ctime;
}

time_t afs_fileinfo::statMtime() const
{
    return m_st.st_mtime;
}

time_t afs_fileinfo::statAtime() const
{
    return m_st.st_atime;
}

uid_t afs_fileinfo::statUid() const
{
    return m_st.st_uid;
}

gid_t afs_fileinfo::statGid() const
{
    return m_st.st_gid;
}

mode_t afs_fileinfo::statMode() const
{
    return m_st.st_mode;
}

size_t afs_fileinfo::statSize() const
{
    return m_st.st_size;
}

size_t afs_fileinfo::statBlockSize() const
{
    return m_st.st_blksize;
}

size_t afs_fileinfo::statBlocks() const
{
    return m_st.st_blocks;
}

size_t afs_fileinfo::statnLink() const
{
    return m_st.st_nlink;
}

void afs_fileinfo::setIno(ino_t ino)
{
    m_st.st_ino = ino;
}

void afs_fileinfo::setStatCtime(time_t t)
{
    m_st.st_ctime = t;
}

void afs_fileinfo::setStatMtime(time_t t)
{
    m_st.st_mtime = t;
}

void afs_fileinfo::setStatAtime(time_t t)
{
    m_st.st_atime = t;
}

void afs_fileinfo::setStatUid(uid_t uid)
{
    m_st.st_uid = uid;
}

void afs_fileinfo::setStatGid(gid_t gid)
{
    m_st.st_gid = gid;
}

void afs_fileinfo::setStatMode(mode_t mode)
{
    m_st.st_mode = mode;
}

void afs_fileinfo::setStatSize(size_t size)
{
    m_st.st_size = size;
}

void afs_fileinfo::setStatBlockSize(size_t blocksize)
{
    m_st.st_blksize = blocksize;
}

void afs_fileinfo::setStatBlocks(size_t blocks)
{
    m_st.st_blocks = blocks;
}

void afs_fileinfo::setnLink(size_t count)
{
    m_st.st_nlink = count;
}

void afs_fileinfo::erase(int pos, int count)
{
    std::vector<afs_fileinfo>::iterator it;
    int idx = 0;
    for (it = m_children.begin(); it != m_children.end(); it++) {
        if (idx < pos)
            continue;
        if (idx >= pos + count)
            break;
        m_children.erase(it);
        idx++;
    }
}

void afs_fileinfo::erase(std::vector<afs_fileinfo>::iterator pos)
{
    m_children.erase(pos);
}

void afs_fileinfo::rename(std::string newname)
{
    m_name = newname;
}

void afs_fileinfo::insert(afs_fileinfo* child)
{
    std::vector<afs_fileinfo>::iterator it = m_children.begin(); m_children.insert(it, *child);
}

bool afs_fileinfo::remove(afs_fileinfo* child)
{
    std::vector<afs_fileinfo>::iterator it;
    int idx = 0;
    for (it = m_children.begin(); it != m_children.end(); it++) {
        const afs_fileinfo* node = &m_children.at(idx);
        if (child->name() == node->name()) {
            m_children.erase(it);
            return true;
        }
        idx++;
    }
    return false;
}

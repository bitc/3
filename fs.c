// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation 
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "buf.h"
#include "fs.h"
#include "file.h"

#define MAX_SYMLINK_LOOPS 16

#define min(a, b) ((a) < (b) ? (a) : (b))
static void itrunc(struct inode*);

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;
  
  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;
  
  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks. 

// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;
  struct superblock sb;

  bp = 0;
  readsb(dev, &sb);
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb.ninodes));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  struct superblock sb;
  int bi, m;

  readsb(dev, &sb);
  bp = bread(dev, BBLOCK(b, sb.ninodes));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk immediately after
// the superblock. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->flags.
//
// An inode and its in-memory represtative go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, iput() frees if
//   the link count has fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() to find or
//   create a cache entry and increment its ref, iput()
//   to decrement ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when the I_VALID bit
//   is set in ip->flags. ilock() reads the inode from
//   the disk and sets I_VALID, while iput() clears
//   I_VALID if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode. The I_BUSY flag indicates
//   that the inode is locked. ilock() sets I_BUSY,
//   while iunlock clears it.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

/**************/
/*** TASK 2 ***/
/**************/

#define NINODES 200

struct {
  struct spinlock lock;
  int pids[NINODES][NPROC];
} unlocked_inodes;

void unlock_inode(struct inode* ip)
{
  int i;
  acquire(&unlocked_inodes.lock);
  for(i = 0; i < NPROC; i++){
    if(unlocked_inodes.pids[ip->inum][i] == proc->pid){
      // The inode is already unlocked for this pid
      release(&unlocked_inodes.lock);
      return;
    }
  }

  // The inode is not already unlocked, look for the first free slot
  for(i = 0; i < NPROC; i++){
    if(unlocked_inodes.pids[ip->inum][i] == 0){
      // Found a free slot
      unlocked_inodes.pids[ip->inum][i] = proc->pid;
      release(&unlocked_inodes.lock);
      return;
    }
  }
  panic("unlock_inodes: No free pid slots found");
}

void free_inode_locks(int pid)
{
  int i, j;

  acquire(&unlocked_inodes.lock);
  for(i = 0; i < NINODES; i++){
    for(j = 0; j < NPROC; j++){
      if(unlocked_inodes.pids[i][j] == pid){
        unlocked_inodes.pids[i][j] = 0;
      }
    }
  }
  release(&unlocked_inodes.lock);
}

int
is_inode_unlocked(struct inode* ip)
{
  int i;

  acquire(&unlocked_inodes.lock);

  for(i = 0; i < NPROC; i++){
    if(unlocked_inodes.pids[ip->inum][i] == proc->pid){
      release(&unlocked_inodes.lock);
      return 1;
    }
  }

  release(&unlocked_inodes.lock);
  return 0;
}

void
fork_pids(int old_pid, int new_pid)
{
  int i, j, k;

  acquire(&unlocked_inodes.lock);

  for(i = 0; i < NINODES; i++){
    for(j = 0; j < NPROC; j++){
      if(unlocked_inodes.pids[i][j] == old_pid){
        for(k = 0; k < NPROC; k++){
          if(unlocked_inodes.pids[i][k] == 0){
            unlocked_inodes.pids[i][k] = new_pid;
            break;
          }
        }
        if(k == NPROC){
          panic("fork_pids: No free pid slots found");
        }
        break;
      }
    }
  }

  release(&unlocked_inodes.lock);
}

void
iinit(void)
{
  initlock(&icache.lock, "icache");
/*vvv  TASK 2    vvv*/
  initlock(&unlocked_inodes.lock, "unlocked_inodes");
/*^^^^^^^^^^^^^^^^^^*/
}

static struct inode* iget(uint dev, uint inum);

//PAGEBREAK!
// Allocate a new inode with the given type on device dev.
// A free inode has a type of zero.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;
  struct superblock sb;

  readsb(dev, &sb);

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
/*vvv  TASK 1.1  vvv*/
  dip->indirect2 = ip->indirect2;
/*^^^^^^^^^^^^^^^^^^*/
/*vvv  TASK 2    vvv*/
  memmove(dip->password, ip->password, sizeof(ip->password));
/*^^^^^^^^^^^^^^^^^^*/
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->flags = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquire(&icache.lock);
  while(ip->flags & I_BUSY)
    sleep(ip, &icache.lock);
  ip->flags |= I_BUSY;
  release(&icache.lock);

  if(!(ip->flags & I_VALID)){
    bp = bread(ip->dev, IBLOCK(ip->inum));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
/*vvv  TASK 1.1  vvv*/
    ip->indirect2 = dip->indirect2;
/*^^^^^^^^^^^^^^^^^^*/
/*vvv  TASK 2    vvv*/
    memmove(ip->password, dip->password, sizeof(ip->password));
/*^^^^^^^^^^^^^^^^^^*/
    brelse(bp);
    ip->flags |= I_VALID;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !(ip->flags & I_BUSY) || ip->ref < 1)
    panic("iunlock");

  acquire(&icache.lock);
  ip->flags &= ~I_BUSY;
  wakeup(ip);
  release(&icache.lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
void
iput(struct inode *ip)
{
  acquire(&icache.lock);
  if(ip->ref == 1 && (ip->flags & I_VALID) && ip->nlink == 0){
    // inode has no links: truncate and free inode.
    if(ip->flags & I_BUSY)
      panic("iput busy");
    ip->flags |= I_BUSY;
    release(&icache.lock);
    itrunc(ip);
    ip->type = 0;
    iupdate(ip);
    acquire(&icache.lock);
    ip->flags = 0;
    wakeup(ip);
  }
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are 
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }
/*vvv  TASK 1.1  vvv*/
  bn -= NINDIRECT;

  if(bn < NINDIRECT * NINDIRECT){
    // Load double indirect block, allocating if necessary.
    if((addr = ip->indirect2) == 0)
      ip->indirect2 = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn / NINDIRECT]) == 0){
      a[bn / NINDIRECT] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);

    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn % NINDIRECT]) == 0){
      a[bn % NINDIRECT] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }
/*^^^^^^^^^^^^^^^^^^*/

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
static void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp, *bp2;
  uint *a, *a2;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }
  
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if(a[j])
        bfree(ip->dev, a[j]);
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

/*vvv  TASK 1.1  vvv*/
  if(ip->indirect2){
    bp = bread(ip->dev, ip->indirect2);
    a = (uint*)bp->data;
    for(i = 0; i < NINDIRECT; i++){
      if(a[i]){
        bp2 = bread(ip->dev, a[i]);
        a2 = (uint*)bp2->data;
        for(j = 0; j < NINDIRECT; j++){
          if(a2[j])
            bfree(ip->dev, a2[j]);
        }
        brelse(bp2);
        bfree(ip->dev, a[i]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->indirect2);
    ip->indirect2 = 0;
  }
/*^^^^^^^^^^^^^^^^^^*/

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//PAGEBREAK!
// Read data from inode.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(bp->data + off%BSIZE, src, m);
    log_write(bp);
    brelse(bp);
  }

  if(n > 0 && off > ip->size){
    ip->size = off;
    iupdate(ip);
  }
  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");
  
  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(proc->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}

/****************/
/*** TASK 1.2 ***/
/****************/

int
filereadlink(const char *pathname, char *buf, int bufsiz)
{
  int loops_left = MAX_SYMLINK_LOOPS;
  int l;

  char name[DIRSIZ+1];
  char result[MAXPATH];
  int result_off;
  char origpath[MAXPATH];
  char *origpath_p;

  struct inode* ip;

  if(*pathname == '\0'){
    return -1;
  }

  if(pathname[0] == '/' && pathname[1] == '\0') {
    buf[0] = '/';
    buf[1] = '\0';
    return 2;
  }

  strncpy(origpath, pathname, MAXPATH);

restart:
  result_off = 0;

  if(origpath[0] == '/'){
    result[0] = '/';
    result_off = 1;
  }
  result[result_off] = '\0';

  origpath_p = origpath;

  for(;;){
    origpath_p = skipelem(origpath_p, name);
    if(!origpath_p){
      return -1;
    }

    if(*origpath_p == '\0'){
      // Last element
      // `name' contains the filename

      safestrcpy(&result[result_off], name, MAXPATH);
      result_off += strlen(name);
      result[result_off] = '\0';

      ip = namei(result);
      if(!ip){
        return -1;
      }
      ilock(ip);
      if(ip->type == T_FILE || ip->type == T_DEV || ip->type == T_DIR){
        iunlock(ip);
        if(bufsiz < result_off + 1){
          return -1;
        } else {
          safestrcpy(buf, result, MAXPATH);
          return result_off;
        }
      } else if(ip->type == T_SYMLINK) {
        l = readi(ip, origpath, 0, MAXPATH);
        iunlock(ip);
        origpath[l] = '\0';
        if(origpath[0] != '/'){
          // Relative symlink
          result_off -= strlen(name);
          safestrcpy(&result[result_off], origpath, MAXPATH);
          safestrcpy(origpath, result, MAXPATH);
        }
        loops_left--;
        if(loops_left == 0){
          return -1;
        }
        goto restart;
      } else {
        panic("filereadlink: unknown inode type");
      }
    } else {
      safestrcpy(&result[result_off], name, MAXPATH);
      result_off += strlen(name);
      result[result_off] = '\0';

      ip = namei(result);
      if(!ip){
        return -1;
      }
      ilock(ip);
      if(ip->type == T_FILE || ip->type == T_DEV){
        // Tried to traverse through a file/dev as if it was a directory
        return -1;
      } else if(ip->type == T_DIR) {
        result[result_off] = '/';
        result_off++;
        result[result_off] = '\0';
      } else if(ip->type == T_SYMLINK) {
        result_off -= strlen(name);
        l = readi(ip, &result[result_off], 0, MAXPATH);
        iunlock(ip);
        result[result_off+l] = '/';
        safestrcpy(&result[result_off+l+1], origpath_p, MAXPATH);
        if(result[result_off] == '/') {
          // Absolute symlink
          safestrcpy(origpath, &result[result_off], MAXPATH);
        } else {
          safestrcpy(origpath, result, MAXPATH);
        }
        loops_left--;
        if(loops_left == 0){
          return -1;
        }
        goto restart;
      } else {
        panic("filereadlink: unknown inode type");
      }
      iunlock(ip);
    }
  }
}

// Same as filereadlink, but doesn't dereference the final path element
int
filereadlinki(const char *pathname, char *buf, int bufsiz)
{
  int loops_left = MAX_SYMLINK_LOOPS;
  int l;

  char name[DIRSIZ+1];
  char result[MAXPATH];
  int result_off;
  char origpath[MAXPATH];
  char *origpath_p;

  struct inode* ip;

  if(*pathname == '\0'){
    return -1;
  }

  if(pathname[0] == '/' && pathname[1] == '\0') {
    buf[0] = '/';
    buf[1] = '\0';
    return 2;
  }

  strncpy(origpath, pathname, MAXPATH);

restart:
  result_off = 0;

  if(origpath[0] == '/'){
    result[0] = '/';
    result_off = 1;
  }
  result[result_off] = '\0';

  origpath_p = origpath;

  for(;;){
    origpath_p = skipelem(origpath_p, name);
    if(!origpath_p){
      return -1;
    }

    if(*origpath_p == '\0'){
      // Last element
      // `name' contains the filename

      safestrcpy(&result[result_off], name, MAXPATH);
      result_off += strlen(name);
      result[result_off] = '\0';

      if(bufsiz < result_off + 1){
        return -1;
      } else {
        safestrcpy(buf, result, MAXPATH);
        return result_off;
      }
    } else {
      safestrcpy(&result[result_off], name, MAXPATH);
      result_off += strlen(name);
      result[result_off] = '\0';

      ip = namei(result);
      if(!ip){
        return -1;
      }
      ilock(ip);
      if(ip->type == T_FILE || ip->type == T_DEV){
        // Tried to traverse through a file/dev as if it was a directory
        return -1;
      } else if(ip->type == T_DIR) {
        result[result_off] = '/';
        result_off++;
        result[result_off] = '\0';
      } else if(ip->type == T_SYMLINK) {
        result_off -= strlen(name);
        l = readi(ip, &result[result_off], 0, MAXPATH);
        iunlock(ip);
        result[result_off+l] = '/';
        safestrcpy(&result[result_off+l+1], origpath_p, MAXPATH);
        if(result[result_off] == '/') {
          // Absolute symlink
          safestrcpy(origpath, &result[result_off], MAXPATH);
        } else {
          safestrcpy(origpath, result, MAXPATH);
        }
        loops_left--;
        if(loops_left == 0){
          return -1;
        }
        goto restart;
      } else {
        panic("filereadlink: unknown inode type");
      }
      iunlock(ip);
    }
  }
}

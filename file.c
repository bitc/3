//
// File descriptors
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "file.h"
#include "spinlock.h"
#include "stat.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);
  
  if(ff.type == FD_PIPE)
    pipeclose(ff.pipe, ff.writable);
  else if(ff.type == FD_INODE){
    begin_trans();
    iput(ff.ip);
    commit_trans();
  }
}

// Get metadata about file f.
int
filestat(struct file *f, struct stat *st)
{
  if(f->type == FD_INODE){
    ilock(f->ip);
    stati(f->ip, st);
    iunlock(f->ip);
    return 0;
  }
  return -1;
}

// Read from file f.
int
fileread(struct file *f, char *addr, int n)
{
  int r;

  if(f->readable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return piperead(f->pipe, addr, n);
  if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
    return r;
  }
  panic("fileread");
}

//PAGEBREAK!
// Write to file f.
int
filewrite(struct file *f, char *addr, int n)
{
  int r;

  if(f->writable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return pipewrite(f->pipe, addr, n);
  if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((LOGSIZE-1-1-2) / 2) * 512;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_trans();
      ilock(f->ip);
      if ((r = writei(f->ip, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      commit_trans();

      if(r < 0)
        break;
      if(r != n1)
        panic("short filewrite");
      i += r;
    }
    return i == n ? n : -1;
  }
  panic("filewrite");
}

/**************/
/*** TASK 2 ***/
/**************/

int
filefprot(const char *pathname, const char *password)
{
  char final_path[MAXPATH];
  struct inode* ip;

  if(filereadlinki(pathname, final_path, MAXPATH) < 0){
    return -1;
  }

  ip = namei(final_path);
  if(!ip){
    return -1;
  }
  ilock(ip);

  if(ip->type != T_FILE){
    // Not a file
    iunlock(ip);
    return -1;
  }

  if(ip->password[0] != '\0'){
    // File already has a password
    iunlock(ip);
    return -1;
  }

  if(is_inode_open(ip)){
    // File is already open by some process
    iunlock(ip);
    return -1;
  }

  safestrcpy(ip->password, password, PASSLEN);

  iunlock(ip);
  return 0;
}

int
filefunprot(const char *pathname, const char *password)
{
  char final_path[MAXPATH];
  struct inode* ip;

  if(filereadlinki(pathname, final_path, MAXPATH) < 0){
    return -1;
  }

  ip = namei(final_path);
  if(!ip){
    return -1;
  }
  ilock(ip);

  if(ip->password[0] == '\0'){
    // File has no password (is already unlocked)
    iunlock(ip);
    return 0;
  }

  if(strncmp(ip->password, password, PASSLEN) != 0){
    // Password doesn't match
    iunlock(ip);
    return -1;
  }

  // Remove the password:
  ip->password[0] = '\0';

  iunlock(ip);
  return 0;
}

int
filefunlock(const char *pathname, const char *password)
{
  char final_path[MAXPATH];
  struct inode* ip;

  if(filereadlinki(pathname, final_path, MAXPATH) < 0){
    return -1;
  }

  ip = namei(final_path);
  if(!ip){
    return -1;
  }
  ilock(ip);

  if(ip->password[0] == '\0'){
    // File has no password (is unlocked). This is considered an error
    iunlock(ip);
    return -1;
  }

  if(strncmp(ip->password, password, PASSLEN) != 0){
    // Password doesn't match
    iunlock(ip);
    return -1;
  }

  unlock_inode(ip);

  iunlock(ip);
  return 0;
}

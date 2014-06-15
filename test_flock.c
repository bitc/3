#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int
main(int argc, char *argv[])
{
  int fd;
  char *password = argv[1];
  char *path = argv[2];
  char buf[1024];

  if(argc != 3){
    printf(1, "usage: test_flock password path\n");
    exit();
  }

  fprot(path, password);

  if(fork() == 0){
    // Child

    funlock(path, password);
    fd = open(path, 0);
    if(fd < 0){
      printf(1, "child error opening file: %s\n", path);
      exit();
    }
    if(read(fd, buf, 1024) < 0){
      printf(1, "child error reading file: %s\n", path);
      exit();
    }
    printf(1, "%s\n", buf);
    if(close(fd) < 0){
      printf(1, "child error closing file: %s\n", path);
      exit();
    }
  } else {
    // Parent
    wait();

    printf(1, "parent opening file...\n");

    fd = open(path, 0);
    if(fd < 0){
      printf(1, "parent error opening file: %s\n", path);
    } else {
      close(fd);
    }

    printf(1, "parent trying to unprotect with wrong password...\n");
    if(funprot(path, "wrong") == 0) {
      printf(1, "ok\n");
    } else {
      printf(1, "failed\n");
    }

    printf(1, "parent trying to unprotect with correct password...\n");
    if(funprot(path, password) == 0) {
      printf(1, "ok\n");
    } else {
      printf(1, "failed\n");
    }
  }

  exit();
}

#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

#define TEST_FILE "/large_file"

// One kilobyte of data:
static char data[1024];

int
main(int argc, char *argv[])
{
  int fd, i;
  char *fname;

  if(argc == 2){
    fname = argv[1];
  }
  else{
    fname = TEST_FILE;
  }

  if((fd = open(fname, O_CREATE | O_WRONLY)) < 0){
    printf(1, "error opening file: %s\n", fname);
    exit();
  }

  for(i = 0; i < 1024; ++i){
    if(write(fd, data, 1024) < 0){
      printf(1, "error writing to file: %s\n", fname);
      close(fd);
      exit();
    }
    printf(1, "wrote %d kilobytes\n", i + 1);
  }

  printf(1, "closing: %s\n", fname);
  if(close(fd) < 0){
    printf(1, "error closing file: %s\n", fname);
    exit();
  }

  exit();
}

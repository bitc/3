#include "types.h"
#include "user.h"


int
main(int argc, char *argv[])
{
  if(argc != 2){
    printf(1, "Usage: test_readlink file\n");
    exit();
  }

  char output[512];
  if(readlink(argv[1], output, 512) < 0){
    printf(1, "error: readlink\n");
    exit();
  }

  printf(1, "%d\n", strlen(output));
  printf(1, "%s\n", output);
  exit();
}

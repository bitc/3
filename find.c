#include "types.h"
#include "user.h"
#include "fcntl.h"
#include "stat.h"
#include "fs.h"

struct {
  int follow;                                         // boolean: 0 or 1
  char *name_exact;                                   // May be null for any name
  enum { FT_ANY, FT_DIR, FT_FILE, FT_SYMLINK } type;
  uint min_size;
  uint max_size;
} search_options;

static void
set_default_search_options(void)
{
  search_options.follow = 0;
  search_options.name_exact = 0;
  search_options.type = FT_ANY;
  search_options.min_size = 0;
  search_options.max_size = 0xFFFFFFFF;
}

static char*
basename(char *path)
{
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;
  return p;
}

static void
search(char *path, int following_symlink)
{
  int fd;
  struct stat st;
  int skip = 0;
  struct dirent de;
  int cur_entry, i;
  char buf[MAXPATH];
  char *p;

  fd = open(path, following_symlink ? 0 : O_IGNLINK);

  if(fd < 0 && following_symlink){
    // Error opening symlink or broken symlink. Ignore it
    return;
  }

  if(fd < 0) {
    printf(1, "Error opening file: %s\n", path);
    exit();
  }

  if(fstat(fd, &st) < 0){
    printf(1, "Error fstat file: %s\n", path);
    exit();
  }

  close(fd);

  switch(st.type){
    case T_DIR:
      if(!skip && search_options.name_exact && strcmp(basename(path), search_options.name_exact) != 0){
        skip = 1;
      }
      if(!skip && !(search_options.type == FT_ANY || search_options.type == FT_DIR)){
        skip = 1;
      }
      if(!skip && !(st.size >= search_options.min_size && st.size <= search_options.max_size)){
        skip = 1;
      }

      if(!skip){
        printf(1, "%s\n", path);
      }

      strcpy(buf, path);
      p = buf + strlen(path);
      if(strcmp(path, "/") != 0){ // Handle special case of root directory
        *p = '/';
        p++;
      }

      cur_entry = 0;
      for(;;){
        fd = open(path, following_symlink ? 0 : O_IGNLINK);
        if(fd < 0) {
          printf(1, "Error opening file: %s\n", path);
          exit();
        }
        // Not possible to seek through files, so use read to skip forward
        for(i = 0; i < cur_entry - 1; i++){
          read(fd, &de, sizeof(de));
        }
        if(read(fd, &de, sizeof(de)) == sizeof(de)){
          close(fd);
          if(de.inum != 0 && strcmp(de.name, ".") != 0 && strcmp(de.name, "..") != 0){
            memmove(p, de.name, DIRSIZ);
            p[DIRSIZ] = '\0';

            search(buf, 0);
          }
        } else {
          close(fd);
          break;
        }
        cur_entry++;
      }
      return;
    case T_FILE:
      if(!skip && search_options.name_exact && strcmp(basename(path), search_options.name_exact) != 0){
        skip = 1;
      }
      if(!skip && !(search_options.type == FT_ANY || search_options.type == FT_FILE)){
        skip = 1;
      }
      if(!skip && !(st.size >= search_options.min_size && st.size <= search_options.max_size)){
        skip = 1;
      }

      if(!skip){
        printf(1, "%s\n", path);
      }
      return;
    case T_DEV:
      if(!skip && search_options.name_exact && strcmp(basename(path), search_options.name_exact) != 0){
        skip = 1;
      }
      if(!skip && search_options.type != FT_ANY){
        skip = 1;
      }
      if(!skip && !(st.size >= search_options.min_size && st.size <= search_options.max_size)){
        skip = 1;
      }

      if(!skip){
        printf(1, "%s\n", path);
      }
      return;
    case T_SYMLINK:
      if(search_options.follow){
        search(path, 1);
      } else {
        if(!skip && search_options.name_exact && strcmp(basename(path), search_options.name_exact) != 0){
          skip = 1;
        }
        if(!skip && !(search_options.type == FT_ANY || search_options.type == FT_SYMLINK)){
          skip = 1;
        }
        if(!skip && !(st.size >= search_options.min_size && st.size <= search_options.max_size)){
          skip = 1;
        }

        if(!skip){
          printf(1, "%s\n", path);
        }
      }
      return;
  }
}

static void
print_usage()
{
  printf(1, "Usage: find path <options> <preds>\n");
  printf(1, "\n");
  printf(1, "Options:\n");
  printf(1, "  -follow\n");
  printf(1, "  -help\n");
  printf(1, "\n");
  printf(1, "Predicates:\n");
  printf(1, "  -name filename\n");
  printf(1, "  -size [+/-]n\n");
  printf(1, "  -type (d|f|s)\n");
}

static void
print_search_options()
{
  printf(1, "Search Options:\n");
  printf(1, "follow: %d\n", search_options.follow);
  printf(1, "name_exact: %s\n", search_options.name_exact);
  switch(search_options.type){
    case FT_ANY:
      printf(1, "type: FT_ANY\n");
      break;
    case FT_DIR:
      printf(1, "type: FT_DIR\n");
      break;
    case FT_FILE:
      printf(1, "type: FT_FILE\n");
      break;
    case FT_SYMLINK:
      printf(1, "type: FT_SYMLINK\n");
      break;
    default:
      printf(1, "Error, unknown type: %d\n", search_options.type);
      exit();
  }
  printf(1, "min_size: %d\n", search_options.min_size);
  printf(1, "max_size: %d\n", search_options.max_size);
}

int
main(int argc, char *argv[])
{
  int i;
  char *path;

  if(argc < 2){
    printf(1, "Error, not enough arguments\n");
    print_usage();
    exit();
  }

  set_default_search_options();

  path = argv[1];

  // Special case:
  if(strcmp(path, "-help") == 0){
    print_usage();
    exit();
  }

  for(i = 2; i < argc; i++){
    if(strcmp(argv[i], "-help") == 0){
      print_usage();
      exit();
    } else if(strcmp(argv[i], "-follow") == 0){
      search_options.follow = 1;
    } else if(strcmp(argv[i], "-name") == 0){
      if(i+1 >= argc){
        printf(1, "Error, missing parameter for %s\n", argv[i]);
        print_usage();
        exit();
      }
      i++;
      search_options.name_exact = argv[i];
      if(*search_options.name_exact == '\0'){
        printf(1, "Error, name parameter cannot be empty\n");
        exit();
      }
    } else if(strcmp(argv[i], "-size") == 0){
      if(i+1 >= argc){
        printf(1, "Error, missing parameter for %s\n", argv[i]);
        print_usage();
        exit();
      }
      i++;
      if(*argv[i] == '\0'){
        printf(1, "Error, size parameter cannot be empty\n");
        exit();
      }
      if(argv[i][0] == '+'){
        search_options.min_size = atoi(&argv[i][1]) + 1; // Add one since the + means "more than"
      } else if(argv[i][0] == '-'){
        search_options.max_size = atoi(&argv[i][1]) - 1; // Subtract one since the + means "more than"
      } else {
        search_options.min_size = atoi(argv[i]);
        search_options.max_size = atoi(argv[i]);
      }
    } else if(strcmp(argv[i], "-type") == 0){
      if(i+1 >= argc){
        printf(1, "Error, missing parameter for %s\n", argv[i]);
        print_usage();
        exit();
      }
      i++;
      if(strcmp(argv[i], "d") == 0){
        search_options.type = FT_DIR;
      } else if(strcmp(argv[i], "f") == 0){
        search_options.type = FT_FILE;
      } else if(strcmp(argv[i], "s") == 0){
        search_options.type = FT_SYMLINK;
      } else {
        printf(1, "Error, unknown parameter for -type: %s\n", argv[i]);
        print_usage();
        exit();
      }
    } else {
      printf(1, "Error, unrecognized argument: %s\n", argv[i]);
      print_usage();
      exit();
    }
  }

  if(0){ // Toggle to enable debugging output
    print_search_options();
  }

  search(path, 0);

  exit();
}

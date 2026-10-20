#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  void *image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!image) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
  dlerror();
  int (*add)(int, int) = (int (*)(int, int))dlsym(image, "nds_add");
  const char *error = dlerror();
  if (error) { fprintf(stderr, "dlsym: %s\n", error); dlclose(image); return 4; }
  int value = add(20, 22);
  printf("NDS ARM image result=%d expected=42\n", value);
  dlclose(image);
  return value != 42;
}
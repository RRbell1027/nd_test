#include "nd_runtime.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static nd_runtime *session;
static char *session_path;
static size_t references;
nd_runtime *nd_rt_acquire(const char *path) {
 pthread_mutex_lock(&lock);
 if (!path) path = getenv("ND_RUNTIME_DEVICE_PATH");
 if (!path) path = session_path ? session_path : "/dev/ttyS0";
 if (session) {
  if (strcmp(path, session_path)) { errno = EBUSY; pthread_mutex_unlock(&lock); return NULL; }
  ++references;
 } else {
  char *copy = strdup(path);
  if (!copy) { pthread_mutex_unlock(&lock); return NULL; }
  session = nd_rt_open(path);
  if (!session) { int e = errno; free(copy); pthread_mutex_unlock(&lock); errno = e; return NULL; }
  session_path = copy; references = 1;
 }
 nd_runtime *result = session;
 pthread_mutex_unlock(&lock);
 return result;
}
void nd_rt_release(nd_runtime *rt) {
 pthread_mutex_lock(&lock);
 if (rt && rt == session && --references == 0) {
  nd_rt_close(session); session = NULL;
  free(session_path); session_path = NULL;
 }
 pthread_mutex_unlock(&lock);
}

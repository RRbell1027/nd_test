#ifndef ND_RUNTIME_WIRE_H
#define ND_RUNTIME_WIRE_H
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/socket.h>
#define NDR_MAGIC 0x3152444eU
#define NDR_HEADER 24
#define NDR_MAX_BODY (16U * 1024U * 1024U)
#define NDR_MAX_ARGS 16
#define NDR_SLOTS 64
enum ndr_op { NDR_HELLO=1, NDR_LOAD, NDR_UNLOAD, NDR_ALLOC, NDR_FREE,
              NDR_PUT, NDR_GET, NDR_SYMBOL, NDR_LAUNCH, NDR_SHUTDOWN };
static inline uint32_t ndr_get32(const void *v) {
 const unsigned char *p=v;return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static inline uint64_t ndr_get64(const void *v) {
 const unsigned char *p=v;return (uint64_t)ndr_get32(p)|((uint64_t)ndr_get32(p+4)<<32);
}
static inline void ndr_put32(void *v,uint32_t x) {
 unsigned char *p=v;for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(x>>(8*i));
}
static inline void ndr_put64(void *v,uint64_t x) {
 unsigned char *p=v;ndr_put32(p,(uint32_t)x);ndr_put32(p+4,(uint32_t)(x>>32));
}
/* Idle executor may wait indefinitely for a header; transaction I/O is bounded. */
static inline int ndr_io(int fd,void *data,size_t size,int writing,int timeout) {
 unsigned char *p=data;
 while(size) {
  struct pollfd event={fd,writing?POLLOUT:POLLIN,0};int ready;
  do{ready=poll(&event,1,timeout);}while(ready<0&&errno==EINTR);
  if(ready<=0){if(!ready)errno=ETIMEDOUT;return -1;}
  if(!(event.revents&event.events)){errno=EPIPE;return -1;}
  ssize_t n;
  if(writing) { n=send(fd,p,size,MSG_NOSIGNAL);if(n<0&&errno==ENOTSOCK)n=write(fd,p,size); }
  else n=read(fd,p,size);
  if(n<0&&(errno==EINTR||errno==EAGAIN))continue;
  if(n<=0){if(!n)errno=EPIPE;return -1;}
  p+=n;size-=(size_t)n;
 }
 return 0;
}
#endif

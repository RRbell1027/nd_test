#define _DEFAULT_SOURCE
#include "nd_runtime.h"
#include "wire.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/stat.h>
struct nd_runtime {int fd,broken,bits;uint32_t sequence;pthread_mutex_t mutex;char error[256];};
static int rpc(nd_runtime *r,uint32_t op,const void *body,size_t size,void *answer,size_t capacity,size_t *received) {
 if(!r||size>NDR_MAX_BODY){errno=EINVAL;return -1;}
 pthread_mutex_lock(&r->mutex);int result=-1;unsigned char h[NDR_HEADER]={0};
 if(r->broken){errno=EPIPE;goto done;}
 ndr_put32(h,NDR_MAGIC);ndr_put32(h+4,op);ndr_put32(h+8,++r->sequence);ndr_put32(h+16,(uint32_t)size);
 if(ndr_io(r->fd,h,sizeof h,1,10000)||ndr_io(r->fd,(void*)body,size,1,10000)||ndr_io(r->fd,h,sizeof h,0,10000))goto broken;
 if(ndr_get32(h)!=NDR_MAGIC||ndr_get32(h+4)!=op||ndr_get32(h+8)!=r->sequence||ndr_get32(h+20)) {errno=EPROTO;goto broken;}
 size_t n=ndr_get32(h+16);int32_t status=(int32_t)ndr_get32(h+12);
 if(status>0||status < -4095){errno=EPROTO;goto broken;}
 if(status) {
  if(n>=sizeof r->error){errno=EPROTO;goto broken;}
  if(ndr_io(r->fd,r->error,n,0,10000))goto broken;
  r->error[n]=0;errno=-status;goto done;
 }
 if(n>capacity){errno=EMSGSIZE;goto broken;}
 if(ndr_io(r->fd,answer,n,0,10000))goto broken;
 if(received)*received=n;
 r->error[0]=0;result=0;goto done;
broken:
 r->broken=1;snprintf(r->error,sizeof r->error,"RPC stream: %s",strerror(errno));
done: {
 int saved=errno;pthread_mutex_unlock(&r->mutex);errno=saved;return result;
 }
}
static int value_rpc(nd_runtime *r,uint32_t op,const void *b,size_t n,uint64_t *v) {
 unsigned char answer[8];size_t got;
 if(rpc(r,op,b,n,answer,8,&got))return -1;
 if(got!=8){errno=EPROTO;return -1;}*v=ndr_get64(answer);return 0;
}
static int empty_rpc(nd_runtime *r,uint32_t op,const void *b,size_t n) {
 size_t got;return rpc(r,op,b,n,NULL,0,&got);
}
nd_runtime *nd_rt_connect_fd(int fd) {
 nd_runtime *r=calloc(1,sizeof *r);if(!r){close(fd);return NULL;}
 r->fd=fd;int rc=pthread_mutex_init(&r->mutex,NULL);
 if(rc){close(fd);free(r);errno=rc;return NULL;}
 unsigned char info[8];size_t n;
 if(rpc(r,NDR_HELLO,NULL,0,info,sizeof info,&n)||n!=8||(ndr_get32(info)!=32&&ndr_get32(info)!=64)) {
  int e=errno?errno:EPROTO;nd_rt_close(r);errno=e;return NULL;
 }
 r->bits=(int)ndr_get32(info);return r;
}
nd_runtime *nd_rt_open(const char *path) {
 int fd=open(path,O_RDWR|O_CLOEXEC|O_NOCTTY|O_NONBLOCK);if(fd<0)return NULL;
 struct stat st;
 if(fstat(fd,&st)||(!S_ISCHR(st.st_mode)&&!S_ISSOCK(st.st_mode))){close(fd);errno=ENOTTY;return NULL;}
 if(isatty(fd)) {
  struct termios t;
  if(tcgetattr(fd,&t)){close(fd);return NULL;}
  cfmakeraw(&t);t.c_cflag|=CLOCAL|CREAD;cfsetispeed(&t,B115200);cfsetospeed(&t,B115200);
  if(tcsetattr(fd,TCSANOW,&t)){close(fd);return NULL;}
 }
 return nd_rt_connect_fd(fd);
}
void nd_rt_close(nd_runtime *r){if(r){close(r->fd);pthread_mutex_destroy(&r->mutex);free(r);}}
const char *nd_rt_error(nd_runtime *r){return r?r->error:"runtime unavailable";}
int nd_rt_device_bits(nd_runtime *r){return r?r->bits:0;}
int nd_rt_load_image(nd_runtime *r,const void *image,size_t n,uint64_t *id) {
 if(!image||n<20||n>NDR_MAX_BODY||!id){errno=EINVAL;return -1;}
 return value_rpc(r,NDR_LOAD,image,n,id);
}
static int handle_rpc(nd_runtime *r,uint32_t op,uint64_t id) {
 unsigned char b[8];ndr_put64(b,id);return empty_rpc(r,op,b,8);
}
int nd_rt_unload_image(nd_runtime *r,uint64_t id){return handle_rpc(r,NDR_UNLOAD,id);}
int nd_rt_free(nd_runtime *r,uint64_t id){return handle_rpc(r,NDR_FREE,id);}
int nd_rt_alloc(nd_runtime *r,size_t n,uint64_t *v) {
 if(!v||!n||n>NDR_MAX_BODY){errno=EINVAL;return -1;}
 unsigned char b[8];ndr_put64(b,n);return value_rpc(r,NDR_ALLOC,b,8,v);
}
int nd_rt_submit(nd_runtime *r,uint64_t address,const void *data,size_t n) {
 if(n>NDR_MAX_BODY-8||(!data&&n)){errno=EINVAL;return -1;}
 unsigned char *b=malloc(n+8);if(!b)return -1;ndr_put64(b,address);if(n)memcpy(b+8,data,n);
 int result=empty_rpc(r,NDR_PUT,b,n+8);int e=errno;free(b);errno=e;return result;
}
int nd_rt_retrieve(nd_runtime *r,void *data,uint64_t address,size_t n) {
 if(n>NDR_MAX_BODY||(!data&&n)){errno=EINVAL;return -1;}
 unsigned char b[16];ndr_put64(b,address);ndr_put64(b+8,n);size_t got;
 if(rpc(r,NDR_GET,b,16,data,n,&got))return -1;
 if(got!=n){errno=EPROTO;return -1;}return 0;
}
int nd_rt_symbol(nd_runtime *r,uint64_t image,const char *name,size_t size,uint64_t *v) {
 if(!name||!v||strlen(name)>255||size>NDR_MAX_BODY){errno=EINVAL;return -1;}
 unsigned char b[272];ndr_put64(b,image);ndr_put64(b+8,size);size_t n=strlen(name)+1;memcpy(b+16,name,n);
 return value_rpc(r,NDR_SYMBOL,b,n+16,v);
}
int nd_rt_launch(nd_runtime *r,uint64_t function,const uint64_t *args,size_t count) {
 if(count>NDR_MAX_ARGS||(!args&&count)){errno=EINVAL;return -1;}
 unsigned char b[16+8*NDR_MAX_ARGS]={0};ndr_put64(b,function);ndr_put32(b+8,(uint32_t)count);
 for(size_t i=0;i<count;i++)ndr_put64(b+16+8*i,args[i]);
 return empty_rpc(r,NDR_LAUNCH,b,16+count*8);
}
int nd_rt_shutdown(nd_runtime *r){return empty_rpc(r,NDR_SHUTDOWN,NULL,0);}

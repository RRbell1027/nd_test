#define _GNU_SOURCE
#include "wire.h"
#include <dlfcn.h>
#include <elf.h>
#include <ffi.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct image {uint64_t id;void *dl;};
struct region {void *address;size_t size;uint64_t image;int owned;};
struct symbol {uint64_t id,image;void *address;};
static struct image images[NDR_SLOTS];
static struct region regions[NDR_SLOTS];
static struct symbol symbols[NDR_SLOTS];
static uint64_t next_id=1;
static size_t allocated;
static int native_elf(const unsigned char *p,size_t n) {
#if defined(__arm__)
 const unsigned machine=EM_ARM;
#elif defined(__x86_64__)
 const unsigned machine=EM_X86_64;
#else
#error Unsupported executor architecture
#endif
 return n>=sizeof(Elf32_Ehdr)&&!memcmp(p,ELFMAG,SELFMAG)&&p[EI_DATA]==ELFDATA2LSB&&p[EI_VERSION]==EV_CURRENT&&p[EI_CLASS]==(sizeof(void*)==4?ELFCLASS32:ELFCLASS64)&&((unsigned)p[16]|(unsigned)p[17]<<8)==ET_DYN&&((unsigned)p[18]|(unsigned)p[19]<<8)==machine;
}
static int range(uint64_t address,uint64_t n) {
 if(address>UINTPTR_MAX||n>SIZE_MAX)return 0;
 for(size_t i=0;i<NDR_SLOTS;i++) {
  uint64_t base=(uintptr_t)regions[i].address;
  if(regions[i].address&&address>=base&&address-base<=regions[i].size&&n<=regions[i].size-(address-base))return 1;
 }
 return 0;
}
static void unload(size_t i) {
 uint64_t id=images[i].id;
 for(size_t j=0;j<NDR_SLOTS;j++) {
  if(symbols[j].image==id)memset(&symbols[j],0,sizeof symbols[j]);
  if(regions[j].image==id)memset(&regions[j],0,sizeof regions[j]);
 }
 dlclose(images[i].dl);memset(&images[i],0,sizeof images[i]);
}
static int operation(uint32_t op,unsigned char *b,size_t n,unsigned char **answer,size_t *size,char *error,size_t cap) {
 uint64_t value=0;*size=0;*answer=NULL;
 switch(op) {
 case NDR_HELLO:
  if(n)return EINVAL;
  *answer=malloc(8);
  if(!*answer)return ENOMEM;
  ndr_put32(*answer,sizeof(void*)*8);ndr_put32(*answer+4,0);*size=8;return 0;
 case NDR_LOAD: {
  if(!native_elf(b,n)){snprintf(error,cap,"expected native shared ELF bytes, not a pathname");return ENOEXEC;
  }
  size_t i;for(i=0;i<NDR_SLOTS&&images[i].id;i++);
  if(i==NDR_SLOTS)return ENOSPC;
  char path[]="/tmp/nd-image-XXXXXX";int fd=mkstemp(path);
  if(fd<0)return errno;
  int status=0;
  if(ndr_io(fd,b,n,1,10000))status=errno;
  if(close(fd)&&!status)status=errno;
  if(status){unlink(path);return status;}
  dlerror();void *dl=dlopen(path,RTLD_NOW|RTLD_LOCAL);
  if(!dl){const char *e=dlerror();snprintf(error,cap,"dlopen transferred ELF: %s",e?e:"unknown");unlink(path);return ENOEXEC;
  }
  unlink(path);images[i]=(struct image){next_id++,dl};value=images[i].id;break;
 }
 case NDR_UNLOAD: {
  if(n!=8)return EINVAL;
  uint64_t id=ndr_get64(b);size_t i;
  for(i=0;i<NDR_SLOTS&&images[i].id!=id;i++);
  if(i==NDR_SLOTS)return ENOENT;
  unload(i);return 0;
 }
 case NDR_ALLOC: {
  if(n!=8)return EINVAL;
  uint64_t bytes=ndr_get64(b);
  if(!bytes||bytes>NDR_MAX_BODY-allocated)return ENOMEM;
  size_t i;for(i=0;i<NDR_SLOTS&&regions[i].address;i++);
  if(i==NDR_SLOTS)return ENOSPC;
  void *p=calloc(1,(size_t)bytes);
  if(!p)return ENOMEM;
  regions[i]=(struct region){p,(size_t)bytes,0,1};allocated+=(size_t)bytes;value=(uintptr_t)p;break;
 }
 case NDR_FREE: {
  if(n!=8)return EINVAL;
  uint64_t addr=ndr_get64(b);size_t i;
  for(i=0;i<NDR_SLOTS;i++)if(regions[i].owned&&(uintptr_t)regions[i].address==addr)break;
  if(i==NDR_SLOTS)return EINVAL;
  allocated-=regions[i].size;free(regions[i].address);memset(&regions[i],0,sizeof regions[i]);return 0;
 }
 case NDR_PUT:
  if(n<8||!range(ndr_get64(b),n-8))return EFAULT;
  memcpy((void*)(uintptr_t)ndr_get64(b),b+8,n-8);return 0;
 case NDR_GET: {
  if(n!=16)return EINVAL;
  uint64_t bytes=ndr_get64(b+8);
  if(bytes>NDR_MAX_BODY||!range(ndr_get64(b),bytes))return EFAULT;
  *answer=malloc(bytes?bytes:1);
  if(!*answer)return ENOMEM;
  memcpy(*answer,(void*)(uintptr_t)ndr_get64(b),(size_t)bytes);*size=(size_t)bytes;return 0;
 }
 case NDR_SYMBOL: {
  if(n<18||n>272||b[n-1]||memchr(b+16,0,n-17))return EINVAL;
  uint64_t id=ndr_get64(b),bytes=ndr_get64(b+8);size_t i;
  if(bytes>NDR_MAX_BODY)return EINVAL;
  for(i=0;i<NDR_SLOTS&&images[i].id!=id;i++);
  if(i==NDR_SLOTS)return ENOENT;
  dlerror();void *p=dlsym(images[i].dl,(char*)b+16);const char *e=dlerror();
  if(e){snprintf(error,cap,"dlsym: %s",e);return ENOENT;
  }
  if(bytes) {
   for(i=0;i<NDR_SLOTS&&regions[i].address;i++);
   if(i==NDR_SLOTS)return ENOSPC;
   regions[i]=(struct region){p,(size_t)bytes,id,0};value=(uintptr_t)p;
  }else{
   for(i=0;i<NDR_SLOTS&&symbols[i].id;i++);
   if(i==NDR_SLOTS)return ENOSPC;
   symbols[i]=(struct symbol){next_id++,id,p};value=symbols[i].id;
  }
  break;
 }
 case NDR_LAUNCH: {
  if(n<16||ndr_get32(b+12))return EINVAL;
  unsigned count=ndr_get32(b+8);
  if(count>NDR_MAX_ARGS||n!=16+8*count)return EINVAL;
  uint64_t id=ndr_get64(b);size_t i;for(i=0;i<NDR_SLOTS&&symbols[i].id!=id;i++);
  if(i==NDR_SLOTS)return ENOENT;
  void *values[NDR_MAX_ARGS],*arguments[NDR_MAX_ARGS];ffi_type *types[NDR_MAX_ARGS];
  for(unsigned j=0;j<count;j++) {
   uint64_t ptr=ndr_get64(b+16+8*j);
   if(ptr&&!range(ptr,1)){snprintf(error,cap,"launch supports mapped pointer arguments only");return EFAULT;
   }
   values[j]=(void*)(uintptr_t)ptr;arguments[j]=&values[j];types[j]=&ffi_type_pointer;
  }
  ffi_cif cif;
  if(ffi_prep_cif(&cif,FFI_DEFAULT_ABI,count,&ffi_type_void,types)!=FFI_OK)return ENOTSUP;
  void (*function)(void);
  _Static_assert(sizeof function == sizeof symbols[i].address, "POSIX function pointer representation" );
  memcpy(&function,&symbols[i].address,sizeof function);
  ffi_call(&cif,function,NULL,arguments);return 0;
 }
 case NDR_SHUTDOWN: return n?EINVAL:0;
 default:return ENOTSUP;
 }
 *answer=malloc(8);
 if(!*answer)return ENOMEM;
 ndr_put64(*answer,value);*size=8;return 0;
}
int main(int argc,char **argv) {
 int in=STDIN_FILENO,out=STDOUT_FILENO;
 if(argc==3&&!strcmp(argv[1],"--fd")){in=out=atoi(argv[2]);}
 else if(argc!=1)return 2;
 int result=0;
 for(;;) {
  unsigned char h[NDR_HEADER];
  if(ndr_io(in,h,sizeof h,0,-1))break;
  uint32_t op=ndr_get32(h+4),size=ndr_get32(h+16);
  if(ndr_get32(h)!=NDR_MAGIC||ndr_get32(h+12)||ndr_get32(h+20)||size>NDR_MAX_BODY){result=2;break;}
  unsigned char *body=malloc(size?size:1);
  if(!body){result=3;break;}
  if(ndr_io(in,body,size,0,10000)){free(body);result=2;break;}
  unsigned char *answer=NULL;size_t bytes=0;char error[240]={0};
  int status=operation(op,body,size,&answer,&bytes,error,sizeof error);free(body);
  if(status){free(answer);answer=NULL;
  if(!*error)snprintf(error,sizeof error,"%s",strerror(status));
  bytes=strlen(error);}
  ndr_put32(h+12,(uint32_t)-status);ndr_put32(h+16,(uint32_t)bytes);
  int io=ndr_io(out,h,sizeof h,1,10000)||ndr_io(out,status?(void*)error:answer,bytes,1,10000);free(answer);
  if(io){result=2;break;}if(op==NDR_SHUTDOWN&&!status)break;
 }
 for(size_t i=0;i<NDR_SLOTS;i++)if(images[i].id)unload(i);
 for(size_t i=0;i<NDR_SLOTS;i++)if(regions[i].owned)free(regions[i].address);
 return result;
}

#include "nd.h"
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
int main(int argc, char **argv) {
 if (argc != 2) { std::fprintf(stderr,"usage: %s SD_FILE\n",argv[0]); return 2; }
 constexpr int Count=128;
 constexpr size_t Bytes=Count*sizeof(float);
 constexpr uint64_t Offset=3; // Non-sector-aligned file offset.
 nd_device *device=nd_device_open(nullptr);
 if(!device){std::perror("device open");return 1;}
 nd_file *file=nd_open(argv[1]);
 if(!file){std::perror("nd_open");nd_device_close(device);return 1;}
 nd_device_ptr device_ptr=nullptr;
 int result=1;
 auto run=[&]() -> bool {
  std::vector<float> expected(Count),actual(Count);
  FILE *fd=std::fopen(argv[1],"rb");
  if(!fd)return false;
  if(std::fseek(fd,Offset,SEEK_SET)){std::fclose(fd);return false;}
  size_t got=std::fread(expected.data(),1,Bytes,fd);std::fclose(fd);
  if(got!=Bytes)return false;
  for(float x:expected)if(!std::isfinite(x))return false;
  if(nd_alloc(device,Bytes,&device_ptr))return false;
  if(nd_read(file,Offset,Bytes,device_ptr)!=Bytes)return false;
  if(nd_copy(device,actual.data(),device_ptr,Bytes,ND_DEVICE_TO_HOST))return false;
  if(std::memcmp(expected.data(),actual.data(),Bytes)) {
   std::fprintf(stderr,"FAIL: SD physical mapping differs from Guest file\n");return false;
  }
  std::puts("PASS: SD direct read matches Guest pread, offset=3");
  errno=0;
  if(nd_read(file,Offset,Bytes+1,device_ptr)!=-1||errno!=EFAULT)return false;
  if(nd_read(file,nd_file_size(file),Bytes,device_ptr)!=0)return false;
  float *data=static_cast<float *>(device_ptr);
  for(int round=0;round<2;++round) {
#pragma omp target is_device_ptr(data)
   {
    for(int i=0;i<Count;++i)data[i]=data[i]*0.5f+1.0f;
   }
   for(float &x:expected)x=x*0.5f+1.0f;
   if(nd_copy(device,actual.data(),device_ptr,Bytes,ND_DEVICE_TO_HOST))return false;
   for(int i=0;i<Count;++i)if(actual[i]!=expected[i]) {
    std::fprintf(stderr,"FAIL: round=%d element=%d expected=%g actual=%g\n",round,i,expected[i],actual[i]);return false;
   }
   std::printf("PASS: is_device_ptr float kernel round=%d, all %d results match\n",round,Count);
  }
  return true;
 };
 if(run())result=0;else std::fprintf(stderr,"FAIL: %s (errno=%d)\n",nd_device_error(device),errno);
 if(device_ptr&&nd_free(device,device_ptr))result=1;
 if(nd_close(file))result=1;
 nd_device_close(device);
 return result;
}

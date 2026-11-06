#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#include "pvr_spirv_descriptors.h"
int main(int argc,char**argv){
 for(int a=1;a<argc;a++){
  FILE*f=fopen(argv[a],"rb"); if(!f){printf("%s missing\n",argv[a]);continue;}
  fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  uint32_t*w=malloc(n); if(fread(w,1,n,f)!=(size_t)n){} fclose(f);
  struct pvr_desc d[PVR_MAX_DESC];
  int nd=pvr_parse_descriptors(w,n/4,d,PVR_MAX_DESC);
  printf("%s: %d descriptors\n",argv[a],nd);
  for(int i=0;i<nd;i++) printf("   set%u binding%u type%d\n",d[i].set,d[i].binding,d[i].type);
  free(w);
 }
 return 0;
}

#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void){
  VkApplicationInfo ai; memset(&ai,0,sizeof(ai)); ai.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.apiVersion=VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici; memset(&ici,0,sizeof(ici)); ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo=&ai;
  VkInstance inst; if(vkCreateInstance(&ici,NULL,&inst)!=VK_SUCCESS){printf("no instance\n");return 2;}
  uint32_t n=0; vkEnumeratePhysicalDevices(inst,&n,NULL); VkPhysicalDevice*pd=calloc(n,sizeof(*pd)); vkEnumeratePhysicalDevices(inst,&n,pd);
  uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd[0],&qn,NULL); VkQueueFamilyProperties*q=calloc(qn,sizeof(*q)); vkGetPhysicalDeviceQueueFamilyProperties(pd[0],&qn,q);
  uint32_t qfi=0; for(uint32_t i=0;i<qn;i++) if(q[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){qfi=i;break;}
  float prio=1; VkDeviceQueueCreateInfo qci; memset(&qci,0,sizeof(qci)); qci.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qci.queueFamilyIndex=qfi; qci.queueCount=1; qci.pQueuePriorities=&prio;
  VkDeviceCreateInfo dci; memset(&dci,0,sizeof(dci)); dci.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
  printf("creating device...\n"); fflush(stdout);
  VkDevice dev; if(vkCreateDevice(pd[0],&dci,NULL,&dev)!=VK_SUCCESS){printf("no device\n");return 2;}
  printf("device created %p\n",(void*)dev); fflush(stdout);
  { uint64_t *w=(uint64_t*)dev; uint64_t p0=w[0]; uint64_t magic=0;
    if(p0>0x1000) magic=*(uint64_t*)p0;
    printf("  device[0]=%016lx -> magic=%016lx (expect 10aded040410aded)\n", p0, magic); fflush(stdout);}
  const char *names[] = {"vkCreateShaderModule","vkCreateGraphicsPipelines","vkCreatePipelineLayout",
    "vkCreateDescriptorSetLayout","vkDestroyShaderModule","vkDestroyPipeline","vkQueueSubmit",
    "vkCreateBuffer","vkAllocateMemory","vkGetDeviceQueue","vkCmdDraw","vkCreateImage","vkMapMemory",
    "vkCreateSwapchainKHR","vkAcquireNextImageKHR","vkQueuePresentKHR","vkCreateCommandPool",
    "vkAllocateCommandBuffers","vkBeginCommandBuffer","vkCmdPipelineBarrier","vkCreateFence",
    "vkWaitForFences","vkResetFences","vkCreateSemaphore","vkGetPipelineLayout","vkCreateRenderPass",
    "vkCreateFramebuffer","vkCreateImageView","vkDestroyPipelineLayout","vkUpdateDescriptorSets",
    "vkAllocateDescriptorSets","vkResetDescriptorPool","vkCreateDescriptorPool","vkGetDescriptorSetLayoutSupportKHR"};
  int nulls=0;
  for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++){
    printf("  asking for %s ... ",names[i]); fflush(stdout);
    void*p=vkGetDeviceProcAddr(dev,names[i]);
    printf("%s\n", p?"ok":"NULL");
    if(!p){printf("NULL: %s\n",names[i]);nulls++;}
  }
  printf("%d NULL device functions\n",nulls);
  return nulls?1:0;
}

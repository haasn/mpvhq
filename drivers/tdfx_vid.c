
#include <linux/config.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/errno.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,10)
#include <linux/malloc.h>
#else
#include <linux/slab.h>
#endif

#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/agp_backend.h>

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/io.h>

#include "tdfx_vid.h"
#include "3dfx.h"


#define TDFX_VID_MAJOR 178

MODULE_AUTHOR("Albeu");

#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif

#ifndef min
#define min(x,y) (((x)<(y))?(x):(y))
#endif

static struct pci_dev *pci_dev;

static uint8_t *tdfx_mmio_base = 0;
static uint32_t tdfx_mem_base = 0;
static struct voodoo_2d_reg_t* tdfx_2d_regs = NULL;

static int tdfx_ram_size = 0;

static int tdfx_vid_in_use = 0;

static drm_agp_t *drm_agp = NULL;
static agp_kern_info agp_info;
static agp_memory *agp_mem = NULL;



static inline u32 tdfx_inl(unsigned int reg) {
  return readl(tdfx_mmio_base + reg);
}

static inline void tdfx_outl(unsigned int reg, u32 val) {
  writel(val,tdfx_mmio_base  + reg);
}

static inline void banshee_make_room(int size) {
  while((tdfx_inl(STATUS) & 0x1f) < size);
}
 
static inline void banshee_wait_idle(void) {
  int i = 0;

  banshee_make_room(1);
  tdfx_outl(COMMAND_3D, COMMAND_3D_NOP);

  while(1) {
    i = (tdfx_inl(STATUS) & STATUS_BUSY) ? 0 : i + 1;
    if(i == 3) break;
  }
}

static unsigned long get_lfb_size(void) {
  u32 draminit0 = 0;
  u32 draminit1 = 0;
  //  u32 miscinit1 = 0;
  u32 lfbsize   = 0;
  int sgram_p     = 0;

  draminit0 = tdfx_inl(DRAMINIT0);  
  draminit1 = tdfx_inl(DRAMINIT1);

  if ((pci_dev->device == PCI_DEVICE_ID_3DFX_BANSHEE) ||
      (pci_dev->device == PCI_DEVICE_ID_3DFX_VOODOO3)) {
    sgram_p = (draminit1 & DRAMINIT1_MEM_SDRAM) ? 0 : 1;
  
    lfbsize = sgram_p ?
      (((draminit0 & DRAMINIT0_SGRAM_NUM)  ? 2 : 1) * 
       ((draminit0 & DRAMINIT0_SGRAM_TYPE) ? 8 : 4) * 1024 * 1024) :
      16 * 1024 * 1024;
  } else {
    /* Voodoo4/5 */
    u32 chips, psize, banks;

    chips = ((draminit0 & (1 << 26)) == 0) ? 4 : 8;
    psize = 1 << ((draminit0 & 0x38000000) >> 28);
    banks = ((draminit0 & (1 << 30)) == 0) ? 2 : 4;
    lfbsize = chips * psize * banks;
    lfbsize <<= 20;
  }

#if 0
  /* disable block writes for SDRAM (why?) */
  miscinit1 = tdfx_inl(MISCINIT1);
  miscinit1 |= sgram_p ? 0 : MISCINIT1_2DBLOCK_DIS;
  miscinit1 |= MISCINIT1_CLUT_INV;

  banshee_make_room(1); 
  tdfx_outl(MISCINIT1, miscinit1);
#endif

  return lfbsize;
}

static int tdfx_vid_find_card(void)
{
  struct pci_dev *dev = NULL;
  //  unsigned int card_option;

  if((dev = pci_find_device(PCI_VENDOR_ID_3DFX, PCI_DEVICE_ID_3DFX_BANSHEE, NULL)))
    printk(KERN_INFO "tdfx_vid: Found VOODOO BANSHEE\n");
  else if((dev = pci_find_device(PCI_VENDOR_ID_3DFX, PCI_DEVICE_ID_3DFX_VOODOO3, NULL)))
    printk(KERN_INFO "tdfx_vid: Found VOODOO 3 \n");
  else
    return 0;

  
  pci_dev = dev;

#if LINUX_VERSION_CODE >= 0x020300
  tdfx_mmio_base = ioremap_nocache(dev->resource[0].start,1 << 24);
  tdfx_mem_base =  dev->resource[1].start;
#else
  tdfx_mmio_base = ioremap_nocache(dev->base_address[1] & PCI_BASE_ADDRESS_MEM_MASK,0x4000);
  tdfx_mem_base =  dev->base_address[1] & PCI_BASE_ADDRESS_MEM_MASK;
#endif
  printk(KERN_INFO "tdfx_vid: MMIO at 0x%p\n", tdfx_mmio_base);
  tdfx_2d_regs = (struct voodoo_2d_reg_t*)tdfx_mmio_base;
  tdfx_ram_size = get_lfb_size();

  printk(KERN_INFO "tdfx_vid: Found %d MB (%d bytes) of memory\n",
	 tdfx_ram_size / 1024 / 1024,tdfx_ram_size);

  
#if 0
  {
    int temp;
    printk("List resources -----------\n");
    for(temp=0;temp<DEVICE_COUNT_RESOURCE;temp++){
      struct resource *res=&pci_dev->resource[temp];
      if(res->flags){
	int size=(1+res->end-res->start)>>20;
	printk(KERN_DEBUG "res %d:  start: 0x%X   end: 0x%X  (%d MB) flags=0x%X\n",temp,res->start,res->end,size,res->flags);
	if(res->flags&(IORESOURCE_MEM|IORESOURCE_PREFETCH)){
	  if(size>tdfx_ram_size && size<=64) tdfx_ram_size=size;
	}
      }
    }
  }
#endif


  return 1;
}

static int agp_init(void) {

  drm_agp = (drm_agp_t*)inter_module_get("drm_agp");

  if(!drm_agp) {
    printk(KERN_ERR "tdfx_vid: Unable to get drm_agp pointer\n");
    return 0;
  }

  if(drm_agp->acquire()) {
    printk(KERN_ERR "tdfx_vid: Unable to acquire the agp backend\n");
    drm_agp = NULL;
    return 0;
  }

  drm_agp->copy_info(&agp_info);
#if 0
  printk(KERN_DEBUG "AGP Version : %d %d\n"
	 "AGP Mode: %#X\nAperture Base: %p\nAperture Size: %d\n"
	 "Max memory = %d\nCurrent mem = %d\nCan use perture : %s\n"
	 "Page mask = %#X\n",
	 agp_info.version.major,agp_info.version.minor,
	 agp_info.mode,agp_info.aper_base,agp_info.aper_size,
	 agp_info.max_memory,agp_info.current_memory,
	 agp_info.cant_use_aperture ? "no" : "yes",
	 agp_info.page_mask);
#endif
  drm_agp->enable(agp_info.mode);

  
  printk(KERN_INFO "AGP Enabled\n");

  return 1;
}
    
static void agp_close(void) {

  if(!drm_agp) return;

  if(agp_mem) {
    drm_agp->unbind_memory(agp_mem);
    drm_agp->free_memory(agp_mem);
    agp_mem = NULL;
  }
      

  drm_agp->release();

}

static int agp_move(tdfx_vid_agp_move_t* m) {
  //  u32 mov = 0;
  u32 src = 0;
  u32 src_h,src_l;

  if(!agp_mem)
    return (-EAGAIN);

  if(m->move2 > 3) {
    printk(KERN_DEBUG "tdfx_vid: AGP move invalid destination %d\n",
	   m->move2);
    return (-EAGAIN);
  }


  src =  agp_info.aper_base +  m->src;

  src_l = (u32)src;
  src_h = (m->width | (m->src_stride << 14)) & 0x0FFFFFFF;

  //  banshee_wait_idle();
  banshee_make_room(6);
  tdfx_outl(AGPHOSTADDRESSHIGH,src_h);
  tdfx_outl(AGPHOSTADDRESSLOW,src_l);

  tdfx_outl(AGPGRAPHICSADDRESS, m->dst);
  tdfx_outl(AGPGRAPHICSSTRIDE, m->dst_stride);
  tdfx_outl(AGPREQSIZE,m->src_stride*m->height);

  tdfx_outl(AGPMOVECMD,m->move2 << 3);
  banshee_wait_idle();

  banshee_make_room(5);
  tdfx_outl(AGPHOSTADDRESSHIGH,0);
  tdfx_outl(AGPHOSTADDRESSLOW,0);

  tdfx_outl(AGPGRAPHICSADDRESS,0);
  tdfx_outl(AGPGRAPHICSSTRIDE,0);
  tdfx_outl(AGPREQSIZE,0);
  banshee_wait_idle();

  return 0;
}

static void setup_fifo(u32 offset,ssize_t pages) {
  long addr = agp_info.aper_base + offset;
  u32 size = pages | 0x700; // fifo on, in agp mem, disable hole cnt

  banshee_wait_idle();

  tdfx_outl(CMDBASEADDR0,addr >> 4);
  tdfx_outl(CMDRDPTRL0, addr << 4);
  tdfx_outl(CMDRDPTRH0, addr >> 28);
  tdfx_outl(CMDAMIN0, (addr - 4) & 0xFFFFFF);
  tdfx_outl(CMDAMAX0, (addr - 4) & 0xFFFFFF);
  tdfx_outl(CMDFIFODEPTH0, 0);
  tdfx_outl(CMDHOLECNT0, 0);
  tdfx_outl(CMDBASESIZE0,size);

  banshee_wait_idle();
  
}

static int bump_fifo(u16 size) {

  banshee_wait_idle();
  tdfx_outl(CMDBUMP0 , size);
  banshee_wait_idle();

  return 0;
}

static void tdfx_vid_get_config(tdfx_vid_config_t* cfg) {
  u32 in;

  cfg->version = TDFX_VID_VERSION;
  cfg->ram_size = tdfx_ram_size;

  in = tdfx_inl(VIDSCREENSIZE);
  cfg->screen_width = in & 0xFFF;
  cfg->screen_height = (in >> 12) & 0xFFF;
  in = (tdfx_inl(VIDPROCCFG)>> 18)& 0x7;
  switch(in) {
  case 0:
    cfg->screen_format = TDFX_VID_FORMAT_BGR8;
    break;
  case 1:
    cfg->screen_format = TDFX_VID_FORMAT_BGR16;
    break;
  case 2:
    cfg->screen_format = TDFX_VID_FORMAT_BGR24;
    break;
  case 3:
    cfg->screen_format = TDFX_VID_FORMAT_BGR32;
    break;
  default:
    printk(KERN_INFO "tdfx_vid: unknow screen format %d\n",in);
    cfg->screen_format = 0;
    break;
  }
  cfg->screen_stride = tdfx_inl(VIDDESKSTRIDE);
  cfg->screen_start = tdfx_inl(VIDDESKSTART);
}

inline static u32 tdfx_vid_make_format(int src,u16 stride,u32 fmt) {
  u32 r = stride & 0xFFF3;
  u32 tdfx_fmt = 0;

  // src and dest formats
  switch(fmt) {
  case TDFX_VID_FORMAT_BGR8:
    tdfx_fmt = 1;
    break;
  case TDFX_VID_FORMAT_BGR16:
    tdfx_fmt = 3;
    break;
  case TDFX_VID_FORMAT_BGR24:
    tdfx_fmt = 4;
    break;
  case TDFX_VID_FORMAT_BGR32:
    tdfx_fmt = 5;
    break;
  }

  if(!src && !tdfx_fmt) {
    printk(KERN_INFO "tdfx_vid: Invalid destination format %#X\n",fmt);
    return 0;
  }

  if(src && !tdfx_fmt) {
    // src only format
    switch(fmt){
    case TDFX_VID_FORMAT_BGR1:
      tdfx_fmt = 0;
      break;
    case TDFX_VID_FORMAT_YUY2:
      tdfx_fmt = 8;
      break;
    case TDFX_VID_FORMAT_UYVY:
      tdfx_fmt = 9;
      break;
    default:
      printk(KERN_INFO "tdfx_vid: Invalid source format %#X\n",fmt);
      return 0;
    }
  }

  r |= tdfx_fmt << 16;

  return r;
}

static int tdfx_vid_blit(tdfx_vid_blit_t* blit) {
  u32 src_fmt,dst_fmt;
  u32 cmin,cmax,srcbase,srcxy,srcfmt,srcsize;
  u32 dstbase,dstxy,dstfmt,dstsize;
  
  //printk(KERN_INFO "tdfx_vid: Make src fmt 0x%x\n",blit->src_format);
  src_fmt = tdfx_vid_make_format(1,blit->src_stride,blit->src_format);
  if(!src_fmt)
    return 0;
  //printk(KERN_INFO "tdfx_vid: Make dst fmt 0x%x\n", blit->dst_format);
  dst_fmt = tdfx_vid_make_format(0,blit->dst_stride,blit->dst_format);
  if(!dst_fmt)
    return 0;

  // Save the regs otherwise fb get crazy
  // we can perhaps avoid some ...
  cmin = tdfx_inl(CLIP0MIN);
  cmax = tdfx_inl(CLIP0MAX);
  srcbase = tdfx_inl(SRCBASE);
  srcxy = tdfx_inl(SRCXY);
  srcfmt = tdfx_inl(SRCFORMAT);
  srcsize = tdfx_inl(SRCSIZE);
  dstbase = tdfx_inl(DSTBASE);
  dstxy = tdfx_inl(DSTXY);
  dstfmt = tdfx_inl(DSTFORMAT);
  dstsize = tdfx_inl(DSTSIZE);

  // Get rid of the clipping at the moment
  banshee_make_room(11);
  
  tdfx_outl(CLIP0MIN,0);
  tdfx_outl(CLIP0MAX,0x0fff0fff);

  // Setup the src
  tdfx_outl(SRCBASE,blit->src & 0x00FFFFFF);
  tdfx_outl(SRCXY,XYREG(blit->src_x,blit->src_y));
  tdfx_outl(SRCFORMAT,src_fmt);
  tdfx_outl(SRCSIZE,XYREG(blit->src_w,blit->src_h));

  // Setup the dst
  tdfx_outl(DSTBASE,blit->dst & 0x00FFFFFF);
  tdfx_outl(DSTXY,XYREG(blit->dst_x,blit->dst_y));
  tdfx_outl(DSTFORMAT,dst_fmt);
  tdfx_outl(DSTSIZE,XYREG(blit->dst_w,blit->dst_h));

  // Send the command
  tdfx_outl(COMMAND_2D,0xcc000102); // | (ROP_COPY << 24));
  banshee_wait_idle();

  // Now restore the regs to make fb happy
  banshee_make_room(10);
  tdfx_outl(CLIP0MIN, cmin);
  tdfx_outl(CLIP0MAX, cmax);
  tdfx_outl(SRCBASE, srcbase);
  tdfx_outl(SRCXY, srcxy);
  tdfx_outl(SRCFORMAT, srcfmt);
  tdfx_outl(SRCSIZE, srcsize);
  tdfx_outl(DSTBASE, dstbase);
  tdfx_outl(DSTXY, dstxy);
  tdfx_outl(DSTFORMAT, dstfmt);
  tdfx_outl(DSTSIZE, dstsize);
  banshee_wait_idle();
  
  return 1;
}

static int tdfx_vid_set_yuv(unsigned long arg) {
  tdfx_vid_yuv_t yuv;

  if(copy_from_user(&yuv,(tdfx_vid_yuv_t*)arg,sizeof(tdfx_vid_yuv_t))) {
    printk(KERN_DEBUG "tdfx_vid:failed copy from userspace\n");
    return(-EFAULT); 
  }
  banshee_make_room(2);
  tdfx_outl(YUVBASEADDRESS,yuv.base & 0x01FFFFFF);
  tdfx_outl(YUVSTRIDE, yuv.stride & 0x3FFF);
  
  banshee_wait_idle();
  
  return 0;
}

static int tdfx_vid_get_yuv(unsigned long arg) {
  tdfx_vid_yuv_t yuv;

  yuv.base = tdfx_inl(YUVBASEADDRESS) & 0x01FFFFFF;
  yuv.stride = tdfx_inl(YUVSTRIDE) & 0x3FFF;

  if(copy_to_user((tdfx_vid_yuv_t*)arg,&yuv,sizeof(tdfx_vid_yuv_t))) {
      printk(KERN_INFO "tdfx_vid:failed copy to userspace\n");
      return(-EFAULT); 
  }

  return 0;
}

static int tdfx_vid_ioctl(struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
  tdfx_vid_agp_move_t move;
  tdfx_vid_config_t cfg;
  tdfx_vid_blit_t blit;
  u16 int16;

  switch(cmd) {
  case TDFX_VID_AGP_MOVE:
    if(copy_from_user(&move,(tdfx_vid_agp_move_t*)arg,sizeof(tdfx_vid_agp_move_t))) {
      printk(KERN_INFO "tdfx_vid:failed copy from userspace\n");
      return(-EFAULT); 
    }
    return agp_move(&move);
  case TDFX_VID_BUMP0:
    if(copy_from_user(&int16,(u16*)arg,sizeof(u16))) {
      printk(KERN_INFO "tdfx_vid:failed copy from userspace\n");
      return(-EFAULT); 
    }
    return bump_fifo(int16);
  case TDFX_VID_BLIT:
    if(copy_from_user(&blit,(tdfx_vid_blit_t*)arg,sizeof(tdfx_vid_blit_t))) {
      printk(KERN_INFO "tdfx_vid:failed copy from userspace\n");
      return(-EFAULT); 
    }
    if(!tdfx_vid_blit(&blit)) {
      printk(KERN_INFO "tdfx_vid: Blit failed\n");
      return(-EFAULT); 
    }
    return 0;
  case TDFX_VID_GET_CONFIG:
    if(copy_from_user(&cfg,(tdfx_vid_config_t*)arg,sizeof(tdfx_vid_config_t))) {
      printk(KERN_INFO "tdfx_vid:failed copy from userspace\n");
      return(-EFAULT); 
    }
    tdfx_vid_get_config(&cfg);
    if(copy_to_user((tdfx_vid_config_t*)arg,&cfg,sizeof(tdfx_vid_config_t))) {
      printk(KERN_INFO "tdfx_vid:failed copy to userspace\n");
      return(-EFAULT); 
    }
    return 0;
  case TDFX_VID_SET_YUV:
    return tdfx_vid_set_yuv(arg);
  case TDFX_VID_GET_YUV:
    return tdfx_vid_get_yuv(arg);
  default:
    printk(KERN_ERR "tdfx_vid: Invalid ioctl %d\n",cmd);
    return (-EINVAL);
  } 
  return 0;
}



static ssize_t tdfx_vid_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
	return 0;
}

static ssize_t tdfx_vid_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{

	return 0;
}


static int tdfx_vid_mmap(struct file *file, struct vm_area_struct *vma)
{
  size_t size;
  //u32 pages;
#ifdef MP_DEBUG
  printk(KERN_DEBUG "tdfx_vid: mapping agp memory into userspace\n");
#endif

  if(agp_mem)
    return(-EAGAIN);
	
  size = (vma->vm_end-vma->vm_start + PAGE_SIZE - 1) / PAGE_SIZE;
  
  agp_mem = drm_agp->allocate_memory(size,AGP_NORMAL_MEMORY);
  if(!agp_mem) {
    printk(KERN_ERR "Failed to allocate AGP memory\n");
    return(-ENOMEM);
  }

  if(drm_agp->bind_memory(agp_mem,0)) {
    printk(KERN_ERR "Failed to bind the AGP memory\n");
    drm_agp->free_memory(agp_mem);
    agp_mem = NULL;
    return(-ENOMEM);
  }

  printk(KERN_INFO "%d pages of AGP mem allocated (%ld/%ld bytes) :)))\n",
	 size,vma->vm_end-vma->vm_start,size*PAGE_SIZE);

  //setup_fifo(0,size);


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,3)
  if(remap_page_range(vma, vma->vm_start,agp_info.aper_base,
		      vma->vm_end - vma->vm_start, vma->vm_page_prot)) 
#else
  if(remap_page_range(vma->vm_start, (unsigned long)agp_info.aper_base,
		      vma->vm_end - vma->vm_start, vma->vm_page_prot)) 
#endif
    {
      printk(KERN_ERR "tdfx_vid: error mapping video memory\n");
      return(-EAGAIN);
    }

  
  printk(KERN_INFO "AGP Mem mapped in user space !!!!!\n");

  return 0;
}


static int tdfx_vid_release(struct inode *inode, struct file *file)
{
  //Close the window just in case
#ifdef MP_DEBUG
  printk(KERN_DEBUG "tdfx_vid: Video OFF (release)\n");
#endif

  // Release the agp mem
  if(agp_mem) {
    drm_agp->unbind_memory(agp_mem);
    drm_agp->free_memory(agp_mem);
    agp_mem = NULL;
  }
  
  tdfx_vid_in_use = 0;

  MOD_DEC_USE_COUNT;
  return 0;
}

static long long tdfx_vid_lseek(struct file *file, long long offset, int origin)
{
	return -ESPIPE;
}					 

static int tdfx_vid_open(struct inode *inode, struct file *file)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,2)
	int minor = MINOR(inode->i_rdev.value);
#else
	int minor = MINOR(inode->i_rdev);
#endif

	if(minor != 0)
	 return(-ENXIO);

	if(tdfx_vid_in_use == 1) 
		return(-EBUSY);

	tdfx_vid_in_use = 1;
	MOD_INC_USE_COUNT;
	return(0);
}

#if LINUX_VERSION_CODE >= 0x020400
static struct file_operations tdfx_vid_fops =
{
  llseek:  tdfx_vid_lseek,
  read:	   tdfx_vid_read,
  write:   tdfx_vid_write,
  ioctl:   tdfx_vid_ioctl,
  mmap:	   tdfx_vid_mmap,
  open:    tdfx_vid_open,
  release: tdfx_vid_release
};
#else
static struct file_operations tdfx_vid_fops =
{
  tdfx_vid_lseek,
  tdfx_vid_read,
  tdfx_vid_write,
  NULL,
  NULL,
  tdfx_vid_ioctl,
  tdfx_vid_mmap,
  tdfx_vid_open,
  NULL,
  tdfx_vid_release
};
#endif


int init_module(void)
{
  tdfx_vid_in_use = 0;

  if(register_chrdev(TDFX_VID_MAJOR, "tdfx_vid", &tdfx_vid_fops)) {
    printk(KERN_ERR "tdfx_vid: unable to get major: %d\n", TDFX_VID_MAJOR);
    return -EIO;
  }

  if(!agp_init()) {
    printk(KERN_ERR "tdfx_vid: AGP init failed\n");
    unregister_chrdev(TDFX_VID_MAJOR, "tdfx_vid");
    return -EINVAL;
  }

  if (!tdfx_vid_find_card()) {
    printk(KERN_ERR "tdfx_vid: no supported devices found\n");
    agp_close();
    unregister_chrdev(TDFX_VID_MAJOR, "tdfx_vid");
    return -EINVAL;
  }

  

  return (0);

}

void cleanup_module(void)
{
  if(tdfx_mmio_base)
    iounmap(tdfx_mmio_base);
  agp_close();
  printk(KERN_INFO "tdfx_vid: Cleaning up module\n");
  unregister_chrdev(TDFX_VID_MAJOR, "tdfx_vid");
}

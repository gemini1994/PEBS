#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/delay.h>
//#include <linux/slab.h>
//#include <linux/kprobes.h>
//#include <linux/kallsyms.h>
//#include <linux/percpu.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <asm/msr.h>
#include <asm/uaccess.h>
//#include <asm/processor.h>
//#include <asm/cpufeature.h>
#define PEBSBUFFERMAXLEN 11000

struct pebs_v1 {
    u64 flags;
    u64 ip;
    u64 regs[16];
    u64 status;
    u64 dla;
    u64 dse;
    u64 lat;
};

struct debug_store {
    u64 buffer_base;
    u64 index;
    u64 absolute_maximum;
    u64 interrupt_threshold;
    u64 pebs_buffer_base;
    u64 pebs_index;
    u64 pebs_absolute_maximum;
    u64 pebs_interrupt_threshold;
    u64 pebs_event_reset[4];
};

static DEFINE_PER_CPU(struct debug_store *, ds_base);

static struct pebs_v1 pebs_buffer[PEBSBUFFERMAXLEN];
static struct debug_store ds;

void filewrite(char *filename, char *data){
    struct file *filp;
    mm_segment_t fs;
    filp = filp_open(filename, O_RDWR|O_APPEND, 0644);
    if(IS_ERR(filp)){
        printk("open error!\n");
        return;
    }
    fs=get_fs();
    set_fs(KERNEL_DS);
    filp->f_op->write(filp,data,strlen(data),&filp->f_pos);
    set_fs(fs);
    filp_close(filp,NULL);
}

static int __init config(void){
    //struct debug_store *ds = this_cpu_read(ds_base);
    u64 dsval,eax;
    //check version
    eax = cpuid_eax(10);
    if ((eax & 0xff) < 3){
        pr_err("Need at least arch_perfmon version 3, your version is %llu\n",(eax && 0xff));
        return -EIO;
    }

    u64 pebs_en,ds_area,evtsel3;
    rdmsrl(MSR_IA32_PEBS_ENABLE, pebs_en);
    rdmsrl(MSR_IA32_DS_AREA, ds_area);
    rdmsrl(0x189,evtsel3);
    printk("pre msr value: pebs_en: %llu\n ds_area: %llu\n evtsel3: %llu\n",pebs_en,ds_area,evtsel3);
    //pebs config
    //rdmsrl(MSR_IA32_DS_AREA, dsval);
    //ds = (struct debug_store *)dsval;
    //this_cpu_write(ds_base,ds);
    ds.pebs_interrupt_threshold = 10000;
    ds.pebs_absolute_maximum = PEBSBUFFERMAXLEN;
    ds.pebs_index = 0;
    ds.pebs_buffer_base = (u64)pebs_buffer;
    ds.pebs_event_reset[3] = 0xffffffffffff;
    wrmsrl(MSR_IA32_DS_AREA, (u64)&ds);
    //enable PMC3 and precise store facility
    wrmsrl(MSR_IA32_PEBS_ENABLE, 32);//(0xffffffffffffffff & 0x8000000000000008));
    //MEM_TRANS_RETIRED.PRECISE_STORE
    //EVENT CODE: 0XCD MASK: 0X02
    wrmsrl(0x189, (0x02 << 8) + 0xcd);//0x186+3
    rdmsrl(MSR_IA32_PEBS_ENABLE, pebs_en);
    rdmsrl(MSR_IA32_DS_AREA, ds_area);
    rdmsrl(0x189,evtsel3);
    printk("cur msr value: pebs_en: %llu\n ds_area: %llu\n evtsel3: %llu\n",pebs_en,ds_area,evtsel3);
    msleep(10000);
    printk(KERN_INFO "PEBS threshold: %llu, Max: %llu, index: %llu, pebs_buffer_base: %llu\n"
        ,ds.pebs_interrupt_threshold,ds.pebs_absolute_maximum,ds.pebs_index,ds.pebs_buffer_base);
    printk(KERN_INFO "pebs record[0] dla: %llu\n",pebs_buffer[0].dla);
    //enable pebs and precise store
    //wrmsrl();
    //filewrite(TARGET_FILE,(char*)&dsval);
    return 0;
}

static void __exit clean(void){
    printk(KERN_INFO "Goodbye\n");
}

module_init(config);
module_exit(clean);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("yaocheng <jsbyxyc@me.com>");
MODULE_DESCRIPTION("pebs configure");

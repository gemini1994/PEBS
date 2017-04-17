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

//istatic struct debug_store ds;
static int __init config(void){
    struct debug_store *ds;
    u64 eax;
    //check version
    eax = cpuid_eax(10);
    if ((eax & 0xff) < 3){
        pr_err("Need at least arch_perfmon version 3, your version is %llu\n",(eax && 0xff));
        return -EIO;
    }
    //printk("PEBS version: %u %d %d %d %d\n",(cap >> 8) & 0xf,sizeof(unsigned int),sizeof(unsigned long),sizeof(unsigned long long),BIT(3));
    u64 pebs_en,ds_area,evtsel0,evtsel1,evtsel2,evtsel3,evtsel4,evtsel5,evtsel6,evtsel7;

    rdmsrl(MSR_IA32_DS_AREA, ds_area);\
    ds = (struct debug_store*)ds_area;
    rdmsrl(0x186,evtsel0);
    rdmsrl(0x187,evtsel1);
    rdmsrl(0x188,evtsel2);
    rdmsrl(0x189,evtsel3);
    rdmsrl(0x190,evtsel4);
    rdmsrl(0x191,evtsel5);
    rdmsrl(0x192,evtsel6);
    rdmsrl(0x193,evtsel7);
    struct pebs_v1 *pebs_base = (struct pebs_v1*)ds->pebs_buffer_base;
    printk("evtsel: %llu %llu %llu %llu %llu %llu %llu %llu\n",evtsel0,evtsel1,evtsel2,evtsel3,evtsel4,evtsel5,evtsel6,evtsel7);
    printk("pebs_buffer_base:%llu,pebs_index:%llu,pebs_max:%llu,pebs_thred:%llu,pebs_reset[0]:%llu\n",ds->pebs_buffer_base,ds->pebs_index,ds->pebs_absolute_maximum,ds->pebs_interrupt_threshold,ds->pebs_event_reset[0]);
    printk("dla:%llu %llu\n",pebs_base->dla,(struct pebs_v1*)(pebs_base+sizeof(struct pebs_v1))->dla);
}
static void __exit clean(void){
    printk(KERN_INFO "Goodbye\n");
}

module_init(config);
module_exit(clean);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("yaocheng <jsbyxyc@me.com>");
MODULE_DESCRIPTION("pebs configure");

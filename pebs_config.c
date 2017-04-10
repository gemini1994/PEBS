#include <linux/kernel.h>
#include <linux/fs.h>
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
#define TARGET_FILE "/root/pebs_result.txt"

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
    u64 pebs_event_reset[0];
};

static DEFINE_PER_CPU(struct debug_store *, ds_base);

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
    struct debug_store *ds = this_cpu_read(ds_base);
    u64 dsval,eax;
    if(!ds){printk("ds is null\n");}
    //rdmsrl(MSR_IA32_DS_AREA, dsval);
    //this_cpu_write(ds->pebs_interrupt_threshold, 100);
    //this_cpu_write(ds->absolute_maximum, 200);
    //this_cpu_write(ds->pebs_index, 0);
    //wrmsrl(MSR_IA32_DS_AREA,(u64)ds);
    //ds = (struct debug_store *)dsval;
    //printk(KERN_INFO "PEBS threshold: %llu, Max: %llu, index: %llu\n",ds->pebs_interrupt_threshold,ds->pebs_absolute_maximum,ds->pebs_index);
    //check version
    eax = cpuid_eax(10);
    if ((eax & 0xff) < 3){
        pr_err("Need at least arch_perfmon version 3, your version is %llu\n",(eax && 0xff));
        return -EIO;
    }
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

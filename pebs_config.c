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
    u64 dsval;
    rdmsrl(MSR_IA32_DS_AREA, dsval);
    //wrmsrl();
    //filewrite(TARGET_FILE,(char*)&dsval);
    printk(KERN_INFO "msr value: %llu\n",dsval);
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

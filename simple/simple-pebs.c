/* Minimal Linux PEBS driver. */

/*
 * Copyright (c) 2015, Intel Corporation
 * Author: Andi Kleen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Alternatively this code can be used under the terms of the GPLv2.
 */


#define DEBUG 1
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/poll.h>
#include <linux/percpu.h>
#include <linux/wait.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <asm/msr.h>
#include <asm/desc.h>
#include <asm/uaccess.h>
#include <cpuid.h>
#include "simple-pebs.h"

#define MSR_IA32_PERFCTR0    		0x000000c1
#define MSR_IA32_EVNTSEL0    		0x00000186

#define MSR_IA32_PERF_CABABILITIES  	0x00000345
#define MSR_IA32_GLOBAL_STATUS 		0x0000038e
#define MSR_IA32_GLOBAL_CTRL 		0x0000038f
#define MSR_IA32_GLOBAL_OVF_CTRL 	0x00000390
#define MSR_IA32_PEBS_ENABLE		0x000003f1
#define MSR_IA32_DS_AREA     		0x00000600

#define EVTSEL_USR BIT(16)
#define EVTSEL_OS  BIT(17)
#define EVTSEL_INT BIT(20)
#define EVTSEL_EN  BIT(22)

#define PEBS_BUFFER_SIZE	(64 * 1024 * 4 * 16) /* PEBS buffer size */
#define OUT_BUFFER_SIZE		(64 * 1024 * 4 * 16) /* must be multiple of 4k */
#define PERIOD 1

static unsigned pebs_event;

static volatile int pebs_error;

static int pebs_vector = 0xf0;

struct pebs_v1 {
	u64 flags;
	u64 ip;
	u64 ax;
	u64 bx;
	u64 cx;
	u64 dx;
	u64 si;
	u64 di;
	u64 bp;
	u64 sp;
	u64 r8;
	u64 r9;
	u64 r10;
	u64 r11;
	u64 r12;
	u64 r13;
	u64 r14;
	u64 r15;
	u64 status;
	u64 dla;
	u64 dse;
	u64 lat;
};

struct pebs_v2 {
	struct pebs_v1 v1;
	u64 eventing_ip;
	u64 tsx;
};

static int pebs_record_size = sizeof(struct pebs_v1);

#define FEAT1_PDCM 	BIT(15)
#define FEAT2_DS 	BIT(21)

static bool check_cpu(void)
{
	unsigned a, b, c, d;
	unsigned max, model, fam;
	unsigned feat1, feat2;

	__cpuid(0, max, b, c, d);
	if (memcmp(&b, "Genu", 4)) {
		pr_err("Not an Intel CPU\n");
		return false;
	}

	__cpuid(1, a, b, feat1, feat2);
	model = ((a >> 4) & 0xf);
	fam = (a >> 8) & 0xf;
	if (fam == 6 || fam == 0xf)
		model += ((a >> 16) & 0xf) << 4;
	if (fam != 6) {
		pr_err("Not an supported Intel CPU\n");
		return false;
	}

	switch (model) {
    case 30:
        pebs_event = 0x1d1;
        break;
	case 58: /* IvyBridge */
	case 69: /* Haswell */
		pebs_event = 0x1c2; /* UOPS_RETIRED.ALL */
		break;

	case 55: /* Bay Trail */
	case 76: /* Airmont */
	case 77: /* Avoton */
		pebs_event = 0x0c5; /* BR_MISP_RETIRED.ALL_BRANCHES */
		break;
	case 79:
		pebs_event = 0x10d1;//l2 miss
		break;
	default:
		pr_err("Unknown CPU model %d\n", model);
		return false;
	}

	/* Check if we support arch perfmon */
	if (max >= 0xa) {
		__cpuid(0xa, a, b, c, d);
		if ((a & 0xff) < 1) {
			pr_err("No arch perfmon support\n");
			return false;
		}
		if (((a >> 8) & 0xff) < 1) {
			pr_err("No generic counters\n");
			return false;
		}
	} else {
		pr_err("No arch perfmon support\n");
		return false;
	}

	/* check if we support DS */
	if (!(feat2 & FEAT2_DS)) {
		pr_err("No debug store support\n");
		return false;
	}

	/* check perf capability */
	if (feat1 & FEAT1_PDCM) {
		u64 cap;

		rdmsrl(MSR_IA32_PERF_CAPABILITIES, cap);
		switch ((cap >> 8) & 0xf) {
		case 1:
			pebs_record_size = sizeof(struct pebs_v1);
			break;
		case 2:
			pebs_record_size = sizeof(struct pebs_v2);
			break;
		default:
			pr_err("Unsupported PEBS format\n");
			return false;
		}
		/* Could check PEBS_TRAP */
	} else {
		pr_err("No PERF_CAPABILITIES support\n");
		return false;
	}

	return true;
}

static DEFINE_PER_CPU(void *, out_buffer_base);
static DEFINE_PER_CPU(void *, out_buffer);
static DEFINE_PER_CPU(void *, out_buffer_end);

static int simple_pebs_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long len = vma->vm_end - vma->vm_start;
	int cpu = (int)(long)file->private_data;

	if (len % PAGE_SIZE || len != OUT_BUFFER_SIZE || vma->vm_pgoff)
		return -EINVAL;

	if (vma->vm_flags & VM_WRITE)
		return -EPERM;

	if (!cpu_online(cpu))
		return -EIO;

	return remap_pfn_range(vma, vma->vm_start,
			       __pa(per_cpu(out_buffer_base, cpu)) >> PAGE_SHIFT,
			       OUT_BUFFER_SIZE,
			       vma->vm_page_prot);
}

static DEFINE_PER_CPU(wait_queue_head_t, simple_pebs_wait);

static unsigned int simple_pebs_poll(struct file *file, poll_table *wait)
{
	unsigned long cpu = (unsigned long)file->private_data;
	poll_wait(file, &per_cpu(simple_pebs_wait, cpu), wait);
	if (per_cpu(out_buffer, cpu) > per_cpu(out_buffer_base, cpu))
		return POLLIN | POLLRDNORM;
	return 0;
}

static void status_dump(char *where)
{
#if 0
	u64 val, val2;
	rdmsrl(MSR_IA32_GLOBAL_STATUS, val);
	rdmsrl(MSR_IA32_GLOBAL_CTRL, val2);
	pr_debug("%d: %s: status %llx ctrl %llx counter %llx\n", smp_processor_id(), where, val, val2, __builtin_ia32_rdpmc(0));
#endif
}

static void start_stop_cpu(void *arg)
{
	wrmsrl(MSR_IA32_GLOBAL_CTRL, arg ? 1 : 0);
	status_dump("stop");
}

static void reset_buffer_cpu(void *arg)
{
	wrmsrl(MSR_IA32_GLOBAL_CTRL, 0);
	__this_cpu_write(out_buffer, __this_cpu_read(out_buffer_base));
	wrmsrl(MSR_IA32_GLOBAL_CTRL, 1);
}

static DEFINE_MUTEX(reset_mutex);

static long simple_pebs_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	unsigned long cpu = (unsigned long)file->private_data;

	switch (cmd) {
	case SIMPLE_PEBS_SET_CPU:
		cpu = arg;
		if (cpu >= NR_CPUS || !cpu_online(cpu))
			return -EINVAL;
		file->private_data = (void *)cpu;
		return 0;
	case SIMPLE_PEBS_GET_SIZE:
		return put_user(OUT_BUFFER_SIZE, (int *)arg);
	case SIMPLE_PEBS_GET_OFFSET: {
		unsigned len = per_cpu(out_buffer, cpu) - per_cpu(out_buffer_base, cpu);
		return put_user(len, (unsigned *)arg);
	}
	case SIMPLE_PEBS_START:
	case SIMPLE_PEBS_STOP:
		mutex_lock(&reset_mutex);
		smp_call_function_single(cpu, start_stop_cpu,
				(void *)(long)(cmd == SIMPLE_PEBS_START), 1);
		mutex_unlock(&reset_mutex);
		return 0;
	case SIMPLE_PEBS_RESET:
		mutex_lock(&reset_mutex);
		smp_call_function_single(cpu, reset_buffer_cpu, NULL, 1);
		mutex_unlock(&reset_mutex);
		return 0;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations simple_pebs_fops = {
	.owner = THIS_MODULE,
	.mmap =	simple_pebs_mmap,
	.poll = simple_pebs_poll,
	.unlocked_ioctl = simple_pebs_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice simple_pebs_miscdev = {
	MISC_DYNAMIC_MINOR,
	"simple-pebs",
	&simple_pebs_fops
};

struct debug_store {
	u64 bts_base;
	u64 bts_index;
	u64 bts_max;
	u64 bts_thresh;

	u64 pebs_base;
	u64 pebs_index;
	u64 pebs_max;
	u64 pebs_thresh;
	u64 pebs_reset[4];
};

static DEFINE_PER_CPU(struct debug_store *, cpu_ds);
static DEFINE_PER_CPU(unsigned long, cpu_old_ds);

/* Allocate DS and PEBS buffer */
static int allocate_buffer(void)
{
	struct debug_store *ds;
	unsigned num_pebs;

	ds = kmalloc(sizeof(struct debug_store), GFP_KERNEL);
	if (!ds) {
		pr_err("Cannot allocate DS\n");
		return -1;
	}
	memset(ds, 0, sizeof(struct debug_store));
	/* Set up buffer */
	ds->pebs_base = (unsigned long)kmalloc(PEBS_BUFFER_SIZE, GFP_KERNEL);
	if (!ds->pebs_base) {
		pr_err("Cannot allocate PEBS buffer\n");
		kfree(ds);
		return -1;
	}
	memset((void *)ds->pebs_base, 0, PEBS_BUFFER_SIZE);
	num_pebs = PEBS_BUFFER_SIZE / pebs_record_size;
	ds->pebs_index = ds->pebs_base;
	ds->pebs_max = ds->pebs_base + (num_pebs - 1) * pebs_record_size + 1;
	ds->pebs_thresh = ds->pebs_base + (num_pebs - num_pebs/10) * pebs_record_size ;
	ds->pebs_reset[0] = -(long long)PERIOD;
	__this_cpu_write(cpu_ds, ds);

	status_dump("allocate_buffer");
	return 0;
}

static int allocate_out_buf(void)
{
	void *outbu_base;

	outbu_base = kmalloc(OUT_BUFFER_SIZE, GFP_KERNEL);
	if (!outbu_base) {
		pr_err("Cannot allocate out buffer\n");
		return -1;
	}
	__this_cpu_write(out_buffer_base, outbu_base);
	__this_cpu_write(out_buffer, outbu_base);
	__this_cpu_write(out_buffer_end, outbu_base + OUT_BUFFER_SIZE);
	return 0;
}

extern void simple_pebs_entry(void);

#ifdef CONFIG_64BIT

asm("    .globl simple_pebs_entry\n"
    "simple_pebs_entry:\n"
    "    cld\n"
    "    testq $3,8(%rsp)\n"
    "    jz    1f\n"
    "    swapgs\n"
    "1:\n"
    "    pushq $0\n" /* error code */
    "    pushq %rdi\n"
    "    pushq %rsi\n"
    "    pushq %rdx\n"
    "    pushq %rcx\n"
    "    pushq %rax\n"
    "    pushq %r8\n"
    "    pushq %r9\n"
    "    pushq %r10\n"
    "    pushq %r11\n"
    "    pushq %rbx\n"
    "    pushq %rbp\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "1:  call simple_pebs_pmi\n"
    "    popq %r15\n"
    "    popq %r14\n"
    "    popq %r13\n"
    "    popq %r12\n"
    "    popq %rbp\n"
    "    popq %rbx\n"
    "    popq %r11\n"
    "    popq %r10\n"
    "    popq %r9\n"
    "    popq %r8\n"
    "    popq %rax\n"
    "    popq %rcx\n"
    "    popq %rdx\n"
    "    popq %rsi\n"
    "    popq %rdi\n"
    "    addq $8,%rsp\n" /* error code */
    "    testq $3,8(%rsp)\n"
    "    jz 2f\n"
    "    swapgs\n"
    "2:\n"
    "    iretq");

#else
#error write me
#endif

void simple_pebs_pmi(void)
{
	struct debug_store *ds;
	struct pebs_v1 *pebs, *end;
	u64 *outbu, *outbu_end, *outbu_start;

	status_dump("pmi1");

	/* disable PMU */
	wrmsrl(MSR_IA32_GLOBAL_CTRL, 0);

	/* global status ack */
	wrmsrl(MSR_IA32_GLOBAL_OVF_CTRL, 1ULL << 62);

	wrmsrl(MSR_IA32_PERFCTR0, -PERIOD); /* ? sign extension on width ? */

	status_dump("pmi2");

	/* write data to buffer */
	ds = __this_cpu_read(cpu_ds);
	outbu_start = __this_cpu_read(out_buffer_base);
	outbu = __this_cpu_read(out_buffer);
	outbu_end = __this_cpu_read(out_buffer_end);
	end = (struct pebs_v1 *)ds->pebs_index;
	for (pebs = (struct pebs_v1 *)ds->pebs_base;
	     pebs < end && outbu < outbu_end;
	     pebs = (struct pebs_v1 *)((char *)pebs + pebs_record_size)) {
		uint64_t ip = pebs->ip;
		if (pebs_record_size >= sizeof(struct pebs_v2))
			ip = ((struct pebs_v2 *)pebs)->eventing_ip;
		*outbu++ = ip;
	}
	this_cpu_write(out_buffer, outbu);

#if 0
	pr_debug("%d: pmi %llx out %lx max %llx counter %llx num %llu\n",
		smp_processor_id(),
		 ds->pebs_index - ds->pebs_base,
		 (void *)outbu - (void *)outbu_start,
		 ds->pebs_max - ds->pebs_base,
		 __builtin_ia32_rdpmc(0),
		 (ds->pebs_index - ds->pebs_base) / pebs_record_size);
#endif

	/* reset ds */
	ds->pebs_index = ds->pebs_base;

	/* ack apic */
	apic_eoi();
	/* Unmask PMI as, as it got implicitely masked. */
	apic_write(APIC_LVTPC, pebs_vector);

	/* Don't enable for now until the buffer is flushed as we need a real
	 * fifo
	 */

	if ((void *)outbu - (void *)outbu_start >= OUT_BUFFER_SIZE/2) {
		wake_up(this_cpu_ptr(&simple_pebs_wait));
	} else
		wrmsrl(MSR_IA32_GLOBAL_CTRL, 1);

	status_dump("pmi3");
}

/* Get vector */
static int simple_pebs_get_vector(void)
{
	gate_desc desc, *idt;

	/* No locking */
	while (test_bit(pebs_vector, used_vectors)) {
		if (pebs_vector == 0x40) {
			pr_err("No free vector found\n");
			return -1;
		}
		pebs_vector--;
	}
	set_bit(pebs_vector, used_vectors);
	idt = (gate_desc *)kallsyms_lookup_name("idt_table");
	if (!idt) {
		pr_err("Could not resolve idt_table. Did you enable CONFIG_KALLSYMS_ALL?\n");
		return -1;
	}

	pack_gate(&desc, GATE_INTERRUPT, (unsigned long)simple_pebs_entry,
			0, 0, 0);
	write_idt_entry(idt, pebs_vector,&desc);
	return 0;
}

static void simple_pebs_free_vector(void)
{
	clear_bit(pebs_vector, used_vectors);
	/* Not restoring the IDT. Assume the kernel always inits when it reallocates */
}

static DEFINE_PER_CPU(u64, old_lvtpc);
static DEFINE_PER_CPU(int, cpu_initialized);

static void simple_pebs_cpu_init(void *arg)
{
	u64 val;
	unsigned long old_ds;

	/* Set up DS and buffer */
	if (__this_cpu_read(cpu_ds) == NULL) {
		if (allocate_buffer() < 0) {
			pebs_error = 1;
			return;
		}
	}

	if (__this_cpu_read(out_buffer) == NULL) {
		if (allocate_out_buf() < 0) {
			pebs_error = 1;
			return;
		}
	}

	init_waitqueue_head(this_cpu_ptr(&simple_pebs_wait));

	/* Check if someone else is using the PMU */
	rdmsrl(MSR_IA32_EVNTSEL0, val);
	if (val & EVTSEL_EN) {
		pr_err("Someone else using perf counter 0\n");
		pebs_error = 1;
		return;
	}

	/* Set up DS */
	rdmsrl(MSR_IA32_DS_AREA, old_ds);
	__this_cpu_write(cpu_old_ds, old_ds);
	wrmsrl(MSR_IA32_DS_AREA, __this_cpu_read(cpu_ds));

	/* Set up LVT */
	__this_cpu_write(old_lvtpc, apic_read(APIC_LVTPC));
	apic_write(APIC_LVTPC, pebs_vector);

	/* Initialize PMU */

	/* First disable PMU to avoid races */
	wrmsrl(MSR_IA32_GLOBAL_CTRL, 0);

	wrmsrl(MSR_IA32_PERFCTR0, -PERIOD); /* ? sign extension on width ? */
	wrmsrl(MSR_IA32_EVNTSEL0,
		pebs_event | EVTSEL_EN | EVTSEL_USR | EVTSEL_OS);

	/* Enable PEBS for counter 0 */
	wrmsrl(MSR_IA32_PEBS_ENABLE, 1);

	wrmsrl(MSR_IA32_GLOBAL_CTRL, 1);
	__this_cpu_write(cpu_initialized, 1);
}

static void simple_pebs_cpu_reset(void *arg)
{
	if (__this_cpu_read(cpu_initialized)) {
		wrmsrl(MSR_IA32_GLOBAL_CTRL, 0);
		wrmsrl(MSR_IA32_PEBS_ENABLE, 0);
		wrmsrl(MSR_IA32_EVNTSEL0, 0);
		wrmsrl(MSR_IA32_PERFCTR0, 0);
		wrmsrl(MSR_IA32_DS_AREA, __this_cpu_read(cpu_old_ds));
		apic_write(APIC_LVTPC, __this_cpu_read(old_lvtpc));
		__this_cpu_write(cpu_initialized, 0);
	}
	if (__this_cpu_read(out_buffer)) {
		kfree(__this_cpu_read(out_buffer));
		__this_cpu_write(out_buffer, 0);
	}
	if (__this_cpu_read(cpu_ds)) {
		struct debug_store *ds = __this_cpu_read(cpu_ds);
		kfree((void *)ds->pebs_base);
		kfree(ds);
		__this_cpu_write(cpu_ds, 0);
	}
}

static int simple_pebs_cpu(struct notifier_block *nb, unsigned long action,
			 void *v)
{
	switch (action) {
	case CPU_STARTING:
		simple_pebs_cpu_init(NULL);
		break;
	case CPU_DYING:
		simple_pebs_cpu_reset(NULL);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block cpu_notifier = {
	.notifier_call = simple_pebs_cpu,
};

struct file* file_open(const char* path, int flags, int rights){
	struct file* filp = NULL;
	mm_segment_t oldfs;
	int err = 0;

	oldfs = get_fs();
	set_fs(get_ds());
	filp = filp_open(path, flags, rights);
	set_fs(oldfs);
	if(IS_ERR(filp)){
		err = PTR_ERR(filp);
		return NULL;
	}
	return filp;
}

void file_close(struct file* file){
	filp_close(file, NULL);
}

int file_write(struct file* file, unsigned long long offset, unsigned char* data, unsigned int size){
	mm_segment_t oldfs;
	int ret;

	oldfs = get_fs();
	set_fs(get_ds());

	ret = vfs_write(file, data, size, &offset);

	set_fs(oldfs);
	return ret;
}

static int simple_pebs_init(void)
{
	int err;

	if (!check_cpu())
		return -EIO;

	err = simple_pebs_get_vector();
	if (err < 0)
		return -EIO;

	get_online_cpus();
	on_each_cpu(simple_pebs_cpu_init, NULL, 1);
	register_cpu_notifier(&cpu_notifier);
	put_online_cpus();
	if (pebs_error) {
		pr_err("PEBS initialization failed\n");
		err = -EIO;
		goto out_notifier;
	}

	err = misc_register(&simple_pebs_miscdev);
	if (err < 0) {
		pr_err("Cannot register simple-pebs device\n");
		goto out_notifier;
	}
	pr_info("Initialized\n");

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
    struct pebs_v2 *pebs_base = ds->pebs_base;
    printk("evtsel: %llx %llx %llx %llx %llx %llx %llx %llx\n",evtsel0,evtsel1,evtsel2,evtsel3,evtsel4,evtsel5,evtsel6,evtsel7);
    printk("pebs_buffer_base:%llx,pebs_index:%llx,pebs_max:%llx,pebs_thred:%llx,pebs_reset[0]:%llx\n",ds->pebs_base,ds->pebs_index,ds->pebs_max,ds->pebs_thresh,ds->pebs_reset[0]);
	msleep(1000);
    int num = (ds->pebs_index-ds->pebs_base)/(sizeof(struct pebs_v2));
	printk("record num:%d\n",num);

	u64 *record = kmalloc(sizeof(u64)*num,GFP_KERNEL);
	int i = 0;
	while(i<num){
		
		//struct file* fp;
		//fp = file_open("home/tmp_pebs.txt",O_RDWR | O_CREAT, 0644);
		u64 tmp = (struct pebs_v2*)(pebs_base+sizeof(struct pebs_v2)*i)->v1.dla;
		record[i] = tmp;
		//loff_t pos = 0;
		//char *buf = (char *)&tmp;
		//file_write(fp, tmp, sizeof(tmp), &pos);
		i++;
	}
	int j=0;
	struct file *fp;
	mm_segment_t fs;
	loff_t pos;
	fp = filp_open("/home/wkuang/xyc/kernel_file.txt", O_RDWR | O_CREAT, 0644);
	if(IS_ERR(fp)){
		printk("create file error\n");
		return err;
	}
	fs = get_fs();
	set_fs(KERNEL_DS);
	pos = 0;
	vfs_write(fp, record, sizeof(u64)*num, &pos);
	filp_close(fp,NULL);
	set_fs(fs);
	return 0;

out_notifier:
	unregister_cpu_notifier(&cpu_notifier);
	on_each_cpu(simple_pebs_cpu_reset, NULL, 1);
	simple_pebs_free_vector();
	return err;
}
module_init(simple_pebs_init);

static void simple_pebs_exit(void)
{
	misc_deregister(&simple_pebs_miscdev);
	get_online_cpus();
	on_each_cpu(simple_pebs_cpu_reset, NULL, 1);
	unregister_cpu_notifier(&cpu_notifier);
	put_online_cpus();
	simple_pebs_free_vector();
	/* Could PMI still be pending? For now just wait a bit. (XXX) */
	schedule_timeout(HZ);
	pr_info("Exited\n");
}

module_exit(simple_pebs_exit)
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andi Kleen");


// hok_mem.c — 王者荣耀(HoK) ESP 内核读内存模块 (arm64, GKI 通用)
// 设计目标:
//   1) 完全不走 ptrace / process_vm_readv / /proc/pid/mem, 游戏进程里不留 TracerPid、不留额外 fd、不留 va
//   2) 模块自身从 /proc/modules 与 /sys/module 中消失
//   3) 只读, 默认不提供写与改页表, 减少可被扫描的特征
//
// 编译: 见 Makefile (需要目标机同版本内核源码或 GKI kernel-headers)
// 加载: insmod hok_mem.ko kptr=0 page_offset=0xffff000000000000

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/version.h>
#include <linux/delay.h>
#include <asm/pgtable.h>
#include <asm/page.h>
#include <asm/memory.h>
#include <asm/io.h>

#include "hok_mem.h"

#define TAG "hok_mem"

/* ---- 跨版本兼容 ---- */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
#define mmap_read_lock(mm)   down_read(&(mm)->mmap_sem)
#define mmap_read_unlock(mm) up_read(&(mm)->mmap_sem)
#endif
#ifndef PUD_SHIFT
#define PUD_SHIFT 30
#endif
#ifndef PMD_SHIFT
#define PMD_SHIFT 21
#endif
#define HOK_PUD_MASK (~((1UL << PUD_SHIFT) - 1))
#define HOK_PMD_MASK (~((1UL << PMD_SHIFT) - 1))

/* arm64 描述符: bits[1:0]==0b01 为 block(大页), 0b11 为 table(下一级).
   不用 pud_leaf/pmd_leaf, 免得跨版本宏/内联函数签名不一致 */
static inline int hok_pud_block(pud_t p) { return (pud_val(p) & 3UL) == 1UL; }
static inline int hok_pmd_block(pmd_t p) { return (pmd_val(p) & 3UL) == 1UL; }

/* ------------------------------------------------------------------ */
/* 符号解析: kprobe 技巧拿 kallsyms_lookup_name, 6.x GKI 失败时回落到    */
/* 用户态通过 module param 传入的地址                                    */
/* ------------------------------------------------------------------ */
typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t kln = NULL;

static unsigned long addr_init_task   = 0;
static unsigned long addr_memstart    = 0;
static unsigned long addr_kimage_voff = 0;

module_param_named(init_task, addr_init_task, ulong, 0644);
module_param_named(memstart, addr_memstart, ulong, 0644);
module_param_named(kimg_voff, addr_kimage_voff, ulong, 0644);
static unsigned long page_offset = 0xffff000000000000UL;
module_param_named(page_offset, page_offset, ulong, 0644);

/* task_struct 偏移, 不同内核版本不同, 从 /sys/kernel/btf/vmlinux 用 pahole 导出 */
static int off_tasks = 0x4a8;   /* task_struct.tasks   */
static int off_pid   = 0x4d0;   /* task_struct.pid     */
static int off_mm    = 0x550;   /* task_struct.mm      */
static int off_comm  = 0x588;   /* task_struct.comm    */
module_param(off_tasks, int, 0644);
module_param(off_pid,   int, 0644);
module_param(off_mm,    int, 0644);
module_param(off_comm,  int, 0644);

static int resolve_syms(void)
{
    struct kprobe kp;
    int ret;

    memset(&kp, 0, sizeof(kp));
    kp.symbol_name = "kallsyms_lookup_name";
    ret = register_kprobe(&kp);
    if (ret == 0) {
        kln = (kallsyms_lookup_name_t)kp.addr;
        unregister_kprobe(&kp);
    }
    if (!kln)
        return -EINVAL;

    if (!addr_init_task)
        addr_init_task = kln("init_task");
    if (!addr_memstart)
        addr_memstart = kln("memstart_addr");
    if (!addr_kimage_voff)
        addr_kimage_voff = kln("kimage_voffset");

    pr_info(TAG ": syms init_task=%px memstart=%px kimgvoff=%px\n",
            (void *)addr_init_task, (void *)addr_memstart, (void *)addr_kimage_voff);
    return (addr_init_task && addr_memstart) ? 0 : -EINVAL;
}

static inline unsigned long memstart_val(void)
{
    if (!addr_memstart)
        return 0;
    return *(unsigned long *)addr_memstart;
}

/* ------------------------------------------------------------------ */
/* task 查找: 沿 init_task->tasks 链表走, 不碰 pid 哈希                  */
/* ------------------------------------------------------------------ */
#define OFF(p, o) (*(unsigned long *)((char *)(p) + (o)))

static struct task_struct *find_task(pid_t pid)
{
    struct task_struct *t = (struct task_struct *)addr_init_task;
    struct task_struct *cand;
    int guard = 0;

    if (!t)
        return NULL;
    do {
        if ((pid_t)OFF(t, off_pid) == pid)
            return t;
        cand = (struct task_struct *)OFF(t, off_tasks);
        if (!cand)
            break;
        /* tasks 是 list_head: next 指回 &next->tasks, 减偏移得到 task */
        t = (struct task_struct *)((char *)cand - off_tasks);
        if (t == (struct task_struct *)addr_init_task)
            break;
    } while (++guard < 8192);
    return NULL;
}

static struct mm_struct *get_task_mm_safe(struct task_struct *t)
{
    struct mm_struct *mm = (struct mm_struct *)OFF(t, off_mm);
    if (!mm)
        return NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 19, 0)
    if (!mmget_not_zero(mm))
        return NULL;
#else
    atomic_inc(&mm->mm_count);
#endif
    return mm;
}

/* ------------------------------------------------------------------ */
/* arm64 页表走查: pgd/p4d/pud/pmd(block) → pte                        */
/* ------------------------------------------------------------------ */
static unsigned long va_to_kaddr(struct mm_struct *mm, unsigned long va)
{
    pgd_t *pgd; p4d_t *p4d; pud_t *pud; pmd_t *pmd; pte_t *pte;
    unsigned long phys;
    unsigned long ms = memstart_val();

    pgd = pgd_offset(mm, va);
    if (!pgd || pgd_none(*pgd) || pgd_bad(*pgd))
        return 0;
    p4d = p4d_offset(pgd, va);
    if (!p4d || p4d_none(*p4d))
        return 0;
    pud = pud_offset(p4d, va);
    if (!pud || pud_none(*pud))
        return 0;
    if (hok_pud_block(*pud)) {
        phys = (pud_val(*pud) & HOK_PUD_MASK) | (va & ~HOK_PUD_MASK);
        return phys - ms + page_offset;
    }
    pmd = pmd_offset(pud, va);
    if (!pmd || pmd_none(*pmd) || pmd_bad(*pmd))
        return 0;
    if (hok_pmd_block(*pmd)) {
        phys = (pmd_val(*pmd) & HOK_PMD_MASK) | (va & ~HOK_PMD_MASK);
        return phys - ms + page_offset;
    }
    pte = pte_offset_kernel(pmd, va);
    if (!pte || !pte_present(*pte) || pte_special(*pte))
        return 0;
    if (!pte_young(*pte) && !pte_dirty(*pte))
        ; /* 只读页允许 */
    phys = (pte_val(*pte) & PAGE_MASK) | (va & ~PAGE_MASK);
    return phys - ms + page_offset;
}

/* 逐页拷贝, 跨页安全; 不可读页返回失败并清零该段 */
static int vm_read(struct mm_struct *mm, unsigned long va, void *dst, size_t len)
{
    size_t done = 0;
    int ret = 0;

    if (!mm || !dst || len == 0)
        return -EINVAL;

    mmap_read_lock(mm);
    while (done < len) {
        unsigned long cur = va + done;
        size_t n = PAGE_SIZE - (cur & (PAGE_SIZE - 1));
        unsigned long kaddr;

        if (n > len - done)
            n = len - done;

        kaddr = va_to_kaddr(mm, cur);
        if (!kaddr) {
            memset((char *)dst + done, 0, n);
            ret = -EFAULT;
        } else {
            memcpy((char *)dst + done, (void *)kaddr, n);
        }
        done += n;
    }
    mmap_read_unlock(mm);
    return ret;
}

/* ------------------------------------------------------------------ */
/* ioctl 接口                                                          */
/* ------------------------------------------------------------------ */
struct hok_state {
    pid_t pid;
    struct task_struct *task;
    struct mm_struct *mm;
    unsigned long mm_cookie;
};
static struct hok_state g_st;

static void detach_pid(void);

static int attach_pid(pid_t pid)
{
    struct task_struct *t;
    struct mm_struct *mm;

    detach_pid();
    t = find_task(pid);
    if (!t)
        return -ESRCH;
    mm = get_task_mm_safe(t);
    if (!mm)
        return -ESRCH;
    g_st.pid = pid;
    g_st.task = t;
    g_st.mm = mm;
    return 0;
}

static void detach_pid(void)
{
    if (g_st.mm)
        mmput(g_st.mm);
    memset(&g_st, 0, sizeof(g_st));
}

static long hok_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct hok_req req;
    void *kbuf;
    int ret;

    switch (cmd) {
    case HOK_IOCTL_SET_PID:
        ret = attach_pid((pid_t)arg);
        return ret;

    case HOK_IOCTL_CLEAR:
        detach_pid();
        return 0;

    case HOK_IOCTL_READ:
        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        if (!g_st.mm)
            return -ENODEV;
        if (req.size == 0 || req.size > 0x4000)
            return -EINVAL;
        kbuf = kmalloc(req.size, GFP_KERNEL);
        if (!kbuf)
            return -ENOMEM;
        ret = vm_read(g_st.mm, (unsigned long)req.addr, kbuf, req.size);
        /* 读失败也把已读到/清零的数据回填, 保持上层逻辑简单 */
        if (copy_to_user((void __user *)(uintptr_t)req.buf, kbuf, req.size)) {
            kfree(kbuf);
            return -EFAULT;
        }
        kfree(kbuf);
        return ret;

    case HOK_IOCTL_PING:
        return 0x484F4B;  /* 'HOK' */
    default:
        return -EINVAL;
    }
}

static const struct file_operations hok_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = hok_ioctl,
    .compat_ioctl   = hok_ioctl,
};

static struct miscdevice hok_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "tty_hok",     /* 非显眼名, 见 post-fs-data 的节点重命名 */
    .fops  = &hok_fops,
    .mode  = 0600,
};

/* ------------------------------------------------------------------ */
/* 自隐藏                                                              */
/* ------------------------------------------------------------------ */
static void hide_self(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
    if (THIS_MODULE->mkobj.kobj.sd)
        kobject_del(&THIS_MODULE->mkobj.kobj);
#endif
    if (!list_empty(&THIS_MODULE->list))
        list_del(&THIS_MODULE->list);          /* /proc/modules 消失 */
    THIS_MODULE->state = MODULE_STATE_LIVE;
}

static int __init hok_init(void)
{
    int ret;

    ret = resolve_syms();
    if (ret) {
        pr_err(TAG ": resolve symbols failed\n");
        return ret;
    }
    ret = misc_register(&hok_misc);
    if (ret) {
        pr_err(TAG ": misc_register failed %d\n", ret);
        return ret;
    }
    hide_self();
    pr_info(TAG ": loaded, node=/dev/%s\n", hok_misc.name);
    return 0;
}

static void __exit hok_exit(void)
{
    detach_pid();
    misc_deregister(&hok_misc);
    pr_info(TAG ": unloaded\n");
}

module_init(hok_init);
module_exit(hok_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("local-lab");
MODULE_DESCRIPTION("readonly page-table memory reader");

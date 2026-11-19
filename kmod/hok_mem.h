/* hok_mem.h — 内核模块与用户态 daemon 的共享 ABI */
#ifndef _HOK_MEM_H
#define _HOK_MEM_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
typedef __u64 u64_;
typedef __u32 u32_;
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint64_t u64_;
typedef uint32_t u32_;
#endif

#define HOK_IOC_MAGIC   'h'

struct hok_req {
    u64_ addr;    /* 目标虚拟地址 */
    u64_ buf;     /* 用户态缓冲地址 */
    u64_ size;    /* 长度, <= 0x4000 */
};

#define HOK_IOCTL_SET_PID  _IOW(HOK_IOC_MAGIC, 1, u32_)
#define HOK_IOCTL_CLEAR    _IO(HOK_IOC_MAGIC, 2)
#define HOK_IOCTL_READ     _IOWR(HOK_IOC_MAGIC, 3, struct hok_req)
#define HOK_IOCTL_PING     _IO(HOK_IOC_MAGIC, 4)

/* 用户态封装路径: 由 post-fs-data.sh 把 misc 节点改名/搬到该路径 */
#define HOK_DEV_PATH "/data/local/tmp/.wq/ks"

#endif

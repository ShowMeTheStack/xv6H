#define NPROC       64                // maximum number of processes
#define NCPU        8                 // maximum number of CPUs
#define NOFILE      16                // open files per process
#define NFILE       100               // open files per system
#define NINODE      50                // maximum number of active i-nodes
#define NDEV        10                // maximum major device number
#define ROOTDEV     1                 // device number of file system root disk
#define MAXARG      32                // max exec arguments
#define MAXOPBLOCKS 10                // max # of blocks any FS op writes
#define LOGBLOCKS   (MAXOPBLOCKS * 3) // max data blocks in on-disk log
#define NBUF        (MAXOPBLOCKS * 3) // size of disk block cache
#define FSSIZE      4000              // size of file system in blocks
#define MAXPATH     128               // maximum file path name
#define USERSTACK   1                 // user stack pages
#define NVIRTIO     8                 // virtio-mmio slots on qemu virt
#ifndef HOSTMEM_MB
#define HOSTMEM_MB  128               // RAM the host kernel keeps for itself
#endif
#define HOSTMEM     ((uint64)HOSTMEM_MB * 1024 * 1024)
#define NVM         8                 // maximum number of guest VMs
#define NVCPU       NCPU              // maximum vcpus per VM

#define NPROC        64  // maximum number of processes
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGBLOCKS    (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
#define FSSIZE       30000  // was 2000, expanded for swap area
#define MAXPATH      128   // maximum file path name
#define USERSTACK    1     // user stack pages

// project 3에서 필요한 상수들 정의
// protection flags
#define PROT_READ 0x1
#define PROT_WRITE 0x2
// mapping flags
#define MAP_ANONYMOUS 0x1
#define MAP_POPULATE 0x2
// mmap base
#define MMAPBASE 0x40000000L
// maximum number of mappings
#define MAXMMAP 64
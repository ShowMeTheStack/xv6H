#ifndef XV6_VMSTATE_H
#define XV6_VMSTATE_H
//
// what a machine is, written down (kernel/vmstate.c, §9).
//
// A saved machine is a header and a list of named sections. Nothing in
// the file is a host address: a section holds the guest's own registers,
// the emulated devices' registers, and the pages of guest memory that
// are not zero, each named by its guest physical address. What that
// leaves out is everything that belongs to the host and is made again on
// restore -- the G-stage page tables, the chunk table, the host
// processes running the vcpus, the scheduler group, the switch port.
//
// The version is checked and refused rather than guessed at. A file
// written by a kernel built from different structures is not a machine
// this one can continue, and saying so is the only safe answer: the
// sections carry structures by value, which is what keeps this readable
// and what makes the version matter.
//
#define VMSTATE_MAGIC "XV6HVMST"
#define VMSTATE_VERSION 7   // 4: a handshake after CFG before any memory moves
// the destination's answer to the CFG section, over a connection
#define MIG_HS_ACCEPT    'A'
#define MIG_HS_REFUSE    'R'   // followed by a reason (MIG_REFUSE_*)
#define MIG_HS_NEGOTIATE 'N'   // followed by a REV_* mask the guest must cede first
#define MIG_HS_ABORT     'X'   // the source gives up (the guest would not cede)
#define MIG_REFUSE_VECTOR 1    // the guest holds a vector unit this host cannot give it
#define MIG_REFUSE_PAGING 2    // the guest's paging mode is deeper than this host's
#define MIG_REFUSE_MEMORY 3    // the guest holds more memory than this host has

struct vmstate_hdr {
  char magic[8];
  uint32 version;
  uint32 nsec;            // sections after this header
  uint64 len;             // bytes after this header
  uint64 saved_time;      // the host's `time' when it was written
  uint32 flags;
  uint32 pad;
};

// every section: eight characters of name, then its bytes. "END" ends
// them: MEM may appear several times (a migration's pre-copy rounds, and
// the pages the host wrote under a live snapshot), so the last MEM is not
// the end of anything and something has to say where the end is.
struct vmstate_sec {
  char name[8];           // "CFG", "VCPU", "TIME", "DEV", "MEM", "END"
  uint64 len;
};

// MEM is a sequence of these, each followed by PGSIZE bytes, ending with
// one whose gpa is VMSTATE_END. A page of guest memory that is not backed,
// or is all zeroes, is not here at all: a 128 MB guest that has touched
// 12 MB writes 12 MB. The sentinel rather than a count is what lets the
// same sections go down a socket, which cannot be written backwards.
#define VMSTATE_END 0ULL

struct vmstate_page {
  uint64 gpa;
};

// TIME: what the guest's clock has to keep meaning across the save.
struct vmstate_time {
  uint64 saved_time;      // the host counter when it was saved
  uint64 htimedelta;      // what it was running with
  uint64 pad[2];
};

// the snapshot's bitmaps: one bit per guest page. `done' is set when the
// file has that page (or never needed it); `dirty' is set when the page
// has been written since the round began and has to go again. Both this
// file's owner and the G-stage code touch them, so the helpers live here
// rather than in either. vmsnap_mark is the one a fault or a device write
// uses: no lock, because a bit is a bit and the count that matters is
// taken with the machine stopped.
static inline int
vmsnap_bit(const uint32 *m, int i)
{
  return (m[i / 32] >> (i % 32)) & 1;
}

static inline void
vmsnap_set(uint32 *m, int i)
{
  m[i / 32] |= 1u << (i % 32);
}

static inline void
vmsnap_mark(uint32 *m, int i)
{
  __sync_fetch_and_or(&m[i / 32], 1u << (i % 32));
}

// how many of them are set, which is how a pre-copy round decides whether
// another round is worth it.
static inline int
vmsnap_count(const uint32 *m, int npages)
{
  int n = 0;

  for (int i = 0; i < npages; i++)
    if (vmsnap_bit(m, i))
      n++;
  return n;
}

struct vm;

// vmm save NAME FILE, vmm restore FILE: the machine is paused and drained
// for the length of a save, and created by the restore.
int vm_save(struct vm *vm, char *path);
int vm_restore(char *path, const char *name, int *idp);
// the same, to and from something already open -- a socket, for migration
struct file;
int vm_save_file(struct vm *vm, struct file *f);
int vm_restore_file(struct file *f, const char *name, int *idp);
int vmstate_ack_put(struct file *f, int ok);
int vmstate_commit_put(struct file *f);
int vmstate_ack_get(struct file *f);
// vmm clone BASE NEW [-f DISK]: the same machine again, sharing its pages
int vm_clone(struct vm *src, const char *name, const char *disk, int *idp);

#endif

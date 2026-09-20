#ifndef XV6_ELF_H
#define XV6_ELF_H
#include "types.h"
// Format of an ELF executable file

#define ELF_MAGIC 0x464C457FU // "\x7FELF" in little endian

// File header
struct elfhdr {
  uint magic; // must equal ELF_MAGIC
  uchar elf[12];
  ushort type;
  ushort machine;
  uint version;
  uint64 entry;
  uint64 phoff;
  uint64 shoff;
  uint flags;
  ushort ehsize;
  ushort phentsize;
  ushort phnum;
  ushort shentsize;
  ushort shnum;
  ushort shstrndx;
};

// e_type
#define ET_REL  1
#define ET_EXEC 2
#define ET_DYN  3

// where a position-independent executable and its interpreter are
// placed. page zero stays unmapped, so a null dereference faults.
#define ELF_PIE_BASE    0x10000UL
#define ELF_INTERP_BASE 0x20000000UL

// Program section header
struct proghdr {
  uint32 type;
  uint32 flags;
  uint64 off;
  uint64 vaddr;
  uint64 paddr;
  uint64 filesz;
  uint64 memsz;
  uint64 align;
};

// Values for Proghdr type
#define ELF_PROG_NULL    0
#define ELF_PROG_LOAD    1
#define ELF_PROG_DYNAMIC 2
#define ELF_PROG_INTERP  3
#define ELF_PROG_NOTE    4
#define ELF_PROG_PHDR    6
#define ELF_PROG_TLS     7

// Flag bits for Proghdr flags
#define ELF_PROG_FLAG_EXEC  1
#define ELF_PROG_FLAG_WRITE 2
#define ELF_PROG_FLAG_READ  4
#endif

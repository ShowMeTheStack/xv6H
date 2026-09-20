//
// the dynamic loader's entry point.
//
// the kernel starts us here instead of the program, with the argument
// registers the program would have had. nothing has relocated us yet,
// so this code may not touch a global: it finds where it was loaded
// and where its own dynamic section is with pc-relative addressing,
// hands both to the C code, and that applies our own relocations
// before anything else happens.
//
.section .text
.global _dlstart
.type _dlstart, @function
_dlstart:
        mv s0, a0               // argc
        mv s1, a1               // argv
        mv s2, a2               // envp

        lla a3, __ehdr_start    // where we really are
        lla a4, _DYNAMIC        // and our own dynamic section
        mv a0, a3
        mv a1, a4
        call __dls1             // apply our relocations; returns nothing

        mv a0, s0
        mv a1, s1
        mv a2, s2
        lla a3, __ehdr_start
        call __dls2             // load everything and return the entry point

        mv t0, a0               // the program's entry
        mv a0, s0
        mv a1, s1
        mv a2, s2
        jr t0

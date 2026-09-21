#ifndef XV6_ERRNO_H
#define XV6_ERRNO_H
// error numbers: system calls return the negation of these, as Linux
// does, and the library turns them into errno (kernel/../libc later).
#define EPERM    1  // operation not permitted
#define ENOENT   2  // no such file or directory
#define ESRCH    3  // no such process
#define EINTR    4  // interrupted system call
#define EIO      5  // input/output error
#define ENXIO    6  // no such device or address
#define E2BIG    7  // argument list too long
#define ENOEXEC  8  // executable format error
#define EBADF    9  // bad file descriptor
#define ECHILD  10  // no child processes
#define EAGAIN  11  // resource temporarily unavailable
#define ENOMEM  12  // cannot allocate memory
#define EACCES  13  // permission denied
#define EFAULT  14  // bad address
#define EBUSY   16  // device or resource busy
#define EEXIST  17  // file exists
#define EXDEV   18  // invalid cross-device link
#define ENODEV  19  // no such device
#define ENOTDIR 20  // not a directory
#define EISDIR  21  // is a directory
#define EINVAL  22  // invalid argument
#define ENFILE  23  // too many open files in system
#define EMFILE  24  // too many open files
#define ENOTTY  25  // inappropriate ioctl for device
#define ETXTBSY 26  // text file busy
#define EFBIG   27  // file too large
#define ENOSPC  28  // no space left on device
#define ESPIPE  29  // illegal seek
#define EROFS   30  // read-only file system
#define EMLINK  31  // too many links
#define EPIPE   32  // broken pipe
#define EDEADLK 35  // a deadlock would happen
#define EDOM   33  // numerical argument out of domain
#define ERANGE  34  // numerical result out of range
#define ENAMETOOLONG 36
#define ENOLCK  37  // no locks available
#define ENOSYS  38  // function not implemented
#define ENOTEMPTY 39
#define ELOOP   40  // too many levels of symbolic links
#define ENOMSG  42
#define EIDRM   43
#define EILSEQ  84  // illegal byte sequence
#define EUSERS  87
#define ENOTSOCK 88
#define EDESTADDRREQ 89
#define EMSGSIZE 90
#define EPROTOTYPE 91
#define ENOPROTOOPT 92
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define EAFNOSUPPORT 97
#define EADDRINUSE 98
#define EADDRNOTAVAIL 99
#define ENETDOWN 100
#define ENETUNREACH 101
#define ECONNABORTED 103
#define ECONNRESET 104
#define ENOBUFS 105
#define EISCONN 106
#define ENOTCONN 107
#define ETIMEDOUT 110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EALREADY 114
#define EINPROGRESS 115
#define ESTALE   116
#define EDQUOT   122
#define ECANCELED 125
#define EOWNERDEAD 130
#define ENOTRECOVERABLE 131
#define EOVERFLOW 75
#define ENODATA  61
#define ENOSTR   60
#define ETIME    62
#define EBADMSG  74
#define ENOTSUP  EOPNOTSUPP
#define EUCLEAN  117  // a structure on the disk is not one this format allows
#define EWOULDBLOCK EAGAIN

#endif

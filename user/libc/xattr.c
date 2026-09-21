//
// extended attributes. the kernel has four calls; each takes a first
// argument saying whether the file is named by a descriptor, by a
// path, or by a path whose last element is a symbolic link that is not
// to be followed. these are the twelve names POSIX programs use.
//
#include <sys/xattr.h>
#include <unistd.h>
#include <errno.h>

int __setxattr(int fd, const char *path, const char *name, const void *value, int size, int flags);
int __getxattr(int fd, const char *path, const char *name, void *value, int size);
int __listxattr(int fd, const char *path, char *list, int size);
int __removexattr(int fd, const char *path, const char *name);

int
setxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
  return __setxattr(XATTR_AT_PATH, path, name, value, (int)size, flags);
}

int
lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
  return __setxattr(XATTR_AT_LPATH, path, name, value, (int)size, flags);
}

int
fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
  return __setxattr(fd, "", name, value, (int)size, flags);
}

ssize_t
getxattr(const char *path, const char *name, void *value, size_t size)
{
  return __getxattr(XATTR_AT_PATH, path, name, value, (int)size);
}

ssize_t
lgetxattr(const char *path, const char *name, void *value, size_t size)
{
  return __getxattr(XATTR_AT_LPATH, path, name, value, (int)size);
}

ssize_t
fgetxattr(int fd, const char *name, void *value, size_t size)
{
  return __getxattr(fd, "", name, value, (int)size);
}

ssize_t
listxattr(const char *path, char *list, size_t size)
{
  return __listxattr(XATTR_AT_PATH, path, list, (int)size);
}

ssize_t
llistxattr(const char *path, char *list, size_t size)
{
  return __listxattr(XATTR_AT_LPATH, path, list, (int)size);
}

ssize_t
flistxattr(int fd, char *list, size_t size)
{
  return __listxattr(fd, "", list, (int)size);
}

int
removexattr(const char *path, const char *name)
{
  return __removexattr(XATTR_AT_PATH, path, name);
}

int
lremovexattr(const char *path, const char *name)
{
  return __removexattr(XATTR_AT_LPATH, path, name);
}

int
fremovexattr(int fd, const char *name)
{
  return __removexattr(fd, "", name);
}

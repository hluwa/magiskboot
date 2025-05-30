#include <sched.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#ifndef SVB_WIN32
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/sendfile.h>
#include <sys/inotify.h>
#endif
#include <sys/ptrace.h>
#include <sys/mount.h>
#include <poll.h>
#else
#include <errno.h>
#include "windows.h"
#endif

#include <base.hpp>

using namespace std;

FILE *xfopen(const char *pathname, const char *mode) {
    FILE *fp = fopen(pathname, mode);
    if (fp == nullptr) {
        PLOGE("fopen: %s", pathname);
    }
    return fp;
}

FILE *xfdopen(int fd, const char *mode) {
    FILE *fp = fdopen(fd, mode);
    if (fp == nullptr) {
        PLOGE("fopen");
    }
    return fp;
}

int xopen(const char *pathname, int flags) {
    int fd = open(pathname, flags);
    if (fd < 0) {
        PLOGE("open: %s", pathname);
    }
    return fd;
}

int xopen(const char *pathname, int flags, mode_t mode) {
    int fd = open(pathname, flags, mode);
    if (fd < 0) {
        PLOGE("open: %s", pathname);
    }
    return fd;
}

#ifndef SVB_MINGW
int xopenat(int dirfd, const char *pathname, int flags) {
    int fd = openat(dirfd, pathname, flags);
    if (fd < 0) {
        PLOGE("openat: %s", pathname);
    }
    return fd;
}

int xopenat(int dirfd, const char *pathname, int flags, mode_t mode) {
    int fd = openat(dirfd, pathname, flags, mode);
    if (fd < 0) {
        PLOGE("openat: %s", pathname);
    }
    return fd;
}
#endif

// Write exact same size as count
ssize_t xwrite(int fd, const void *buf, size_t count) {
    size_t write_sz = 0;
    ssize_t ret;
    do {
        ret = write(fd, (::byte *) buf + write_sz, count - write_sz);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            PLOGE("write");
            return ret;
        }
        write_sz += ret;
    } while (write_sz != count && ret != 0);
    if (write_sz != count) {
        PLOGE("write (%zu != %zu)", count, write_sz);
    }
    return write_sz;
}

// Read error other than EOF
ssize_t xread(int fd, void *buf, size_t count) {
    int ret = read(fd, buf, count);
    if (ret < 0) {
        PLOGE("read");
    }
    return ret;
}

// Read exact same size as count
ssize_t xxread(int fd, void *buf, size_t count) {
    size_t read_sz = 0;
    ssize_t ret;
    do {
        ret = read(fd, (::byte *) buf + read_sz, count - read_sz);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            PLOGE("read");
            return ret;
        }
        read_sz += ret;
    } while (read_sz != count && ret != 0);
    if (read_sz != count) {
        PLOGE("read (%zu != %zu)", count, read_sz);
    }
    return read_sz;
}

off_t xlseek(int fd, off_t offset, int whence) {
    off_t ret = lseek(fd, offset, whence);
    if (ret < 0) {
        PLOGE("lseek");
    }
    return ret;
}

#if defined(__linux__)
int xpipe2(int pipefd[2], int flags) {
    int ret = pipe2(pipefd, flags);
    if (ret < 0) {
        PLOGE("pipe2");
    }
    return ret;
}
#elif defined(__APPLE__)
int xpipe2(int pipefd[2], int flags) {
    if (pipe(pipefd) < 0) {
        PLOGE("pipe");
        return -1;
    }
    if (flags & O_CLOEXEC) {
        if (fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) == -1 || 
            fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) == -1) {
            PLOGE("fcntl F_SETFD FD_CLOEXEC for pipe");
            close(pipefd[0]);
            close(pipefd[1]);
            return -1;
        }
    }
    if (flags & O_NONBLOCK) {
        if (fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK) == -1 ||
            fcntl(pipefd[1], F_SETFL, fcntl(pipefd[1], F_GETFL) | O_NONBLOCK) == -1) {
            PLOGE("fcntl F_SETFL O_NONBLOCK for pipe");
            close(pipefd[0]);
            close(pipefd[1]);
            return -1;
        }
    }
    // Other flags like O_DIRECT are not standard for pipe2 and not handled here.
    return 0;
}
#else
// Fallback for other non-Linux, non-Apple - might error or not support flags
int xpipe2(int pipefd[2], int flags) {
    if (flags == 0) { // Only support non-flagged pipe for generic fallback
        if (pipe(pipefd) < 0) {
            PLOGE("pipe");
            return -1;
        }
        return 0;
    }
    PLOGE("xpipe2 with flags not implemented for this platform");
    return -1; 
}
#endif // __linux__ / __APPLE__ / else for xpipe2

#ifndef SVB_WIN32

#if defined(__linux__)
int xsetns(int fd, int nstype) {
    int ret = setns(fd, nstype);
    if (ret < 0) {
        PLOGE("setns");
    }
    return ret;
}

int xunshare(int flags) {
    int ret = unshare(flags);
    if (ret < 0) {
        PLOGE("unshare");
    }
    return ret;
}
#endif

DIR *xopendir(const char *name) {
    DIR *d = opendir(name);
    if (d == nullptr) {
        PLOGE("opendir: %s", name);
    }
    return d;
}

#ifndef SVB_MINGW
DIR *xfdopendir(int fd) {
    DIR *d = fdopendir(fd);
    if (d == nullptr) {
        PLOGE("fdopendir");
    }
    return d;
}
#endif

struct dirent *xreaddir(DIR *dirp) {
    errno = 0;
    for (dirent *e;;) {
        e = readdir(dirp);
        if (e == nullptr) {
            if (errno)
                PLOGE("readdir");
            return nullptr;
        } else if (e->d_name == "."sv || e->d_name == ".."sv) {
            // Filter . and .. for users
            continue;
        }
        return e;
    }
}

#ifndef SVB_WIN32
pid_t xsetsid() {
    pid_t pid = setsid();
    if (pid < 0) {
        PLOGE("setsid");
    }
    return pid;
}

int xsocket(int domain, int type, int protocol) {
    int fd = socket(domain, type, protocol);
    if (fd < 0) {
        PLOGE("socket");
    }
    return fd;
}

int xbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int ret = bind(sockfd, addr, addrlen);
    if (ret < 0) {
        PLOGE("bind");
    }
    return ret;
}

int xlisten(int sockfd, int backlog) {
    int ret = listen(sockfd, backlog);
    if (ret < 0) {
        PLOGE("listen");
    }
    return ret;
}

#if defined(__linux__)
int xaccept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    int fd = accept4(sockfd, addr, addrlen, flags);
    if (fd < 0) {
        PLOGE("accept4");
    }
    return fd;
}
#endif

void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (p == nullptr) {
        PLOGE("malloc");
    }
    return p;
}

void *xcalloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb, size);
    if (p == nullptr) {
        PLOGE("calloc");
    }
    return p;
}

void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (p == nullptr) {
        PLOGE("realloc");
    }
    return p;
}
#endif // Closes #ifndef SVB_WIN32 from ~L196 (xsetsid, xsocket...xrealloc block)

#ifndef SVB_WIN32 // This is for xsendmsg, xrecvmsg, xpthread_create from ~L246
ssize_t xsendmsg(int sockfd, const struct msghdr *msg, int flags) {
    int sent = sendmsg(sockfd, msg, flags);
    if (sent < 0) {
        PLOGE("sendmsg");
    }
    return sent;
}

ssize_t xrecvmsg(int sockfd, struct msghdr *msg, int flags) {
    int rec = recvmsg(sockfd, msg, flags);
    if (rec < 0) {
        PLOGE("recvmsg");
    }
    return rec;
}

int xpthread_create(pthread_t *thread, const pthread_attr_t *attr,
                    void *(*start_routine) (void *), void *arg) {
    errno = pthread_create(thread, attr, start_routine, arg);
    if (errno) {
        PLOGE("pthread_create");
    }
    return errno;
}
#endif
int xaccess(const char *path, int mode) {
    int ret = access(path, mode);
    if (ret < 0) {
        PLOGE("access %s", path);
    }
    return ret;
}

int xstat(const char *pathname, struct stat *buf) {
    int ret = stat(pathname, buf);
    if (ret < 0) {
        PLOGE("stat %s", pathname);
    }
    return ret;
}

int xlstat(const char *pathname, struct stat *buf) {
    int ret = lstat(pathname, buf);
    if (ret < 0) {
        PLOGE("lstat %s", pathname);
    }
    return ret;
}

int xfstat(int fd, struct stat *buf) {
    int ret = fstat(fd, buf);
    if (ret < 0) {
        PLOGE("fstat %d", fd);
    }
    return ret;
}

#ifndef SVB_MINGW
int xfstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
    int ret = fstatat(dirfd, pathname, buf, flags);
    if (ret < 0) {
        PLOGE("fstatat %s", pathname);
    }
    return ret;
}
#endif

int xdup(int fd) {
    int ret = dup(fd);
    if (ret < 0) {
        PLOGE("dup");
    }
    return ret;
}

int xdup2(int oldfd, int newfd) {
    int ret = dup2(oldfd, newfd);
    if (ret < 0) {
        PLOGE("dup2");
    }
    return ret;
}

#if defined(__linux__)
int xdup3(int oldfd, int newfd, int flags) {
    int ret = dup3(oldfd, newfd, flags);
    if (ret < 0) {
        PLOGE("dup3");
    }
    return ret;
}
#endif

ssize_t xreadlink(const char *pathname, char *buf, size_t bufsiz) {
    ssize_t ret = readlink(pathname, buf, bufsiz);
    if (ret < 0) {
        PLOGE("readlink %s", pathname);
    } else {
        buf[ret] = '\0';
    }
    return ret;
}

#ifndef SVB_MINGW
ssize_t xreadlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz) {
    // readlinkat() may fail on x86 platform, returning random value
    // instead of number of bytes placed in buf (length of link)
#if defined(__i386__) || defined(__x86_64__)
    memset(buf, 0, bufsiz);
    ssize_t ret = readlinkat(dirfd, pathname, buf, bufsiz);
    if (ret < 0) {
        PLOGE("readlinkat %s", pathname);
    }
    return ret;
#else
    ssize_t ret = readlinkat(dirfd, pathname, buf, bufsiz);
    if (ret < 0) {
        PLOGE("readlinkat %s", pathname);
    } else {
        buf[ret] = '\0';
    }
    return ret;
#endif
}

int xfaccessat(int dirfd, const char *pathname) {
    int ret = faccessat(dirfd, pathname, F_OK, 0);
    if (ret < 0) {
        PLOGE("faccessat %s", pathname);
    }
#if defined(__i386__) || defined(__x86_64__)
    if (ret > 0 && errno == 0) {
        LOGD("faccessat success but ret is %d\n", ret);
        ret = 0;
    }
#endif
    return ret;
}
#endif

#if defined(SVB_WIN32) && !defined(SVB_MINGW)
#define symlink xxsymlink
#endif
int xsymlink(const char *target, const char *linkpath) {
    int ret = symlink(target, linkpath);
    if (ret < 0) {
        PLOGE("symlink %s->%s", target, linkpath);
    }
    return ret;
}

#if defined SVB_WIN32 && !defined SVB_MINGW
#define SYMLINK_ID	"!<symlink>\xff\xfe"
#define SYMLINK_IDLEN	strlen(SYMLINK_ID)
#define SYMLINK_MAXSIZE	1024
int xxsymlink(const char *target, const char *file)
{
    int sz = strlen(target) + 1;
    char buf[sz * sizeof(WCHAR)];

    FILE *lnk = fopen(file, "wb");
    if (!lnk || fprintf(lnk, SYMLINK_ID) < 0)
        return -1;

    if (MultiByteToWideChar(CP_UTF8, 0, target, sz, (LPWSTR)buf, sz) != sz) {
        errno = EINVAL;
        sz = -1;
        goto err;
    }
    sz = fwrite(buf, 1, sizeof(buf), lnk);
    if (sz != sizeof(buf)) {
	sz = -1;
        goto err;
    }
    if (!SetFileAttributes(file, FILE_ATTRIBUTE_SYSTEM)) {
        sz = -1;
        goto err;
    }
    sz = 0;
err:
    fclose(lnk);
    return sz;
}
#endif

#ifndef SVB_MINGW
int xsymlinkat(const char *target, int newdirfd, const char *linkpath) {
    int ret = symlinkat(target, newdirfd, linkpath);
    if (ret < 0) {
        PLOGE("symlinkat %s->%s", target, linkpath);
    }
    return ret;
}

int xlinkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) {
    int ret = linkat(olddirfd, oldpath, newdirfd, newpath, flags);
    if (ret < 0) {
        PLOGE("linkat %s->%s", oldpath, newpath);
    }
    return ret;
}
#endif

#ifndef SVB_WIN32

#if defined(__linux__)
int xmount(const char *source, const char *target,
    const char *filesystemtype, unsigned long mountflags,
    const void *data) {
    int ret = mount(source, target, filesystemtype, mountflags, data);
    if (ret < 0) {
        PLOGE("mount %s->%s", source, target);
    }
    return ret;
}
#endif // __linux__ for xmount

int xumount(const char *target) {
#if defined(__APPLE__)
    int ret = unmount(target, 0); // macOS is unmount(target, flags)
#elif defined(__linux__)
    int ret = umount(target);     // Linux umount
#else
    // Fallback for other systems, may not work
    int ret = umount(target); // Assuming POSIX umount if not Apple/Linux
#endif
    if (ret < 0) {
        PLOGE("umount/unmount %s", target);
    }
    return ret;
}

#if defined(__linux__)
int xumount2(const char *target, int flags) {
    int ret = umount2(target, flags);
    if (ret < 0) {
        PLOGE("umount2 %s", target);
    }
    return ret;
}
#endif // __linux__ for xumount2

#endif // SVB_WIN32 for the block containing xmount, xumount, xumount2

int xrename(const char *oldpath, const char *newpath) {
    int ret = rename(oldpath, newpath);
    if (ret < 0) {
        PLOGE("rename %s->%s", oldpath, newpath);
    }
    return ret;
}

int xmkdir(const char *pathname, mode_t mode) {
#ifdef SVB_MINGW
#define mkdir(y, x) mkdir(y)
#endif
    int ret = mkdir(pathname, mode);
    if (ret < 0 && errno != EEXIST) {
        PLOGE("mkdir %s %u", pathname, mode);
    }
    return ret;
}

int xmkdirs(const char *pathname, mode_t mode) {
    int ret = mkdirs(pathname, mode);
    if (ret < 0) {
        PLOGE("mkdirs %s", pathname);
    }
    return ret;
}

#ifndef SVB_MINGW
int xmkdirat(int dirfd, const char *pathname, mode_t mode) {
    int ret = mkdirat(dirfd, pathname, mode);
    if (ret < 0 && errno != EEXIST) {
        PLOGE("mkdirat %s %u", pathname, mode);
    }
    return ret;
}
#endif

void *xmmap(void *addr, size_t length, int prot, int flags,
    int fd, off_t offset) {
    void *ret = mmap(addr, length, prot, flags, fd, offset);
    if (ret == MAP_FAILED) {
        PLOGE("mmap");
        return nullptr;
    }
    return ret;
}

ssize_t xsendfile(int out_fd, int in_fd, off_t *offset, size_t count) {
#if defined(__APPLE__)
    off_t len = count;
    // Note: macOS sendfile has in_fd and out_fd swapped compared to Linux
    // It also expects len to be a pointer, and offset is the starting offset in the input file.
    // The offset parameter in Linux sendfile is a pointer to an offset that is updated.
    // For this specific usage (offset is nullptr, meaning read from current offset),
    // we pass 0 as the offset to macOS sendfile and it should behave similarly for seeking files.
    // If offset was used on Linux, a more complex adaptation would be needed.
    int ret = sendfile(in_fd, out_fd, (offset ? *offset : 0), &len, nullptr, 0);
    if (ret < 0) {
        PLOGE("sendfile");
        return -1;
    }
    // macOS sendfile returns 0 on success, and len is updated with bytes sent.
    // Linux sendfile returns bytes sent, or -1 on error.
    return len; 
#else
    ssize_t ret = sendfile(out_fd, in_fd, offset, count);
    if (ret < 0) {
        PLOGE("sendfile");
    }
    return ret;
#endif
}

#ifndef SVB_WIN32
pid_t xfork() {
    int ret = fork();
    if (ret < 0) {
        PLOGE("fork");
    }
    return ret;
}

int xpoll(struct pollfd *fds, nfds_t nfds, int timeout) {
    int ret = poll(fds, nfds, timeout);
    if (ret < 0) {
        PLOGE("poll");
    }
    return ret;
}

#if defined(__linux__)
int xinotify_init1(int flags) {
    int ret = inotify_init1(flags);
    if (ret < 0) {
        PLOGE("inotify_init1");
    }
    return ret;
}
#endif
#endif

#ifndef SVB_MINGW
char *xrealpath(const char *path, char *resolved_path) {
    char buf[PATH_MAX];
    char *ret = realpath(path, buf);
    if (ret == nullptr) {
        PLOGE("xrealpath");
    } else {
        strcpy(resolved_path, buf);
    }
    return ret;
}
#endif

#ifndef SVB_WIN32
int xmknod(const char *pathname, mode_t mode, dev_t dev) {
    int ret = mknod(pathname, mode, dev);
    if (ret < 0) {
        PLOGE("mknod");
    }
    return ret;
}

#if defined(__linux__)
long xptrace(int request, pid_t pid, void *addr, void *data) {
    long ret = ptrace(request, pid, addr, data);
    if (ret < 0) // On Linux, -1 always means error for ptrace
        PLOGE("ptrace %d", pid);
    return ret;
}
#elif defined(__APPLE__)
// macOS ptrace has different signature for 3rd and 4th arg
// and different error reporting (-1 can be valid return for some requests)
long xptrace(int request, pid_t pid, void *addr, int data) { // Note: 4th arg is int for macOS ptrace
    // The actual type of addr for ptrace on macOS is caddr_t (char*).
    // The data arg is int.
    // Need to be careful if the original `void *data` was meant for larger types.
    // Assuming for now it was for an int-sized value or address that fits in int.
    long ret = ptrace(request, pid, (caddr_t)addr, data);
    if (ret == -1 && errno != 0) { // Check errno on macOS for actual error
        PLOGE("ptrace %d", pid);
    }
    return ret;
}
#else
// Fallback or error for other non-Linux, non-Apple platforms if ptrace is used
long xptrace(int request, pid_t pid, void *addr, void *data) {
    PLOGE("ptrace not implemented for this platform");
    return -1;
}
#endif // Platform specific ptrace

#endif // SVB_WIN32 for xmknod and xptrace block

#endif // Potentially missing #endif for an outer #ifndef SVB_WIN32

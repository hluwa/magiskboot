#ifndef SVB_WIN32
#if defined(__linux__)
#include <sys/sendfile.h>
#include <linux/fs.h>
#elif defined(__APPLE__)
// macOS uses a different sendfile, or it's part of unistd.h / sys/socket.h
// For now, we'll assume it's available via unistd.h or similar, 
// and will adjust xsendfile if needed later.
#include <sys/socket.h> // sendfile is often here on BSD-like systems
#include <sys/disk.h>   // For DOKIOCGETBLOCKCOUNT etc. if needed, or sys/ioctl.h for basic ioctls
#include <sys/ioctl.h>  // For basic ioctls if needed
#endif
#endif

#include <fcntl.h>
#include <unistd.h>
#include <libgen.h>

#include <base.hpp>
#ifndef SVB_WIN32
#if defined(__linux__)
#include <selinux.hpp>
#endif
#endif

using namespace std;

#ifdef SVB_MINGW
#include "libnt.h"
#define mkdir(y, x) mkdir(y)
#endif

#ifndef SVB_WIN32
ssize_t fd_path(int fd, char *path, size_t size) {
    snprintf(path, size, "/proc/self/fd/%d", fd);
    return xreadlink(path, path, size);
}

int fd_pathat(int dirfd, const char *name, char *path, size_t size) {
    if (fd_path(dirfd, path, size) < 0)
        return -1;
    auto len = strlen(path);
    path[len] = '/';
    strlcpy(path + len + 1, name, size - len - 1);
    return 0;
}
#endif

int mkdirs(const char *path, mode_t mode) {
    char buf[4096];
    strlcpy(buf, path, sizeof(buf));
    errno = 0;
    for (char *p = &buf[1]; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, mode) == -1) {
                if (errno != EEXIST)
                    return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(buf, mode) == -1) {
        if (errno != EEXIST)
            return -1;
    }
    return 0;
}

#ifndef SVB_MINGW
template <typename Func>
static void post_order_walk(int dirfd, const Func &fn) {
    auto dir = xopen_dir(dirfd);
    if (!dir) return;

    for (dirent *entry; (entry = xreaddir(dir.get()));) {
        if (entry->d_type == DT_DIR)
            post_order_walk(xopenat(dirfd, entry->d_name, O_RDONLY | O_CLOEXEC), fn);
        fn(dirfd, entry);
    }
}

enum walk_result {
    CONTINUE, SKIP, ABORT
};

template <typename Func>
static walk_result pre_order_walk(int dirfd, const Func &fn) {
    auto dir = xopen_dir(dirfd);
    if (!dir) {
        close(dirfd);
        return SKIP;
    }

    for (dirent *entry; (entry = xreaddir(dir.get()));) {
        switch (fn(dirfd, entry)) {
        case CONTINUE:
            break;
        case SKIP:
            continue;
        case ABORT:
            return ABORT;
        }
        if (entry->d_type == DT_DIR) {
            int fd = xopenat(dirfd, entry->d_name, O_RDONLY | O_CLOEXEC);
            if (pre_order_walk(fd, fn) == ABORT)
                return ABORT;
        }
    }
    return CONTINUE;
}

static void remove_at(int dirfd, struct dirent *entry) {
    unlinkat(dirfd, entry->d_name, entry->d_type == DT_DIR ? AT_REMOVEDIR : 0);
}

void frm_rf(int dirfd) {
    post_order_walk(dirfd, remove_at);
}

void rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0)
        return;
    if (S_ISDIR(st.st_mode))
        frm_rf(xopen(path, O_RDONLY | O_CLOEXEC));
    remove(path);
}
#endif

#ifndef SVB_WIN32
void mv_path(const char *src, const char *dest) {
    file_attr attr;
    getattr(src, &attr);
    if (S_ISDIR(attr.st.st_mode)) {
        if (access(dest, F_OK) != 0) {
            xmkdirs(dest, 0);
            setattr(dest, &attr);
        }
        mv_dir(xopen(src, O_RDONLY | O_CLOEXEC), xopen(dest, O_RDONLY | O_CLOEXEC));
    } else{
        xrename(src, dest);
    }
    rmdir(src);
}

void mv_dir(int src, int dest) {
    auto dir = xopen_dir(src);
    run_finally f([=]{ close(dest); });
    for (dirent *entry; (entry = xreaddir(dir.get()));) {
        switch (entry->d_type) {
        case DT_DIR:
            if (xfaccessat(dest, entry->d_name) == 0) {
                // Destination folder exists, needs recursive move
                int newsrc = xopenat(src, entry->d_name, O_RDONLY | O_CLOEXEC);
                int newdest = xopenat(dest, entry->d_name, O_RDONLY | O_CLOEXEC);
                mv_dir(newsrc, newdest);
                unlinkat(src, entry->d_name, AT_REMOVEDIR);
                break;
            }
            // Else fall through
        case DT_LNK:
        case DT_REG:
            renameat(src, entry->d_name, dest, entry->d_name);
            break;
        }
    }
}

void cp_afc(const char *src, const char *dest) {
    file_attr a;
    getattr(src, &a);

    if (S_ISDIR(a.st.st_mode)) {
        xmkdirs(dest, 0);
        clone_dir(xopen(src, O_RDONLY | O_CLOEXEC), xopen(dest, O_RDONLY | O_CLOEXEC));
    } else{
        unlink(dest);
        if (S_ISREG(a.st.st_mode)) {
            int sfd = xopen(src, O_RDONLY | O_CLOEXEC);
            int dfd = xopen(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0);
            xsendfile(dfd, sfd, nullptr, a.st.st_size);
            close(sfd);
            close(dfd);
        } else if (S_ISLNK(a.st.st_mode)) {
            char buf[4096];
            xreadlink(src, buf, sizeof(buf));
            xsymlink(buf, dest);
        }
    }
    setattr(dest, &a);
}

void clone_dir(int src, int dest) {
    auto dir = xopen_dir(src);
    run_finally f([&]{ close(dest); });
    for (dirent *entry; (entry = xreaddir(dir.get()));) {
        file_attr a;
        getattrat(src, entry->d_name, &a);
        switch (entry->d_type) {
            case DT_DIR: {
                xmkdirat(dest, entry->d_name, 0);
                setattrat(dest, entry->d_name, &a);
                int sfd = xopenat(src, entry->d_name, O_RDONLY | O_CLOEXEC);
                int dst = xopenat(dest, entry->d_name, O_RDONLY | O_CLOEXEC);
                clone_dir(sfd, dst);
                break;
            }
            case DT_REG: {
                int sfd = xopenat(src, entry->d_name, O_RDONLY | O_CLOEXEC);
                int dfd = xopenat(dest, entry->d_name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0);
                xsendfile(dfd, sfd, nullptr, a.st.st_size);
                fsetattr(dfd, &a);
                close(dfd);
                close(sfd);
                break;
            }
            case DT_LNK: {
                char buf[4096];
                xreadlinkat(src, entry->d_name, buf, sizeof(buf));
                xsymlinkat(buf, dest, entry->d_name);
                setattrat(dest, entry->d_name, &a);
                break;
            }
        }
    }
}

void link_path(const char *src, const char *dest) {
    link_dir(xopen(src, O_RDONLY | O_CLOEXEC), xopen(dest, O_RDONLY | O_CLOEXEC));
}

void link_dir(int src, int dest) {
    auto dir = xopen_dir(src);
    run_finally f([&]{ close(dest); });
    for (dirent *entry; (entry = xreaddir(dir.get()));) {
        if (entry->d_type == DT_DIR) {
            file_attr a;
            getattrat(src, entry->d_name, &a);
            xmkdirat(dest, entry->d_name, 0);
            setattrat(dest, entry->d_name, &a);
            int sfd = xopenat(src, entry->d_name, O_RDONLY | O_CLOEXEC);
            int dfd = xopenat(dest, entry->d_name, O_RDONLY | O_CLOEXEC);
            link_dir(sfd, dfd);
        } else {
            xlinkat(src, entry->d_name, dest, entry->d_name, 0);
        }
    }
}

int getattr(const char *path, file_attr *a) {
    if (xlstat(path, &a->st) == -1)
        return -1;
#if defined(__linux__)
    char *con;
    if (lgetfilecon(path, &con) == -1)
        return -1;
    strcpy(a->con, con);
    freecon(con);
#else
    a->con[0] = '\0'; // No SELinux context on non-Linux
#endif
    return 0;
}

int getattrat(int dirfd, const char *name, file_attr *a) {
    char path[4096];
    fd_pathat(dirfd, name, path, sizeof(path));
    return getattr(path, a);
}

int fgetattr(int fd, file_attr *a) {
    if (xfstat(fd, &a->st) < 0)
        return -1;
#if defined(__linux__)
    char *con;
    if (fgetfilecon(fd, &con) < 0)
        return -1;
    strcpy(a->con, con);
    freecon(con);
#else
    a->con[0] = '\0'; // No SELinux context on non-Linux
#endif
    return 0;
}

int setattr(const char *path, file_attr *a) {
    if (chmod(path, a->st.st_mode & 0777) < 0)
        return -1;
    if (chown(path, a->st.st_uid, a->st.st_gid) < 0)
        return -1;
#if defined(__linux__)
    if (a->con[0] && lsetfilecon(path, a->con) < 0)
        return -1;
#endif
    return 0;
}

int setattrat(int dirfd, const char *name, file_attr *a) {
    char path[4096];
    fd_pathat(dirfd, name, path, sizeof(path));
    return setattr(path, a);
}

int fsetattr(int fd, file_attr *a) {
    if (fchmod(fd, a->st.st_mode & 0777) < 0)
        return -1;
    if (fchown(fd, a->st.st_uid, a->st.st_gid) < 0)
        return -1;
#if defined(__linux__)
    if (a->con[0] && fsetfilecon(fd, a->con) < 0)
        return -1;
#endif
    return 0;
}

void clone_attr(const char *src, const char *dest) {
    file_attr a;
    getattr(src, &a);
    setattr(dest, &a);
}

void fclone_attr(int src, int dest) {
    file_attr a;
    fgetattr(src, &a);
    fsetattr(dest, &a);
}
#endif

void full_read(int fd, string &str) {
    char buf[4096];
    for (ssize_t len; (len = xread(fd, buf, sizeof(buf))) > 0;)
        str.insert(str.end(), buf, buf + len);
}

void full_read(const char *filename, string &str) {
    if (int fd = xopen(filename, O_RDONLY | O_CLOEXEC); fd >= 0) {
        full_read(fd, str);
        close(fd);
    }
}

string full_read(int fd) {
    string str;
    full_read(fd, str);
    return str;
}

string full_read(const char *filename) {
    string str;
    full_read(filename, str);
    return str;
}

void write_zero(int fd, size_t size) {
    char buf[4096] = {0};
    size_t len;
    while (size > 0) {
        len = sizeof(buf) > size ? size : sizeof(buf);
        write(fd, buf, len);
        size -= len;
    }
}

void file_readline(bool trim, FILE *fp, const function<bool(string_view)> &fn) {
    size_t len = 1024;
    char *buf = (char *) malloc(len);
    char *start;
    ssize_t read;
    while ((read = getline(&buf, &len, fp)) >= 0) {
        start = buf;
        if (trim) {
            while (read && "\n\r "sv.find(buf[read - 1]) != string::npos)
                --read;
            buf[read] = '\0';
            while (*start == ' ')
                ++start;
        }
        if (!fn(start))
            break;
    }
    free(buf);
}

void file_readline(bool trim, const char *file, const function<bool(string_view)> &fn) {
    if (auto fp = open_file(file, "re"))
        file_readline(trim, fp.get(), fn);
}

void file_readline(const char *file, const function<bool(string_view)> &fn) {
    file_readline(false, file, fn);
}

void parse_prop_file(FILE *fp, const function<bool(string_view, string_view)> &fn) {
    file_readline(true, fp, [&](string_view line_view) -> bool {
        char *line = (char *) line_view.data();
        if (line[0] == '#')
            return true;
        char *eql = strchr(line, '=');
        if (eql == nullptr || eql == line)
            return true;
        *eql = '\0';
        return fn(line, eql + 1);
    });
}

void parse_prop_file(const char *file, const function<bool(string_view, string_view)> &fn) {
    if (auto fp = open_file(file, "re"))
        parse_prop_file(fp.get(), fn);
}

#ifndef SVB_WIN32
#if defined(__linux__)
// Original source: https://android.googlesource.com/platform/bionic/+/master/libc/bionic/mntent.cpp
// License: AOSP, full copyright notice please check original source
static struct mntent *compat_getmntent_r(FILE *fp, struct mntent *e, char *buf, int buf_len) {
    memset(e, 0, sizeof(*e));
    while (fgets(buf, buf_len, fp) != nullptr) {
        // Entries look like "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0".
        // That is: mnt_fsname mnt_dir mnt_type mnt_opts 0 0.
        int fsname0, fsname1, dir0, dir1, type0, type1, opts0, opts1;
        if (sscanf(buf, " %n%*s%n %n%*s%n %n%*s%n %n%*s%n %d %d",
                   &fsname0, &fsname1, &dir0, &dir1, &type0, &type1, &opts0, &opts1,
                   &e->mnt_freq, &e->mnt_passno) == 2) {
            e->mnt_fsname = &buf[fsname0];
            buf[fsname1] = '\0';
            e->mnt_dir = &buf[dir0];
            buf[dir1] = '\0';
            e->mnt_type = &buf[type0];
            buf[type1] = '\0';
            e->mnt_opts = &buf[opts0];
            buf[opts1] = '\0';
            return e;
        }
    }
    return nullptr;
}

void parse_mnt(const char *file, const function<bool(mntent*)> &fn) {
    auto fp = sFILE(setmntent(file, "re"), endmntent);
    if (fp) {
        mntent mentry{};
        char buf[4096];
        // getmntent_r from system's libc.so is broken on old platform
        // use the compat one instead
        while (compat_getmntent_r(fp.get(), &mentry, buf, sizeof(buf))) {
            if (!fn(&mentry))
                break;
        }
    }
}
#endif

void backup_folder(const char *dir, vector<raw_file> &files) {
    char path[PATH_MAX];
    xrealpath(dir, path);
    int len = strlen(path);
    pre_order_walk(xopen(dir, O_RDONLY), [&](int dfd, dirent *entry) -> walk_result {
        int fd = xopenat(dfd, entry->d_name, O_RDONLY);
        if (fd < 0)
            return SKIP;
        run_finally f([&]{ close(fd); });
        if (fd_path(fd, path, sizeof(path)) < 0)
            return SKIP;
        raw_file file;
        file.path = path + len + 1;
        if (fgetattr(fd, &file.attr) < 0)
            return SKIP;
        if (entry->d_type == DT_REG) {
            file.content = full_read(fd);
        } else if (entry->d_type == DT_LNK) {
            xreadlinkat(dfd, entry->d_name, path, sizeof(path));
            file.content = path;
        }
        files.emplace_back(std::move(file));
        return CONTINUE;
    });
}

void restore_folder(const char *dir, vector<raw_file> &files) {
    string base(dir);
    // Pre-order means folders will always be first
    for (raw_file &file : files) {
        string path = base + "/" + file.path;
        if (S_ISDIR(file.attr.st.st_mode)) {
            mkdirs(path.c_str(), 0);
        } else if (S_ISREG(file.attr.st.st_mode)) {
            if (auto fp = xopen_file(path.data(), "we"))
                fwrite(file.content.data(), 1, file.content.size(), fp.get());
        } else if (S_ISLNK(file.attr.st.st_mode)) {
            symlink(file.content.data(), path.data());
        }
        setattr(path.data(), &file.attr);
    }
}
#endif

sDIR make_dir(DIR *dp) {
#ifdef SVB_MINGW
    return sDIR(dp);
#else
    return sDIR(dp, [](DIR *dp){ return dp ? closedir(dp) : 1; });
#endif
}

sFILE make_file(FILE *fp) {
    return sFILE(fp, [](FILE *fp){ return fp ? fclose(fp) : 1; });
}

int byte_data::patch(bool log, str_pairs list) {
    if (buf == nullptr)
        return 0;
    int count = 0;
    for (uint8_t *p = buf, *eof = buf + sz; p < eof; ++p) {
        for (auto [from, to] : list) {
            if (memcmp(p, from.data(), from.length() + 1) == 0) {
                if (log) LOGD("Replace [%s] -> [%s]\n", from.data(), to.data());
                memset(p, 0, from.length());
                memcpy(p, to.data(), to.length());
                ++count;
                p += from.length();
            }
        }
    }
    return count;
}

bool byte_data::contains(string_view pattern, bool log) const {
    if (buf == nullptr)
        return false;
    for (uint8_t *p = buf, *eof = buf + sz; p < eof; ++p) {
        if (memcmp(p, pattern.data(), pattern.length() + 1) == 0) {
            if (log) LOGD("Found pattern [%s]\n", pattern.data());
            return true;
        }
    }
    return false;
}

void byte_data::swap(byte_data &o) {
    std::swap(buf, o.buf);
    std::swap(sz, o.sz);
}

mmap_data::mmap_data(const char *name, bool rw) {
    int fd = xopen(name, (rw ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st))
        return;
#ifndef SVB_WIN32
#if defined(__linux__)
    if (S_ISBLK(st.st_mode)) {
        uint64_t b_size = 0;
        if (ioctl(fd, BLKGETSIZE64, &b_size) == 0) {
            sz = b_size;
        } else {
            // Fallback or error
            sz = st.st_size;
        }
    } else {
        sz = st.st_size;
    }
#elif defined(__APPLE__)
    if (S_ISBLK(st.st_mode)) {
        // On macOS, st.st_size should typically work for block devices.
        // Alternatively, one could use DOKIOCGETBLOCKCOUNT and DOKIOCGETBLOCKSIZE from <sys/disk.h>
        // For simplicity, using st_size first.
        sz = st.st_size;
        // Example using ioctl if st_size is not reliable for some block devices:
        // uint64_t block_count = 0;
        // uint32_t block_size = 0;
        // if (ioctl(fd, DKIOCGETBLOCKCOUNT, &block_count) == 0 && 
        //     ioctl(fd, DKIOCGETBLOCKSIZE, &block_size) == 0) {
        //     sz = block_count * block_size;
        // } else {
        //     sz = st.st_size; // Fallback
        // }
    } else {
        sz = st.st_size;
    }
#else
    // Other non-Windows, non-Linux, non-Apple
    sz = st.st_size;
#endif
#else // SVB_WIN32
    sz = st.st_size;
#endif
    void *b = sz > 0
            ? xmmap(nullptr, sz, rw ? PROT_READ | PROT_WRITE : PROT_READ, rw ? MAP_SHARED : MAP_PRIVATE, fd, 0)
            : nullptr;
    close(fd);
    buf = static_cast<uint8_t *>(b);
}

#ifndef SVB_WIN32
string find_apk_path(const char *pkg) {
    char buf[PATH_MAX];
    pre_order_walk(xopen("/data/app", O_RDONLY), [&](int dfd, dirent *entry) -> walk_result {
        if (entry->d_type != DT_DIR)
            return SKIP;
        size_t len = strlen(pkg);
        if (strncmp(entry->d_name, pkg, len) == 0 && entry->d_name[len] == '-') {
            fd_pathat(dfd, entry->d_name, buf, sizeof(buf));
            return ABORT;
        } else if (strncmp(entry->d_name, "~~", 2) == 0) {
            return CONTINUE;
        } else return SKIP;
    });
    string path(buf);
    return path.append("/base.apk");
}
#endif

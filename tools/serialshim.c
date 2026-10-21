/*
 * LD_PRELOAD shim for wine's serial layer on Android.
 *
 * Android's SELinux policy only whitelists a few tty ioctls for apps. Wine's GetCommState/SetCommState use
 * TCGETS2/TCSETS2 (termios2) and the modem line ioctls, which come back as EACCES on our virtual (pty) ports,
 * so serial programs think the port is unusable. When one of these calls fails, emulate it with the classic
 * TCGETS/TCSETS calls (which are allowed) or report sane values. Calls that succeed are never touched.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#define K_TCGETS     0x5401
#define K_TCSETS     0x5402
#define K_TCSETSW    0x5403
#define K_TCSETSF    0x5404
#define K_TCGETS2    0x802c542aUL
#define K_TCSETS2    0x402c542bUL
#define K_TCSETSW2   0x402c542cUL
#define K_TCSETSF2   0x402c542dUL
#define K_TIOCMGET   0x5415
#define K_TIOCMBIS   0x5416
#define K_TIOCMBIC   0x5417
#define K_TIOCMSET   0x5418
#define K_TIOCSSERIAL 0x541f

#define K_CBAUD   0x100f
#define K_CIBAUD  0x100f0000
#define K_BOTHER  0x1000

struct k_termios  { unsigned int c_iflag, c_oflag, c_cflag, c_lflag; unsigned char c_line; unsigned char c_cc[19]; };
struct k_termios2 { unsigned int c_iflag, c_oflag, c_cflag, c_lflag; unsigned char c_line; unsigned char c_cc[19]; unsigned int c_ispeed, c_ospeed; };

static const struct { unsigned int rate, code; } baud_table[] = {
    {0, 0}, {50, 1}, {75, 2}, {110, 3}, {134, 4}, {150, 5}, {200, 6}, {300, 7}, {600, 8}, {1200, 9}, {1800, 10},
    {2400, 11}, {4800, 12}, {9600, 13}, {19200, 14}, {38400, 15}, {57600, 0x1001}, {115200, 0x1002},
    {230400, 0x1003}, {460800, 0x1004}
};

static unsigned int saved_baud[1024];
static int (*real_ioctl)(int, unsigned long, void *);
static int force_fail = -1;


/* ---- optional I/O trace (GPS_SERIAL_LOG=1): shows what wine/the program does on the virtual COM ports ---- */
static int log_on = -1;
static int log_fd = -1;
static unsigned log_lines;
static unsigned char is_serial[1024];
static struct timespec last_read[1024];
static unsigned short seen[1024][16];
static ssize_t (*real_read)(int, void *, size_t);
static int (*real_close)(int);
static int (*real_open)(const char *, int, ...);

static void log_init(void)
{
    if (log_on >= 0) return;
    log_on = getenv("GPS_SERIAL_LOG") ? 1 : 0;
    if (!log_on) return;
    real_open = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    const char *wp = getenv("WINEPREFIX");
    char path[600];
    snprintf(path, sizeof(path), "%s/drive_c/serialshim.log", wp ? wp : "/tmp");
    if (real_open) log_fd = real_open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd >= 0) {
        char cmd[240] = {0};
        int f = real_open("/proc/self/cmdline", O_RDONLY);
        if (f >= 0) {
            if (!real_read) real_read = (ssize_t (*)(int, void *, size_t))dlsym(RTLD_NEXT, "read");
            ssize_t n = real_read ? real_read(f, cmd, sizeof(cmd) - 1) : 0;
            for (ssize_t i = 0; i < n; i++) if (cmd[i] == 0) cmd[i] = ' ';
            if (!real_close) real_close = (int (*)(int))dlsym(RTLD_NEXT, "close");
            if (real_close) real_close(f);
        }
        char line[400];
        int len = snprintf(line, sizeof(line), "==== process %d: %s\n", (int)getpid(), cmd);
        if (write(log_fd, line, len) < 0) {}
    }
}

static void tlog(const char *fmt, ...)
{
    if (log_fd < 0 || log_lines > 4000) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    char buf[700];
    int n = snprintf(buf, sizeof(buf), "[%ld.%03ld p%d] ", (long)t.tv_sec % 100000, t.tv_nsec / 1000000, (int)getpid());
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    buf[n++] = '\n';
    if (write(log_fd, buf, n) < 0) {}
    log_lines++;
}

static int mark_serial(int fd)
{
    if (fd < 0 || fd >= 1024) return 0;
    if (is_serial[fd]) return 1;
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) >= 136 && major(st.st_rdev) <= 143) {
        is_serial[fd] = 1;
        char link[64], path[128] = {0};
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        tlog("fd %d is a pty: %s", fd, n > 0 ? path : "?");
        return 1;
    }
    return 0;
}

/* log only the first few occurrences of a chatty request per fd */
static int first_few(int fd, unsigned long req)
{
    unsigned k = (unsigned)(req & 15);
    if (fd < 0 || fd >= 1024) return 0;
    return seen[fd][k]++ < 3;
}

static const char *req_name(unsigned long r)
{
    switch (r) {
    case K_TCGETS: return "TCGETS"; case K_TCSETS: return "TCSETS"; case K_TCSETSW: return "TCSETSW"; case K_TCSETSF: return "TCSETSF";
    case K_TCGETS2: return "TCGETS2"; case K_TCSETS2: return "TCSETS2"; case K_TCSETSW2: return "TCSETSW2"; case K_TCSETSF2: return "TCSETSF2";
    case K_TIOCMGET: return "TIOCMGET"; case K_TIOCMBIS: return "TIOCMBIS"; case K_TIOCMBIC: return "TIOCMBIC"; case K_TIOCMSET: return "TIOCMSET";
    case K_TIOCSSERIAL: return "TIOCSSERIAL"; case 0x541e: return "TIOCGSERIAL"; case 0x540b: return "TCFLSH"; case 0x541b: return "FIONREAD";
    case 0x5411: return "TIOCOUTQ"; case 0x545d: return "TIOCGICOUNT"; case 0x5459: return "TIOCSERGETLSR"; case 0x5409: return "TCSBRK";
    }
    return NULL;
}

static void log_termios2(const struct k_termios2 *t)
{
    tlog("   termios2: iflag=0x%x oflag=0x%x cflag=0x%x lflag=0x%x CREAD=%d CLOCAL=%d CSIZE=0x%x PARENB=%d baud=%u VMIN=%d VTIME=%d",
         t->c_iflag, t->c_oflag, t->c_cflag, t->c_lflag, !!(t->c_cflag & 0x80), !!(t->c_cflag & 0x800), t->c_cflag & 0x30,
         !!(t->c_cflag & 0x100), t->c_ospeed, t->c_cc[6], t->c_cc[5]);
}

ssize_t read(int fd, void *buf, size_t n)
{
    if (!real_read) real_read = (ssize_t (*)(int, void *, size_t))dlsym(RTLD_NEXT, "read");
    ssize_t r = real_read ? real_read(fd, buf, n) : (ssize_t)syscall(SYS_read, fd, buf, n);
    if (log_on > 0 && fd >= 0 && fd < 1024 && is_serial[fd]) {
        int e = errno;
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        long gap = last_read[fd].tv_sec ? (t.tv_sec - last_read[fd].tv_sec) * 1000 + (t.tv_nsec - last_read[fd].tv_nsec) / 1000000 : -1;
        last_read[fd] = t;
        char head[40] = {0};
        for (ssize_t i = 0; i < r && i < 30; i++) head[i] = (((unsigned char *)buf)[i] >= 32 && ((unsigned char *)buf)[i] < 127) ? ((char *)buf)[i] : '.';
        tlog("read(fd %d, %zu) = %zd, %ld ms since previous read on this fd, starts: %s", fd, n, r, gap, head);
        errno = e;
    }
    return r;
}

int close(int fd)
{
    if (!real_close) real_close = (int (*)(int))dlsym(RTLD_NEXT, "close");
    if (log_on > 0 && fd >= 0 && fd < 1024 && is_serial[fd]) { tlog("close(fd %d)", fd); is_serial[fd] = 0; memset(seen[fd], 0, sizeof(seen[fd])); last_read[fd].tv_sec = 0; }
    return real_close ? real_close(fd) : (int)syscall(SYS_close, fd);
}

static int do_ioctl(int fd, unsigned long req, void *arg)
{
    if (!real_ioctl) {
        real_ioctl = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "ioctl");
        if (!real_ioctl) return (int)syscall(SYS_ioctl, fd, req, arg);
    }
    return real_ioctl(fd, req, arg);
}

static unsigned int code_to_rate(unsigned int code)
{
    for (unsigned i = 0; i < sizeof(baud_table) / sizeof(baud_table[0]); i++)
        if (baud_table[i].code == code) return baud_table[i].rate;
    return 38400;
}

static unsigned int rate_to_code(unsigned int rate)
{
    for (unsigned i = 0; i < sizeof(baud_table) / sizeof(baud_table[0]); i++)
        if (baud_table[i].rate == rate) return baud_table[i].code;
    return 15; /* B38400: closest harmless choice, the exact rate is remembered separately */
}

static int ioctl_core(int fd, unsigned long req, void *arg);

static int emulated;

static int ioctl_core(int fd, unsigned long req, void *arg)
{
    if (force_fail < 0) force_fail = getenv("SERIALSHIM_FORCE_FAIL") ? 1 : 0;

    int handled = (req == K_TCGETS2 || req == K_TCSETS2 || req == K_TCSETSW2 || req == K_TCSETSF2 ||
                   req == K_TIOCMGET || req == K_TIOCMBIS || req == K_TIOCMBIC || req == K_TIOCMSET || req == K_TIOCSSERIAL);

    int ret, err;
    if (handled && force_fail) { ret = -1; errno = EACCES; }
    else ret = do_ioctl(fd, req, arg);
    if (ret >= 0 || !handled) return ret;

    err = errno;
    emulated = 1;
    if (err != EACCES && err != EPERM && err != ENOTTY && err != EINVAL && err != EOPNOTSUPP) { emulated = 0; errno = err; return ret; }

    switch (req) {
    case K_TCGETS2: {
        struct k_termios kt;
        if (do_ioctl(fd, K_TCGETS, &kt) < 0) return -1;
        struct k_termios2 *t = (struct k_termios2 *)arg;
        memset(t, 0, sizeof(*t));
        t->c_iflag = kt.c_iflag; t->c_oflag = kt.c_oflag; t->c_cflag = kt.c_cflag; t->c_lflag = kt.c_lflag;
        t->c_line = kt.c_line; memcpy(t->c_cc, kt.c_cc, sizeof(kt.c_cc));
        unsigned int rate = (fd >= 0 && fd < 1024 && saved_baud[fd]) ? saved_baud[fd] : code_to_rate(kt.c_cflag & K_CBAUD);
        t->c_ispeed = t->c_ospeed = rate;
        return 0;
    }
    case K_TCSETS2: case K_TCSETSW2: case K_TCSETSF2: {
        const struct k_termios2 *t = (const struct k_termios2 *)arg;
        struct k_termios kt;
        memset(&kt, 0, sizeof(kt));
        kt.c_iflag = t->c_iflag; kt.c_oflag = t->c_oflag; kt.c_lflag = t->c_lflag; kt.c_line = t->c_line;
        memcpy(kt.c_cc, t->c_cc, sizeof(kt.c_cc));
        unsigned int rate = t->c_ospeed ? t->c_ospeed : code_to_rate(t->c_cflag & K_CBAUD);
        kt.c_cflag = (t->c_cflag & ~(K_CBAUD | K_CIBAUD)) | rate_to_code(rate);
        unsigned long classic = req == K_TCSETS2 ? K_TCSETS : (req == K_TCSETSW2 ? K_TCSETSW : K_TCSETSF);
        do_ioctl(fd, classic, &kt); /* a pty ignores the baud rate anyway: failing here is not fatal */
        if (fd >= 0 && fd < 1024) saved_baud[fd] = rate;
        return 0;
    }
    case K_TIOCMGET:
        /* a virtual port: report DTR, RTS, CTS, DSR and carrier as asserted */
        *(int *)arg = 0x002 | 0x004 | 0x020 | 0x040 | 0x100;
        return 0;
    case K_TIOCMBIS: case K_TIOCMBIC: case K_TIOCMSET: case K_TIOCSSERIAL:
        return 0;
    }
    errno = err;
    return ret;
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    log_init();
    emulated = 0;
    int ret = ioctl_core(fd, req, arg);

    const char *name;
    if (log_on > 0 && (name = req_name(req)) != NULL && mark_serial(fd)) {
        int e = errno;
        int chatty = (req == 0x541b || req == 0x5411 || req == K_TIOCMGET || req == 0x545d || req == 0x5459);
        if (!chatty || first_few(fd, req)) {
            if (ret < 0) tlog("ioctl(fd %d, %s) = -1 errno=%d", fd, name, e);
            else tlog("ioctl(fd %d, %s) = %d%s", fd, name, ret, emulated ? " [emulated by shim]" : "");
            if (ret >= 0 && (req == K_TCGETS2 || req == K_TCSETS2 || req == K_TCSETSW2 || req == K_TCSETSF2)) log_termios2((const struct k_termios2 *)arg);
            if (ret >= 0 && req == K_TIOCMGET) tlog("   modem bits=0x%x", *(unsigned int *)arg);
            if (ret >= 0 && (req == 0x541b || req == 0x5411)) tlog("   count=%d", *(int *)arg);
            if (req == 0x540b) tlog("   TCFLSH queue selector=%ld", (long)(intptr_t)arg);
        }
        errno = e;
    }
    return ret;
}

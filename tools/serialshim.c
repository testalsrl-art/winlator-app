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
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

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

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (force_fail < 0) force_fail = getenv("SERIALSHIM_FORCE_FAIL") ? 1 : 0;

    int handled = (req == K_TCGETS2 || req == K_TCSETS2 || req == K_TCSETSW2 || req == K_TCSETSF2 ||
                   req == K_TIOCMGET || req == K_TIOCMBIS || req == K_TIOCMBIC || req == K_TIOCMSET || req == K_TIOCSSERIAL);

    int ret, err;
    if (handled && force_fail) { ret = -1; errno = EACCES; }
    else ret = do_ioctl(fd, req, arg);
    if (ret >= 0 || !handled) return ret;

    err = errno;
    if (err != EACCES && err != EPERM && err != ENOTTY && err != EINVAL && err != EOPNOTSUPP) { errno = err; return ret; }

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

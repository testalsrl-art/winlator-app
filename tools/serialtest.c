/* Serial port probe for wine: tries COM1..COM8 the way the navigation programs do and logs every result. */
#include <windows.h>
#include <stdio.h>

static FILE *lf;

static void test_blocking(int n, DWORD share)
{
    WCHAR name[32];
    wsprintfW(name, L"\\\\.\\COM%d", n);
    HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, share, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(lf, "COM%d [blocking share=%lu] OPEN FAILED err=%lu\n", n, share, GetLastError());
        return;
    }
    DCB dcb; memset(&dcb, 0, sizeof(dcb)); dcb.DCBlength = sizeof(dcb);
    BOOL g = GetCommState(h, &dcb); DWORD ge = GetLastError();
    dcb.BaudRate = 4800; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    BOOL s = SetCommState(h, &dcb); DWORD se = GetLastError();
    COMMTIMEOUTS to = {50, 0, 500, 0, 0};
    BOOL t = SetCommTimeouts(h, &to);
    char buf[512]; DWORD got = 0;
    BOOL r = ReadFile(h, buf, sizeof(buf) - 1, &got, NULL); DWORD re = GetLastError();
    fprintf(lf, "COM%d [blocking share=%lu] open ok | GetCommState=%d(err %lu) SetCommState=%d(err %lu) SetTimeouts=%d | ReadFile=%d(err %lu) bytes=%lu\n",
            n, share, g, ge, s, se, t, r, re, got);
    if (got) { buf[got > 70 ? 70 : got] = 0; fprintf(lf, "    first bytes: %s\n", buf); }
    CloseHandle(h);
}

static void test_overlapped(int n)
{
    WCHAR name[32];
    wsprintfW(name, L"\\\\.\\COM%d", n);
    HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(lf, "COM%d [overlapped+WaitCommEvent] OPEN FAILED err=%lu\n", n, GetLastError());
        return;
    }
    BOOL m = SetCommMask(h, EV_RXCHAR);
    OVERLAPPED ov; memset(&ov, 0, sizeof(ov)); ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD mask = 0;
    BOOL ok = WaitCommEvent(h, &mask, &ov); DWORD we = GetLastError();
    const char *res = "completed immediately";
    if (!ok) {
        if (we == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, 3000);
            res = (w == WAIT_OBJECT_0) ? "event signalled" : "TIMEOUT (no event for 3s)";
        } else res = "WaitCommEvent FAILED";
    }
    fprintf(lf, "COM%d [overlapped+WaitCommEvent] SetCommMask=%d WaitCommEvent=%d(err %lu) -> %s mask=0x%lx\n", n, m, ok, we, res, mask);
    CloseHandle(ov.hEvent);
    CloseHandle(h);
}

int main(void)
{
    lf = fopen("C:\\serialtest.log", "w");
    if (!lf) return 1;
    for (int n = 1; n <= 8; n++) test_blocking(n, 0);
    for (int n = 1; n <= 8; n++) test_blocking(n, 3);
    for (int n = 1; n <= 8; n++) test_overlapped(n);
    fprintf(lf, "done\n");
    fclose(lf);
    MessageBoxA(NULL, "Finished. Result is in C:\\serialtest.log (Z:/ or the container's drive_c).", "serialtest", MB_OK);
    return 0;
}

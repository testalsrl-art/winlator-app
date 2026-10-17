#define _GNU_SOURCE
#include <jni.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <string.h>

// Virtual serial port helper: creates a pty pair. The slave (/dev/pts/N) is
// symlinked into wine's dosdevices as comN, the master is fed with NMEA data.

JNIEXPORT jintArray JNICALL
Java_com_winlator_core_SerialPty_openPty(JNIEnv *env, jclass obj) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (master < 0) return NULL;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return NULL;
    }

    char name[64];
    if (ptsname_r(master, name, sizeof(name)) != 0) {
        close(master);
        return NULL;
    }

    // Keep one slave fd open on our side so the master never gets EIO/hangup
    // when the guest program opens and closes the port.
    int slave = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (slave < 0) {
        close(master);
        return NULL;
    }

    struct termios tio;
    if (tcgetattr(slave, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        tcsetattr(slave, TCSANOW, &tio);
    }

    int number = atoi(strrchr(name, '/') + 1);
    jint values[3] = {master, slave, number};
    jintArray result = (*env)->NewIntArray(env, 3);
    (*env)->SetIntArrayRegion(env, result, 0, 3, values);
    return result;
}

JNIEXPORT jint JNICALL
Java_com_winlator_core_SerialPty_write(JNIEnv *env, jclass obj, jint master, jint slave, jbyteArray data, jint length) {
    jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);
    int written = write(master, bytes, length);
    if (written < 0 && errno == EAGAIN) {
        // Nobody is reading: drop stale data so the port always holds fresh fixes.
        tcflush(slave, TCIFLUSH);
        written = write(master, bytes, length);
    }
    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
    return written;
}

JNIEXPORT void JNICALL
Java_com_winlator_core_SerialPty_close(JNIEnv *env, jclass obj, jint master, jint slave) {
    if (slave >= 0) close(slave);
    if (master >= 0) close(master);
}

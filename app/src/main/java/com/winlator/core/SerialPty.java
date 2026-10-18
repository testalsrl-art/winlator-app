package com.winlator.core;

public abstract class SerialPty {
    static {
        System.loadLibrary("winlator");
    }

    /** @return {masterFd, slaveFd, slaveNumber} or null on failure. */
    public static native int[] openPty();
    public static native int write(int master, int slave, byte[] data, int length);
    public static native void close(int master, int slave);
    public static native int queued(int slave);
    public static native int readMaster(int master, byte[] buffer, int max);
}

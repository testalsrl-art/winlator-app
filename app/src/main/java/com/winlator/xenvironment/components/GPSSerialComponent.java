package com.winlator.xenvironment.components;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.location.GnssStatus;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.location.OnNmeaMessageListener;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.system.Os;

import com.winlator.core.Callback;
import com.winlator.core.FileUtils;
import com.winlator.core.SerialPty;
import com.winlator.core.WineRegistryEditor;
import com.winlator.xenvironment.EnvironmentComponent;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashSet;
import java.util.Set;
import java.util.Locale;
import java.util.TimeZone;

/**
 * Feeds the Android GPS position as NMEA 0183 sentences to virtual serial ports
 * (COM1..COM{PORT_COUNT}) inside the wine container.
 */
public class GPSSerialComponent extends EnvironmentComponent {
    public static final int PORT_COUNT = 8;

    private static class Port {
        final int master, slave, number;
        volatile int queuedBefore = 0;      // unread bytes left from the previous second (0 = the program reads the data)
        volatile long rxBytes = 0;          // bytes the guest program wrote to this port
        volatile String rxSample = "";
        Port(int[] v) { master = v[0]; slave = v[1]; number = v[2]; }
    }

    private final File dosdevicesDir;
    private final ArrayList<Port> ports = new ArrayList<>();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private LocationManager locationManager;
    private volatile Location lastLocation;
    private volatile boolean running = false;
    private volatile Set<Integer> connectedPorts = new HashSet<>();
    private volatile boolean hasPermission = false;
    private Thread scanThread;
    private final java.util.concurrent.ConcurrentLinkedQueue<String> nmeaQueue = new java.util.concurrent.ConcurrentLinkedQueue<>();
    private final OnNmeaMessageListener nmeaListener = (message, timestamp) -> {
        if (nmeaQueue.size() < 300) nmeaQueue.add(message);
    };
    private volatile int lastRawCount = 0;
    private volatile int lastConvertedCount = 0;
    private volatile GnssStatus gnssStatus;
    private final GnssStatus.Callback gnssCallback = new GnssStatus.Callback() {
        @Override public void onSatelliteStatusChanged(GnssStatus status) { gnssStatus = status; }
    };

    private static class Sat { int prn, elev, az, snr; boolean used; }
    private long sentCount = 0;
    private long okWrites = 0;
    private volatile int lastOpenedPort = 0;
    private volatile long lastOpenedAt = 0;
    private Callback<String> statusCallback;

    private final LocationListener locationListener = new LocationListener() {
        @Override public void onLocationChanged(Location location) { lastLocation = location; }
        @Override public void onStatusChanged(String provider, int status, Bundle extras) {}
        @Override public void onProviderEnabled(String provider) {}
        @Override public void onProviderDisabled(String provider) {}
    };

    private final Runnable sender = new Runnable() {
        @Override
        public void run() {
            if (!running) return;
            String raw = drainRawNmea();
            sendNMEA(raw.isEmpty() ? buildSentences(lastLocation) : raw);
            sentCount++;
            if (statusCallback != null) statusCallback.call(buildStatusText());
            handler.postDelayed(this, 1000);
        }
    };

    /**
     * Wine builds HKLM\HARDWARE\DEVICEMAP\SERIALCOMM (the list programs use to find COM ports) from
     * /dev/tty* devices plus user-defined entries in HKLM\Software\Wine\Ports. Android has no usable
     * /dev/ttyS*, so register our virtual ports there. Must happen before wineserver starts.
     */
    private void registerWinePorts() {
        File systemRegFile = new File(dosdevicesDir.getParentFile(), "system.reg");
        if (!systemRegFile.isFile()) return;

        ArrayList<String[]> items = new ArrayList<>();
        for (int i = 0; i < ports.size(); i++) items.add(new String[]{"COM"+(i+1), "/dev/pts/"+ports.get(i).number});

        try (WineRegistryEditor registryEditor = new WineRegistryEditor(systemRegFile)) {
            registryEditor.setStringValues("Software\\Wine\\Ports", items.toArray(new String[0][]));
        }
    }

    public void setStatusCallback(Callback<String> statusCallback) {
        this.statusCallback = statusCallback;
    }

    private String buildStatusText() {
        String gps;
        Location location = lastLocation;
        if (!hasPermission) gps = "위치 권한 없음";
        else if (locationManager == null || !locationManager.isProviderEnabled(LocationManager.GPS_PROVIDER)) gps = "GPS 꺼짐";
        else if (location == null || System.currentTimeMillis() - location.getTime() > 10000) gps = "신호 찾는 중...";
        else gps = String.format(Locale.US, "수신 중 (오차 %dm)", location.hasAccuracy() ? Math.round(location.getAccuracy()) : 0);

        Set<Integer> connected = connectedPorts;
        String com;
        if (!connected.isEmpty()) {
            StringBuilder sb = new StringBuilder();
            for (int i = 1; i <= PORT_COUNT; i++) if (connected.contains(i)) sb.append(sb.length() > 0 ? ", " : "").append("COM").append(i);
            com = sb+" 열려 있음";
        }
        else if (lastOpenedAt > 0) com = "닫힘 (마지막으로 COM"+lastOpenedPort+" 열림, "+((System.currentTimeMillis() - lastOpenedAt) / 1000)+"초 전)";
        else com = "프로그램이 연 적 없음";

        String portInfo = ports.isEmpty() ? "가상 포트 생성 실패!" : "가상 포트 "+ports.size()+"개 (COM1-COM"+ports.size()+", /dev/pts/"+ports.get(0).number+"...)";
        int focus = !connected.isEmpty() ? java.util.Collections.min(connected) : lastOpenedPort;
        String detail = "";
        if (focus > 0 && focus <= ports.size()) {
            Port port = ports.get(focus - 1);
            detail = "\nCOM"+focus+": 안 읽힌 데이터 "+port.queuedBefore+"B | 앱이 보낸 "+port.rxBytes+"B"+(port.rxSample.isEmpty() ? "" : " ["+port.rxSample+"]");
        }
        long totalRx = 0;
        for (Port port : ports) totalRx += port.rxBytes;
        return "GPS: "+gps+"\n"+portInfo+" | 쓰기 "+okWrites+"회\nCOM: "+com+detail+"\n앱→포트 전체 "+totalRx+"B\nWine 포트 등록: "+countWineRegistered()+"/"+ports.size()+"\n폰 원본 NMEA: 초당 "+lastRawCount+"줄 (GP 변환 "+lastConvertedCount+"줄 추가)"+(lastRawCount == 0 ? " → 원본 없음, 합성 문장 사용" : "");
    }

    private final byte[] rxBuffer = new byte[256];

    private void drainProgramOutput() {
        for (Port port : new ArrayList<>(ports)) {
            int count;
            while ((count = SerialPty.readMaster(port.master, rxBuffer, rxBuffer.length)) > 0) {
                port.rxBytes += count;
                StringBuilder sb = new StringBuilder();
                for (int i = 0; i < Math.min(count, 24); i++) {
                    int c = rxBuffer[i] & 0xFF;
                    sb.append(c >= 32 && c < 127 ? String.valueOf((char)c) : String.format(Locale.US, "\\x%02X", c));
                }
                port.rxSample = sb.toString();
            }
        }
    }

    /** Our pre-created links use "/dev/pts/../pts/N"; wine's mountmgr re-creates them as "/dev/pts/N" when it registers a port. */
    private int countWineRegistered() {
        int count = 0;
        for (int i = 0; i < ports.size(); i++) {
            try {
                String target = Os.readlink((new File(dosdevicesDir, "com"+(i+1))).getPath());
                if (target.equals("/dev/pts/"+ports.get(i).number)) count++;
            }
            catch (Exception e) {}
        }
        return count;
    }

    /** Finds which of our virtual ports another process (wine) currently has open, via /proc/<pid>/fd. */
    private void scanConnectedPorts() {
        HashSet<Integer> found = new HashSet<>();
        String[] pids = (new File("/proc")).list();
        int myPid = Process.myPid();
        if (pids != null) {
            for (String pid : pids) {
                if (pid.isEmpty() || !Character.isDigit(pid.charAt(0)) || pid.equals(String.valueOf(myPid))) continue;
                String[] fds = (new File("/proc/"+pid+"/fd")).list();
                if (fds == null) continue;
                for (String fd : fds) {
                    try {
                        String target = Os.readlink("/proc/"+pid+"/fd/"+fd);
                        if (!target.startsWith("/dev/pts/")) continue;
                        int number = Integer.parseInt(target.substring(9));
                        for (int i = 0; i < ports.size(); i++) if (ports.get(i).number == number) found.add(i+1);
                    }
                    catch (Exception e) {}
                }
            }
        }
        connectedPorts = found;
        if (!found.isEmpty()) {
            lastOpenedAt = System.currentTimeMillis();
            lastOpenedPort = java.util.Collections.min(found);
        }
    }

    public GPSSerialComponent(File wineDosdevicesDir) {
        this.dosdevicesDir = wineDosdevicesDir;
    }

    @Override
    public void start() {
        for (int i = 1; i <= PORT_COUNT; i++) {
            int[] values = SerialPty.openPty();
            if (values == null) continue;
            Port port = new Port(values);
            ports.add(port);
            FileUtils.symlink("/dev/pts/../pts/"+port.number, (new File(dosdevicesDir, "com"+i)).getPath());
        }
        registerWinePorts();

        Context context = environment.getContext();
        hasPermission = context.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED;
        if (hasPermission) {
            locationManager = (LocationManager)context.getSystemService(Context.LOCATION_SERVICE);
            try {
                lastLocation = locationManager.getLastKnownLocation(LocationManager.GPS_PROVIDER);
                locationManager.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000, 0, locationListener, Looper.getMainLooper());
                locationManager.registerGnssStatusCallback(gnssCallback, handler);
                locationManager.addNmeaListener(nmeaListener, handler);
            }
            catch (SecurityException | IllegalArgumentException e) {}
        }

        running = true;
        handler.post(sender);

        scanThread = new Thread(() -> {
            while (running) {
                scanConnectedPorts();
                drainProgramOutput();
                try { Thread.sleep(250); } catch (InterruptedException e) { break; }
            }
        }, "gps-port-scan");
        scanThread.setDaemon(true);
        scanThread.start();
    }

    @Override
    public void stop() {
        running = false;
        statusCallback = null;
        handler.removeCallbacks(sender);
        if (scanThread != null) scanThread.interrupt();
        if (locationManager != null) {
            locationManager.removeUpdates(locationListener);
            locationManager.unregisterGnssStatusCallback(gnssCallback);
            locationManager.removeNmeaListener(nmeaListener);
            locationManager = null;
        }
        for (Port port : ports) SerialPty.close(port.master, port.slave);
        ports.clear();
    }

    private void sendNMEA(String sentences) {
        byte[] data = sentences.getBytes(StandardCharsets.US_ASCII);
        for (Port port : ports) port.queuedBefore = Math.max(0, SerialPty.queued(port.slave));
        for (Port port : ports) {
            if (SerialPty.write(port.master, port.slave, data, data.length) > 0) okWrites++;
        }
    }

    private static String withChecksum(String body) {
        int checksum = 0;
        for (int i = 0; i < body.length(); i++) checksum ^= body.charAt(i);
        return String.format(Locale.US, "$%s*%02X\r\n", body, checksum);
    }

    private static String formatLatLon(double value, boolean isLat) {
        double abs = Math.abs(value);
        int degrees = (int)abs;
        double minutes = (abs - degrees) * 60.0;
        String hemisphere = isLat ? (value >= 0 ? "N" : "S") : (value >= 0 ? "E" : "W");
        return String.format(Locale.US, isLat ? "%02d%07.4f,%s" : "%03d%07.4f,%s", degrees, minutes, hemisphere);
    }

    /**
     * Phone's own NMEA: every original sentence is forwarded as is, and each GN* sentence
     * (multi-constellation talker) is also forwarded as a GP* copy with a recalculated checksum,
     * because older navigation programs only recognise $GPxxx.
     */
    private String drainRawNmea() {
        StringBuilder sb = new StringBuilder();
        int raw = 0, converted = 0;
        String message;
        while ((message = nmeaQueue.poll()) != null) {
            String s = message.trim();
            if (s.length() < 8 || s.charAt(0) != '$') continue;
            sb.append(s).append("\r\n");
            raw++;
            if (s.startsWith("$GN")) {
                int star = s.indexOf('*');
                String body = star > 0 ? s.substring(1, star) : s.substring(1);
                sb.append(withChecksum("GP"+body.substring(2)));
                converted++;
            }
        }
        lastRawCount = raw;
        lastConvertedCount = converted;
        return sb.toString();
    }

    private ArrayList<Sat> gpsSatellites() {
        ArrayList<Sat> list = new ArrayList<>();
        GnssStatus status = gnssStatus;
        if (status == null) return list;
        for (int i = 0; i < status.getSatelliteCount(); i++) {
            if (status.getConstellationType(i) != GnssStatus.CONSTELLATION_GPS) continue;
            Sat sat = new Sat();
            sat.prn = status.getSvid(i);
            sat.elev = (int)status.getElevationDegrees(i);
            sat.az = (int)status.getAzimuthDegrees(i);
            sat.snr = (int)status.getCn0DbHz(i);
            sat.used = status.usedInFix(i);
            list.add(sat);
        }
        return list;
    }

    private String buildGSV(ArrayList<Sat> sats) {
        int n = sats.size();
        if (n == 0) return withChecksum("GPGSV,1,1,00");
        int total = (n + 3) / 4;
        StringBuilder sb = new StringBuilder();
        for (int m = 0; m < total; m++) {
            String body = String.format(Locale.US, "GPGSV,%d,%d,%02d", total, m+1, n);
            for (int k = m * 4; k < Math.min(n, m * 4 + 4); k++) {
                Sat sat = sats.get(k);
                body += String.format(Locale.US, ",%02d,%02d,%03d,%02d", sat.prn, Math.max(0, sat.elev), Math.max(0, sat.az), Math.max(0, sat.snr));
            }
            sb.append(withChecksum(body));
        }
        return sb.toString();
    }

    private String buildSentences(Location location) {
        Date now = new Date();
        SimpleDateFormat timeFormat = new SimpleDateFormat("HHmmss.00", Locale.US);
        SimpleDateFormat dateFormat = new SimpleDateFormat("ddMMyy", Locale.US);
        timeFormat.setTimeZone(TimeZone.getTimeZone("UTC"));
        dateFormat.setTimeZone(TimeZone.getTimeZone("UTC"));
        String time = timeFormat.format(now);
        String date = dateFormat.format(now);

        // No fix yet (or stale fix): report "void" so programs show "searching".
        if (location == null || System.currentTimeMillis() - location.getTime() > 10000) {
            return withChecksum("GPRMC,"+time+",V,,,,,,,"+date+",,,N") +
                   withChecksum("GPGGA,"+time+",,,,,0,00,,,M,,M,,");
        }

        ArrayList<Sat> sats = gpsSatellites();
        int usedCount = 0;
        StringBuilder slots = new StringBuilder();
        for (Sat sat : sats) {
            if (!sat.used || usedCount >= 12) continue;
            slots.append(String.format(Locale.US, "%02d,", sat.prn));
            usedCount++;
        }
        for (int i = usedCount; i < 12; i++) slots.append(",");

        int satellites = usedCount > 0 ? usedCount : (location.getExtras() != null ? location.getExtras().getInt("satellites", 0) : 0);
        if (satellites <= 0) satellites = 6;
        float accuracy = location.hasAccuracy() ? location.getAccuracy() : 5f;
        double hdop = Math.max(0.5, accuracy / 5.0);
        double speedKnots = location.hasSpeed() ? location.getSpeed() * 1.943844 : 0;
        double speedKmh = location.hasSpeed() ? location.getSpeed() * 3.6 : 0;
        float course = location.hasBearing() ? location.getBearing() : 0;
        String lat = formatLatLon(location.getLatitude(), true);
        String lon = formatLatLon(location.getLongitude(), false);

        String gga = String.format(Locale.US, "GPGGA,%s,%s,%s,1,%02d,%.1f,%.1f,M,0.0,M,,", time, lat, lon, satellites, hdop, location.getAltitude());
        String rmc = String.format(Locale.US, "GPRMC,%s,A,%s,%s,%.1f,%.1f,%s,,,A", time, lat, lon, speedKnots, course, date);
        String vtg = String.format(Locale.US, "GPVTG,%.1f,T,,M,%.1f,N,%.1f,K,A", course, speedKnots, speedKmh);
        String gsa = String.format(Locale.US, "GPGSA,A,3,%s%.1f,%.1f,%.1f", slots, hdop*1.5, hdop, hdop*1.2);
        return withChecksum(gga) + withChecksum(gsa) + buildGSV(sats) + withChecksum(rmc) + withChecksum(vtg);
    }
}

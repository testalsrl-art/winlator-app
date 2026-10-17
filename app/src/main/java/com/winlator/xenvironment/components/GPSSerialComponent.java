package com.winlator.xenvironment.components;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import com.winlator.core.FileUtils;
import com.winlator.core.SerialPty;
import com.winlator.xenvironment.EnvironmentComponent;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
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
        Port(int[] v) { master = v[0]; slave = v[1]; number = v[2]; }
    }

    private final File dosdevicesDir;
    private final ArrayList<Port> ports = new ArrayList<>();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private LocationManager locationManager;
    private volatile Location lastLocation;
    private volatile boolean running = false;

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
            sendNMEA(buildSentences(lastLocation));
            handler.postDelayed(this, 1000);
        }
    };

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
            FileUtils.symlink("/dev/pts/"+port.number, (new File(dosdevicesDir, "com"+i)).getPath());
        }

        Context context = environment.getContext();
        if (context.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED) {
            locationManager = (LocationManager)context.getSystemService(Context.LOCATION_SERVICE);
            try {
                lastLocation = locationManager.getLastKnownLocation(LocationManager.GPS_PROVIDER);
                locationManager.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000, 0, locationListener, Looper.getMainLooper());
            }
            catch (SecurityException | IllegalArgumentException e) {}
        }

        running = true;
        handler.post(sender);
    }

    @Override
    public void stop() {
        running = false;
        handler.removeCallbacks(sender);
        if (locationManager != null) {
            locationManager.removeUpdates(locationListener);
            locationManager = null;
        }
        for (Port port : ports) SerialPty.close(port.master, port.slave);
        ports.clear();
    }

    private void sendNMEA(String sentences) {
        byte[] data = sentences.getBytes(StandardCharsets.US_ASCII);
        for (Port port : ports) SerialPty.write(port.master, port.slave, data, data.length);
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

        int satellites = location.getExtras() != null ? location.getExtras().getInt("satellites", 0) : 0;
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
        String gsa = String.format(Locale.US, "GPGSA,A,3,,,,,,,,,,,,,%.1f,%.1f,%.1f", hdop*1.5, hdop, hdop*1.2);
        return withChecksum(gga) + withChecksum(gsa) + withChecksum(rmc) + withChecksum(vtg);
    }
}

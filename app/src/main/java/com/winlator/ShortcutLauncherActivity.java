package com.winlator;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

public class ShortcutLauncherActivity extends Activity {
    public static final String ACTION_RUN_SHORTCUT = "com.winlator.action.RUN_SHORTCUT";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int containerId = getIntent().getIntExtra("container_id", 0);
        String shortcutPath = getIntent().getStringExtra("shortcut_path");

        if (containerId > 0 && shortcutPath != null && !shortcutPath.isEmpty()) {
            Intent intent = new Intent(this, XServerDisplayActivity.class);
            intent.putExtra("container_id", containerId);
            intent.putExtra("shortcut_path", shortcutPath);
            startActivity(intent);
        }
        finish();
    }
}

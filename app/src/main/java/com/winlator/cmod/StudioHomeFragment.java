package com.winlator.cmod;

import android.content.Intent;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;
import androidx.fragment.app.Fragment;

import com.winlator.cmod.container.Container;
import com.winlator.cmod.container.ContainerManager;
import com.winlator.cmod.contents.ContentsManager;
import com.winlator.cmod.core.AppPaths;
import com.winlator.cmod.core.EnvVars;
import com.winlator.cmod.core.FileUtils;
import com.winlator.cmod.xenvironment.ImageFs;

import org.json.JSONException;

import java.io.File;
import java.io.IOException;

/**
 * The only screen a user needs: create the Studio container, run the Studio installer, open Studio.
 */
public class StudioHomeFragment extends Fragment {
    private static final String TAG = "StudioHomeFragment";
    private static final String INSTALLER_PREFIX = "robloxstudioinstaller";
    private static final String STUDIO_EXE = "RobloxStudioBeta.exe";

    private ContainerManager manager;
    private TextView tvStatus;
    private TextView tvHint;
    private Button btAction;
    private ProgressBar pbBusy;
    private boolean busy;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
        View view = inflater.inflate(R.layout.studio_home_fragment, container, false);
        tvStatus = view.findViewById(R.id.TVStudioStatus);
        tvHint = view.findViewById(R.id.TVStudioHint);
        btAction = view.findViewById(R.id.BTStudioAction);
        pbBusy = view.findViewById(R.id.PBStudioBusy);
        return view;
    }

    @Override
    public void onStart() {
        super.onStart();
        ((AppCompatActivity)getActivity()).getSupportActionBar().setTitle(R.string.app_name);
        refresh();
    }

    public void refresh() {
        if (!isAdded() || busy) return;

        if (!ImageFs.find(requireContext()).isValid()) {
            show(R.string.studio_status_system, 0, 0, null, true);
            return;
        }

        manager = new ContainerManager(requireContext());
        Container studio = findStudioContainer();

        if (studio == null) {
            show(R.string.studio_status_no_env, R.string.studio_action_create, 0, (v) -> createEnvironment(), false);
            return;
        }

        File studioExe = findStudioExe(studio);
        if (studioExe != null) {
            show(R.string.studio_status_ready, R.string.studio_action_launch, R.string.studio_hint_launch,
                    (v) -> launch(studio, "Roblox Studio", toWindowsPath(studio, studioExe)), false);
            return;
        }

        File installer = findInstaller();
        if (installer != null) {
            show(R.string.studio_status_not_installed, R.string.studio_action_install, 0,
                    (v) -> launch(studio, "Studio Installer", "D:\\" + installer.getName()), false);
        }
        else {
            show(R.string.studio_status_installer_missing, R.string.studio_action_recheck, R.string.studio_hint_installer,
                    (v) -> refresh(), false);
        }
    }

    private void show(int status, int actionLabel, int hint, View.OnClickListener onAction, boolean showBusy) {
        tvStatus.setText(status);
        tvHint.setText(hint != 0 ? getString(hint) : "");
        pbBusy.setVisibility(showBusy ? View.VISIBLE : View.GONE);

        btAction.setVisibility(actionLabel != 0 ? View.VISIBLE : View.GONE);
        if (actionLabel != 0) btAction.setText(actionLabel);
        btAction.setEnabled(onAction != null);
        btAction.setOnClickListener(onAction);
    }

    private Container findStudioContainer() {
        for (Container container : manager.getContainers()) {
            if (StudioPreset.CONTAINER_NAME.equals(container.getName())) return container;
        }
        return null;
    }

    private void createEnvironment() {
        busy = true;
        show(R.string.studio_status_creating, 0, 0, null, true);

        try {
            ContentsManager contentsManager = new ContentsManager(requireContext());
            contentsManager.syncContents();

            manager.createContainerAsync(StudioPreset.buildContainerData(), contentsManager, (container) -> {
                busy = false;
                if (!isAdded()) return;
                if (container == null) {
                    show(R.string.studio_status_create_failed, R.string.studio_action_create, 0, (v) -> createEnvironment(), false);
                }
                else refresh();
            });
        }
        catch (JSONException e) {
            Log.e(TAG, "Unable to build Studio container data", e);
            busy = false;
            show(R.string.studio_status_create_failed, R.string.studio_action_create, 0, (v) -> createEnvironment(), false);
        }
    }

    /** Studio installs itself under drive_c/users/xuser/AppData/Local/Roblox/Versions/version-xxxx/. */
    private File findStudioExe(Container container) {
        File dir = new File(container.getRootDir(), ".wine/drive_c");
        for (String name : new String[]{"users", ImageFs.USER, "AppData", "Local", "Roblox", "Versions"}) {
            dir = childIgnoreCase(dir, name);
            if (dir == null) return null;
        }

        File[] versions = dir.listFiles(File::isDirectory);
        if (versions == null) return null;

        File best = null;
        for (File version : versions) {
            File exe = childIgnoreCase(version, STUDIO_EXE);
            if (exe != null && (best == null || exe.lastModified() > best.lastModified())) best = exe;
        }
        return best;
    }

    private static File childIgnoreCase(File dir, String name) {
        File[] children = dir.listFiles();
        if (children == null) return null;
        for (File child : children) if (child.getName().equalsIgnoreCase(name)) return child;
        return null;
    }

    /** The Download folder is mapped to D: inside Wine. */
    private File findInstaller() {
        File[] files = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS).listFiles();
        if (files == null) return null;

        File best = null;
        for (File file : files) {
            String name = file.getName().toLowerCase();
            if (!file.isFile() || !name.startsWith(INSTALLER_PREFIX) || !name.endsWith(".exe")) continue;
            if (best == null || file.lastModified() > best.lastModified()) best = file;
        }
        return best;
    }

    private String toWindowsPath(Container container, File exe) {
        String driveC = new File(container.getRootDir(), ".wine/drive_c").getPath();
        return "C:" + exe.getPath().substring(driveC.length()).replace('/', '\\');
    }

    private void ensureDiagnostics(Container container) {
        EnvVars envVars = new EnvVars(container.getEnvVars());
        boolean changed = false;
        for (String[] pair : StudioPreset.DIAGNOSTIC_ENV_VARS) {
            if (!pair[1].equals(envVars.get(pair[0]))) {
                envVars.put(pair[0], pair[1]);
                changed = true;
            }
        }

        if (changed) {
            container.setEnvVars(envVars.toString());
            container.saveData();
        }
    }

    private void launch(Container container, String shortcutName, String windowsPath) {
        try {
            ensureDiagnostics(container);
            File desktopFile = writeShortcut(container, shortcutName, windowsPath);

            Intent intent = new Intent(requireContext(), XServerDisplayActivity.class);
            intent.setAction(Intent.ACTION_VIEW);
            intent.putExtra("container_id", container.id);
            intent.putExtra("shortcut_path", desktopFile.getPath());
            intent.putExtra("shortcut_name", shortcutName);
            startActivity(intent);
        }
        catch (IOException e) {
            Log.e(TAG, "Unable to create shortcut for " + windowsPath, e);
        }
    }

    /** Same .desktop layout the Shortcuts screen writes; backslashes are escaped the way Shortcut parses them. */
    private File writeShortcut(Container container, String name, String windowsPath) throws IOException {
        File desktopDir = container.getDesktopDir();
        desktopDir.mkdirs();
        File desktopFile = new File(desktopDir, name + ".desktop");

        String winePrefix = AppPaths.IMAGEFS + "/home/" + ImageFs.USER + "/.wine";
        String escapedPath = windowsPath.replace("\\", "\\\\\\\\");

        String content =
                "[Desktop Entry]\n" +
                "Name=" + name + "\n" +
                "Exec=env WINEPREFIX=\"" + winePrefix + "\" wine " + escapedPath + "\n" +
                "Type=Application\n" +
                "StartupNotify=true\n" +
                "Icon=\n" +
                "StartupWMClass=" + FileUtils.getName(windowsPath.replace('\\', '/')) + "\n\n" +
                "[Extra Data]\n" +
                "container_id:" + container.id + "\n";

        if (!FileUtils.writeString(desktopFile, content)) throw new IOException("Unable to write " + desktopFile);
        return desktopFile;
    }
}

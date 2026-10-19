package com.winlator.cmod;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
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
import androidx.appcompat.widget.Toolbar;
import androidx.fragment.app.Fragment;
import androidx.preference.PreferenceManager;

import com.winlator.cmod.container.Container;
import com.winlator.cmod.container.ContainerManager;
import com.winlator.cmod.contents.ContentsManager;
import com.winlator.cmod.core.AppPaths;
import com.winlator.cmod.core.AppUtils;
import com.winlator.cmod.core.EnvVars;
import com.winlator.cmod.core.FileUtils;
import com.winlator.cmod.xenvironment.ImageFs;

import org.json.JSONException;

import java.io.File;
import java.io.IOException;
import java.util.concurrent.Executors;

/**
 * The only screen a user needs: create the Studio container, run the Studio installer, open Studio.
 */
public class StudioHomeFragment extends Fragment {
    private static final String TAG = "StudioHomeFragment";
    private static final String INSTALLER_PREFIX = "robloxstudioinstaller";
    private static final String STUDIO_EXE = "RobloxStudioBeta.exe";
    private static final String PREF_API_DUMP = "studio_api_dump";

    private ContainerManager manager;
    private TextView tvStatus;
    private TextView tvHint;
    private Button btAction;
    private ProgressBar pbBusy;
    private Button btExport;
    private Button btApiDump;
    private boolean busy;

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container, @Nullable Bundle savedInstanceState) {
        View view = inflater.inflate(R.layout.studio_home_fragment, container, false);
        tvStatus = view.findViewById(R.id.TVStudioStatus);
        tvHint = view.findViewById(R.id.TVStudioHint);
        btAction = view.findViewById(R.id.BTStudioAction);
        pbBusy = view.findViewById(R.id.PBStudioBusy);
        btExport = view.findViewById(R.id.BTStudioExport);
        btApiDump = view.findViewById(R.id.BTStudioApiDump);
        return view;
    }

    @Override
    public void onStart() {
        super.onStart();
        ((AppCompatActivity)getActivity()).getSupportActionBar().setTitle(R.string.app_name);
        ((AppCompatActivity)getActivity()).getSupportActionBar().setSubtitle(getBuildLabel());
        ((Toolbar)getActivity().findViewById(R.id.Toolbar)).setSubtitleTextColor(Color.WHITE);
        refresh();
    }

    private String getBuildLabel() {
        try {
            Context context = requireContext();
            return "build " + context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName;
        }
        catch (PackageManager.NameNotFoundException e) {
            return "";
        }
    }

    public void refresh() {
        if (!isAdded() || busy) return;

        btExport.setVisibility(View.GONE);
        btApiDump.setVisibility(View.GONE);

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

        btExport.setVisibility(View.VISIBLE);
        btExport.setOnClickListener((v) -> exportStudioLogs(studio));
        btApiDump.setVisibility(View.VISIBLE);
        updateApiDumpButton();
        btApiDump.setOnClickListener((v) -> {
            SharedPreferences preferences = PreferenceManager.getDefaultSharedPreferences(requireContext());
            preferences.edit().putBoolean(PREF_API_DUMP, !preferences.getBoolean(PREF_API_DUMP, false)).apply();
            updateApiDumpButton();
        });

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

    /**
     * Studio installs under drive_c/Program Files/Roblox/Versions/version-xxxx/ when Wine reports a system account,
     * or under drive_c/users/xuser/AppData/Local/Roblox/Versions/ for a normal user install.
     */
    private File findStudioExe(Container container) {
        File driveC = new File(container.getRootDir(), ".wine/drive_c");

        File best = null;
        for (File versionsDir : new File[]{
                resolve(driveC, "Program Files", "Roblox", "Versions"),
                resolve(driveC, "Program Files (x86)", "Roblox", "Versions"),
                resolve(driveC, "users", ImageFs.USER, "AppData", "Local", "Roblox", "Versions")}) {
            if (versionsDir == null) continue;

            File[] versions = versionsDir.listFiles(File::isDirectory);
            if (versions == null) continue;

            for (File version : versions) {
                File exe = childIgnoreCase(version, STUDIO_EXE);
                if (exe != null && (best == null || exe.lastModified() > best.lastModified())) best = exe;
            }
        }
        return best;
    }

    private static File resolve(File dir, String... names) {
        for (String name : names) {
            dir = childIgnoreCase(dir, name);
            if (dir == null) return null;
        }
        return dir;
    }

    /** Copies Studio's own logs into the Download folder so they can be read without a Wine shell. */
    private void exportStudioLogs(Container container) {
        final Context context = requireContext();
        Executors.newSingleThreadExecutor().execute(() -> {
            File driveC = new File(container.getRootDir(), ".wine/drive_c");
            File destDir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "CTbloxStudio/logs");

            // A system-account install writes under systemprofile instead of the user profile.
            File[] sources = {
                resolve(driveC, "users", ImageFs.USER, "AppData", "Local", "Roblox", "logs"),
                resolve(driveC, "windows", "system32", "config", "systemprofile", "AppData", "Local", "Roblox", "logs")
            };
            String[] labels = {"user", "system"};

            int copied = 0;
            for (int i = 0; i < sources.length; i++) {
                if (sources[i] == null || countFiles(sources[i]) == 0) continue;

                File labelDir = new File(destDir, labels[i]);
                FileUtils.copy(sources[i], labelDir);
                copied += countFiles(labelDir);
            }

            if (copied == 0) AppUtils.showToast(context, R.string.studio_export_none);
            else AppUtils.showToast(context, context.getString(R.string.studio_export_done, copied));
        });
    }

    private static int countFiles(File dir) {
        File[] children = dir.listFiles();
        if (children == null) return 0;

        int count = 0;
        for (File child : children) count += child.isDirectory() ? countFiles(child) : 1;
        return count;
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

    private boolean isApiDumpEnabled() {
        return PreferenceManager.getDefaultSharedPreferences(requireContext()).getBoolean(PREF_API_DUMP, false);
    }

    private void updateApiDumpButton() {
        btApiDump.setText(isApiDumpEnabled() ? R.string.studio_apidump_on : R.string.studio_apidump_off);
    }

    /** The api_dump layer library ships in imagefs but has no manifest, so the Vulkan loader cannot find it. */
    private void writeApiDumpManifest() {
        File dir = new File(AppPaths.IMAGEFS + "/usr/share/vulkan/explicit_layer.d");
        dir.mkdirs();

        String manifest = "{\n" +
                "  \"file_format_version\": \"1.0.0\",\n" +
                "  \"layer\": {\n" +
                "    \"name\": \"" + StudioPreset.API_DUMP_LAYER + "\",\n" +
                "    \"type\": \"GLOBAL\",\n" +
                "    \"library_path\": \"" + AppPaths.IMAGEFS + "/usr/lib/libVkLayer_api_dump.so\",\n" +
                "    \"api_version\": \"1.3.0\",\n" +
                "    \"implementation_version\": \"2\",\n" +
                "    \"description\": \"LunarG API dump layer\"\n" +
                "  }\n" +
                "}\n";
        FileUtils.writeString(new File(dir, "VkLayer_api_dump.json"), manifest);
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

        boolean apiDump = isApiDumpEnabled();
        if (apiDump) writeApiDumpManifest();
        for (String[] pair : StudioPreset.API_DUMP_ENV_VARS) {
            if (apiDump && !pair[1].equals(envVars.get(pair[0]))) {
                envVars.put(pair[0], pair[1]);
                changed = true;
            }
            else if (!apiDump && envVars.has(pair[0])) {
                envVars.remove(pair[0]);
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

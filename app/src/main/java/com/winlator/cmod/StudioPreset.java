package com.winlator.cmod;

import com.winlator.cmod.box86_64.Box86_64Preset;
import com.winlator.cmod.container.Container;
import com.winlator.cmod.core.DefaultVersion;
import com.winlator.cmod.core.WineInfo;

import org.json.JSONException;
import org.json.JSONObject;

/**
 * Container settings for the dedicated Studio environment. Mirrors the combination that reached the
 * Studio login screen on a Mali-G72: Proton 9.0 x86_64, Wrapper driver ("System" version), DXVK 1.10.3.
 */
public final class StudioPreset {
    private StudioPreset() {}

    public static final String CONTAINER_NAME = "CTblox Studio";
    public static final String SCREEN_SIZE = "1280x720";
    public static final String DXVK_VERSION = "1.10.3";

    /** Applied before each launch so DXVK writes its device/feature-level log to the Download folder (D: in Wine). */
    public static final String[][] DIAGNOSTIC_ENV_VARS = {
        {"DXVK_LOG_LEVEL", "info"},
        {"DXVK_LOG_PATH", "D:/"},
    };

    public static final String API_DUMP_LAYER = "VK_LAYER_LUNARG_api_dump";

    /**
     * The loader here only picks up implicit layers (like UtilLayer), so api_dump is registered as one and
     * switched on with ENABLE_API_DUMP. Output goes to stdout, i.e. the Wine log.
     */
    public static final String[][] API_DUMP_ENV_VARS = {
        {"ENABLE_API_DUMP", "1"},
        {"VK_APIDUMP_OUTPUT_FORMAT", "text"},
        {"VK_APIDUMP_DETAILED", "false"},
        {"VK_APIDUMP_NO_ADDR", "true"},
        {"VK_APIDUMP_FLUSH", "true"},
    };

    /** Set by an earlier build that registered api_dump as an explicit layer, which was never loaded. */
    public static final String[] API_DUMP_LEGACY_ENV_VARS = {"VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE"};

    public static JSONObject buildContainerData() throws JSONException {
        JSONObject data = new JSONObject();
        data.put("name", CONTAINER_NAME);
        data.put("screenSize", SCREEN_SIZE);
        data.put("wineVersion", WineInfo.MAIN_WINE_VERSION.identifier());
        data.put("emulator", "Box64");
        data.put("box64Preset", Box86_64Preset.STABILITY);
        data.put("graphicsDriver", Container.DEFAULT_GRAPHICS_DRIVER);
        data.put("graphicsDriverConfig", Container.DEFAULT_GRAPHICSDRIVERCONFIG);
        data.put("dxwrapper", Container.DEFAULT_DXWRAPPER);
        data.put("dxwrapperConfig", "version=" + DXVK_VERSION + ",framerate=0,maxDeviceMemory=0,async=0,asyncCache=0"
                + ",vkd3dVersion=" + DefaultVersion.VKD3D + ",vkd3dLevel=12_1");
        data.put("drives", Container.DEFAULT_DRIVES);
        return data;
    }
}

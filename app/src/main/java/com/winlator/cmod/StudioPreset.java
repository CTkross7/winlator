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
    /** STABILITY made Studio's main thread stall for 20s+ on the MT8788 (HangMonitor in Studio logs); PERFORMANCE trades safety for speed. */
    public static final String BOX64_PRESET = Box86_64Preset.PERFORMANCE;
    /**
 * Stock DXVK 1.10.3 with a patched d3d11.dll (tools/dxvk/build_d3d11.sh, packed by CI as dxwrapper/dxvk-1.10.3-ctblox.tzst).
 * The Mali wrapper rejects VK_EXT_transform_feedback in vkCreateDevice, which stock DXVK requires for feature level 10_0.
 * Bump the suffix whenever tools/dxvk changes: Winlator only re-extracts DXVK when this version string changes, and CI
 * names the archive after this constant.
 */
    public static final String DXVK_VERSION = "1.10.3-ctblox20";

    /** Applied before each launch so DXVK writes its device/feature-level log to the Download folder (D: in Wine). */
    public static final String[][] DIAGNOSTIC_ENV_VARS = {
        {"DXVK_LOG_LEVEL", "info"},
        {"DXVK_LOG_PATH", "D:/"},
        {"CTBLOX_TRACE", "1"},
        {"DXVK_SHADER_DUMP_PATH", "D:"},
        {"CTBLOX_PROBE", "1"},
        {"CTBLOX_FORCEPS", "1"},
    };

    /** Left behind by builds that tried to enable the Vulkan api_dump layer, which this loader never picked up. */
    public static final String[] STALE_ENV_VARS = {
        "VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "ENABLE_API_DUMP",
        "VK_APIDUMP_OUTPUT_FORMAT", "VK_APIDUMP_DETAILED", "VK_APIDUMP_NO_ADDR", "VK_APIDUMP_FLUSH", "CTBLOX_OVERLAY_HIDE", "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", "CTBLOX_FLIPVP",
    };

    public static final String EXTENSION_BLACKLIST_ENV = "WRAPPER_EXTENSION_BLACKLIST";

    /**
     * Vulkan device extensions to hide from DXVK, one experiment at a time. DXVK reports D3D11 device creation
     * failing right after it lists the extensions it enabled, so one of them may be advertised but not usable.
     * VK_KHR_timeline_semaphore must stay visible: the wrapper's WSI code asserts without it and kills vkCreateInstance.
     * Each row is {label, comma separated extension names}.
     */
    public static final String[][] GPU_EXPERIMENTS = {
        {"기본(숨기지 않음)", ""},
        {"선택 확장 전부 숨김(timeline 제외)", "VK_EXT_host_query_reset,VK_EXT_transform_feedback,VK_KHR_create_renderpass2,VK_KHR_depth_stencil_resolve,VK_KHR_driver_properties,VK_KHR_image_format_list,VK_KHR_sampler_mirror_clamp_to_edge"},
        {"transform_feedback만", "VK_EXT_transform_feedback"},
        {"host_query_reset만", "VK_EXT_host_query_reset"},
        {"renderpass2 + depth_stencil_resolve", "VK_KHR_create_renderpass2,VK_KHR_depth_stencil_resolve"},
        {"driver_properties + image_format_list + sampler_mirror", "VK_KHR_driver_properties,VK_KHR_image_format_list,VK_KHR_sampler_mirror_clamp_to_edge"},
    };

    /** Added to the automatic test as a sanity check: this one is known to fail, so if it "succeeds" the setting never reached the child. */
    public static final String[] GPU_PROBE_CONTROL = {"대조군: timeline 숨김(실패해야 정상)", "VK_KHR_timeline_semaphore"};

    public static final String PROBE_EXE = "gpuprobe.exe";
    public static final String PROBE_EXPERIMENTS_FILE = "gpuprobe_experiments.txt";
    public static final String PROBE_RESULT_FILE = "gpuprobe_result.txt";

    public static JSONObject buildContainerData() throws JSONException {
        JSONObject data = new JSONObject();
        data.put("name", CONTAINER_NAME);
        data.put("screenSize", SCREEN_SIZE);
        data.put("wineVersion", WineInfo.MAIN_WINE_VERSION.identifier());
        data.put("emulator", "Box64");
        data.put("box64Preset", BOX64_PRESET);
        data.put("graphicsDriver", Container.DEFAULT_GRAPHICS_DRIVER);
        data.put("graphicsDriverConfig", Container.DEFAULT_GRAPHICSDRIVERCONFIG);
        data.put("dxwrapper", Container.DEFAULT_DXWRAPPER);
        data.put("dxwrapperConfig", "version=" + DXVK_VERSION + ",framerate=0,maxDeviceMemory=0,async=0,asyncCache=0"
                + ",vkd3dVersion=" + DefaultVersion.VKD3D + ",vkd3dLevel=12_1");
        data.put("drives", Container.DEFAULT_DRIVES);
        return data;
    }
}

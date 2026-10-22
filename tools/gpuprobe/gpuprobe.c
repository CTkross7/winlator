/*
 * GPU probe for CTblox Studio. Runs once inside Wine. For every line of D:\gpuprobe_experiments.txt
 * ("index<TAB>label<TAB>WRAPPER_EXTENSION_BLACKLIST value") it runs, in a child process so the variable reaches the
 * Vulkan wrapper at process start:
 *   - a direct Vulkan query (device extensions, features that are off),
 *   - D3D11CreateDevice with the feature levels Studio asks for.
 * A line "STUDIO<TAB>C:\path\RobloxStudioBeta.exe" makes it also start Studio itself once per experiment and keep
 * its DXVK log, which is the closest thing to the real failure.
 * Everything, DXVK logs included, goes into the single file D:\gpuprobe_result.txt.
 *
 *   gpuprobe.exe         parent
 *   gpuprobe.exe child   child: env dump + Vulkan query + D3D11CreateDevice, exit code 0 = created, else HRESULT
 */
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define EXPERIMENTS_FILE "D:\\gpuprobe_experiments.txt"
#define RESULT_FILE      "D:\\gpuprobe_result.txt"
#define CHILD_TIMEOUT_MS 120000
#define STUDIO_TIMEOUT_S 60

typedef HRESULT (WINAPI *PFN_CREATE)(void *, UINT, HMODULE, UINT, const UINT *, UINT, UINT, void **, UINT *, void **);

static void log_line(const char *fmt, ...) {
    FILE *f = fopen(RESULT_FILE, "ab");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Appends a file's text under a header, then deletes it so only the result file is left behind. */
static void merge_file(const char *path, const char *header) {
    FILE *in = fopen(path, "rb");
    if (!in) {
        log_line("  [%s] (not written)", header);
        return;
    }
    log_line("  [%s]", header);
    FILE *out = fopen(RESULT_FILE, "ab");
    char buffer[4096];
    size_t n;
    while (out && (n = fread(buffer, 1, sizeof buffer, in)) > 0) fwrite(buffer, 1, n, out);
    if (out) fclose(out);
    fclose(in);
    DeleteFileA(path);
}

static void merge_dxvk_logs(const char *dir) {
    char pattern[MAX_PATH], path[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*.log", dir);
    WIN32_FIND_DATAA find;
    HANDLE handle = FindFirstFileA(pattern, &find);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            snprintf(path, sizeof path, "%s\\%s", dir, find.cFileName);
            merge_file(path, find.cFileName);
        } while (FindNextFileA(handle, &find));
        FindClose(handle);
    }
    RemoveDirectoryA(dir);
}

static void dump_env(void) {
    static const char *prefixes[] = { "WRAPPER_", "VK_", "DXVK_", "MESA_", "BOX64_", "WINEDLLOVERRIDES", "LD_LIBRARY_PATH" };
    char *block = GetEnvironmentStringsA();
    for (char *e = block; *e; e += strlen(e) + 1) {
        for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++) {
            if (strncmp(e, prefixes[i], strlen(prefixes[i])) == 0) {
                log_line("  env %s", e);
                break;
            }
        }
    }
    FreeEnvironmentStringsA(block);
}


/* ---- Unix-side environment (what the Vulkan wrapper really sees) ---------------------------------------------- */

static char unixEnv[1 << 17];
static size_t unixEnvLen;

static void load_unix_env(void) {
    unixEnvLen = 0;
    FILE *f = fopen("Z:\\proc\\self\\environ", "rb");
    if (!f) return;
    size_t n;
    while (unixEnvLen < sizeof unixEnv - 1 && (n = fread(unixEnv + unixEnvLen, 1, sizeof unixEnv - 1 - unixEnvLen, f)) > 0) unixEnvLen += n;
    fclose(f);
    unixEnv[unixEnvLen] = 0;
}

static void dump_unix_env(const char *who) {
    load_unix_env();
    int count = 0;
    for (size_t i = 0; i < unixEnvLen; i += strlen(unixEnv + i) + 1) count++;
    log_line("  unix env of %s: %d entries (%lu bytes)", who, count, (unsigned long)unixEnvLen);
    for (size_t i = 0; i < unixEnvLen; i += strlen(unixEnv + i) + 1) {
        const char *e = unixEnv + i;
        if (strncmp(e, "LD_", 3) == 0 || strncmp(e, "VK_", 3) == 0 || strncmp(e, "WRAPPER_", 8) == 0 ||
            strncmp(e, "VKD3D", 5) == 0 || strncmp(e, "BOX64_LD", 8) == 0 || strncmp(e, "BOX64_PATH", 10) == 0 ||
            strncmp(e, "WINE", 4) == 0 || strncmp(e, "MESA_VK", 7) == 0)
            log_line("    unix %s", e);
    }
}

/* Copies every unix variable the Win32 view does not have into the Win32 environment so children inherit it. */
static void copy_unix_env_to_win32(void) {
    load_unix_env();
    int added = 0;
    for (size_t i = 0; i < unixEnvLen; i += strlen(unixEnv + i) + 1) {
        char name[512];
        const char *e = unixEnv + i, *eq = strchr(e, '=');
        if (!eq || eq == e || (size_t)(eq - e) >= sizeof name) continue;
        memcpy(name, e, eq - e);
        name[eq - e] = 0;
        if (GetEnvironmentVariableA(name, NULL, 0) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            SetEnvironmentVariableA(name, eq + 1);
            log_line("    copied to win32 env: %s", name);
            added++;
        }
    }
    log_line("  unix->win32 env copy: %d variables added", added);
}

/* ---- Direct Vulkan query ------------------------------------------------------------------------------------- */

typedef struct { int sType; const void *pNext; unsigned flags; const void *pApp; unsigned layerCount; const char *const *layers;
                 unsigned extCount; const char *const *exts; } InstanceInfo;
typedef struct { char name[256]; unsigned specVersion; } ExtProps;
typedef void *(WINAPI *PFN_GPA)(void *, const char *);

static const char *FEATURE_NAMES[55] = {
    "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader", "tessellationShader",
    "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect", "drawIndirectFirstInstance", "depthClamp",
    "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines", "largePoints", "alphaToOne", "multiViewport",
    "samplerAnisotropy", "textureCompressionETC2", "textureCompressionASTC_LDR", "textureCompressionBC",
    "occlusionQueryPrecise", "pipelineStatisticsQuery", "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics",
    "shaderTessellationAndGeometryPointSize", "shaderImageGatherExtended", "shaderStorageImageExtendedFormats",
    "shaderStorageImageMultisample", "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
    "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
    "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance",
    "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency", "shaderResourceMinLod",
    "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D", "sparseResidencyImage3D", "sparseResidency2Samples",
    "sparseResidency4Samples", "sparseResidency8Samples", "sparseResidency16Samples", "sparseResidencyAliased",
    "variableMultisampleRate", "inheritedQueries",
};


typedef struct { int sType; const void *pNext; unsigned flags; unsigned family; unsigned count; const float *priorities; } QueueInfo;
typedef struct { int sType; const void *pNext; unsigned flags; unsigned queueCount; const QueueInfo *queues; unsigned layerCount;
                 const char *const *layers; unsigned extCount; const char *const *exts; const unsigned *features; } DeviceInfo;
typedef struct { int sType; void *pNext; unsigned a, b; } FeatureStruct;

static const char *DXVK_EXTS[11] = {
    "VK_EXT_host_query_reset", "VK_EXT_transform_feedback", "VK_KHR_create_renderpass2", "VK_KHR_depth_stencil_resolve",
    "VK_KHR_driver_properties", "VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32", "VK_KHR_image_format_list",
    "VK_KHR_sampler_mirror_clamp_to_edge", "VK_KHR_swapchain", "VK_KHR_timeline_semaphore",
};

/* Tries vkCreateDevice with the given subset (bit mask over DXVK_EXTS) and the features DXVK enables for FL 10_0. */
static void try_create_device(PFN_GPA gipa, void *instance, void *physical, unsigned mask, const char *label) {
    int (WINAPI *createDevice)(void *, const DeviceInfo *, const void *, void **) = gipa(instance, "vkCreateDevice");
    void (WINAPI *destroyDevice)(void *, const void *) = gipa(instance, "vkDestroyDevice");

    static const int ON[] = { 0, 1, 3, 4, 6, 11, 12, 13, 18, 19, 22, 23, 28, 32, 37, 38 };
    unsigned features[55] = {0};
    for (unsigned i = 0; i < sizeof ON / sizeof *ON; i++) features[ON[i]] = 1;

    const char *exts[11];
    unsigned count = 0;
    int hasTf = 0, hasHqr = 0, hasTl = 0;
    for (int i = 0; i < 11; i++) {
        if (!(mask & (1u << i))) continue;
        exts[count++] = DXVK_EXTS[i];
        if (i == 1) hasTf = 1;
        if (i == 0) hasHqr = 1;
        if (i == 10) hasTl = 1;
    }

    FeatureStruct tf = { 1000028000, NULL, 1, 1 }, hqr = { 1000261000, NULL, 1, 0 }, tl = { 1000207000, NULL, 1, 0 };
    void *chain = NULL;
    if (hasTf) { tf.pNext = chain; chain = &tf; }
    if (hasHqr) { hqr.pNext = chain; chain = &hqr; }
    if (hasTl) { tl.pNext = chain; chain = &tl; }

    float priority = 1.0f;
    QueueInfo queue = { 2 /* DEVICE_QUEUE_CREATE_INFO */, NULL, 0, 0, 1, &priority };
    DeviceInfo info = { 3 /* DEVICE_CREATE_INFO */, chain, 0, 1, &queue, 0, NULL, count, exts, features };

    void *device = NULL;
    int r = createDevice(physical, &info, NULL, &device);
    log_line("    vkCreateDevice %-34s -> %d%s", label, r, r == 0 ? " (ok)" : "");
    if (r == 0 && destroyDevice) destroyDevice(device, NULL);
}

static void device_tests(PFN_GPA gipa, void *instance, void *physical) {
    log_line("  vkCreateDevice tests (queue family 0, DXVK's FL10_0 core features):");
    try_create_device(gipa, instance, physical, 0, "no extensions");
    try_create_device(gipa, instance, physical, 0x7FF, "all 11 extensions (what DXVK asks)");
    for (int i = 0; i < 11; i++) {
        char label[96];
        snprintf(label, sizeof label, "only %s", DXVK_EXTS[i] + 3);
        try_create_device(gipa, instance, physical, 1u << i, label);
    }
    for (int i = 0; i < 11; i++) {
        char label[96];
        snprintf(label, sizeof label, "all except %s", DXVK_EXTS[i] + 3);
        try_create_device(gipa, instance, physical, 0x7FF & ~(1u << i), label);
    }
}

static void vulkan_query(int withDeviceTests) {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { log_line("  vulkan: vulkan-1.dll not found"); return; }
    PFN_GPA gipa = (PFN_GPA)GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!gipa) { log_line("  vulkan: no vkGetInstanceProcAddr"); return; }

    int (WINAPI *createInstance)(const InstanceInfo *, const void *, void **) = gipa(NULL, "vkCreateInstance");
    InstanceInfo info = { 1 /* INSTANCE_CREATE_INFO */ };
    void *instance = NULL;
    int r = createInstance ? createInstance(&info, NULL, &instance) : -1;
    if (r != 0) { log_line("  vulkan: vkCreateInstance = %d", r); return; }

    int (WINAPI *enumDevices)(void *, unsigned *, void **) = gipa(instance, "vkEnumeratePhysicalDevices");
    unsigned count = 1;
    void *device = NULL;
    r = enumDevices(instance, &count, &device);
    if (!device) { log_line("  vulkan: vkEnumeratePhysicalDevices = %d, count=%u", r, count); return; }

    int (WINAPI *enumExts)(void *, const char *, unsigned *, ExtProps *) = gipa(instance, "vkEnumerateDeviceExtensionProperties");
    unsigned extCount = 0;
    enumExts(device, NULL, &extCount, NULL);
    ExtProps *exts = calloc(extCount ? extCount : 1, sizeof *exts);
    enumExts(device, NULL, &extCount, exts);

    char list[8192];
    list[0] = 0;
    for (unsigned i = 0; i < extCount && strlen(list) < sizeof list - 300; i++) {
        strcat(list, exts[i].name + (strncmp(exts[i].name, "VK_", 3) == 0 ? 3 : 0));
        strcat(list, " ");
    }
    log_line("  vulkan device extensions (%u, VK_ prefix cut): %s", extCount, list);
    free(exts);

    void (WINAPI *getFeatures)(void *, unsigned *) = gipa(instance, "vkGetPhysicalDeviceFeatures");
    unsigned features[55] = {0};
    getFeatures(device, features);
    list[0] = 0;
    for (int i = 0; i < 55; i++) if (!features[i]) { strcat(list, FEATURE_NAMES[i]); strcat(list, " "); }
    log_line("  vulkan core features that are OFF: %s", list[0] ? list : "(none)");

    if (withDeviceTests) device_tests(gipa, instance, device);
}


/* ---- Window surface tests (DXVK's "Failed to create surface") ------------------------------------------------- */

static LRESULT CALLBACK probe_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static HWND make_window(DWORD style, DWORD exStyle, HWND parent, const char *title, int show) {
    static int registered;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = probe_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "GpuProbeWnd";
        RegisterClassA(&wc);
        registered = 1;
    }
    HWND h = CreateWindowExA(exStyle, "GpuProbeWnd", title, style, 0, 0, 640, 480, parent, NULL, GetModuleHandleA(NULL), NULL);
    if (h && show) { ShowWindow(h, SW_SHOW); UpdateWindow(h); }

    MSG msg;
    DWORD end = GetTickCount() + 600;
    while (GetTickCount() < end) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(10);
    }
    return h;
}

typedef struct { int sType; const void *pNext; unsigned flags; HINSTANCE hinstance; HWND hwnd; } Win32SurfaceInfo;

static void vulkan_surface_test(PFN_GPA gipa, void *instance, HWND hwnd, const char *label) {
    int (WINAPI *create)(void *, const Win32SurfaceInfo *, const void *, void **) = gipa(instance, "vkCreateWin32SurfaceKHR");
    if (!create) { log_line("    vkCreateWin32SurfaceKHR missing"); return; }
    Win32SurfaceInfo info = { 1000009000, NULL, 0, GetModuleHandleA(NULL), hwnd };
    void *surface = NULL;
    int r = create(instance, &info, NULL, &surface);
    log_line("    vkCreateWin32SurfaceKHR %-22s hwnd=%p -> %d%s", label, (void *)hwnd, r, r == 0 ? " (ok)" : "");
}

static void swapchain_test(HWND hwnd, const char *label) {
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN create = d3d11 ? (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain") : NULL;
    if (!create) { log_line("    D3D11CreateDeviceAndSwapChain missing"); return; }

    DXGI_SWAP_CHAIN_DESC desc = {0};
    desc.BufferDesc.Width = 640;
    desc.BufferDesc.Height = 480;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = hwnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    IDXGISwapChain *swapChain = NULL;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL level = 0;
    HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4, D3D11_SDK_VERSION,
                        &desc, &swapChain, &device, &level, &context);
    log_line("    D3D11CreateDeviceAndSwapChain %-18s -> 0x%08lx%s", label, (unsigned long)hr, SUCCEEDED(hr) ? " (ok)" : "");
}

static void test_hwnd(PFN_GPA gipa, void *instance, HWND hwnd, const char *label, int withSwapchain) {
    RECT rect = {0};
    GetWindowRect(hwnd, &rect);
    log_line("    [%s] style=0x%08lx ex=0x%08lx visible=%d rect=%ld,%ld,%ld,%ld", label, (unsigned long)GetWindowLongA(hwnd, GWL_STYLE),
             (unsigned long)GetWindowLongA(hwnd, GWL_EXSTYLE), IsWindowVisible(hwnd), rect.left, rect.top, rect.right, rect.bottom);
    log_line("      desktop=%p ancestor(parent)=%p ancestor(root)=%p GetParent=%p owner=%p", (void *)GetDesktopWindow(),
             (void *)GetAncestor(hwnd, GA_PARENT), (void *)GetAncestor(hwnd, GA_ROOT), (void *)GetParent(hwnd), (void *)GetWindow(hwnd, GW_OWNER));
    if (instance) vulkan_surface_test(gipa, instance, hwnd, label);
    if (withSwapchain) swapchain_test(hwnd, label);
    else log_line("    D3D11CreateDeviceAndSwapChain %s skipped", label);
}

static void window_case(PFN_GPA gipa, void *instance, const char *label, DWORD style, DWORD exStyle, int show, int withChild) {
    HWND host = make_window(style, exStyle, NULL, label, show);
    if (!host) { log_line("    [%s] window creation failed (err=%lu)", label, GetLastError()); return; }

    HWND child = NULL;
    if (withChild) {
        child = make_window(WS_CHILD, 0, host, "child", 1);
        if (!child) log_line("    [%s] child window creation failed (err=%lu)", label, GetLastError());
    }

    test_hwnd(gipa, instance, host, label, 1);
    if (child) {
        char childLabel[96];
        snprintf(childLabel, sizeof childLabel, "child of %s", label);
        test_hwnd(gipa, instance, child, childLabel, 1);
        DestroyWindow(child);
    }
    DestroyWindow(host);
}

static void surface_tests(void) {
    log_line("  window surface tests:");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    const char *(__cdecl *wineVersion)(void) = ntdll ? (const char *(__cdecl *)(void))GetProcAddress(ntdll, "wine_get_version") : NULL;
    const char *(__cdecl *wineBuild)(void) = ntdll ? (const char *(__cdecl *)(void))GetProcAddress(ntdll, "wine_get_build_id") : NULL;
    log_line("    wine version: %s build: %s", wineVersion ? wineVersion() : "(unknown)", wineBuild ? wineBuild() : "(unknown)");

    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    PFN_GPA gipa = vk ? (PFN_GPA)GetProcAddress(vk, "vkGetInstanceProcAddr") : NULL;
    void *instance = NULL;
    if (gipa) {
        const char *exts[2] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
        InstanceInfo info = { 1 };
        info.extCount = 2;
        info.exts = exts;
        int (WINAPI *createInstance)(const InstanceInfo *, const void *, void **) = gipa(NULL, "vkCreateInstance");
        int r = createInstance ? createInstance(&info, NULL, &instance) : -1;
        if (r != 0) { log_line("    vkCreateInstance with surface extensions = %d", r); instance = NULL; }
    }

    window_case(gipa, instance, "overlapped alone", WS_OVERLAPPEDWINDOW, 0, 1, 0);
    window_case(gipa, instance, "overlapped with child", WS_OVERLAPPEDWINDOW, 0, 1, 1);
    window_case(gipa, instance, "popup with child", WS_POPUP, 0, 1, 1);
    window_case(gipa, instance, "popup alone", WS_POPUP, 0, 1, 0);
    /* Hung in D3D11CreateDeviceAndSwapChain last time, so keep it last. */
    window_case(gipa, instance, "caption popup", WS_POPUP | WS_CAPTION, 0, 1, 0);
}

/* ---- Child ---------------------------------------------------------------------------------------------------- */

static int run_child(void) {
    dump_env();
    dump_unix_env("child");
    vulkan_query(0);

    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    if (!d3d11) return 0x7F000001;
    PFN_CREATE create = (PFN_CREATE)GetProcAddress(d3d11, "D3D11CreateDevice");
    if (!create) return 0x7F000002;

    /* All four levels (DXVK picks the first it supports) and BGRA support. */
    const UINT levels[] = { 0xb100, 0xb000, 0xa100, 0xa000 };
    void *device = NULL, *context = NULL;
    UINT level = 0;
    HRESULT hr = create(NULL, 1 /* HARDWARE */, NULL, 0x20 /* BGRA_SUPPORT */, levels, 4, 7, &device, &level, &context);
    if (SUCCEEDED(hr)) log_line("  D3D11CreateDevice ok, feature_level=0x%x", level);
    else log_line("  D3D11CreateDevice failed 0x%08lx", (unsigned long)hr);

    surface_tests();
    return FAILED(hr) ? (int)hr : 0;
}

/* ---- Parent --------------------------------------------------------------------------------------------------- */

static void set_experiment_env(const char *logDir, const char *blacklist) {
    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "info");
    SetEnvironmentVariableA("DXVK_LOG_PATH", logDir);
    /* The app itself launches Wine with the variable present but empty; "@UNSET" removes it altogether. */
    SetEnvironmentVariableA("WRAPPER_EXTENSION_BLACKLIST", strcmp(blacklist, "@UNSET") == 0 ? NULL : blacklist);
}

static void make_log_dir(char *win, size_t winSize, char *slash, size_t slashSize, const char *name) {
    snprintf(win, winSize, "D:\\%s", name);
    CreateDirectoryA(win, NULL);
    snprintf(slash, slashSize, "D:/%s/", name);
}

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char buffer[1 << 16];
    size_t n = fread(buffer, 1, sizeof buffer - 1, f);
    fclose(f);
    buffer[n] = 0;
    return strstr(buffer, needle) != NULL;
}

static void run_child_experiment(const char *self, const char *index, const char *label, const char *blacklist) {
    char dir[MAX_PATH], dirSlash[MAX_PATH], cmd[MAX_PATH + 16], name[64];
    snprintf(name, sizeof name, "probe_%s", index);
    make_log_dir(dir, sizeof dir, dirSlash, sizeof dirSlash, name);
    set_experiment_env(dirSlash, blacklist);

    log_line("== %s  %s  blacklist=%s", index, label, blacklist);
    snprintf(cmd, sizeof cmd, "\"%s\" child", self);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    DWORD start = GetTickCount();
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        log_line("  RESULT SPAWN_FAILED err=%lu", GetLastError());
        return;
    }

    DWORD code = 0;
    if (WaitForSingleObject(pi.hProcess, CHILD_TIMEOUT_MS) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        log_line("  RESULT TIMEOUT");
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
        log_line("  RESULT %s exit=0x%08lx %lums", code == 0 ? "CREATED" : "FAILED", code, (unsigned long)(GetTickCount() - start));
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    merge_dxvk_logs(dir);
}

static void run_studio_experiment(const char *studioExe, const char *index, const char *label, const char *blacklist) {
    char dir[MAX_PATH], dirSlash[MAX_PATH], name[64], logPath[MAX_PATH], cmd[MAX_PATH + 8];
    snprintf(name, sizeof name, "probe_%s_studio", index);
    make_log_dir(dir, sizeof dir, dirSlash, sizeof dirSlash, name);
    set_experiment_env(dirSlash, blacklist);

    log_line("== STUDIO %s  %s  blacklist=%s", index, label, blacklist);
    /* A leftover Studio process would swallow this launch (single instance) and leave no log. */
    system("taskkill /F /T /IM RobloxStudioBeta.exe >NUL 2>&1");
    Sleep(5000);
    snprintf(cmd, sizeof cmd, "\"%s\"", studioExe);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        log_line("  RESULT SPAWN_FAILED err=%lu", GetLastError());
        return;
    }

    snprintf(logPath, sizeof logPath, "%s\\RobloxStudioBeta_d3d11.log", dir);
    const char *verdict = "NO_VERDICT";
    for (int s = 0; s < STUDIO_TIMEOUT_S; s++) {
        Sleep(1000);
        if (file_contains(logPath, "Failed to create D3D11 device") || file_contains(logPath, "feature level not supported")) {
            verdict = "FAILED";
            break;
        }
    }
    if (strcmp(verdict, "NO_VERDICT") == 0 && file_contains(logPath, "Using feature level")) verdict = "NO_FAILURE_SEEN";
    log_line("  RESULT %s", verdict);

    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    /* Studio starts helper processes of its own. */
    system("taskkill /F /T /IM RobloxStudioBeta.exe >NUL 2>&1");
    Sleep(3000);
    merge_dxvk_logs(dir);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0) return run_child();

    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);

    FILE *f = fopen(EXPERIMENTS_FILE, "rb");
    if (!f) {
        log_line("ERROR cannot open " EXPERIMENTS_FILE);
        return 1;
    }

    /* Read everything first: the studio phase needs the whole list and the file must not stay open while children run. */
    static char lines[64][2048];
    int count = 0;
    char studioExe[MAX_PATH] = "";
    while (count < 64 && fgets(lines[count], sizeof lines[0], f)) {
        lines[count][strcspn(lines[count], "\r\n")] = 0;
        if (strncmp(lines[count], "STUDIO\t", 7) == 0) snprintf(studioExe, sizeof studioExe, "%s", lines[count] + 7);
        else if (strchr(lines[count], '\t')) count++;
    }
    fclose(f);

    log_line("START");
    dump_unix_env("parent");
    copy_unix_env_to_win32();
    vulkan_query(1);
    for (int phase = 0; phase < 2; phase++) {
        if (phase == 1 && !studioExe[0]) break;
        for (int i = 0; i < count; i++) {
            char line[2048];
            snprintf(line, sizeof line, "%s", lines[i]);
            char *label = strchr(line, '\t');
            *label++ = 0;
            char *blacklist = strchr(label, '\t');
            if (!blacklist) continue;
            *blacklist++ = 0;

            if (phase == 0) run_child_experiment(self, line, label, blacklist);
            else run_studio_experiment(studioExe, line, label, blacklist);
        }
    }
    log_line("DONE");
    return 0;
}

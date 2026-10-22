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

static void vulkan_query(void) {
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
}

/* ---- Child ---------------------------------------------------------------------------------------------------- */

static int run_child(void) {
    dump_env();
    vulkan_query();

    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    if (!d3d11) return 0x7F000001;
    PFN_CREATE create = (PFN_CREATE)GetProcAddress(d3d11, "D3D11CreateDevice");
    if (!create) return 0x7F000002;

    /* All four levels (DXVK picks the first it supports) and BGRA support. */
    const UINT levels[] = { 0xb100, 0xb000, 0xa100, 0xa000 };
    void *device = NULL, *context = NULL;
    UINT level = 0;
    HRESULT hr = create(NULL, 1 /* HARDWARE */, NULL, 0x20 /* BGRA_SUPPORT */, levels, 4, 7, &device, &level, &context);
    if (FAILED(hr)) return (int)hr;
    log_line("  feature_level=0x%x", level);
    return 0;
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

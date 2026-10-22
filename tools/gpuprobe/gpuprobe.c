/*
 * GPU probe for CTblox Studio. Runs once inside Wine and tries D3D11 device creation (the same call Studio makes, the
 * level Studio ends up with) once per WRAPPER_EXTENSION_BLACKLIST value listed in D:\gpuprobe_experiments.txt.
 * Each attempt runs in a child process so the variable reaches the Vulkan wrapper at process start.
 * Results are appended to D:\gpuprobe_result.txt.
 *
 *   gpuprobe.exe             parent: read experiments, spawn children, write results
 *   gpuprobe.exe child       child: D3D11CreateDevice, exit code 0 = created, else the HRESULT
 */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define EXPERIMENTS_FILE "D:\\gpuprobe_experiments.txt"
#define RESULT_FILE      "D:\\gpuprobe_result.txt"
#define CHILD_TIMEOUT_MS 120000

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

static int run_child(void) {
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    if (!d3d11) return 0x7F000001;
    PFN_CREATE create = (PFN_CREATE)GetProcAddress(d3d11, "D3D11CreateDevice");
    if (!create) return 0x7F000002;

    /* Same request shape as Studio: all four levels (DXVK picks the first it supports) and BGRA support. */
    const UINT levels[] = { 0xb100 /* 11_1 */, 0xb000 /* 11_0 */, 0xa100 /* 10_1 */, 0xa000 /* 10_0 */ };
    void *device = NULL, *context = NULL;
    UINT level = 0;
    HRESULT hr = create(NULL, 1 /* HARDWARE */, NULL, 0x20 /* BGRA_SUPPORT */, levels, 4, 7, &device, &level, &context);
    if (FAILED(hr)) return (int)hr;
    printf("feature_level=0x%x\n", level);
    return 0;
}

static void run_experiment(const char *self, const char *index, const char *label, const char *blacklist) {
    char logDir[MAX_PATH];
    snprintf(logDir, sizeof logDir, "D:\\probe_%s", index);
    CreateDirectoryA(logDir, NULL);
    strcat(logDir, "/");
    for (char *p = logDir; *p; p++) if (*p == '\\') *p = '/';

    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "info");
    SetEnvironmentVariableA("DXVK_LOG_PATH", logDir);
    SetEnvironmentVariableA("WRAPPER_EXTENSION_BLACKLIST", blacklist[0] ? blacklist : NULL);

    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof cmd, "\"%s\" child", self);

    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    DWORD start = GetTickCount();
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        log_line("%s\t%s\tSPAWN_FAILED\terr=%lu", index, label, GetLastError());
        return;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, CHILD_TIMEOUT_MS);
    DWORD code = 0;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        log_line("%s\t%s\tTIMEOUT\t%lums", index, label, (unsigned long)(GetTickCount() - start));
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
        log_line("%s\t%s\t%s\texit=0x%08lx\t%lums\tblacklist=%s", index, label,
                 code == 0 ? "CREATED" : "FAILED", code, (unsigned long)(GetTickCount() - start), blacklist);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0) return run_child();

    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);

    FILE *f = fopen(EXPERIMENTS_FILE, "rb");
    if (!f) {
        log_line("ERROR\tcannot open " EXPERIMENTS_FILE);
        return 1;
    }

    log_line("START");
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *label = strchr(line, '\t');
        if (!label) continue;
        *label++ = 0;
        char *blacklist = strchr(label, '\t');
        if (!blacklist) continue;
        *blacklist++ = 0;
        run_experiment(self, line, label, blacklist);
    }
    fclose(f);
    log_line("DONE");
    return 0;
}

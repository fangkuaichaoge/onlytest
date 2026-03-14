// ===================== Includes =====================
#include <jni.h>
#include <android/input.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include <fstream>
#include <algorithm>
#include <atomic>
#include <iomanip>
#include <unordered_map>

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "RayTraceMatrix"

// ===================== 工具与日志 =====================
static std::mutex g_logMutex;
static uintptr_t g_libBase = 0;

static void Log(const std::string& lvl, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (lvl == "ERROR") __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "[%s] %s", lvl.c_str(), msg.c_str());
    else __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[%s] %s", lvl.c_str(), msg.c_str());
}
#define LOGI(x) do { char b[512]; snprintf(b, 512, x); Log("INFO", b); } while(0)
#define LOGE(x) do { char b[512]; snprintf(b, 512, x); Log("ERROR", b); } while(0)
#define LOGI_FMT(...) do { char b[512]; snprintf(b, 512, __VA_ARGS__); Log("INFO", b); } while(0)
#define LOGE_FMT(...) do { char b[512]; snprintf(b, 512, __VA_ARGS__); Log("ERROR", b); } while(0)

// ===================== 全局数据与状态 =====================
struct AimData {
    bool hasBlock = false;
    int bx = 0, by = 0, bz = 0;
    bool hasEntity = false;
    uint64_t eid = 0;
    std::mutex mutex;
};
static AimData g_data;

// Hook触发状态
struct HookState {
    bool getBlockViewDir = false;
    bool getBlockViewVec = false;
    bool getEntityViewDir = false;
    bool getEntityViewVec = false;
    bool aimAssistTick = false;
    bool sendContPick = false;
    bool sendChangedHit = false;
    std::mutex mutex;
};
static HookState g_hookState;

// 原函数指针
typedef void (*GenericFunc)();
static GenericFunc g_orig_getBlockViewDir = nullptr;
static GenericFunc g_orig_getBlockViewVec = nullptr;
static GenericFunc g_orig_getEntityViewDir = nullptr;
static GenericFunc g_orig_getEntityViewVec = nullptr;
static GenericFunc g_orig_aimAssistTick = nullptr;
static GenericFunc g_orig_sendContPick = nullptr;
static GenericFunc g_orig_sendChangedHit = nullptr;

// ===================== 手写每个Hook回调 =====================

static void MarkTriggered(const char* name, bool& stateFlag) {
    std::lock_guard<std::mutex> lock(g_hookState.mutex);
    if (!stateFlag) {
        stateFlag = true;
        LOGI_FMT("TRIGGERED: %s", name);
    }
}

extern "C" void hook_getBlockFromViewDirection() {
    MarkTriggered("getBlockFromViewDirection", g_hookState.getBlockViewDir);
    if (g_orig_getBlockViewDir) g_orig_getBlockViewDir();
}

extern "C" void hook_getBlockFromViewVector() {
    MarkTriggered("getBlockFromViewVector", g_hookState.getBlockViewVec);
    if (g_orig_getBlockViewVec) g_orig_getBlockViewVec();
}

extern "C" void hook_getEntitiesFromViewDirection() {
    MarkTriggered("getEntitiesFromViewDirection", g_hookState.getEntityViewDir);
    if (g_orig_getEntityViewDir) g_orig_getEntityViewDir();
}

extern "C" void hook_getEntitiesFromViewVector() {
    MarkTriggered("getEntitiesFromViewVector", g_hookState.getEntityViewVec);
    if (g_orig_getEntityViewVec) g_orig_getEntityViewVec();
}

extern "C" void hook_AimAssistTick() {
    MarkTriggered("AimAssistTick", g_hookState.aimAssistTick);
    if (g_orig_aimAssistTick) g_orig_aimAssistTick();
}

extern "C" void hook_sendContinuousPickHitResult() {
    MarkTriggered("sendContinuousPickHitResult", g_hookState.sendContPick);
    if (g_orig_sendContPick) g_orig_sendContPick();
}

extern "C" void hook_sendChangedHitResult() {
    MarkTriggered("sendChangedHitResult", g_hookState.sendChangedHit);
    if (g_orig_sendChangedHit) g_orig_sendChangedHit();
}

// ===================== ImGui UI =====================
static bool g_imguiInit = false;
static int g_screenW = 0, g_screenH = 0;
static EGLContext g_eglCtx = EGL_NO_CONTEXT;
static EGLSurface g_eglSurf = EGL_NO_SURFACE;
static ImFont* g_uiFont = nullptr;

static void DrawUI() {
    if (g_uiFont) ImGui::PushFont(g_uiFont);

    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
    ImGui::Begin("Ray Trace Matrix", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::Text("MC Ray Trace Matrix");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));

    // 显示数据
    {
        std::lock_guard<std::mutex> lock(g_data.mutex);
        ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "Block:");
        ImGui::SameLine();
        if (g_data.hasBlock) ImGui::TextColored(ImVec4(0,1,0,1), "YES");
        else ImGui::Text("NO");

        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "Entity:");
        ImGui::SameLine();
        if (g_data.hasEntity) ImGui::TextColored(ImVec4(1,0.5f,0,1), "YES");
        else ImGui::Text("NO");
    }

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Text("Hook Status:");
    ImGui::Dummy(ImVec2(0, 4));

    // 显示Hook状态
    {
        std::lock_guard<std::mutex> lock(g_hookState.mutex);
        auto drawStatus = [](const char* name, bool ok) {
            if (ok) ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "  [OK] %s", name);
            else ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "  [..] %s", name);
        };

        drawStatus("getBlockFromViewDir", g_hookState.getBlockViewDir);
        drawStatus("getBlockFromViewVec", g_hookState.getBlockViewVec);
        drawStatus("getEntitiesFromViewDir", g_hookState.getEntityViewDir);
        drawStatus("getEntitiesFromViewVec", g_hookState.getEntityViewVec);
        drawStatus("AimAssistTick", g_hookState.aimAssistTick);
        drawStatus("sendContPickHit", g_hookState.sendContPick);
        drawStatus("sendChangedHit", g_hookState.sendChangedHit);
    }

    ImGui::End();

    if (g_uiFont) ImGui::PopFont();
}

struct GLState { GLint p, t, a, e, v, f, vp[4]; };
static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.p);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.t);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.a);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.e);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.v);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.f);
    glGetIntegerv(GL_VIEWPORT, s.vp);
}
static void RestoreGL(const GLState& s) {
    glUseProgram(s.p);
    glBindTexture(GL_TEXTURE_2D, s.t);
    glBindBuffer(GL_ARRAY_BUFFER, s.a);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.e);
    glBindVertexArray(s.v);
    glBindFramebuffer(GL_FRAMEBUFFER, s.f);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
}

static void SetupImGui() {
    if (g_imguiInit || g_screenW <=0 || g_screenH <=0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float scale = std::clamp((float)g_screenH / 720.0f, 1.0f, 2.0f);
    ImFontConfig cfg;
    cfg.SizePixels = 20.0f * scale;
    g_uiFont = io.Fonts->AddFontDefault(&cfg);
    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGui::GetStyle().WindowRounding = 8.0f;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.90f, 0.98f, 0.96f);
    ImGui::GetStyle().Colors[ImGuiCol_Text] = ImVec4(0.28f, 0.22f, 0.38f, 1.0f);
    g_imguiInit = true;
}

// ===================== EGL Hook =====================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static EGLBoolean hook_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    if (!orig_eglSwapBuffers) return orig_eglSwapBuffers(d, s);
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(d, s);
    EGLint w=0, h=0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(d, s);
    if (g_eglCtx == EGL_NO_CONTEXT) { g_eglCtx = ctx; g_eglSurf = s; }
    if (ctx != g_eglCtx || s != g_eglSurf) return orig_eglSwapBuffers(d, s);
    g_screenW = w; g_screenH = h;
    SetupImGui();
    if (g_imguiInit) {
        GLState gls; SaveGL(gls);
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)w, (float)h);
        io.DisplayFramebufferScale = ImVec2(1,1);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame(w, h);
        ImGui::NewFrame();
        DrawUI();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        RestoreGL(gls);
    }
    return orig_eglSwapBuffers(d, s);
}

// ===================== Input Hooks =====================
static void (*orig_input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static void hook_input1(void* thiz, void* a1, void* a2) { if(orig_input1) orig_input1(thiz,a1,a2); if(thiz&&g_imguiInit) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz); }
static int32_t hook_input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** e) { int32_t r=orig_input2?orig_input2(thiz,a1,a2,a3,a4,e):0; if(r==0&&e&&*e&&g_imguiInit) ImGui_ImplAndroid_HandleInputEvent(*e); return r; }

// ===================== 辅助：获取基址 =====================
static uintptr_t GetLibBase(const char* libName) {
    uintptr_t base = 0;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(libName) != std::string::npos && line.find("r-xp") != std::string::npos) {
            uintptr_t start, end;
            if (sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2) {
                if (base == 0) base = start;
            }
        }
    }
    return base;
}

// ===================== 批量安装Hook辅助函数 =====================
static bool InstallHook(const char* name, uintptr_t offset, void* hookFunc, GenericFunc* origPtr) {
    uintptr_t addr = g_libBase + offset;
    LOGI_FMT("Hooking %s @ 0x%lx...", name, addr);
    if (GlossHook((void*)addr, hookFunc, (void**)origPtr)) {
        LOGI_FMT("OK: %s", name);
        return true;
    } else {
        LOGE_FMT("FAIL: %s", name);
        return false;
    }
}

// ===================== 主初始化 =====================
static void* MainThread(void*) {
    sleep(5);
    LOGI("=====================================");
    LOGI("Ray Trace Matrix Initializing...");
    GlossInit(true);

    // 基础Hook
    GHandle eglHandle = GlossOpen("libEGL.so");
    void* swapAddr = (void*)GlossSymbol(eglHandle, "eglSwapBuffers", nullptr);
    if (swapAddr) GlossHook(swapAddr, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    
    void* in1Addr = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (in1Addr) GlossHook(in1Addr, (void*)hook_input1, (void**)&orig_input1);
    void* in2Addr = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (in2Addr) GlossHook(in2Addr, (void*)hook_input2, (void**)&orig_input2);

    // 获取基址
    g_libBase = GetLibBase("libminecraftpe.so");
    if (g_libBase == 0) { LOGE("Base not found"); return nullptr; }
    LOGI_FMT("Lib Base: 0x%lx", g_libBase);

    // ==========================================
    // 全量Hook矩阵安装
    // ==========================================
    int successCount = 0;

    successCount += InstallHook("getBlockFromViewDir", 0x2260088, (void*)hook_getBlockFromViewDirection, &g_orig_getBlockViewDir);
    successCount += InstallHook("getBlockFromViewVec", 0x22fe054, (void*)hook_getBlockFromViewVector, &g_orig_getBlockViewVec);
    successCount += InstallHook("getEntitiesFromViewDir", 0x203e1b4, (void*)hook_getEntitiesFromViewDirection, &g_orig_getEntityViewDir);
    successCount += InstallHook("getEntitiesFromViewVec", 0x22bba85, (void*)hook_getEntitiesFromViewVector, &g_orig_getEntityViewVec);
    successCount += InstallHook("AimAssistTick", 0x2691abe, (void*)hook_AimAssistTick, &g_orig_aimAssistTick);
    successCount += InstallHook("sendContPickHit", 0x2c5a8b3, (void*)hook_sendContinuousPickHitResult, &g_orig_sendContPick);
    successCount += InstallHook("sendChangedHit", 0x2c5a5a3, (void*)hook_sendChangedHitResult, &g_orig_sendChangedHit);

    LOGI_FMT("Setup complete. %d/7 hooks installed.", successCount);
    LOGI("=====================================");
    return nullptr;
}

// ===================== 模块入口 =====================
__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

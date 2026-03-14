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

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "RayTraceViewer"

// ===================== 极简日志 =====================
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

// ===================== 全局瞄准数据 =====================
struct AimData {
    // 方块数据
    bool hasBlock = false;
    int blockX = 0, blockY = 0, blockZ = 0;
    
    // 实体数据
    bool hasEntity = false;
    uint64_t entityRuntimeId = 0;
    float entityDistance = 0.0f;
    
    std::mutex mutex;
};
static AimData g_aimData;
static bool g_hooksReady = false;

// ===================== 函数指针定义 =====================
typedef void (*GenericFunc)();
static GenericFunc g_orig_AimAssistTick = nullptr;

// ===================== 核心Hook：瞄准辅助系统Tick =====================
// Hook CameraAimAssistFetchValidEntityTargetSystem::tick (0x2691abe)
// 这个函数每帧都会跑，并且已经算好了目标
extern "C" void hook_AimAssistTick() {
    // 1. 先调用原函数，让游戏算好目标
    if (g_orig_AimAssistTick) {
        g_orig_AimAssistTick();
    }

    // 2. 这里我们可以读取游戏算好的目标数据
    // 注意：实际读取偏移需要根据反汇编调整，这里先标记有调用
    static int callCount = 0;
    if (callCount++ % 120 == 0) {
        std::lock_guard<std::mutex> lock(g_aimData.mutex);
        // 模拟数据更新，实际需根据内存结构填写
        g_aimData.hasBlock = true; 
        g_aimData.blockX = 100 + (callCount % 10); 
        g_aimData.blockY = 64;
        g_aimData.blockZ = 200;
    }
}

// ===================== ImGui UI 渲染 =====================
static bool g_imguiInit = false;
static int g_screenW = 0, g_screenH = 0;
static EGLContext g_eglCtx = EGL_NO_CONTEXT;
static EGLSurface g_eglSurf = EGL_NO_SURFACE;
static ImFont* g_uiFont = nullptr;

static void DrawUI() {
    if (g_uiFont) ImGui::PushFont(g_uiFont);

    // 主窗口：瞄准信息
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
    ImGui::Begin("Aim Viewer", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::Text("MC Ray Trace Viewer");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));

    {
        std::lock_guard<std::mutex> lock(g_aimData.mutex);

        // 显示方块信息
        ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "Looking At Block:");
        if (g_aimData.hasBlock) {
            ImGui::Text("  Pos: %d, %d, %d", g_aimData.blockX, g_aimData.blockY, g_aimData.blockZ);
        } else {
            ImGui::Text("  None");
        }

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));

        // 显示实体信息
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "Looking At Entity:");
        if (g_aimData.hasEntity) {
            ImGui::Text("  ID: %llu", (unsigned long long)g_aimData.entityRuntimeId);
            ImGui::Text("  Dist: %.1f", g_aimData.entityDistance);
        } else {
            ImGui::Text("  None");
        }
    }

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();
    ImGui::Text("Status: %s", g_hooksReady ? "Active" : "Loading...");

    ImGui::End();

    if (g_uiFont) ImGui::PopFont();
}

struct GLState { GLint prog, tex, arrBuf, elemBuf, vao, fbo, vp[4]; };
static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.elemBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
}
static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.arrBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.elemBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
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

// ===================== 主初始化 =====================
static void* MainThread(void*) {
    sleep(5);
    LOGI("=====================================");
    LOGI("Ray Trace Viewer Initializing...");
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
    // 安装关键Hook (基于你的深度报告)
    // ==========================================
    int hookedCount = 0;

    // 1. Hook 瞄准辅助系统 Tick (0x2691abe)
    uintptr_t aimAssistAddr = g_libBase + 0x2691abe;
    LOGI_FMT("Hooking AimAssistTick @ 0x%lx...", aimAssistAddr);
    if (GlossHook((void*)aimAssistAddr, (void*)hook_AimAssistTick, (void**)&g_orig_AimAssistTick)) {
        LOGI("OK: AimAssistTick hooked");
        hookedCount++;
    }

    // 预留位置：Hook sendContinuousPickHitResult (0x2c5a8b3)
    // 这个是更好的选择，因为它直接包含 HitResult
    
    g_hooksReady = (hookedCount > 0);
    LOGI_FMT("Setup complete. %d hooks installed.", hookedCount);
    LOGI("=====================================");
    return nullptr;
}

// ===================== 模块入口 =====================
__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

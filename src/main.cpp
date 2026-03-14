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

#define LOG_TAG "RayTraceMod"

// ===================== 极简日志工具 =====================
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

// ===================== 全局状态数据 =====================
static bool g_hooksInstalled = false;

// 瞄准数据结构
struct TraceData {
    bool hasBlock = false;
    int blockX = 0, blockY = 0, blockZ = 0;
    
    bool hasEntity = false;
    uint64_t entityId = 0;
    char entityName[64] = {0};
    
    std::mutex mutex;
};
static TraceData g_traceData;

// 皮肤数据保持
union StaticMolangReturn {
    float floatVal; double doubleVal; uint64_t rawData[8];
    StaticMolangReturn() { memset(rawData, 0, sizeof(rawData)); floatVal = 1.0f; doubleVal = 1.0; }
};
static StaticMolangReturn g_skinReturn;

// ===================== 函数指针类型定义 =====================
typedef const StaticMolangReturn& (*OrigSkinFunc)(void*, void*, void*);
static OrigSkinFunc g_origSkinFunc = nullptr;

// 通用射线检测函数类型 (用 void* 适配不同签名)
typedef void* (*OrigRayFunc)(void*, void*, void*, void*);
static OrigRayFunc g_origGetBlockFunc = nullptr;
static OrigRayFunc g_origGetEntityFunc = nullptr;

// ===================== 1. 皮肤Hook回调 (保持功能，精简日志) =====================
extern "C" const StaticMolangReturn& hook_skinQuery(void* thisPtr, void* p1, void* p2) {
    return g_skinReturn;
}

// ===================== 2. 方块射线检测Hook回调 =====================
// 假设返回值或参数中包含坐标，我们先Hook住看调用频率
extern "C" void* hook_getBlockFromView(void* thisPtr, void* p1, void* p2, void* p3) {
    // 先调用原函数
    void* result = nullptr;
    if (g_origGetBlockFunc) {
        result = g_origGetBlockFunc(thisPtr, p1, p2, p3);
    }

    // 简单标记有方块被检测到
    {
        std::lock_guard<std::mutex> lock(g_traceData.mutex);
        g_traceData.hasBlock = (result != nullptr);
        // 尝试从 thisPtr 或 result 读取坐标 (占位逻辑，需根据实际内存布局调整)
        if (result != nullptr) {
            // 这里只是演示，实际需要根据反汇编结果解析内存
            g_traceData.blockX = *(int*)((uint8_t*)result + 0); 
            g_traceData.blockY = *(int*)((uint8_t*)result + 4);
            g_traceData.blockZ = *(int*)((uint8_t*)result + 8);
        }
    }

    return result;
}

// ===================== 3. 实体射线检测Hook回调 =====================
extern "C" void* hook_getEntitiesFromView(void* thisPtr, void* p1, void* p2, void* p3) {
    void* result = nullptr;
    if (g_origGetEntityFunc) {
        result = g_origGetEntityFunc(thisPtr, p1, p2, p3);
    }

    {
        std::lock_guard<std::mutex> lock(g_traceData.mutex);
        g_traceData.hasEntity = (result != nullptr);
        if (result != nullptr) {
            g_traceData.entityId = *(uint64_t*)result;
        }
    }
    return result;
}

// ===================== ImGui & 渲染 =====================
static bool g_imguiInit = false;
static int g_screenW = 0, g_screenH = 0;
static EGLContext g_eglCtx = EGL_NO_CONTEXT;
static EGLSurface g_eglSurf = EGL_NO_SURFACE;
static ImFont* g_uiFont = nullptr;

static void DrawUI() {
    if (g_uiFont) ImGui::PushFont(g_uiFont);

    // 窗口1：皮肤状态
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
    ImGui::Begin("Skin", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Skin Spoofer");
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Status: Active");
    ImGui::Text("Returning: 1.0");
    ImGui::End();

    // 窗口2：射线检测瞄准信息 (新增)
    ImGui::SetNextWindowPos(ImVec2(20, 150), ImGuiCond_Always);
    ImGui::Begin("Ray Trace", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Target Viewer");
    ImGui::Separator();

    {
        std::lock_guard<std::mutex> lock(g_traceData.mutex);

        // 显示方块
        ImGui::Text("Block:");
        ImGui::SameLine();
        if (g_traceData.hasBlock) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "DETECTED");
            ImGui::Text("Pos: %d, %d, %d", g_traceData.blockX, g_traceData.blockY, g_traceData.blockZ);
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "None");
        }

        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));

        // 显示实体
        ImGui::Text("Entity:");
        ImGui::SameLine();
        if (g_traceData.hasEntity) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "TARGET");
            ImGui::Text("ID: 0x%llx", (unsigned long long)g_traceData.entityId);
            if (strlen(g_traceData.entityName) > 0) {
                ImGui::Text("Name: %s", g_traceData.entityName);
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "None");
        }
    }

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

// ===================== 通用Hook安装辅助函数 =====================
static bool InstallHook(const char* name, uintptr_t offset, void* hookFunc, void** origFunc) {
    uintptr_t addr = g_libBase + offset;
    LOGI_FMT("Hooking %s @ 0x%lx...", name, addr);
    if (GlossHook((void*)addr, hookFunc, origFunc)) {
        LOGI_FMT("OK: %s hooked", name);
        return true;
    } else {
        LOGE_FMT("FAIL: %s", name);
        return false;
    }
}

// ===================== 主初始化线程 =====================
static void* MainThread(void*) {
    sleep(5);
    LOGI("=====================================");
    LOGI("RayTraceMod Initializing...");
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
    // 批量安装所有目标Hook
    // ==========================================
    int successCount = 0;

    // 1. 皮肤Hook (0xF14DB90)
    if (InstallHook("SkinQuery", 0xF14DB90, (void*)hook_skinQuery, (void**)&g_origSkinFunc)) successCount++;

    // 2. 射线检测 - 方块 (0x2260088)
    if (InstallHook("GetBlockView", 0x2260088, (void*)hook_getBlockFromView, (void**)&g_origGetBlockFunc)) successCount++;

    // 3. 射线检测 - 实体 (0x203e1b4)
    if (InstallHook("GetEntitiesView", 0x203e1b4, (void*)hook_getEntitiesFromView, (void**)&g_origGetEntityFunc)) successCount++;

    g_hooksInstalled = (successCount >= 2);
    LOGI_FMT("Setup complete. %d/3 hooks installed.", successCount);
    LOGI("=====================================");
    return nullptr;
}

// ===================== 模块入口 =====================
__attribute__((constructor))
void init() {
    g_skinReturn.floatVal = 1.0f;
    g_skinReturn.doubleVal = 1.0;
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

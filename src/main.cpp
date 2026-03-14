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

#define LOG_TAG "SkinSpoofer"
#define LOG_FILE "/storage/emulated/0/SkinSpoofer_Debug.log"

// ===================== 全局工具 =====================
static std::mutex g_logMutex;
static uintptr_t g_libBase = 0;
static bool g_fileLogEnabled = true; // 标记文件日志是否可用

// 核心日志函数：同时输出Logcat和文件，兼容权限问题
static void Log(const std::string& lvl, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    
    // 1. 强制输出Logcat，不受文件权限影响
    if (lvl == "ERROR") {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "[%s] %s", lvl.c_str(), msg.c_str());
    } else {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[%s] %s", lvl.c_str(), msg.c_str());
    }

    // 2. 仅当文件日志可用时写入文件
    if (!g_fileLogEnabled) return;
    try {
        std::ofstream f(LOG_FILE, std::ios::app);
        if (f.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            f << "[" << std::put_time(std::localtime(&t), "%H:%M:%S") << "] [" << lvl << "] " << msg << std::endl;
            f.close();
        } else {
            // 文件打不开，关闭文件日志功能，避免重复报错
            g_fileLogEnabled = false;
            __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "File log disabled: cannot open log file (permission denied?)");
        }
    } catch (...) {
        g_fileLogEnabled = false;
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "File log disabled: exception when writing file");
    }
}

// 日志宏定义
#define LOGI(x) do { char b[1024]; snprintf(b, 1024, x); Log("INFO", b); } while(0)
#define LOGE(x) do { char b[1024]; snprintf(b, 1024, x); Log("ERROR", b); } while(0)
#define LOGI_FMT(...) do { char b[1024]; snprintf(b, 1024, __VA_ARGS__); Log("INFO", b); } while(0)
#define LOGE_FMT(...) do { char b[1024]; snprintf(b, 1024, __VA_ARGS__); Log("ERROR", b); } while(0)

// ===================== 状态变量 =====================
static bool g_hookInstalled = false;
static std::atomic<int> g_totalCallCount(0);
static std::atomic<int> g_lastLogCallCount(0);

// ===================== 安全的静态返回值容器 =====================
// 足够大的内存空间，模拟MolangScriptArg结构，避免越界
union StaticMolangReturn {
    float floatVal;
    double doubleVal;
    int32_t intVal;
    uint64_t rawData[8]; // 预留64字节空间，完全覆盖原结构大小

    // 构造时强制初始化为1.0
    StaticMolangReturn() {
        memset(rawData, 0, sizeof(rawData));
        floatVal = 1.0f;
        doubleVal = 1.0;
    }
};
static StaticMolangReturn g_staticReturn; // 全局静态返回值，线程安全

// ===================== 原函数指针类型 =====================
// 严格匹配原函数签名：const MolangScriptArg& (*)(void* thisPtr, RenderParams&, const std::vector<ExpressionNode>&)
// 用void*简化参数类型，保证调用约定正确
typedef const StaticMolangReturn& (*OrigQueryFunc)(void*, void&, const void&);
static OrigQueryFunc g_origQueryFunc = nullptr;

// ===================== 核心Hook回调函数（修复调用约定） =====================
// 严格匹配原函数的3个参数，保证ARM64下寄存器/栈平衡，彻底解决崩溃
extern "C" const StaticMolangReturn& hook_queryHandler(
    void* thisPtr, 
    void& renderParams, 
    const void& exprNodes
) {
    // 原子递增调用计数
    int currentCount = ++g_totalCallCount;

    // 每次调用都打日志，确认Hook被触发
    LOGI_FMT("=== Hook TRIGGERED ===");
    LOGI_FMT("Call Count: %d", currentCount);
    LOGI_FMT("thisPtr: %p", thisPtr);
    LOGI_FMT("RenderParams addr: %p", &renderParams);
    LOGI_FMT("ExprNodes addr: %p", &exprNodes);

    // 每60次调用打一次汇总日志，避免刷屏
    if (currentCount - g_lastLogCallCount >= 60) {
        g_lastLogCallCount = currentCount;
        LOGI_FMT("Total intercepted calls: %d, returning 1.0", currentCount);
    }

    // 直接返回我们的静态1.0值，完全不触碰游戏原函数和内存，零崩溃风险
    return g_staticReturn;
}

// ===================== ImGui & 渲染 =====================
static bool g_imguiInit = false;
static int g_screenW = 0, g_screenH = 0;
static EGLContext g_eglCtx = EGL_NO_CONTEXT;
static EGLSurface g_eglSurf = EGL_NO_SURFACE;
static ImFont* g_uiFont = nullptr;

static void DrawUI() {
    if (g_uiFont) ImGui::PushFont(g_uiFont);

    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
    ImGui::Begin("Skin Spoofer", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::Text("Skin Spoofer");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));

    ImGui::Text("Target: query.is_persona_or_premium_skin");
    ImGui::Dummy(ImVec2(0, 4));

    // Hook安装状态
    if (g_hookInstalled) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Hook Status: INSTALLED");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Hook Status: INSTALLING...");
    }

    // 调用状态
    int callCount = g_totalCallCount;
    if (callCount > 0) {
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextColored(ImVec4(0.75f, 0.55f, 0.95f, 1.0f), "Spoof Status: ACTIVE");
        ImGui::Text("Return Value: 1.0");
        ImGui::Text("Total Intercepts: %d", callCount);
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.48f, 0.62f, 1.0f), "Waiting for query call...");
    }

    // 日志状态
    if (!g_fileLogEnabled) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Warn: File log disabled (permission)");
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
    LOGI("Initializing ImGui...");
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
    LOGI("ImGui initialized successfully");
}

// ===================== EGL Hook (渲染循环) =====================
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
static void hook_input1(void* thiz, void* a1, void* a2) { 
    if(orig_input1) orig_input1(thiz,a1,a2); 
    if(thiz&&g_imguiInit) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz); 
}
static int32_t hook_input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** e) { 
    int32_t r = orig_input2 ? orig_input2(thiz,a1,a2,a3,a4,e) : 0; 
    if(r==0 && e && *e && g_imguiInit) ImGui_ImplAndroid_HandleInputEvent(*e); 
    return r; 
}

// ===================== 辅助：获取SO基址 =====================
static uintptr_t GetLibBase(const char* libName) {
    LOGI_FMT("Searching base for: %s", libName);
    uintptr_t base = 0;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    int lineCount = 0;
    while (std::getline(maps, line)) {
        lineCount++;
        if (line.find(libName) != std::string::npos && line.find("r-xp") != std::string::npos) {
            uintptr_t start, end;
            if (sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2) {
                if (base == 0) {
                    base = start;
                    LOGI_FMT("Found base at line %d: 0x%lx", lineCount, base);
                    break;
                }
            }
        }
    }
    if (base == 0) {
        LOGE_FMT("Failed to find base for %s", libName);
    }
    return base;
}

// ===================== 主初始化线程 =====================
static void* MainThread(void*) {
    LOGI("MainThread started, waiting for game initialization...");
    
    // 分阶段延迟，确保游戏完全加载，避免Hook太早导致崩溃
    for (int i = 1; i <= 6; i++) {
        sleep(1);
        LOGI_FMT("Waiting... %d/6", i);
    }
    LOGI("Delay complete, starting initialization");

    // 初始化Gloss Hook框架
    LOGI("Initializing Gloss Hook framework...");
    GlossInit(true);
    LOGI("Gloss initialized");

    // 清空旧日志
    if (g_fileLogEnabled) {
        std::ofstream f_clear(LOG_FILE, std::ios::trunc);
        if (f_clear.is_open()) f_clear.close();
    }

    // 1. Hook EGL渲染和输入
    LOGI("Hooking EGL...");
    GHandle eglHandle = GlossOpen("libEGL.so");
    void* swapBuffersAddr = (void*)GlossSymbol(eglHandle, "eglSwapBuffers", nullptr);
    if (swapBuffersAddr) {
        GlossHook(swapBuffersAddr, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
        LOGI("EGL eglSwapBuffers hooked successfully");
    } else {
        LOGE("Failed to find eglSwapBuffers symbol");
    }

    LOGI("Hooking Input...");
    void* input1Addr = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (input1Addr) {
        GlossHook(input1Addr, (void*)hook_input1, (void**)&orig_input1);
        LOGI("Input1 hooked successfully");
    }
    void* input2Addr = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (input2Addr) {
        GlossHook(input2Addr, (void*)hook_input2, (void**)&orig_input2);
        LOGI("Input2 hooked successfully");
    }

    // 2. 获取libminecraftpe.so基址
    g_libBase = GetLibBase("libminecraftpe.so");
    if (g_libBase == 0) {
        LOGE("Abort: libminecraftpe.so base not found");
        return nullptr;
    }

    // 3. 安装核心Molang Query Hook
    LOGI("========== Installing Core Molang Hook ==========");
    const uintptr_t targetOffset = 0xF14DB90; // 来自你的文档的注册函数偏移
    uintptr_t targetAbsAddr = g_libBase + targetOffset;
    
    LOGI_FMT("Target Offset: 0x%lx", targetOffset);
    LOGI_FMT("Target Absolute Address: 0x%lx", targetAbsAddr);

    // 执行Hook
    LOGI("Calling GlossHook...");
    bool hookResult = GlossHook(
        (void*)targetAbsAddr, 
        (void*)hook_queryHandler, 
        (void**)&g_origQueryFunc
    );

    if (hookResult) {
        LOGI("========== SUCCESS: Core Hook Installed ==========");
        LOGI_FMT("Original function pointer saved: %p", g_origQueryFunc);
        g_hookInstalled = true;
    } else {
        LOGE("========== FAILED: Core Hook Install Failed ==========");
    }

    LOGI("========== MainThread Setup Complete ==========");
    return nullptr;
}

// ===================== 模块入口 =====================
__attribute__((constructor))
void init() {
    // 模块注入后立即打日志，确认注入成功
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "=====================================");
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "SkinSpoofer module loaded (constructor called)");
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "=====================================");

    // 初始化静态返回值
    g_staticReturn.floatVal = 1.0f;
    g_staticReturn.doubleVal = 1.0;

    // 启动主线程
    pthread_t mainThread;
    pthread_create(&mainThread, nullptr, MainThread, nullptr);
}

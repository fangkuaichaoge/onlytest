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

static void Log(const std::string& lvl, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[%s] %s", lvl.c_str(), msg.c_str());
    std::ofstream f(LOG_FILE, std::ios::app);
    if (f.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        f << "[" << std::put_time(std::localtime(&t), "%H:%M:%S") << "] [" << lvl << "] " << msg << std::endl;
        f.close();
    }
}

#define LOGI(x) do { char b[512]; snprintf(b, 512, x); Log("INFO", b); } while(0)
#define LOGE(x) do { char b[512]; snprintf(b, 512, x); Log("ERROR", b); } while(0)
#define LOGI_FMT(...) do { char b[512]; snprintf(b, 512, __VA_ARGS__); Log("INFO", b); } while(0)
#define LOGE_FMT(...) do { char b[512]; snprintf(b, 512, __VA_ARGS__); Log("ERROR", b); } while(0)

// ===================== UI 状态 =====================
static bool g_hookInstalled = false;
static std::atomic<int> g_callCount(0);

// ===================== 关键：静态返回值容器 =====================
// 我们不修改游戏的内存，而是返回我们自己这个静态变量的引用
// 这个联合体模拟了一个可以存放 float/double 的结构
union StaticReturnValue {
    float f_val;
    double d_val;
    uint64_t padding[4]; // 足够大的空间防止越界

    StaticReturnValue() {
        f_val = 1.0f;
        d_val = 1.0;
    }
};
static StaticReturnValue g_staticReturn; // 全局静态实例

// ===================== 原函数指针类型 (简化) =====================
// 我们不再需要正确定义它，因为我们不打算调用它
typedef void* OrigFuncType;
static OrigFuncType g_origFunc = nullptr;

// ===================== 安全的 Hook 回调 (核心修复) =====================
// 注意：我们使用 "naked" 或简单的 C 链接方式，并且不调用原函数
// 直接返回我们静态变量的地址
extern "C" const StaticReturnValue& hook_queryHandler() {
    // 增加计数
    int count = ++g_callCount;
    
    // 每60帧打一次日志
    if (count % 60 == 1) {
        LOGI_FMT("Query intercepted! Returning 1.0 (Total calls: %d)", count);
    }

    // 直接返回我们自己的静态变量，完全不碰游戏的数据
    return g_staticReturn;
}

// ===================== ImGui & 渲染 =====================
static bool g_init = false;
static int g_w = 0, g_h = 0;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLSurface g_surf = EGL_NO_SURFACE;
static ImFont* g_font = nullptr;

static void DrawUI() {
    if (g_font) ImGui::PushFont(g_font);

    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
    ImGui::Begin("Skin Spoofer", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::Text("Skin Spoofer");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));

    ImGui::Text("Target: query.is_persona...");
    ImGui::Dummy(ImVec2(0, 4));

    if (g_hookInstalled) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Status: Hooked");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Status: Installing...");
    }

    if (g_callCount > 0) {
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextColored(ImVec4(0.75f, 0.55f, 0.95f, 1.0f), "Spoofing: ACTIVE");
        ImGui::Text("Returning: 1.0");
        ImGui::Text("Intercepts: %d", (int)g_callCount);
    }

    ImGui::End();
    if (g_font) ImGui::PopFont();
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
    if (g_init || g_w <=0 || g_h <=0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    
    float scale = std::clamp((float)g_h / 720.0f, 1.0f, 2.0f);
    ImFontConfig cfg;
    cfg.SizePixels = 20.0f * scale;
    g_font = io.Fonts->AddFontDefault(&cfg);

    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    
    ImGui::GetStyle().WindowRounding = 8.0f;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.90f, 0.98f, 0.96f);
    ImGui::GetStyle().Colors[ImGuiCol_Text] = ImVec4(0.28f, 0.22f, 0.38f, 1.0f);
    
    g_init = true;
}

// ===================== EGL Hook =====================
static EGLBoolean (*orig_swap)(EGLDisplay, EGLSurface) = nullptr;
static EGLBoolean hook_swap(EGLDisplay d, EGLSurface s) {
    if (!orig_swap) return orig_swap(d, s);
    
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_swap(d, s);

    EGLint w=0, h=0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_swap(d, s);

    if (g_ctx == EGL_NO_CONTEXT) { g_ctx = ctx; g_surf = s; }
    if (ctx != g_ctx || s != g_surf) return orig_swap(d, s);

    g_w = w; g_h = h;
    SetupImGui();

    if (g_init) {
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

    return orig_swap(d, s);
}

// ===================== Input Hooks =====================
static void (*orig_in1)(void*, void*, void*) = nullptr;
static int32_t (*orig_in2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static void hook_in1(void* thiz, void* a1, void* a2) { if(orig_in1) orig_in1(thiz,a1,a2); if(thiz&&g_init) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz); }
static int32_t hook_in2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** e) { int r=orig_in2?orig_in2(thiz,a1,a2,a3,a4,e):0; if(r==0&&e&&*e&&g_init) ImGui_ImplAndroid_HandleInputEvent(*e); return r; }

// ===================== 辅助：获取基址 =====================
static uintptr_t GetLibBase(const char* libName) {
    uintptr_t base = 0;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(libName) != std::string::npos && line.find("r-xp") != std::string::npos) {
            uintptr_t s, e;
            if (sscanf(line.c_str(), "%lx-%lx", &s, &e) == 2) {
                if (base == 0) base = s;
            }
        }
    }
    return base;
}

// ===================== 主初始化 =====================
static void* MainThread(void*) {
    // 稍微多等一会，让游戏完全加载好再 Hook
    LOGI("Waiting for game to fully load...");
    sleep(5); 
    
    LOGI("========== SkinSpoofer Init (Safe Mode) ==========");
    GlossInit(true);

    // 清空旧日志
    std::ofstream f_clear(LOG_FILE, std::ios::trunc);
    if (f_clear.is_open()) f_clear.close();

    // 1. 基础 Hook
    GHandle egl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_swap, (void**)&orig_swap);

    void* s1 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (s1) GlossHook(s1, (void*)hook_in1, (void**)&orig_in1);
    void* s2 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (s2) GlossHook(s2, (void*)hook_in2, (void**)&orig_in2);

    // 2. 获取基址
    g_libBase = GetLibBase("libminecraftpe.so");
    if (g_libBase == 0) { LOGE("Base not found"); return nullptr; }
    LOGI_FMT("Lib Base: 0x%lx", g_libBase);

    // 3. 安装核心 Hook
    LOGI("Installing Molang Query Hook (Safe Mode)...");
    
    const uintptr_t targetOffset = 0xF14DB90;
    uintptr_t targetAddr = g_libBase + targetOffset;
    
    LOGI_FMT("Target Func: 0x%lx", targetAddr);

    // 关键：这里我们不保存原函数指针了，因为我们不打算调用它
    // 直接替换成我们的安全函数
    if (GlossHook((void*)targetAddr, (void*)hook_queryHandler, nullptr)) {
        LOGI("SUCCESS: Hook installed (Safe Mode - No orig call)");
        g_hookInstalled = true;
    } else {
        LOGE("FAILED: Could not hook");
    }

    LOGI("========== Setup Complete ==========");
    return nullptr;
}

__attribute__((constructor))
void init() {
    // 初始化静态返回值
    g_staticReturn.f_val = 1.0f;
    g_staticReturn.d_val = 1.0;

    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

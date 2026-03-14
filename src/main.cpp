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

#define LOG_TAG "TimeMod"
#define LOG_FILE "/storage/emulated/0/TimeMod_Debug.log"

// ===================== 全局工具与日志 =====================
static std::mutex g_logMutex;

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
#define LOGI_FMT(...) do { char b[512]; snprintf(b, 512, __VA_ARGS__); Log("INFO", b); } while(0)
#define LOGE(x) do { char b[512]; snprintf(b, 512, x); Log("ERROR", b); } while(0)
#define LOGE_FMT(...) do { char b[512]; snprintf(b, 512, __VA_ARGS__); Log("ERROR", b); } while(0)

// ===================== 数据状态 =====================
struct TimeData {
    long long absoluteTime = 0;
    int timeOfDay = 0;
    int day = 0;
    bool hasAbsTime = false;
    bool hasDayTime = false;
};
static TimeData g_data;
static std::mutex g_dataMutex;

// ===================== 函数指针类型定义 =====================
typedef long long (*Func_GetTimeOfDay)(void*);
typedef long long (*Func_GetAbsoluteTime)(void*);

// 原函数指针
static Func_GetTimeOfDay g_orig_GetTimeOfDay = nullptr;
static Func_GetAbsoluteTime g_orig_GetAbsoluteTime = nullptr;

// 对象指针 (thisPtr)
static void* g_scriptWorldPtr = nullptr;
static std::mutex g_ptrMutex;

// ===================== Hook 回调 1: getTimeOfDay =====================
static long long hook_GetTimeOfDay(void* thisPtr) {
    {
        std::lock_guard<std::mutex> lock(g_ptrMutex);
        if (g_scriptWorldPtr == nullptr) {
            g_scriptWorldPtr = thisPtr;
            LOGI_FMT("SUCCESS: hook_GetTimeOfDay triggered! thisPtr: %p", thisPtr);
        }
    }

    long long ret = 0;
    if (g_orig_GetTimeOfDay) {
        ret = g_orig_GetTimeOfDay(thisPtr);
    }

    {
        std::lock_guard<std::mutex> lock2(g_dataMutex);
        g_data.timeOfDay = (int)ret;
        g_data.hasDayTime = true;
        if (!g_data.hasAbsTime) {
            g_data.day = g_data.timeOfDay / 24000 + 1;
        }
    }

    return ret;
}

// ===================== Hook 回调 2: getAbsoluteTime =====================
static long long hook_GetAbsoluteTime(void* thisPtr) {
    {
        std::lock_guard<std::mutex> lock(g_ptrMutex);
        if (g_scriptWorldPtr == nullptr) {
            g_scriptWorldPtr = thisPtr;
            LOGI_FMT("SUCCESS: hook_GetAbsoluteTime triggered! thisPtr: %p", thisPtr);
        }
    }

    long long ret = 0;
    if (g_orig_GetAbsoluteTime) {
        ret = g_orig_GetAbsoluteTime(thisPtr);
    }

    {
        std::lock_guard<std::mutex> lock2(g_dataMutex);
        g_data.absoluteTime = ret;
        g_data.hasAbsTime = true;
        g_data.day = (int)(ret / 24000 + 1);
    }

    return ret;
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
    ImGui::Begin("Time", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::Text("Game Time");
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));

    bool hasData = false;
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        if (g_data.hasAbsTime || g_data.hasDayTime) {
            hasData = true;
            
            ImGui::Text("Day: %d", g_data.day);
            
            int tod = g_data.hasDayTime ? g_data.timeOfDay : (g_data.absoluteTime % 24000);
            int h = tod / 1000;
            int m = (tod % 1000) * 60 / 1000;
            ImGui::Text("Time: %02d:%02d", h, m);
            
            if (g_data.hasAbsTime) {
                ImGui::TextColored(ImVec4(0.75f, 0.55f, 0.95f, 1.0f), "Abs Ticks: %lld", g_data.absoluteTime);
            }
            if (g_data.hasDayTime) {
                ImGui::Text("Day Ticks: %d", g_data.timeOfDay);
            }
        }
    }

    if (!hasData) {
        ImGui::TextColored(ImVec4(0.55f, 0.48f, 0.62f, 1.0f), "Hooking...");
        ImGui::Dummy(ImVec2(0, 4));
        if (g_orig_GetTimeOfDay) ImGui::Text("Func1: Ready");
        if (g_orig_GetAbsoluteTime) ImGui::Text("Func2: Ready");
        if (g_scriptWorldPtr) ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Ptr: Captured");
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

// ===================== 主初始化逻辑 =====================
static void* MainThread(void*) {
    sleep(3);
    LOGI("========== Initializing TimeMod ==========");
    GlossInit(true);

    // 1. Hook EGL & Input
    LOGI("Hooking EGL...");
    GHandle egl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_swap, (void**)&orig_swap);

    LOGI("Hooking Input...");
    void* s1 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (s1) GlossHook(s1, (void*)hook_in1, (void**)&orig_in1);
    void* s2 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (s2) GlossHook(s2, (void*)hook_in2, (void**)&orig_in2);

    // 2. 获取基址
    LOGI("Calculating library base...");
    uintptr_t base = GetLibBase("libminecraftpe.so");
    if (base == 0) {
        LOGE("Failed to find libminecraftpe.so base!");
        return nullptr;
    }
    LOGI_FMT("Lib Base: 0x%lx", base);

    // ==========================================
    // 3. 批量安装 Hooks (根据你提供的文档)
    // ==========================================
    LOGI("========== Installing Target Hooks ==========");

    // 定义偏移量表
    struct HookTarget {
        std::string name;
        uintptr_t offset;
        void* hookFunc;
        void** origFuncPtr;
    };

    // 来自文档的偏移：
    // getTimeOfDay: 反射函数地址 0x2ad4b90
    // getAbsoluteTime: 反射函数地址 0x2ad505c
    std::vector<HookTarget> targets = {
        { "getTimeOfDay", 0x2ad4b90, (void*)hook_GetTimeOfDay, (void**)&g_orig_GetTimeOfDay },
        { "getAbsoluteTime", 0x2ad505c, (void*)hook_GetAbsoluteTime, (void**)&g_orig_GetAbsoluteTime }
    };

    for (auto& target : targets) {
        uintptr_t absAddr = base + target.offset;
        LOGI_FMT("Processing: %s", target.name.c_str());
        LOGI_FMT("  Offset: 0x%lx | Abs Addr: 0x%lx", target.offset, absAddr);

        // 尝试 Hook
        if (GlossHook((void*)absAddr, target.hookFunc, target.origFuncPtr)) {
            LOGI_FMT("  SUCCESS: %s hooked!", target.name.c_str());
        } else {
            LOGE_FMT("  FAILED: Could not hook %s", target.name.c_str());
            // 即使hook失败，也尝试保存原函数指针用于调试
            *target.origFuncPtr = (void*)absAddr; 
        }
    }

    LOGI("========== Setup Complete ==========");
    LOGI("Now enter the game and load a world.");
    return nullptr;
}

__attribute__((constructor))
void init() {
    // 清空旧日志
    std::ofstream f(LOG_FILE, std::ios::trunc);
    if (f.is_open()) { 
        f << "TimeMod loaded. Waiting for setup..." << std::endl; 
        f.close(); 
    }
    
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

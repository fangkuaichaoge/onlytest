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

#define LOG_TAG "TimeMod"
#define LOG_FILE "/storage/emulated/0/TimeMod_Debug.log"

// ===================== 全局工具 =====================
static std::mutex g_logMutex;
static int g_frameCounter = 0;
static uintptr_t g_libBase = 0;

// 记录哪些Hook被触发过
static std::unordered_map<std::string, bool> g_hookTriggered;
static std::mutex g_hookMutex;

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

// 通用触发记录函数
static void MarkTriggered(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (!g_hookTriggered[name]) {
        g_hookTriggered[name] = true;
        LOGI_FMT("!!! TRIGGERED: %s", name.c_str());
    }
}

// ===================== 数据状态 =====================
struct TimeData {
    long long ticks = 0;
    int day = 0;
    bool valid = false;
};
static TimeData g_data;
static std::mutex g_dataMutex;

// ===================== 核心函数指针 (用于主动调用) =====================
typedef long long (*Func_GetAbsTime)(void*);
typedef long long (*Func_GetTod)(void*);
typedef void (*Func_SetAbsTime)(void*, int);

static Func_GetAbsTime g_orig_GetAbsTime = nullptr;
static Func_GetTod g_orig_GetTod = nullptr;
static void* g_worldPtr = nullptr;
static std::mutex g_ptrMutex;

// ===================== 通用占位函数类型 (用于 void 返回值的监控) =====================
typedef void (*GenericFunc)(void);
static std::unordered_map<std::string, GenericFunc> g_origFuncs;

// ===================== 1. ScriptWorld 时间函数 Hooks (主动获取+监控) =====================

// Hook: getAbsoluteTime
static long long hook_GetAbsTime(void* thisPtr) {
    MarkTriggered("ScriptWorld::getAbsoluteTime");
    {
        std::lock_guard<std::mutex> lock(g_ptrMutex);
        if (g_worldPtr == nullptr) {
            g_worldPtr = thisPtr;
            LOGI_FMT("CAPTURED thisPtr: %p", thisPtr);
        }
    }
    if (g_orig_GetAbsTime) return g_orig_GetAbsTime(thisPtr);
    return 0;
}

// Hook: getTimeOfDay
static long long hook_GetTod(void* thisPtr) {
    MarkTriggered("ScriptWorld::getTimeOfDay");
    {
        std::lock_guard<std::mutex> lock(g_ptrMutex);
        if (g_worldPtr == nullptr) {
            g_worldPtr = thisPtr;
            LOGI_FMT("CAPTURED thisPtr: %p", thisPtr);
        }
    }
    if (g_orig_GetTod) return g_orig_GetTod(thisPtr);
    return 0;
}

// ===================== 2. 通用监控 Hook 模板 (仅用于观察) =====================
// 这些宏定义生成简单的监控函数，不修改逻辑，只打日志

#define DEFINE_MONITOR_HOOK(name) \
static void hook_##name() { \
    MarkTriggered(#name); \
    auto it = g_origFuncs.find(#name); \
    if (it != g_origFuncs.end() && it->second) { \
        ((void(*)())(it->second))(); \
    } \
}

// 定义所有监控点
DEFINE_MONITOR_HOOK(TimeCommand_QueryGametime);
DEFINE_MONITOR_HOOK(TimeCommand_QueryDaytime);
DEFINE_MONITOR_HOOK(TimeCommand_Set);
DEFINE_MONITOR_HOOK(DaylightCycle_Rule);
DEFINE_MONITOR_HOOK(DoDaylightCycle_Switch);
DEFINE_MONITOR_HOOK(DayCycleStopTime_Flag);

// ===================== 核心逻辑：在渲染循环中主动更新 =====================
static void UpdateTimeInRender() {
    void* ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_ptrMutex);
        ptr = g_worldPtr;
    }

    if (ptr != nullptr) {
        long long time = 0;
        bool success = false;

        if (g_orig_GetAbsTime) {
            time = g_orig_GetAbsTime(ptr);
            success = true;
        } else if (g_orig_GetTod) {
            time = g_orig_GetTod(ptr);
            success = true;
        }

        if (success) {
            std::lock_guard<std::mutex> lock2(g_dataMutex);
            g_data.ticks = time;
            g_data.day = (int)(time / 24000 + 1);
            g_data.valid = true;
        }
    }
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

    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        if (g_data.valid) {
            ImGui::Text("Day: %d", g_data.day);
            int tod = g_data.ticks % 24000;
            int h = tod / 1000;
            int m = (tod % 1000) * 60 / 1000;
            ImGui::Text("Time: %02d:%02d", h, m);
            ImGui::TextColored(ImVec4(0.75f, 0.55f, 0.95f, 1.0f), "Ticks: %lld", g_data.ticks);
        } else {
            ImGui::TextColored(ImVec4(0.55f, 0.48f, 0.62f, 1.0f), "Running...");
        }
    }
    
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Separator();
    ImGui::Text("Hook Status:");
    
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        for (auto& pair : g_hookTriggered) {
            if (pair.second) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "  [OK] %s", pair.first.c_str());
            }
        }
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

// ===================== EGL Hook (主循环) =====================
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

    // 每帧主动更新时间
    UpdateTimeInRender();

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
    sleep(3);
    LOGI("========== TimeMod Mass Hook Init ==========");
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

    // ==========================================
    // 3. 批量安装所有 Hooks (来自你的文档)
    // ==========================================
    LOGI("Installing all hooks...");

    // --- 第一组：核心时间获取 (保存原函数指针以便主动调用) ---
    uintptr_t addr;
    
    // 1. getAbsoluteTime (0x2ad505c)
    addr = g_libBase + 0x2ad505c;
    if (GlossHook((void*)addr, (void*)hook_GetAbsTime, (void**)&g_orig_GetAbsTime)) {
        LOGI_FMT("Hooked: getAbsoluteTime @ 0x%lx", addr);
    }

    // 2. getTimeOfDay (0x2ad4b90)
    addr = g_libBase + 0x2ad4b90;
    if (GlossHook((void*)addr, (void*)hook_GetTod, (void**)&g_orig_GetTod)) {
        LOGI_FMT("Hooked: getTimeOfDay @ 0x%lx", addr);
    }

    // --- 第二组：其他观察点 (仅监控) ---
    // 这里使用宏来简化，把地址和名字对应起来
    
    struct MonitorTarget {
        std::string name;
        uintptr_t offset;
        void* hookFunc;
    };

    std::vector<MonitorTarget> monitors = {
        // 命令相关
        { "TimeCommand_QueryGametime", 0xe663614, (void*)hook_TimeCommand_QueryGametime },
        { "TimeCommand_QueryDaytime", 0xe66347c, (void*)hook_TimeCommand_QueryDaytime },
        { "TimeCommand_Set", 0xe6641bc, (void*)hook_TimeCommand_Set },
        
        // 日夜循环相关
        { "DaylightCycle_Rule", 0x9c9c80c, (void*)hook_DaylightCycle_Rule },
        { "DoDaylightCycle_Switch", 0x10555e20, (void*)hook_DoDaylightCycle_Switch },
        { "DayCycleStopTime_Flag", 0x10549d94, (void*)hook_DayCycleStopTime_Flag },
    };

    for (auto& t : monitors) {
        uintptr_t absAddr = g_libBase + t.offset;
        // 我们需要一个地方存原函数指针，这里用 map
        GenericFunc orig = nullptr;
        if (GlossHook((void*)absAddr, t.hookFunc, (void**)&orig)) {
            g_origFuncs[t.name] = orig;
            LOGI_FMT("Monitored: %s @ 0x%lx", t.name.c_str(), absAddr);
        } else {
            LOGE_FMT("Failed: %s", t.name.c_str());
        }
    }

    LOGI("========== ALL HOOKS INSTALLED ==========");
    return nullptr;
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

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

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "TimeMod"
#define LOG_FILE "/storage/emulated/0/TimeMod_Debug.log"

// ===================== Globals =====================
static std::mutex g_logMutex;
static std::atomic<bool> g_gotData(false);
static std::thread* g_workerThread = nullptr;

// 核心数据
typedef long long (*TimeFunc)(void*);
static TimeFunc g_originalTimeFunc = nullptr; // 真实的函数地址
static void* g_worldPtr = nullptr;             // 真实的对象指针

// 显示数据
struct TimeData {
    long long ticks = 0;
    int day = 0;
    bool valid = false;
};
static TimeData g_displayData;
static std::mutex g_dataMutex;

// ===================== Logging =====================
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

// ===================== Worker Thread (主动调用) =====================
static void WorkerLoop() {
    LOGI("Worker Thread started. Actively polling time...");
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 20 FPS 更新率

        if (g_originalTimeFunc && g_worldPtr) {
            // 直接调用游戏函数！
            long long t = g_originalTimeFunc(g_worldPtr);
            
            std::lock_guard<std::mutex> lock(g_dataMutex);
            g_displayData.ticks = t;
            g_displayData.day = t / 24000 + 1;
            g_displayData.valid = true;
            
            if (!g_gotData) {
                g_gotData = true;
                LOGI_FMT("SUCCESS: Data flowing! First tick: %lld", t);
            }
        }
    }
}

// ===================== Hook Callback (被动捕获) =====================
static long long hook_getTime(void* thisPtr) {
    // 只要进了这里，我们就成功了一半！
    if (g_worldPtr == nullptr) {
        LOGI_FMT("Hook triggered! Captured thisPtr: %p", thisPtr);
        g_worldPtr = thisPtr;
        
        // 启动工作线程
        if (g_workerThread == nullptr) {
            g_workerThread = new std::thread(WorkerLoop);
        }
    }

    // 调用原函数
    if (g_originalTimeFunc) {
        return g_originalTimeFunc(thisPtr);
    }
    return 0;
}

// ===================== ImGui & Render =====================
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
        if (g_displayData.valid) {
            ImGui::Text("Day: %d", g_displayData.day);
            int tod = g_displayData.ticks % 24000;
            int h = tod / 1000;
            int m = (tod % 1000) * 60 / 1000;
            ImGui::Text("Time: %02d:%02d", h, m);
            ImGui::Text("Ticks: %lld", g_displayData.ticks);
        } else {
            ImGui::Text("Waiting for data...");
            if (g_originalTimeFunc) ImGui::Text("Func: OK");
            if (g_worldPtr) ImGui::Text("Ptr: OK");
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
    
    // Simple style
    ImGui::GetStyle().WindowRounding = 8.0f;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.90f, 0.98f, 0.96f);
    ImGui::GetStyle().Colors[ImGuiCol_Text] = ImVec4(0.28f, 0.22f, 0.38f, 1.0f);
    
    g_init = true;
}

// ===================== EGL Hook (渲染循环) =====================
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

// ===================== Main Init =====================
static void* MainThread(void*) {
    sleep(3);
    LOGI("Initializing...");
    GlossInit(true);

    // Hook EGL
    GHandle egl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_swap, (void**)&orig_swap);

    // Hook Input
    void* s1 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (s1) GlossHook(s1, (void*)hook_in1, (void**)&orig_in1);
    void* s2 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (s2) GlossHook(s2, (void*)hook_in2, (void**)&orig_in2);

    // ==========================================
    // 核心逻辑：Hook 目标函数
    // ==========================================
    LOGI("Finding target function...");
    
    // 1. 找基址
    void* mcLib = dlopen("libminecraftpe.so", RTLD_NOLOAD);
    if (!mcLib) mcLib = dlopen("libminecraftpe.so", RTLD_LAZY);
    if (!mcLib) { LOGE("libminecraftpe.so not found"); return nullptr; }

    uintptr_t base = 0;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libminecraftpe.so") != std::string::npos && line.find("r-xp") != std::string::npos) {
            uintptr_t s, e;
            if (sscanf(line.c_str(), "%lx-%lx", &s, &e) == 2) {
                if (base == 0) base = s;
            }
        }
    }
    if (base == 0) { LOGE("Base not found"); return nullptr; }

    // 2. 计算地址
    const uintptr_t offset = 0x2ad4af1; // 你提供的最新地址
    uintptr_t absAddr = base + offset;
    LOGI_FMT("Base: 0x%lx | Offset: 0x%lx | Abs: 0x%lx", base, offset, absAddr);

    // 3. 保存原函数指针（以便我们自己主动调用）
    g_originalTimeFunc = (TimeFunc)absAddr;

    // 4. 安装 Hook（以便我们捕获 thisPtr）
    LOGI("Installing hook...");
    if (GlossHook((void*)absAddr, (void*)hook_getTime, (void**)&g_originalTimeFunc)) {
        LOGI("Hook installed successfully!");
    } else {
        // 如果 GlossHook 失败，尝试只保存不 hook（虽然没 thisPtr 不行，但至少日志有记录）
        LOGI("Hook install failed, but func ptr saved.");
        g_originalTimeFunc = (TimeFunc)absAddr; 
    }

    LOGI("Init complete. Enter the game!");
    return nullptr;
}

__attribute__((constructor))
void init() {
    // 清空旧日志
    std::ofstream f(LOG_FILE, std::ios::trunc);
    if (f.is_open()) { f << "Module loaded." << std::endl; f.close(); }
    
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

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

// ===================== 全局控制 =====================
static std::mutex g_logMutex;
static std::atomic<bool> g_isRunning(false);
static std::thread* g_workerThread = nullptr;

// ===================== 日志 =====================
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

// ===================== 数据状态 =====================
struct TimeData {
    long long absoluteTime = 0;
    int timeOfDay = 0;
    int day = 0;
    bool valid = false;
};
static TimeData g_data;
static std::mutex g_dataMutex;

// ===================== 函数指针 =====================
typedef long long (*Func_GetTimeOfDay)(void*);
typedef long long (*Func_GetAbsoluteTime)(void*);

static Func_GetTimeOfDay g_orig_GetTimeOfDay = nullptr;
static Func_GetAbsoluteTime g_orig_GetAbsoluteTime = nullptr;
static void* g_scriptWorldPtr = nullptr;
static std::mutex g_ptrMutex;

// ===================== 后台工作线程 (核心：持续执行) =====================
static void WorkerLoop() {
    LOGI("========== Worker Thread STARTED ==========");
    int counter = 0;
    
    while (g_isRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 20次/秒

        void* ptr = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_ptrMutex);
            ptr = g_scriptWorldPtr;
        }

        if (ptr != nullptr) {
            // 有对象指针，直接调用原函数！
            long long absTime = 0;
            int todTime = 0;

            if (g_orig_GetAbsoluteTime) {
                absTime = g_orig_GetAbsoluteTime(ptr);
            }
            if (g_orig_GetTimeOfDay) {
                todTime = (int)g_orig_GetTimeOfDay(ptr);
            }

            // 更新数据
            {
                std::lock_guard<std::mutex> lock2(g_dataMutex);
                if (g_orig_GetAbsoluteTime) {
                    g_data.absoluteTime = absTime;
                    g_data.day = (int)(absTime / 24000 + 1);
                }
                if (g_orig_GetTimeOfDay) {
                    g_data.timeOfDay = todTime;
                    if (!g_orig_GetAbsoluteTime) {
                        g_data.day = g_data.timeOfDay / 24000 + 1;
                    }
                }
                g_data.valid = true;
            }
            
            // 每2秒打印一次日志证明还活着
            if (counter++ % 40 == 0) {
                 LOGI_FMT("Worker running: Abs=%lld, Day=%d", g_data.absoluteTime, g_data.day);
            }
        } else {
            // 还没抓到指针，每2秒打印一次等待信息
            if (counter++ % 40 == 0) {
                LOGI("Worker waiting for thisPtr... (Enter the game!)");
            }
        }
    }
    LOGI("Worker Thread stopped.");
}

// ===================== Hook 回调 (只负责抓 thisPtr) =====================
static long long hook_GetTimeOfDay(void* thisPtr) {
    bool isNew = false;
    {
        std::lock_guard<std::mutex> lock(g_ptrMutex);
        if (g_scriptWorldPtr == nullptr) {
            g_scriptWorldPtr = thisPtr;
            isNew = true;
        }
    }

    if (isNew) {
        LOGI_FMT("!!! CAPTURED thisPtr via getTimeOfDay: %p", thisPtr);
    }

    if (g_orig_GetTimeOfDay) return g_orig_GetTimeOfDay(thisPtr);
    return 0;
}

static long long hook_GetAbsoluteTime(void* thisPtr) {
    bool isNew = false;
    {
        std::lock_guard<std::mutex> lock(g_ptrMutex);
        if (g_scriptWorldPtr == nullptr) {
            g_scriptWorldPtr = thisPtr;
            isNew = true;
        }
    }

    if (isNew) {
        LOGI_FMT("!!! CAPTURED thisPtr via getAbsoluteTime: %p", thisPtr);
    }

    if (g_orig_GetAbsoluteTime) return g_orig_GetAbsoluteTime(thisPtr);
    return 0;
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
            int tod = g_data.timeOfDay != 0 ? g_data.timeOfDay : (g_data.absoluteTime % 24000);
            int h = tod / 1000;
            int m = (tod % 1000) * 60 / 1000;
            ImGui::Text("Time: %02d:%02d", h, m);
            ImGui::TextColored(ImVec4(0.75f, 0.55f, 0.95f, 1.0f), "Ticks: %lld", g_data.absoluteTime);
        } else {
            ImGui::TextColored(ImVec4(0.55f, 0.48f, 0.62f, 1.0f), "Running...");
            ImGui::Dummy(ImVec2(0, 4));
            if (g_scriptWorldPtr) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Status: Syncing");
            } else {
                ImGui::Text("Status: Waiting");
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
    GHandle egl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_swap, (void**)&orig_swap);

    void* s1 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (s1) GlossHook(s1, (void*)hook_in1, (void**)&orig_in1);
    void* s2 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (s2) GlossHook(s2, (void*)hook_in2, (void**)&orig_in2);

    // 2. 获取基址
    uintptr_t base = GetLibBase("libminecraftpe.so");
    if (base == 0) { LOGE("Base not found"); return nullptr; }
    LOGI_FMT("Lib Base: 0x%lx", base);

    // 3. 安装 Hooks
    LOGI("Installing Hooks...");
    
    // 目标1: getTimeOfDay (0x2ad4b90)
    uintptr_t addr1 = base + 0x2ad4b90;
    if (GlossHook((void*)addr1, (void*)hook_GetTimeOfDay, (void**)&g_orig_GetTimeOfDay)) {
        LOGI_FMT("Hooked 1/2: getTimeOfDay @ 0x%lx", addr1);
    }

    // 目标2: getAbsoluteTime (0x2ad505c)
    uintptr_t addr2 = base + 0x2ad505c;
    if (GlossHook((void*)addr2, (void*)hook_GetAbsoluteTime, (void**)&g_orig_GetAbsoluteTime)) {
        LOGI_FMT("Hooked 2/2: getAbsoluteTime @ 0x%lx", addr2);
    }

    // 4. 启动后台工作线程 (关键！)
    LOGI("Starting Worker Thread...");
    g_isRunning = true;
    g_workerThread = new std::thread(WorkerLoop);

    LOGI("========== ALL READY ==========");
    return nullptr;
}

__attribute__((constructor))
void init() {
    std::ofstream f(LOG_FILE, std::ios::trunc);
    if (f.is_open()) { f << "Module loaded." << std::endl; f.close(); }
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

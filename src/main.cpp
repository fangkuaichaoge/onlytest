// ===================== System Header Files =====================
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
#include <sstream>
#include <iomanip>
#include <atomic>

// ===================== Project Header Files =====================
#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "TimeMod"
#define LOG_FILE_PATH "/storage/emulated/0/TimeMod_Debug.log"

// ===================== 全局日志锁 =====================
static std::mutex g_logMutex;
static std::atomic<bool> g_isRunning(false);
static std::thread* g_pollingThread = nullptr;

// ===================== 日志函数 =====================
static void LogDebug(const std::string& level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[%s] %s", level.c_str(), msg.c_str());
    std::ofstream logFile(LOG_FILE_PATH, std::ios::app);
    if (logFile.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        logFile << "[" << std::put_time(std::localtime(&time_t_now), "%H:%M:%S") << "] [" << level << "] " << msg << std::endl;
        logFile.close();
    }
}

#define LOGI_F(...) { \
    char buf[1024]; \
    snprintf(buf, sizeof(buf), __VA_ARGS__); \
    LogDebug("INFO", buf); \
}

#define LOGE_F(...) { \
    char buf[1024]; \
    snprintf(buf, sizeof(buf), __VA_ARGS__); \
    LogDebug("ERROR", buf); \
}

// ===================== 时间常量 =====================
const long long TICKS_PER_DAY = 24000;

// ===================== Time State =====================
struct TimeState {
    bool funcFound = false;
    long long absoluteTime = 0;
    int day = 0;
};
static TimeState g_timeState;
static std::mutex g_timeMutex;

// ===================== Hook Function Pointers =====================
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

// ===================== ScriptWorld 时间函数指针 =====================
typedef long long (*ScriptGetTimeOfDayFunc)(void*);
static ScriptGetTimeOfDayFunc orig_scriptGetTimeOfDay = nullptr;
static void* g_scriptWorld = nullptr;
static std::mutex g_scriptWorldMutex;

// ===================== 主动轮询线程 =====================
static void PollingThreadLoop() {
    LOGI_F("Polling Thread started.");
    while (g_isRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 每秒刷新10次

        void* currentWorldPtr = nullptr;
        ScriptGetTimeOfDayFunc currentFunc = nullptr;
        
        {
            std::lock_guard<std::mutex> lock(g_scriptWorldMutex);
            currentWorldPtr = g_scriptWorld;
            currentFunc = orig_scriptGetTimeOfDay;
        }

        if (currentFunc && currentWorldPtr) {
            // 主动调用原函数获取时间！
            long long time = currentFunc(currentWorldPtr);
            
            std::lock_guard<std::mutex> lock2(g_timeMutex);
            g_timeState.absoluteTime = time;
            g_timeState.day = time / TICKS_PER_DAY + 1;
            g_timeState.funcFound = true;
        }
    }
    LOGI_F("Polling Thread stopped.");
}

// ===================== Time Hook (现在只负责捕获 thisPtr) =====================
static long long hook_scriptGetTimeOfDay(void* thisPtr) {
    LOGI_F("hook_scriptGetTimeOfDay CALLED! thisPtr: %p", thisPtr);
    
    // 保存对象指针，供轮询线程使用
    {
        std::lock_guard<std::mutex> lock(g_scriptWorldMutex);
        g_scriptWorld = thisPtr;
    }

    // 启动轮询线程（如果还没启动）
    if (!g_isRunning) {
        g_isRunning = true;
        g_pollingThread = new std::thread(PollingThreadLoop);
    }

    if (!orig_scriptGetTimeOfDay) return 0;
    return orig_scriptGetTimeOfDay(thisPtr);
}

// ===================== ImGui Render Global State =====================
static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;
static ImFont* g_UIFont = nullptr;

// ===================== Theme Style =====================
static float g_FontScale = 1.0f;

static void SetupStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;
    const ImVec4 purplePrimary(0.6f, 0.4f, 0.85f, 1.0f);
    const ImVec4 purpleDark(0.45f, 0.3f, 0.7f, 1.0f);
    const ImVec4 purpleLight(0.75f, 0.55f, 0.95f, 1.0f);
    const ImVec4 bgBase(0.94f, 0.90f, 0.98f, 0.96f);
    const ImVec4 bgSecondary(0.97f, 0.94f, 1.0f, 1.0f);
    const ImVec4 textMain(0.28f, 0.22f, 0.38f, 1.0f);

    c[ImGuiCol_WindowBg] = bgBase;
    c[ImGuiCol_ChildBg] = bgSecondary;
    c[ImGuiCol_FrameBg] = bgSecondary;
    c[ImGuiCol_TitleBgActive] = purpleLight;
    c[ImGuiCol_Button] = purplePrimary;
    c[ImGuiCol_ButtonHovered] = purpleLight;
    c[ImGuiCol_Text] = textMain;

    float roundScale = g_FontScale;
    s.WindowRounding = 10.0f * roundScale;
    s.FrameRounding = 6.0f * roundScale;
    s.WindowPadding = ImVec2(14.0f * roundScale, 12.0f * roundScale);
}

// ===================== UI Interface: Time HUD =====================
static void DrawTimeHUD() {
    if (g_UIFont) ImGui::PushFont(g_UIFont);

    TimeState state;
    {
        std::lock_guard<std::mutex> lock(g_timeMutex);
        state = g_timeState;
    }

    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
    ImGui::Begin("Game Time", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.4f, 0.85f, 1.0f));
    ImGui::Text("Game Time");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8));

    if (state.funcFound) {
        ImGui::TextColored(ImVec4(0.75f, 0.55f, 0.95f, 1.0f), "Ticks: %lld", state.absoluteTime);
        int displayDay = state.day;
        int timeOfDay = state.absoluteTime % TICKS_PER_DAY;
        int hours = timeOfDay / 1000;
        int minutes = (timeOfDay % 1000) * 60 / 1000;
        
        ImGui::Text("Day: %d", displayDay);
        ImGui::Text("Time: %02d:%02d", hours, minutes);
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.48f, 0.62f, 1.0f), "Waiting for Hook...");
    }

    ImGui::End();
    if (g_UIFont) ImGui::PopFont();
}

// ===================== GL State Protection =====================
struct GLState {
    GLint prog, tex, aBuf, eBuf, vao, fbo, vp[4];
};
static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
}
static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
}

// ===================== ImGui Initialization =====================
static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    float baseScale = (float)g_Height / 720.0f;
    g_FontScale = std::clamp(baseScale, 1.0f, 2.0f);

    ImFontConfig cfg;
    cfg.SizePixels = 20.0f * g_FontScale;
    g_UIFont = io.Fonts->AddFontDefault(&cfg);

    ImGui_ImplAndroid_Init(nullptr);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    SetupStyle();
    g_Initialized = true;
}

static void Render() {
    if (!g_Initialized) return;
    GLState s; SaveGL(s);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    io.DisplayFramebufferScale = ImVec2(1, 1);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();
    DrawTimeHUD();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    RestoreGL(s);
}

// ===================== EGL Render Hook =====================
static EGLBoolean hook_eglSwapBuffers(EGLDisplay d, EGLSurface s) {
    if (!orig_eglSwapBuffers) return orig_eglSwapBuffers(d, s);
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(d, s);

    EGLint w = 0, h = 0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(d, s);

    if (g_TargetContext == EGL_NO_CONTEXT) {
        g_TargetContext = ctx;
        g_TargetSurface = s;
    }
    if (ctx != g_TargetContext || s != g_TargetSurface)
        return orig_eglSwapBuffers(d, s);

    g_Width = w; g_Height = h;
    Setup();
    Render();
    return orig_eglSwapBuffers(d, s);
}

// ===================== Input Hook =====================
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (thiz && g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}
static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** e) {
    int32_t r = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, e) : 0;
    if (r == 0 && e && *e && g_Initialized)
        ImGui_ImplAndroid_HandleInputEvent(*e);
    return r;
}
static void HookInput() {
    void* s1 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (s1) GlossHook(s1, (void*)hook_Input1, (void**)&orig_Input1);
    void* s2 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (s2) GlossHook(s2, (void*)hook_Input2, (void**)&orig_Input2);
}

// ===================== 核心逻辑：双路尝试 Hook =====================
static bool findAndHookTime() {
    LOGI_F("Entering findAndHookTime...");
    std::ofstream logFile(LOG_FILE_PATH, std::ios::trunc);
    if (logFile.is_open()) logFile.close();

    void* mcLib = dlopen("libminecraftpe.so", RTLD_NOLOAD);
    if (!mcLib) mcLib = dlopen("libminecraftpe.so", RTLD_LAZY);
    if (!mcLib) { LOGE_F("Failed to open libminecraftpe.so"); return false; }
    
    uintptr_t libBase = 0;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libminecraftpe.so") != std::string::npos && line.find("r-xp") != std::string::npos) {
            uintptr_t start, end;
            if (sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2) {
                if (libBase == 0) { libBase = start; LOGI_F("Found base: 0x%lx", libBase); }
            }
        }
    }
    if (libBase == 0) return false;

    bool hooked = false;
    const uintptr_t reflectionOffset = 0x2ad4af1; // 保持你原来的偏移

    LOGI_F("--- Attempting Hook by Address ---");
    void* funcAddr = (void*)(libBase + reflectionOffset);
    LOGI_F("Target Address: 0x%lx", (uintptr_t)funcAddr);

    orig_scriptGetTimeOfDay = (ScriptGetTimeOfDayFunc)funcAddr;
    if (GlossHook(funcAddr, (void*)hook_scriptGetTimeOfDay, (void**)&orig_scriptGetTimeOfDay)) {
        LOGI_F("SUCCESS: Hook installed. Now enter the game!");
        hooked = true;
    } else {
        LOGE_F("Hook failed.");
    }

    return hooked;
}

// ===================== Main Thread =====================
static void* MainThread(void*) {
    LOGI_F("MainThread started.");
    sleep(3);
    GlossInit(true);
    
    GHandle egl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    
    HookInput();
    findAndHookTime();
    
    LOGI_F("Setup complete.");
    return nullptr;
}

__attribute__((constructor))
void init() {
    std::ofstream logFile(LOG_FILE_PATH, std::ios::trunc);
    if (logFile.is_open()) { logFile << "Module loaded." << std::endl; logFile.close(); }
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

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

// ===================== 日志函数 (同时输出到 Logcat 和文件) =====================
static void LogDebug(const std::string& level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    
    // 1. 输出到 Logcat
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[%s] %s", level.c_str(), msg.c_str());
    
    // 2. 输出到文件
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
const long long TICKS_PER_DAY = 24000; // 0x5DC0

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
static bool g_hookCalled = false; // 标记 Hook 函数是否被调用过

// ===================== Time Hook =====================
static long long hook_scriptGetTimeOfDay(void* thisPtr) {
    // 只要进了这个函数，就说明 Hook 成功了！
    if (!g_hookCalled) {
        LOGI_F("SUCCESS! hook_scriptGetTimeOfDay has been called! thisPtr: %p", thisPtr);
        g_hookCalled = true;
    }

    if (!orig_scriptGetTimeOfDay) return 0;
    
    long long time = orig_scriptGetTimeOfDay(thisPtr);
    g_scriptWorld = thisPtr;
    
    std::lock_guard<std::mutex> lock(g_timeMutex);
    g_timeState.absoluteTime = time;
    g_timeState.day = time / TICKS_PER_DAY + 1;
    
    return time;
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

    // 淡紫色主题
    const ImVec4 purplePrimary(0.6f, 0.4f, 0.85f, 1.0f);
    const ImVec4 purpleDark(0.45f, 0.3f, 0.7f, 1.0f);
    const ImVec4 purpleLight(0.75f, 0.55f, 0.95f, 1.0f);
    const ImVec4 purpleMuted(0.55f, 0.45f, 0.75f, 1.0f);
    const ImVec4 bgBase(0.94f, 0.90f, 0.98f, 0.96f);
    const ImVec4 bgSecondary(0.97f, 0.94f, 1.0f, 1.0f);
    const ImVec4 textMain(0.28f, 0.22f, 0.38f, 1.0f);
    const ImVec4 textMuted(0.55f, 0.48f, 0.62f, 1.0f);

    // 基础颜色
    c[ImGuiCol_WindowBg] = bgBase;
    c[ImGuiCol_ChildBg] = bgSecondary;
    c[ImGuiCol_PopupBg] = ImVec4(0.96f, 0.93f, 0.99f, 0.98f);
    c[ImGuiCol_FrameBg] = bgSecondary;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.99f, 0.96f, 1.0f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1.0f, 0.98f, 1.0f, 1.0f);
    c[ImGuiCol_TitleBg] = bgSecondary;
    c[ImGuiCol_TitleBgActive] = purpleLight;
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.95f, 0.91f, 0.99f, 1.0f);
    c[ImGuiCol_MenuBarBg] = bgSecondary;

    // 滚动条
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.92f, 0.88f, 0.96f, 0.5f);
    c[ImGuiCol_ScrollbarGrab] = purpleMuted;
    c[ImGuiCol_ScrollbarGrabHovered] = purplePrimary;
    c[ImGuiCol_ScrollbarGrabActive] = purpleDark;

    // 控件
    c[ImGuiCol_CheckMark] = purplePrimary;
    c[ImGuiCol_SliderGrab] = purplePrimary;
    c[ImGuiCol_SliderGrabActive] = purpleLight;
    c[ImGuiCol_Button] = purplePrimary;
    c[ImGuiCol_ButtonHovered] = purpleLight;
    c[ImGuiCol_ButtonActive] = purpleDark;

    // Header
    c[ImGuiCol_Header] = ImVec4(0.96f, 0.92f, 1.0f, 0.6f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.98f, 0.95f, 1.0f, 0.8f);
    c[ImGuiCol_HeaderActive] = purplePrimary;

    // 拖拽 grip
    c[ImGuiCol_ResizeGrip] = ImVec4(0.6f, 0.5f, 0.7f, 0.5f);
    c[ImGuiCol_ResizeGripHovered] = purplePrimary;
    c[ImGuiCol_ResizeGripActive] = purpleDark;

    // Tab
    c[ImGuiCol_Tab] = bgSecondary;
    c[ImGuiCol_TabHovered] = purpleLight;
    c[ImGuiCol_TabActive] = purplePrimary;
    c[ImGuiCol_TabUnfocused] = bgSecondary;
    c[ImGuiCol_TabUnfocusedActive] = purpleMuted;

    // 文本
    c[ImGuiCol_Text] = textMain;
    c[ImGuiCol_TextDisabled] = textMuted;

    // 分隔线
    c[ImGuiCol_Separator] = ImVec4(0.7f, 0.62f, 0.82f, 0.4f);
    c[ImGuiCol_SeparatorHovered] = purplePrimary;
    c[ImGuiCol_SeparatorActive] = purpleDark;
    c[ImGuiCol_Border] = ImVec4(0.75f, 0.68f, 0.85f, 0.3f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // 导航
    c[ImGuiCol_NavHighlight] = purpleLight;
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(0.5f, 0.7f, 1.0f, 0.5f);
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.2f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);

    // 圆角 (基于字体缩放)
    float roundScale = g_FontScale;
    s.WindowRounding = (float)(int)(10.0f * roundScale);
    s.ChildRounding = (float)(int)(8.0f * roundScale);
    s.FrameRounding = (float)(int)(6.0f * roundScale);
    s.GrabRounding = (float)(int)(6.0f * roundScale);
    s.ScrollbarRounding = (float)(int)(6.0f * roundScale);
    s.PopupRounding = (float)(int)(8.0f * roundScale);
    s.TabRounding = (float)(int)(6.0f * roundScale);

    // 间距 (基于字体缩放)
    s.WindowPadding = ImVec2(14.0f * roundScale, 12.0f * roundScale);
    s.FramePadding = ImVec2(10.0f * roundScale, 8.0f * roundScale);
    s.ItemSpacing = ImVec2(10.0f * roundScale, 8.0f * roundScale);
    s.ItemInnerSpacing = ImVec2(8.0f * roundScale, 6.0f * roundScale);
    s.TouchExtraPadding = ImVec2(4.0f * roundScale, 4.0f * roundScale);
    s.IndentSpacing = 22.0f * roundScale;
    s.ColumnsMinSpacing = 8.0f * roundScale;

    // 大小
    s.ScrollbarSize = (float)(int)(14.0f * roundScale);
    s.GrabMinSize = (float)(int)(12.0f * roundScale);
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
    ImGui::SetNextWindowSizeConstraints(ImVec2(240, 0), ImVec2(320, 200));

    ImGui::Begin("Game Time", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoTitleBar);

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
        
        ImGui::Dummy(ImVec2(0, 4));
        if (hours >= 6 && hours < 18) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "☀ Daytime");
        } else {
            ImGui::TextColored(ImVec4(0.25f, 0.35f, 0.8f, 1.0f), "🌙 Nighttime");
        }
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.48f, 0.62f, 1.0f), "Loading...");
        // 显示调试状态
        if (g_hookCalled) {
             ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Hook Active!");
        }
    }

    ImGui::End();

    if (g_UIFont) ImGui::PopFont();
}

// ===================== GL State Protection =====================
struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo, vp[4], sc[4], bSrc, bDst, bSrcA, bDstA;
    GLboolean blend, cull, depth, scissor, stencil, dither;
    GLint frontFace, activeTexture;
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.bSrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.bDst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bDstA);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);
    s.dither = glIsEnabled(GL_DITHER);
    glGetIntegerv(GL_FRONT_FACE, &s.frontFace);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.activeTexture);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFuncSeparate(s.bSrc, s.bDst, s.bSrcA, s.bDstA);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    s.stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
    s.dither ? glEnable(GL_DITHER) : glDisable(GL_DITHER);
    glFrontFace(s.frontFace);
}

// ===================== ImGui Initialization =====================
static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    float baseScale = (float)g_Height / 720.0f;
    g_FontScale = std::clamp(baseScale, 1.0f, 2.0f);

    ImFontConfig cfg;
    cfg.SizePixels = (float)(int)(20.0f * g_FontScale);
    cfg.OversampleH = cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
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
        EGLint buf;
        eglQuerySurface(d, s, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = s;
        }
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
    LOGI_F("Attempting to Hook Input...");
    void* s1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (s1) {
        GlossHook(s1, (void*)hook_Input1, (void**)&orig_Input1);
        LOGI_F("Input1 Hooked.");
    } else {
        LOGE_F("Input1 symbol not found.");
    }

    void* s2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (s2) {
        GlossHook(s2, (void*)hook_Input2, (void**)&orig_Input2);
        LOGI_F("Input2 Hooked.");
    } else {
        LOGE_F("Input2 symbol not found.");
    }
}

// ===================== 核心逻辑：双路尝试 Hook =====================
static bool findAndHookTime() {
    LOGI_F("Entering findAndHookTime...");
    
    // 清空旧日志
    std::ofstream logFile(LOG_FILE_PATH, std::ios::trunc);
    if (logFile.is_open()) logFile.close();

    void* mcLib = dlopen("libminecraftpe.so", RTLD_NOLOAD);
    if (!mcLib) {
        LOGI_F("libminecraftpe.so not loaded yet, trying dlopen(RTLD_LAZY)...");
        mcLib = dlopen("libminecraftpe.so", RTLD_LAZY);
    }
    if (!mcLib) {
        LOGE_F("Failed to open libminecraftpe.so");
        return false;
    }
    LOGI_F("libminecraftpe.so handle obtained: %p", mcLib);
    
    uintptr_t libBase = 0;
    // 获取基址
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libminecraftpe.so") != std::string::npos && line.find("r-xp") != std::string::npos) {
            uintptr_t start, end;
            if (sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2) {
                if (libBase == 0) {
                    libBase = start;
                    LOGI_F("Found libminecraftpe.so base: 0x%lx", libBase);
                }
            }
        }
    }

    if (libBase == 0) {
        LOGE_F("Failed to find library base address!");
        return false;
    }

    bool hooked = false;
    const char* targetSymbol = "_ZNK21ScriptModuleMinecraft11ScriptWorld12getTimeOfDayEv";
    const uintptr_t reflectionOffset = 0x2ad4af1;

    // ==========================================
    // 第一步：尝试 符号 Hook
    // ==========================================
    LOGI_F("--- Attempt 1: Hook by Symbol ---");
    void* funcAddr = (void*)GlossSymbol(GlossOpen("libminecraftpe.so"), targetSymbol, nullptr);
    LOGI_F("GlossSymbol result: %p", funcAddr);
    
    if (!funcAddr) {
        funcAddr = dlsym(mcLib, targetSymbol);
        LOGI_F("dlsym result: %p", funcAddr);
    }

    if (funcAddr) {
        LOGI_F("Found function via symbol: 0x%lx", (uintptr_t)funcAddr);
        orig_scriptGetTimeOfDay = (ScriptGetTimeOfDayFunc)funcAddr;
        
        LOGI_F("Calling GlossHook for Symbol...");
        if (GlossHook(funcAddr, (void*)hook_scriptGetTimeOfDay, (void**)&orig_scriptGetTimeOfDay)) {
            LOGI_F("SUCCESS: Hooked via Symbol!");
            hooked = true;
        } else {
            LOGE_F("FAILED: GlossHook returned false for Symbol.");
        }
    } else {
        LOGE_F("FAILED: Symbol not found in library.");
    }

    // ==========================================
    // 第二步：如果符号失败，尝试 反射地址 Hook
    // ==========================================
    if (!hooked) {
        LOGI_F("--- Attempt 2: Hook by Reflection Address ---");
        funcAddr = (void*)(libBase + reflectionOffset);
        LOGI_F("Calculated address: Base(0x%lx) + Offset(0x%lx) = 0x%lx", libBase, reflectionOffset, (uintptr_t)funcAddr);

        orig_scriptGetTimeOfDay = (ScriptGetTimeOfDayFunc)funcAddr;
        
        LOGI_F("Calling GlossHook for Address...");
        if (GlossHook(funcAddr, (void*)hook_scriptGetTimeOfDay, (void**)&orig_scriptGetTimeOfDay)) {
            LOGI_F("SUCCESS: Hooked via Reflection Address!");
            hooked = true;
        } else {
            LOGE_F("FAILED: GlossHook returned false for Address.");
        }
    }

    // ==========================================
    // 最终状态更新
    // ==========================================
    if (hooked) {
        LOGI_F("Hook installation successful. Waiting for game to call function...");
        std::lock_guard<std::mutex> lock(g_timeMutex);
        g_timeState.funcFound = true;
    } else {
        LOGE_F("FATAL: All hook attempts failed.");
    }

    return hooked;
}

// ===================== Main Thread =====================
static void* MainThread(void*) {
    LOGI_F("MainThread started. Sleeping 3 seconds...");
    sleep(3);
    LOGI_F("Initializing Gloss...");
    GlossInit(true);
    
    LOGI_F("Hooking EGL...");
    GHandle egl = GlossOpen("libEGL.so");
    if (!egl) {
        LOGE_F("Failed to open libEGL.so");
        return nullptr;
    }
    void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
    if (swap) {
        GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
        LOGI_F("eglSwapBuffers hooked.");
    } else {
        LOGE_F("eglSwapBuffers symbol not found.");
    }
    
    HookInput();
    
    if (!findAndHookTime()) {
        LOGE_F("findAndHookTime returned false.");
    }
    
    LOGI_F("MainThread setup complete.");
    return nullptr;
}

__attribute__((constructor))
void init() {
    // 立即写一条日志证明模块加载了
    std::ofstream logFile(LOG_FILE_PATH, std::ios::trunc);
    if (logFile.is_open()) {
        logFile << "TimeMod initialized. Constructor called." << std::endl;
        logFile.close();
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Constructor called.");
    
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

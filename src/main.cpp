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

// ===================== Project Header Files =====================
#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "TimeMod"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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

// 时间函数指针
typedef long long (*GetTimeFunc)(void*);
typedef int (*GetDayFunc)(void*);
static GetTimeFunc orig_getTime = nullptr;
static GetDayFunc orig_getDay = nullptr;
static void* g_level = nullptr;

// ===================== Time Hook =====================
static long long hook_getTime(void* thisPtr) {
    if (!orig_getTime) return 0;
    
    long long time = orig_getTime(thisPtr);
    g_level = thisPtr;
    
    std::lock_guard<std::mutex> lock(g_timeMutex);
    g_timeState.absoluteTime = time;
    
    // 如果有 getDay 函数，也获取天数
    if (orig_getDay && g_level) {
        g_timeState.day = orig_getDay(g_level);
    }
    
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

// ===================== UI Interface: Time HUD (强制显示) =====================
static void DrawTimeHUD() {
    if (g_UIFont) ImGui::PushFont(g_UIFont);

    TimeState state;
    {
        std::lock_guard<std::mutex> lock(g_timeMutex);
        state = g_timeState;
    }

    // 强制显示在左上角
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(240, 0), ImVec2(320, 180));

    ImGui::Begin("Game Time", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoTitleBar);

    // 标题
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.4f, 0.85f, 1.0f));
    ImGui::Text("Game Time");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8));

    if (state.funcFound) {
        // 总刻数
        ImGui::TextColored(ImVec4(0.75f, 0.55f, 0.95f, 1.0f), "Ticks: %lld", state.absoluteTime);
        
        // 优先使用直接获取的天数，如果没有则计算
        int displayDay = (state.day > 0) ? state.day : (state.absoluteTime / TICKS_PER_DAY + 1);
        
        int timeOfDay = state.absoluteTime % TICKS_PER_DAY;
        int hours = timeOfDay / 1000;
        int minutes = (timeOfDay % 1000) * 60 / 1000;
        
        ImGui::Text("Day: %d", displayDay);
        ImGui::Text("Time: %02d:%02d", hours, minutes);
        
        // 昼夜判断 (MC 时间: 6:00-18:00 为白天)
        ImGui::Dummy(ImVec2(0, 4));
        if (hours >= 6 && hours < 18) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "☀ Daytime");
        } else {
            ImGui::TextColored(ImVec4(0.25f, 0.35f, 0.8f, 1.0f), "🌙 Nighttime");
        }
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.48f, 0.62f, 1.0f), "Loading...");
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

    // 根据屏幕高度计算字体缩放比例 (基准 720p)
    float baseScale = (float)g_Height / 720.0f;
    g_FontScale = std::clamp(baseScale, 1.0f, 2.0f);

    // 设置字体大小 (基准 20px，根据缩放调整)
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
    DrawTimeHUD(); // 强制显示时间 HUD
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
    void* s1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (s1) GlossHook(s1, (void*)hook_Input1, (void**)&orig_Input1);

    void* s2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_10InputEventEblPjPSA_", nullptr);
    if (s2) GlossHook(s2, (void*)hook_Input2, (void**)&orig_Input2);
}

// ===================== Find and Hook Time Functions (通过 typeinfo 找 Level vtable) =====================
static bool findAndHookTime() {
    void* mcLib = dlopen("libminecraftpe.so", RTLD_NOLOAD);
    if (!mcLib) {
        mcLib = dlopen("libminecraftpe.so", RTLD_LAZY);
    }
    if (!mcLib) {
        LOGE("Failed to open libminecraftpe.so");
        return false;
    }
    
    uintptr_t libBase = 0;
    
    // 1. 找 libminecraftpe.so 基址
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libminecraftpe.so") != std::string::npos && line.find("r-xp") != std::string::npos) {
            uintptr_t start, end;
            if (sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2) {
                if (libBase == 0) libBase = start;
            }
        }
    }
    
    if (libBase == 0) {
        LOGE("Failed to find libminecraftpe.so base address");
        return false;
    }
    
    LOGI("libminecraftpe.so base: 0x%lx", libBase);
    
    // ============================================================
    // 注意：这里需要让 AI 帮你确认 Level 的 typeinfo 字符串
    // 通常是 "5Level"
    // ============================================================
    const char* typeinfoName = "5Level"; // 这里需要换成你版本的正确字符串！
    size_t nameLen = strlen(typeinfoName);
    
    uintptr_t typeinfoNameAddr = 0;
    
    // 2. 找 typeinfo 名字符串
    std::ifstream maps2("/proc/self/maps");
    while (std::getline(maps2, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        if (line.find("r--p") == std::string::npos && line.find("r-xp") == std::string::npos) continue;
        
        uintptr_t start, end;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2) continue;
        
        for (uintptr_t addr = start; addr < end - nameLen; addr++) {
            if (memcmp((void*)addr, typeinfoName, nameLen) == 0) {
                typeinfoNameAddr = addr;
                LOGI("Found typeinfo name at 0x%lx", typeinfoNameAddr);
                break;
            }
        }
        if (typeinfoNameAddr != 0) break;
    }
    
    if (typeinfoNameAddr == 0) {
        LOGE("Failed to find Level typeinfo name");
        return false;
    }
    
    uintptr_t typeinfoAddr = 0;
    
    // 3. 找 typeinfo 结构
    std::ifstream maps3("/proc/self/maps");
    while (std::getline(maps3, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        if (line.find("r--p") == std::string::npos) continue;
        
        uintptr_t start, end;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2) continue;
        
        for (uintptr_t addr = start; addr < end - sizeof(void*); addr += sizeof(void*)) {
            uintptr_t* ptr = (uintptr_t*)addr;
            if (*ptr == typeinfoNameAddr) {
                typeinfoAddr = addr - sizeof(void*);
                LOGI("Found typeinfo at 0x%lx", typeinfoAddr);
                break;
            }
        }
        if (typeinfoAddr != 0) break;
    }
    
    if (typeinfoAddr == 0) {
        LOGE("Failed to find Level typeinfo");
        return false;
    }
    
    uintptr_t vtableAddr = 0;
    
    // 4. 找虚表
    std::ifstream maps4("/proc/self/maps");
    while (std::getline(maps4, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        if (line.find("r--p") == std::string::npos) continue;
        
        uintptr_t start, end;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2) continue;
        
        for (uintptr_t addr = start; addr < end - sizeof(void*); addr += sizeof(void*)) {
            uintptr_t* ptr = (uintptr_t*)addr;
            if (*ptr == typeinfoAddr) {
                vtableAddr = addr + sizeof(void*);
                LOGI("Found vtable at 0x%lx", vtableAddr);
                break;
            }
        }
        if (vtableAddr != 0) break;
    }
    
    if (vtableAddr == 0) {
        LOGE("Failed to find Level vtable");
        return false;
    }
    
    // ============================================================
    // 注意：这里的索引需要让 AI 帮你找
    // 线索：
    // 1. "Returns the current time stamp of the level" -> getTime 索引
    // 2. "Returns the day of the current level." -> getDay 索引
    // ============================================================
    int getTimeIndex = 70; // 这里需要换成你版本的真实索引！
    int getDayIndex = 71;  // 这里需要换成你版本的真实索引！
    
    // 5. Hook getTime
    uint64_t* getTimeSlot = (uint64_t*)(vtableAddr + getTimeIndex * sizeof(void*));
    orig_getTime = (GetTimeFunc)(*getTimeSlot);
    LOGI("Original Level::getTime at: 0x%lx", (uintptr_t)orig_getTime);
    
    // 6. Hook getDay (如果找到索引)
    if (getDayIndex > 0) {
        uint64_t* getDaySlot = (uint64_t*)(vtableAddr + getDayIndex * sizeof(void*));
        orig_getDay = (GetDayFunc)(*getDaySlot);
        LOGI("Original Level::getDay at: 0x%lx", (uintptr_t)orig_getDay);
    }
    
    // 7. 替换虚表
    uintptr_t pageStart = (uintptr_t)getTimeSlot & ~(4095UL);
    if (mprotect((void*)pageStart, 4096, PROT_READ | PROT_WRITE) != 0) {
        LOGE("Failed to make vtable writable");
        return false;
    }
    
    *getTimeSlot = (uint64_t)hook_getTime;
    
    // 如果有 getDay，也替换它 (可选)
    // if (orig_getDay && getDayIndex > 0) {
    //     uint64_t* getDaySlot = (uint64_t*)(vtableAddr + getDayIndex * sizeof(void*));
    //     *getDaySlot = (uint64_t)hook_getDay;
    // }
    
    mprotect((void*)pageStart, 4096, PROT_READ);
    
    std::lock_guard<std::mutex> lock(g_timeMutex);
    g_timeState.funcFound = true;
    
    LOGI("Successfully hooked Level::getTime");
    return true;
}

// ===================== Main Thread =====================
static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);
    
    GHandle egl = GlossOpen("libEGL.so");
    if (!egl) return nullptr;
    void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    
    HookInput();
    
    if (!findAndHookTime()) {
        LOGE("Failed to hook time function");
    }
    
    return nullptr;
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

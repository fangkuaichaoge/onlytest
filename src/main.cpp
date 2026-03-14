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
static int g_callCounter = 0;
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
static bool g_hookTriggered = false;

// ===================== 定义 Molang 返回值结构 (简化版) =====================
// 注意：这是一个占位符结构，实际大小可能更大，但我们只关心如何设置 float 值
// 假设 MolangScriptArg 是一个包含 float 或 double 的联合体/类
struct MolangScriptArg {
    // 强制设置为 1.0 的辅助函数
    // 实际内存布局取决于游戏，但通常第一个字段就是 float/double
    void setToOne() {
        // 尝试将开头的 8 字节设为 double 1.0 或 4 字节设为 float 1.0
        // 这里我们两种都试，确保生效
        float* f_ptr = (float*)this;
        *f_ptr = 1.0f;
        
        double* d_ptr = (double*)this;
        *d_ptr = 1.0;
    }
};

// 原函数指针类型
// 签名: const MolangScriptArg& queryHandler(RenderParams&, const std::vector<ExpressionNode>&)
// 我们用 void* 来简化，因为我们只关心修改返回值
typedef const MolangScriptArg& (*OrigQueryFunc)(void*, void*, void*);
static OrigQueryFunc g_origQueryFunc = nullptr;

// ===================== Hook 回调函数 (核心逻辑) =====================
static const MolangScriptArg& hook_queryHandler(void* thisPtr, void* renderParams, void* exprNodes) {
    // 1. 先调用原函数获取返回值引用
    const MolangScriptArg& originalResult = g_origQueryFunc(thisPtr, renderParams, exprNodes);
    
    // 2. 记录日志 (防止刷屏，每60帧记一次)
    static int counter = 0;
    if (counter++ % 60 == 0) {
        LOGI_FMT("Query triggered! Spoofing to 1.0 (Calls: %d)", counter);
    }
    
    if (!g_hookTriggered) {
        g_hookTriggered = true;
        LOGI("!!! SUCCESS: Hook is active and modifying return value!");
    }

    // 3. 强制修改返回值为 1.0
    // 注意：因为返回的是 const 引用，我们需要去掉 const 限定符来修改
    MolangScriptArg& mutableResult = const_cast<MolangScriptArg&>(originalResult);
    mutableResult.setToOne();

    return originalResult;
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

    ImGui::Text("Target: query.is_persona_or_premium_skin");
    ImGui::Dummy(ImVec2(0, 4));

    if (g_hookInstalled) {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Status: Hooked");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Status: Installing...");
    }

    if (g_hookTriggered) {
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::TextColored(ImVec4(0.75f, 0.55f, 0.95f, 1.0f), "Spoofing: ACTIVE");
        ImGui::Text("Returning: 1.0");
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
    sleep(3);
    LOGI("========== SkinSpoofer Init ==========");
    GlossInit(true);

    // 清空旧日志
    std::ofstream f_clear(LOG_FILE, std::ios::trunc);
    if (f_clear.is_open()) f_clear.close();

    // 1. 基础 Hook (EGL & Input)
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
    // 3. 安装核心 Hook: query.is_persona_or_premium_skin
    // ==========================================
    LOGI("Installing Molang Query Hook...");
    
    // 来自你的文档: 注册函数 sub_F14DB90 (0xF14DB90)
    const uintptr_t targetOffset = 0xF14DB90;
    uintptr_t targetAddr = g_libBase + targetOffset;
    
    LOGI_FMT("Target Func: 0x%lx (Base + 0x%lx)", targetAddr, targetOffset);

    if (GlossHook((void*)targetAddr, (void*)hook_queryHandler, (void**)&g_origQueryFunc)) {
        LOGI("SUCCESS: Molang Query Hooked!");
        g_hookInstalled = true;
    } else {
        LOGE("FAILED: Could not hook Molang Query");
    }

    LOGI("========== Setup Complete ==========");
    return nullptr;
}

__attribute__((constructor))
void init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}

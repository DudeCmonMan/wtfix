// The in-game settings panel (Insert): Dear ImGui drawn from the patched DirectXDriver.present, so nothing in DirectX
// is hooked and other overlays (Steam's) are left alone.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "live.h"

extern "C" void wt_log(const char *format, ...);
extern "C" int wt_shadow_stats(char *out, int size);
extern "C" int wt_fxlook_stats(char *out, int size);
extern "C" int wt_skin_stats(char *out, int size);
extern "C" int wt_dlss_stats(char *out, int size);
extern "C" int wt_defer_stats(char *out, int size);
extern "C" void wt_dlss_startup(ID3D11Device *device, IDXGISwapChain *swapchain);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

struct DxDriver { // directx.hdll's dx_driver, its first fields
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGISwapChain *swapchain;
};
struct HlDynamic { // a boxed HL value: the abstract pointer follows the type
    void *type;
    DxDriver *ptr;
};

static WNDPROC real_wndproc;
static HWND window;
static ID3D11Device *device;
static ID3D11DeviceContext *context;
static bool ready, failed, shown;
static float res_scale = 1, ui_scale = 1;
static const char *status = "";

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && w == VK_INSERT) {
        if (msg == WM_KEYDOWN && !(l & (1 << 30))) shown = !shown;
        return 0;
    }
    if (shown) {
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        const ImGuiIO &io = ImGui::GetIO();
        bool mouse = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST, keys = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
        if ((mouse && io.WantCaptureMouse) || (keys && io.WantCaptureKeyboard)) return 0;
    }
    return CallWindowProcW(real_wndproc, h, msg, w, l);
}

static bool readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    if (!p || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    return (const char *)p + n <= (const char *)m.BaseAddress + m.RegionSize;
}

// Checks the driver layout guess: a COM object's vtable lies in its module's image.
static bool object_of(const void *obj, const wchar_t *module) {
    const unsigned char *base = (const unsigned char *)GetModuleHandleW(module);
    if (!base || !readable(obj, sizeof(void *))) return false;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const unsigned char *vt = *(const unsigned char *const *)obj;
    return vt >= base && vt < base + nt->OptionalHeader.SizeOfImage;
}

// The context is the device's own, asked for: its vtable is made at run time, outside any module to check it against.
static void use_device(ID3D11Device *dev) {
    device = dev;
    device->GetImmediateContext(&context);
    context->Release(); // the device keeps it alive; ImGui holds its own reference
}

static float wanted_scale(void) { return res_scale * 0.8f * (float)wt_live[LIVE_PANEL_SCALE].value; }

// ImGui's fonts are bitmaps of one size: a new scale rebuilds them rather than stretching them.
static void build_style(float scale) {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    wchar_t fonts[MAX_PATH], path[MAX_PATH];
    char utf8[MAX_PATH * 3];
    GetWindowsDirectoryW(fonts, MAX_PATH);
    swprintf(path, MAX_PATH, L"%ls\\Fonts\\segoeui.ttf", fonts);
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof utf8, NULL, NULL);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(utf8, 17.0f * scale)) {
        ImFontConfig font;
        font.SizePixels = 13.0f * (scale < 1.5f ? 1 : 2);
        io.Fonts->AddFontDefault(&font);
    }
    ImGuiStyle &style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.ScaleAllSizes(scale);
    ui_scale = scale;
}

static bool start(const DxDriver *d) {
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(d->swapchain->GetDesc(&desc)) || !desc.OutputWindow) return false;
    use_device(d->device);
    window = desc.OutputWindow;
    RECT r;
    GetClientRect(window, &r);
    res_scale = r.bottom > 1080 ? r.bottom / 1080.0f : 1.0f;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    build_style(wanted_scale());
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, context);
    real_wndproc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)wndproc);
    ready = true;
    wt_log("overlay: ready on the game window (%ldx%ld); Insert opens it", r.right, r.bottom);
    return true;
}

static void row(int id) {
    WtLive *s = &wt_live[id];
    ImGui::PushID(id);
    if (s->toggle) {
        bool on = s->value >= 0.5;
        if (ImGui::Checkbox(s->label, &on)) wt_live_change(id, on ? 1 : 0);
    } else {
        float v = (float)s->value;
        ImGui::SetNextItemWidth(280 * ui_scale);
        if (ImGui::SliderFloat(s->label, &v, (float)s->low, (float)s->high, s->format, ImGuiSliderFlags_AlwaysClamp)) {
            if (id == LIVE_ZOOM_MIN && v > wt_live[LIVE_ZOOM_MAX].value) v = (float)wt_live[LIVE_ZOOM_MAX].value;
            if (id == LIVE_ZOOM_MAX && v < wt_live[LIVE_ZOOM_MIN].value) v = (float)wt_live[LIVE_ZOOM_MIN].value;
            wt_live_change(id, v);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Game")) wt_live_reset(id);
    if (ImGui::IsItemHovered()) {
        char game[64];
        snprintf(game, sizeof game, s->toggle ? (s->game >= 0.5 ? "on" : "off") : s->format, s->game);
        ImGui::SetTooltip("Back to the game's own value: %s", game);
    }
    ImGui::PopID();
}

// Applied on release: a panel resized under the mouse would move the slider away from it.
static void scale_row(void) {
    static float v;
    static bool held;
    WtLive *s = &wt_live[LIVE_PANEL_SCALE];
    if (!held) v = (float)s->value;
    ImGui::SetNextItemWidth(280 * ui_scale);
    ImGui::SliderFloat(s->label, &v, (float)s->low, (float)s->high, s->format, ImGuiSliderFlags_AlwaysClamp);
    held = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) wt_live_change(LIVE_PANEL_SCALE, v);
}

static bool section(const char *name) { return ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen); }

static void panel(void) {
    ImGui::SetNextWindowPos(ImVec2(60 * ui_scale, 40 * ui_scale), ImGuiCond_FirstUseEver);
    ImGui::Begin("wtfix (Insert)", &shown, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    if (section("Battle camera")) {
        row(LIVE_ZOOM_MAX);
        row(LIVE_ZOOM_MIN);
        row(LIVE_BLUR);
        row(LIVE_FOG);
        row(LIVE_SOUND);
    }
    if (section("Unit HUDs")) {
        row(LIVE_HUD_FULL);
        row(LIVE_HUD_BARS);
    }
    if (section("Performance")) {
        row(LIVE_CULL_OFFSCREEN);
        row(LIVE_CULL_SMALL);
        row(LIVE_SHADOW_CACHE);
        row(LIVE_SHADOW_MARGIN);
        char shadow_stats[160];
        wt_shadow_stats(shadow_stats, sizeof shadow_stats);
        ImGui::TextDisabled("Shadow cache: %s", shadow_stats);
        row(LIVE_FX_LOOKAT);
        char fxlook_stats[96];
        wt_fxlook_stats(fxlook_stats, sizeof fxlook_stats);
        ImGui::TextDisabled("FX look-at: %s", fxlook_stats);
        row(LIVE_SUBSKIN_ONCE);
        row(LIVE_SKIN_C);
        char skin_stats[128];
        wt_skin_stats(skin_stats, sizeof skin_stats);
        ImGui::TextDisabled("Skin joints: %s", skin_stats);
        row(LIVE_DRAW_THREADS);
        char defer_stats[160];
        wt_defer_stats(defer_stats, sizeof defer_stats);
        ImGui::TextDisabled("Draw threads: %s", defer_stats);
    }
    if (section("Upscaling")) {
        row(LIVE_DLSS);
        int mode = (int)(wt_live[LIVE_DLSS_MODE].value + 0.5);
        ImGui::SetNextItemWidth(280 * ui_scale);
        if (ImGui::Combo("DLSS mode", &mode, wt_dlss_modes, 5)) wt_live_change(LIVE_DLSS_MODE, mode);
        row(LIVE_DLSS_SHARPEN);
        row(LIVE_DLSS_MIP_BIAS);
        char dlss_stats[160];
        wt_dlss_stats(dlss_stats, sizeof dlss_stats);
        ImGui::TextDisabled("DLSS: %s", dlss_stats);
    }
    ImGui::Separator();
    scale_row();
    if (ImGui::Button("Save to ini")) status = wt_live_save(wt_live_ini()) ? "could not write wtfix.ini" : "saved";
    ImGui::SameLine();
    ImGui::TextDisabled("%s", wt_live_dirty() ? "unsaved changes" : status);
    ImGui::TextDisabled("Changes apply in battle as you make them.");
    ImGui::End();
}

// The back buffer's view is made for the frame and released: the game's ResizeBuffers fails while one is held.
static void draw(IDXGISwapChain *chain) {
    ID3D11Texture2D *back = NULL;
    ID3D11RenderTargetView *target = NULL;
    if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back))) return;
    HRESULT hr = device->CreateRenderTargetView(back, NULL, &target);
    back->Release();
    if (FAILED(hr)) return;
    float scale = wanted_scale();
    if (scale != ui_scale) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        build_style(scale);
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    panel();
    ImGui::Render();
    ID3D11RenderTargetView *old_target = NULL;
    ID3D11DepthStencilView *old_depth = NULL;
    context->OMGetRenderTargets(1, &old_target, &old_depth);
    context->OMSetRenderTargets(1, &target, NULL);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(1, &old_target, old_depth);
    if (old_target) old_target->Release();
    if (old_depth) old_depth->Release();
    target->Release();
}

static void overlay_frame(const HlDynamic *boxed) {
    if (failed) return;
    const DxDriver *d = boxed && readable(boxed, sizeof *boxed) ? boxed->ptr : NULL;
    if (!readable(d, sizeof *d) || !object_of(d->device, L"d3d11.dll") || !object_of(d->swapchain, L"dxgi.dll")) {
        wt_log("overlay: OFF, the game's DirectX driver is not laid out as expected");
        failed = true;
        return;
    }
    wt_dlss_startup(d->device, d->swapchain); // at game start, where its delay goes unnoticed
    if (!ready && !start(d)) return;
    if (d->device != device) { // a new device after a device loss
        ImGui_ImplDX11_Shutdown();
        use_device(d->device);
        ImGui_ImplDX11_Init(device, context);
    }
    if (shown) draw(d->swapchain);
}

extern "C" __declspec(dllexport) void *hlp_overlay_frame(const char **sign) {
    *sign = NULL;
    return (void *)overlay_frame;
}

// Controller chord remapping: LT/RT + face button -> keyboard palette slot.
//
// Hook chain:
//   1. MinHook trampoline on dinput8!DirectInput8Create
//   2. vtable patch on IDirectInput8::CreateDevice to catch the joystick
//   3. vtable patch on IDirectInputDevice8::GetDeviceState on that joystick
//   4. Inside the joystick hook, read XInput (preferred) or DI fallback,
//      detect chord edges, SendInput the matching digit scancode, and zero
//      out the face buttons in the returned DI buffer so the game does NOT
//      see the underlying press (no double-action with attack).

#include <Windows.h>

// INITGUID + <initguid.h> before <dinput.h> so GUID_SysKeyboard / GUID_SysMouse
// are defined in this translation unit instead of needing dinput8.lib (which
// MinGW does not ship).
#define INITGUID
#include <initguid.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <MinHook.h>

#include <imgui.h>

// See the matching block in pixelated_mods.cpp for rationale. reshade.hpp uses
// GetProcAddress + reinterpret_cast to build its addon-event trampolines;
// MSVC's C4191 flags that cast unconditionally. Scoped suppression keeps
// the rest of this translation unit under /W4.
#pragma warning(push)
#pragma warning(disable : 4191)
#include <reshade.hpp>
#pragma warning(pop)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "controller_chords.hpp"
#include "pso_log.hpp"
#include "vtable_patch.hpp"

namespace chords {

using pixelated_mods::VtablePatch;

// ============================================================================
// Threading model
//
// Three threads touch state in this file:
//   - The loader thread (DllMain / chords::Install / chords::Uninstall)
//   - The render thread (ReShade overlay → RenderConfigPanelSection)
//   - The input thread (whatever thread pumps DirectInput polls; in
//     practice the game's main loop thread, which is different from
//     the ReShade render thread under DXVK / multi-renderer setups).
//
// Every piece of mutable cross-thread state below is a std::atomic.
// For the non-scalar `s_joy_button_ofs[16]`, we use a release/acquire
// fence on `s_joy_format_captured` as a publication flag: the writer
// fills the array then store-releases the flag; the reader
// load-acquires the flag and, if true, is guaranteed to see the array
// writes that happened before the release.
// ============================================================================

// ---------- Config state ----------

static std::atomic<bool> s_cc_enabled{false};
static std::atomic<int>  s_cc_trigger_threshold{96}; // 0-255; LT/RT held at >= this

// ---------- XInput (preferred input source) ----------
//
// DirectInput exposes the two triggers on a single combined Z axis, which
// means holding both LT and RT cancels to neutral and the 9/0 chord can
// never fire. XInput gives independent 0-255 bytes per trigger plus a
// guaranteed Xbox face-button mask, so we use it when available and fall
// back to DI otherwise. Loaded dynamically so we don't need an import lib.

typedef struct _XGAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX, sThumbLY;
    SHORT sThumbRX, sThumbRY;
} XGAMEPAD;

typedef struct _XSTATE {
    DWORD    dwPacketNumber;
    XGAMEPAD Gamepad;
} XSTATE;

constexpr WORD XBTN_A = 0x1000;
constexpr WORD XBTN_B = 0x2000;
constexpr WORD XBTN_X = 0x4000;
constexpr WORD XBTN_Y = 0x8000;

typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XSTATE *);

// s_xinput_GetState / s_xinput_available are set ONCE during Install()
// on the loader thread and never modified afterwards. Readers are
// synchronized by the happens-before from DllMain → chords::Install
// → the first hook fire, so no atomic wrapping is needed.
static XInputGetState_t s_xinput_GetState  = nullptr;
static bool             s_xinput_available = false;

// s_last_xinput is only touched by the input thread (written by
// PollXInput, read by DetectAndFireChord immediately after).
// RenderConfigPanelSection does its OWN fresh XInputGetState poll
// into a local on the render thread, so it never touches this
// global. Not shared cross-thread → no atomic.
static XSTATE           s_last_xinput      = {};

// s_last_xinput_rc IS shared: written by input thread (PollXInput),
// read by render thread (config panel's "yes/no controller" label).
static std::atomic<uint32_t> s_last_xinput_rc{0xFFFFFFFFu};

static void TryLoadXInput()
{
    if (s_xinput_GetState != nullptr) return;
    static const char *names[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (const char *name : names)
    {
        HMODULE h = LoadLibraryA(name);
        if (h == nullptr) continue;
        auto fn = reinterpret_cast<XInputGetState_t>(GetProcAddress(h, "XInputGetState"));
        if (fn != nullptr)
        {
            s_xinput_GetState  = fn;
            s_xinput_available = true;
            PSO_LOG("TryLoadXInput: loaded %s XInputGetState=0x%p", name, (void*)fn);
            return;
        }
    }
    PSO_LOG("TryLoadXInput: no xinput dll found, chord will use DI fallback");
}

static bool PollXInput()
{
    if (!s_xinput_available) return false;
    const uint32_t rc = s_xinput_GetState(0, &s_last_xinput);
    s_last_xinput_rc.store(rc, std::memory_order_relaxed);
    return rc == 0;  // ERROR_SUCCESS
}

// ---------- Hook state ----------
//
// s_hook_installed / s_orig_* / s_patched_*_vtable are all written
// once during Install/Uninstall (loader thread) and read by the
// input thread at hook-fire time. The Install→hook-fire ordering is
// serialized by the load-library / hook-install sequence so the
// input thread always sees the fully-initialized values. Not
// wrapped in atomic.

static bool s_hook_installed           = false;

static void *s_orig_CreateDevice       = nullptr;
static void *s_orig_GetDeviceState_joy = nullptr;
static void *s_orig_SetDataFormat_joy  = nullptr;
static void **s_patched_di8_vtable     = nullptr;
static void **s_patched_joy_vtable     = nullptr;

// Statistics displayed in the config panel. Incremented from the
// input thread, read from the render thread. relaxed ordering is
// fine — we don't need anyone to observe any particular "before"
// relationship with other state, just monotonically-increasing
// counts that can occasionally be stale by one frame.
static std::atomic<uint32_t> s_stat_create_device_calls{0};
static std::atomic<uint32_t> s_stat_getstate_joy_calls{0};
static std::atomic<uint32_t> s_stat_chord_fires{0};
static std::atomic<uint32_t> s_stat_sendinput_calls{0};
static std::atomic<uint32_t> s_stat_sendinput_fails{0};

// Deferred key-up state. SendScancodeTap fires the key-down immediately
// and records the matching key-up here; subsequent joystick polls drain
// it once the QPC deadline has passed. This is necessary because PSO's
// DirectInput keyboard poll runs at ~60 Hz — if down and up land on the
// same OS tick PSO sees at most one poll's worth of "held", which is
// unreliable for the single-press case. Holding ~30 ms gives the
// keyboard scanner two full polls to observe the held state.
static uint8_t       s_pending_up_scancode     = 0;     // 0 == none pending
static bool          s_pending_up_ctrl         = false; // release LCtrl too
static uint64_t      s_pending_up_deadline_qpc = 0;
static LARGE_INTEGER s_qpc_freq                = {};    // cached in Install

// Last-seen DI format size (so the panel can show DIJOYSTATE vs DIJOYSTATE2).
// Written input thread, read render thread.
static std::atomic<uint32_t> s_last_joy_cbdata{0};

// Joystick device pointers we've positively identified via
// GetCapabilities in Hook_CreateDevice. IDirectInputDevice8A instances
// share a single COM vtable across device types (joystick, mouse,
// keyboard), so patching the joystick's vtable[9] / vtable[11] also
// routes the mouse and keyboard through our hooks. We must filter by
// the `self` pointer so only real joysticks trigger chord logic —
// otherwise SetDataFormat captures the keyboard's format over the
// joystick's, and SuppressJoystickButtons zeros bytes in the keyboard
// state buffer (wiping our own SendInput'd DIK_1 / DIK_2 / etc).
static constexpr int      kMaxJoystickDevices = 4;
static IDirectInputDevice8A *s_joystick_devices[kMaxJoystickDevices] = {};
static int                s_joystick_device_count = 0;

static bool IsKnownJoystick(IDirectInputDevice8A *dev)
{
    for (int i = 0; i < s_joystick_device_count; ++i)
        if (s_joystick_devices[i] == dev) return true;
    return false;
}

// Mouse device tracking. Same shared-vtable deal as the joystick —
// we register every mouse PSO creates via CreateDevice so the shared
// GetDeviceState hook can distinguish mouse polls from joystick/
// keyboard polls and apply the wheel filter ONLY to the mouse path.
static constexpr int      kMaxMouseDevices = 2;
static IDirectInputDevice8A *s_mouse_devices[kMaxMouseDevices] = {};
static int                s_mouse_device_count = 0;

static bool IsKnownMouse(IDirectInputDevice8A *dev)
{
    for (int i = 0; i < s_mouse_device_count; ++i)
        if (s_mouse_devices[i] == dev) return true;
    return false;
}

// Wheel filter state.
//
//   s_mouse_last_wheel_tick — GetTickCount() at the most recent
//     wheel event we observed (passed or blocked). Used by the
//     quiet-gap detector: a wheel event passes only if at least
//     g_wheel_throttle_ms have elapsed since the last one.
//
//   s_mouse_last_raw_lz — the raw lZ value PSO's mouse device
//     returned last call. Needed because PSO's DI mouse is in
//     ABSOLUTE axis mode when there's no physical mouse plugged
//     in (Windows promotes the touchpad to GUID_SysMouse and the
//     touchpad's Z axis is reported as a cumulative position,
//     not a per-poll delta). In that mode lZ plateaus at the
//     last accumulated value across many polls, which our filter
//     needs to translate to "no new input, delta = 0" rather
//     than "wheel is held at -7204 forever". Tracking the raw
//     value ourselves lets us compute a real delta and rewrite
//     the buffer as a sanitized one-click value per gesture.
static DWORD s_mouse_last_wheel_tick = 0;
static LONG  s_mouse_last_raw_lz     = 0;
static std::atomic<uint32_t> s_stat_mouse_wheel_passes{0};
static std::atomic<uint32_t> s_stat_mouse_wheel_blocks{0};

// Thunks exposed by pixelated_mods.cpp so the chord TU can read the
// wheel filter settings without the globals being directly visible
// across translation units (they live in pixelated_mods.cpp's
// anonymous namespace).
extern "C" bool WheelFilter_Enabled();
extern "C" int  WheelFilter_ThrottleMs();
extern "C" int  WheelFilter_Mode();       // 0=off 1=smart 2=block_tp
extern "C" bool WheelFilter_DebugLog();

// Right-stick config — see pixelated_mods.cpp for the global
// definitions and semantics.
extern "C" bool StickMap_Enabled();
extern "C" bool StickMap_ZoomEnabled();
extern "C" bool StickMap_InvertY();
extern "C" int  StickMap_Deadzone();
extern "C" int  StickMap_RateMs();
extern "C" int  StickMap_LeftScancode();
extern "C" int  StickMap_RightScancode();

// Debug log counter. Bounded to kMaxDebugLogLines so the log file
// doesn't flood during a long test session.
static constexpr int kMaxWheelDebugLogLines = 500;
static std::atomic<int> s_wheel_debug_log_count{0};
static bool s_wheel_debug_log_last = false;  // for edge detect

// PSO uses a non-standard joystick DIDATAFORMAT — the GetDeviceState hook
// log shows PSO polling with cbData=20, which is way smaller than
// DIJOYSTATE (80 B) or DIJOYSTATE2 (272 B). Same situation as the keyboard
// custom format issue: we have to intercept SetDataFormat on the device,
// walk the rgodf[] array, and learn where each input object lives in
// PSO's compact buffer so we can suppress the right bytes when a chord
// fires.
//
// Publication pattern for cross-thread visibility:
// the writer (Hook_SetDataFormat_Joystick, input thread) fills
// s_joy_format_size + s_joy_button_ofs[], then store-releases
// s_joy_format_captured = true. Any reader (input thread for the
// suppress path, render thread for the config panel display) that
// load-acquires s_joy_format_captured and sees true is guaranteed
// to observe all prior writes.
static std::atomic<uint32_t> s_joy_format_size{0};
static uint16_t              s_joy_button_ofs[16];
static std::atomic<bool>     s_joy_format_captured{false};

// Edge-detection prev state for the four face buttons (A/B/X/Y).
static uint8_t s_prev_face[4] = {0, 0, 0, 0};

// XInput bumper masks. XBTN_A/B/X/Y (face buttons) are defined higher up
// at 0x1000..0x8000. The Right Bumper is used as the Ctrl modifier —
// holding RB during a chord emits Ctrl+digit instead of digit, giving
// access to PSO's second palette set (slots 11-20). The Left Bumper is
// intentionally NOT suppressed because it's the in-game camera control.
constexpr WORD XBTN_LB = 0x0100;
constexpr WORD XBTN_RB = 0x0200;

// ---------- Hook typedefs ----------

typedef HRESULT (WINAPI *DirectInput8Create_t)(
    HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
static DirectInput8Create_t s_real_DirectInput8Create = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *CreateDevice_t)(
    IDirectInput8A *, REFGUID, LPDIRECTINPUTDEVICE8A *, LPUNKNOWN);
static CreateDevice_t s_real_CreateDevice = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *GetDeviceState_t)(
    IDirectInputDevice8A *, DWORD, LPVOID);
static GetDeviceState_t s_real_GetDeviceState_joy = nullptr;

// GetDeviceData for the buffered-data path on the shared device
// vtable. PSO reads the mouse wheel through this call in addition to
// GetDeviceState — polling alone wasn't enough to stop the touchpad
// runaway, so we also hook vtable[10] and filter wheel records in
// the DIDEVICEOBJECTDATA array.
typedef HRESULT (STDMETHODCALLTYPE *GetDeviceData_t)(
    IDirectInputDevice8A *, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static GetDeviceData_t s_real_GetDeviceData_shared = nullptr;
static void *s_orig_GetDeviceData_shared = nullptr;

// ---------- Keyboard injection via SendInput ----------
//
// PSO reads palette hotkeys through the Windows message loop (WM_KEYDOWN),
// not via IDirectInputDevice8::GetDeviceState on the keyboard — so poking
// bytes into the DI keyboard state buffer does NOT trigger palette slots.
// SendInput with KEYEVENTF_SCANCODE generates an OS-level input event
// which drives the same paths a physical keypress does.
//
// DIK scancodes for the number row equal the hardware scancodes used by
// KEYEVENTF_SCANCODE: DIK_1 = 0x02, DIK_2 = 0x03, ..., DIK_0 = 0x0B.

static uint8_t DigitToScanCode(char digit)
{
    switch (digit)
    {
    case '1': return 0x02;
    case '2': return 0x03;
    case '3': return 0x04;
    case '4': return 0x05;
    case '5': return 0x06;
    case '6': return 0x07;
    case '7': return 0x08;
    case '8': return 0x09;
    case '9': return 0x0A;
    case '0': return 0x0B;
    default:  return 0;
    }
}

// Fire the deferred key-up (and, if set, the matching Ctrl up) recorded
// by a previous SendScancodeTap. `force=false` only releases once the
// ~30 ms deadline has passed — the normal polling path. `force=true`
// releases immediately, used by the incoming-chord and Uninstall paths
// so we never leave a key stuck down.
static void DrainPendingKeyUp(bool force)
{
    if (s_pending_up_scancode == 0) return;

    if (!force)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (static_cast<uint64_t>(now.QuadPart) < s_pending_up_deadline_qpc)
            return;
    }

    constexpr uint8_t kLCtrl = 0x1D;
    INPUT up[2] = {};
    int n = 0;
    up[n].type       = INPUT_KEYBOARD;
    up[n].ki.wScan   = s_pending_up_scancode;
    up[n].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    ++n;
    if (s_pending_up_ctrl)
    {
        up[n].type       = INPUT_KEYBOARD;
        up[n].ki.wScan   = kLCtrl;
        up[n].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
        ++n;
    }
    SendInput(n, up, sizeof(INPUT));
    PSO_LOG("SendScancodeTap(up):   scancode=0x%02X ctrl=%d force=%d",
            s_pending_up_scancode, s_pending_up_ctrl ? 1 : 0, force ? 1 : 0);

    s_pending_up_scancode     = 0;
    s_pending_up_ctrl         = false;
    s_pending_up_deadline_qpc = 0;
}

// Fire a palette-slot keypress. If `with_ctrl` is true, the sequence is
// (Ctrl down, digit down) now, then (digit up, Ctrl up) ~30 ms later so
// PSO reads "Ctrl + digit" — which in PSO's default keybinds accesses
// the SECOND palette set (action slots 11-20) instead of the first set.
// That's the RB-modifier chord path (RB held during a chord).
//
// Timing matters: PSO polls the DirectInput keyboard at ~60 Hz and only
// reacts to a slot key when it sees the key held on at least one poll.
// The old code fired down+up in a single SendInput call, so both events
// landed on the same OS tick — single presses were unreliable. Now the
// down fires immediately and the up is queued in s_pending_up_*, to be
// drained by DrainPendingKeyUp() on a later joystick hook call once the
// ~30 ms deadline has passed.
static void SendScancodeTap(uint8_t scancode, bool with_ctrl)
{
    if (scancode == 0) return;

    // A pending up from a previous chord is forced out immediately — we
    // must not leave two scancodes held simultaneously, which would look
    // like Ctrl+digit to PSO even when the caller asked for a plain digit.
    DrainPendingKeyUp(/*force=*/true);

    constexpr uint8_t kLCtrl = 0x1D; // hardware scancode for Left Ctrl

    INPUT down[2] = {};
    int n = 0;
    if (with_ctrl)
    {
        down[n].type       = INPUT_KEYBOARD;
        down[n].ki.wScan   = kLCtrl;
        down[n].ki.dwFlags = KEYEVENTF_SCANCODE;
        ++n;
    }
    down[n].type       = INPUT_KEYBOARD;
    down[n].ki.wScan   = scancode;
    down[n].ki.dwFlags = KEYEVENTF_SCANCODE;
    ++n;

    SetLastError(0);
    const UINT sent = SendInput(n, down, sizeof(INPUT));
    const DWORD err = (static_cast<int>(sent) == n) ? 0 : GetLastError();
    s_stat_sendinput_calls.fetch_add(1, std::memory_order_relaxed);
    if (static_cast<int>(sent) != n)
        s_stat_sendinput_fails.fetch_add(1, std::memory_order_relaxed);

    // Queue the key-up. 30 ms == ~2 PSO keyboard polls, leaving enough
    // headroom above the 16 ms poll period that PSO is guaranteed to
    // see the key held on at least one tick.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s_pending_up_scancode     = scancode;
    s_pending_up_ctrl         = with_ctrl;
    s_pending_up_deadline_qpc = static_cast<uint64_t>(now.QuadPart)
        + (static_cast<uint64_t>(s_qpc_freq.QuadPart) * 30ULL / 1000ULL);

    PSO_LOG("SendScancodeTap(down): scancode=0x%02X ctrl=%d sent=%u/%d err=%lu",
            scancode, with_ctrl ? 1 : 0, sent, n, (unsigned long)err);
}

// ---------- Synthetic mouse wheel + tab keypress ----------
//
// Right-stick mappings synthesize these at a rate limit controlled
// by the stick polling code. The wheel path reaches PSO through our
// own DI mouse hook (Hook_GetDeviceState_Joystick mouse branch) —
// in wheel filter mode 1 (smart / delta synthesis) the injected
// ±WHEEL_DELTA turns into a clean zoom step; in mode 2 (block all)
// it gets eaten and stick zoom silently dies. Documented limitation.

static void SendWheelTap(int delta)
{
    INPUT in = {};
    in.type         = INPUT_MOUSE;
    in.mi.dwFlags   = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(delta);
    const UINT sent = SendInput(1, &in, sizeof(INPUT));
    s_stat_sendinput_calls.fetch_add(1, std::memory_order_relaxed);
    if (sent != 1)
        s_stat_sendinput_fails.fetch_add(1, std::memory_order_relaxed);
}

// Fire a single scancode (down + up) via SendInput. Used by the
// right-stick X-axis mapping to send whichever key the user has
// configured for left/right pushes. No deferred key-up: stick
// events are already rate-limited and each push is a discrete
// short tap, so down+up in a single SendInput call is fine.
static void SendScancodeSimple(uint8_t scancode)
{
    if (scancode == 0) return;

    INPUT events[2] = {};
    events[0].type       = INPUT_KEYBOARD;
    events[0].ki.wScan   = scancode;
    events[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    events[1].type       = INPUT_KEYBOARD;
    events[1].ki.wScan   = scancode;
    events[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

    const UINT sent = SendInput(2, events, sizeof(INPUT));
    s_stat_sendinput_calls.fetch_add(1, std::memory_order_relaxed);
    if (sent != 2)
        s_stat_sendinput_fails.fetch_add(1, std::memory_order_relaxed);
}

// ---------- Trigger classification + chord logic ----------

static void ClassifyTriggers(const DIJOYSTATE2 *js, bool *out_lt, bool *out_rt)
{
    const int threshold = s_cc_trigger_threshold.load(std::memory_order_relaxed);

    if (s_last_xinput_rc.load(std::memory_order_relaxed) == 0)
    {
        *out_lt = s_last_xinput.Gamepad.bLeftTrigger  >= threshold;
        *out_rt = s_last_xinput.Gamepad.bRightTrigger >= threshold;
        return;
    }

    // DI fallback — combined Z axis. Holding BOTH cancels to rest, so this
    // path cannot detect the LT+RT modifier; use XInput for 9/0.
    const int threshold32 = (threshold * 32768) / 255;
    const int z = static_cast<int>(js->lZ) - 32768;
    *out_lt = (z >  threshold32);
    *out_rt = (z < -threshold32);
}

// Latched state from the most recent DetectAndFireChord call, used by
// SuppressJoystickButtons to decide whether to zero out the buttons in
// the next GetDeviceState buffer PSO reads. Written on the input
// thread by DetectAndFireChord, read by the same thread inside
// SuppressJoystickButtons a moment later and by the render thread
// for the config panel display. Atomic for the cross-thread case.
static std::atomic<bool> s_chord_suppress_active{false};

// Latest modifier state, consumed by the overlay renderer to draw the
// on-screen chord guide. Updated on every poll from DetectAndFireChord
// so the overlay tracks LT/RT/RB within one input frame.
static std::atomic<bool> s_held_lt{false};
static std::atomic<bool> s_held_rt{false};
static std::atomic<bool> s_held_rb{false};

// Per-slot chord fire state. Indexed 0..9 where index = (digit == '0' ? 9
// : digit - '1'). Packed u64: high 32 bits are a monotonic counter shared
// across all slots, low 32 bits are GetTickCount() at the fire moment.
// The counter is what the render thread uses to detect a new fire — two
// taps landing in the same tick still increment the counter, so a rapid
// double-tap still re-triggers the flash animation.
static std::atomic<uint64_t> s_slot_fire[10] = {};
static std::atomic<uint32_t> s_slot_fire_counter{0};

// Forward decl — PollRightStick is defined after DetectAndFireChord
// so it can sit next to the per-stick state it uses, but the chord
// function needs to call it at the end of its run.
static void PollRightStick();

// Detect the current chord state from XInput (preferred) or the DI
// combined Z axis (fallback), edge-detect face button presses, and fire
// SendInput for the matching palette digit if a chord just triggered.
// Also latches s_chord_suppress_active for the suppression pass.
//
// Called from the joystick GetDeviceState hook on every poll; edge
// detection prevents a single button press from re-firing the chord on
// multiple polls within the same held press.
static void DetectAndFireChord(const DIJOYSTATE2 *js_or_null)
{
    if (!s_cc_enabled.load(std::memory_order_relaxed))
    {
        s_chord_suppress_active.store(false, std::memory_order_relaxed);
        return;
    }

    PollXInput();
    const bool xinput_ok = s_last_xinput_rc.load(std::memory_order_relaxed) == 0;

    bool lt = false, rt = false;
    ClassifyTriggers(js_or_null, &lt, &rt);

    // RB as the Ctrl modifier. Holding RB during a chord makes SendInput
    // emit Ctrl+digit instead of digit, which hits PSO's second palette
    // set (action slots 11-20). Only meaningful when XInput is available;
    // without XInput we can't reliably know which DI button index is RB,
    // and the modifier is simply ignored.
    //
    // LB is deliberately NOT used as a modifier here — in PSO's default
    // binding, LB controls the in-game camera, and stealing it would
    // break camera control during chord play.
    bool rb_held = false;
    if (xinput_ok)
        rb_held = (s_last_xinput.Gamepad.wButtons & XBTN_RB) != 0;

    uint8_t face[4];
    if (xinput_ok)
    {
        const WORD b = s_last_xinput.Gamepad.wButtons;
        face[0] = (b & XBTN_A) ? 0x80 : 0x00;
        face[1] = (b & XBTN_B) ? 0x80 : 0x00;
        face[2] = (b & XBTN_X) ? 0x80 : 0x00;
        face[3] = (b & XBTN_Y) ? 0x80 : 0x00;
    }
    else if (js_or_null != nullptr)
    {
        // DI fallback — assume standard Xbox HID button layout. Won't
        // work on PSO's custom 20-byte format, but that's OK because in
        // the typical deployment (user has a real XInput controller),
        // we never reach this branch.
        face[0] = js_or_null->rgbButtons[0];
        face[1] = js_or_null->rgbButtons[1];
        face[2] = js_or_null->rgbButtons[2];
        face[3] = js_or_null->rgbButtons[3];
    }
    else
    {
        // No XInput, no buffer — nothing to do.
        s_chord_suppress_active.store(false, std::memory_order_relaxed);
        return;
    }

    auto edge_down = [](uint8_t prev, uint8_t cur) {
        return prev < 0x80 && cur >= 0x80;
    };

    // Chord map — face buttons ordered A/X/Y/B so the thumb walks
    // counter-clockwise around the diamond (bottom → left → top →
    // right) instead of bouncing diagonally. Same traversal is used
    // for every modifier row so slots 1-4, 5-8, and 9-10 all share
    // the identical button order (LT+RT only uses the first two:
    // A and X).
    //
    // face[] indices (from the XInput button packing higher up):
    //   face[0] = A, face[1] = B, face[2] = X, face[3] = Y.
    //
    //   LT  + A/X/Y/B  -> 1 / 2 / 3 / 4
    //   RT  + A/X/Y/B  -> 5 / 6 / 7 / 8
    //   LT+RT + A/X    -> 9 / 0
    //
    // RB (Ctrl modifier, palette set 2) uses the same table; the
    // Ctrl+digit keystroke is applied at SendScancodeTap time, not
    // here.
    char digit = 0;
    if (lt && rt)
    {
        if      (edge_down(s_prev_face[0], face[0])) digit = '9';  // A
        else if (edge_down(s_prev_face[2], face[2])) digit = '0';  // X
    }
    else if (lt)
    {
        if      (edge_down(s_prev_face[0], face[0])) digit = '1';  // A
        else if (edge_down(s_prev_face[2], face[2])) digit = '2';  // X
        else if (edge_down(s_prev_face[3], face[3])) digit = '3';  // Y
        else if (edge_down(s_prev_face[1], face[1])) digit = '4';  // B
    }
    else if (rt)
    {
        if      (edge_down(s_prev_face[0], face[0])) digit = '5';  // A
        else if (edge_down(s_prev_face[2], face[2])) digit = '6';  // X
        else if (edge_down(s_prev_face[3], face[3])) digit = '7';  // Y
        else if (edge_down(s_prev_face[1], face[1])) digit = '8';  // B
    }

    if (digit != 0)
    {
        SendScancodeTap(DigitToScanCode(digit), rb_held);
        s_stat_chord_fires.fetch_add(1, std::memory_order_relaxed);

        // Publish a per-slot fire event for the overlay renderer so it
        // can flash the exact slot that just got triggered. Digit char
        // '1'..'9' maps to slot indices 0..8; '0' maps to slot 9.
        const int slot_idx = (digit == '0') ? 9 : (digit - '1');
        if (slot_idx >= 0 && slot_idx < 10)
        {
            const uint32_t ctr =
                s_slot_fire_counter.fetch_add(1, std::memory_order_relaxed) + 1;
            const uint32_t tick = GetTickCount();
            const uint64_t packed = (uint64_t(ctr) << 32) | uint64_t(tick);
            s_slot_fire[slot_idx].store(packed, std::memory_order_relaxed);
        }
    }

    // Latch suppression state for the next SuppressJoystickButtons call.
    // Active ONLY while LT or RT is held — the bumpers themselves stay
    // pass-through so PSO keeps seeing LB (camera control) and RB
    // (Ctrl modifier is injected via keyboard, but PSO can still see RB
    // as a gamepad button; its native binding is whatever the user has
    // configured in the Pad Button Config menu).
    s_chord_suppress_active.store(lt || rt, std::memory_order_relaxed);

    // Publish the held-modifier snapshot for the overlay renderer.
    s_held_lt.store(lt,       std::memory_order_relaxed);
    s_held_rt.store(rt,       std::memory_order_relaxed);
    s_held_rb.store(rb_held,  std::memory_order_relaxed);

    s_prev_face[0] = face[0];
    s_prev_face[1] = face[1];
    s_prev_face[2] = face[2];
    s_prev_face[3] = face[3];

    // Right stick → synthetic wheel + tab. Runs on every chord poll
    // so it rides the joystick poll rate (~60 Hz) without needing
    // its own timer. The rate-limiting lives inside PollRightStick.
    PollRightStick();
}

// Right-stick state between polls. Independent rate limiters for the
// two directions so a held stick can produce steady-tempo clicks on
// one axis while the other is idle.
static DWORD s_stick_last_y_tick = 0;
static DWORD s_stick_last_x_tick = 0;

// Read the XInput right stick (requires s_last_xinput populated by
// PollXInput earlier in the frame) and synthesize wheel / tab events
// when the stick has been pushed past the deadzone far enough that
// one axis dominates the other. Called from the same hook as the
// chord dispatch so it rides the joystick poll cadence.
static void PollRightStick()
{
    if (!StickMap_Enabled()) return;
    if (s_last_xinput_rc.load(std::memory_order_relaxed) != 0) return;

    const SHORT ry = s_last_xinput.Gamepad.sThumbRY;
    const SHORT rx = s_last_xinput.Gamepad.sThumbRX;
    const int ay = (ry < 0) ? -static_cast<int>(ry) : static_cast<int>(ry);
    const int ax = (rx < 0) ? -static_cast<int>(rx) : static_cast<int>(rx);

    const int deadzone = StickMap_Deadzone();
    if (ax < deadzone && ay < deadzone) return;   // resting / drift

    // Axis dominance: the winning axis must be at least 1.5x the
    // loser OR the loser must be below its own deadzone. Expressed
    // as integer multiplies: 2 * winner >= 3 * loser ⇔ winner/loser >= 1.5.
    const bool y_dominates = (2 * ay >= 3 * ax);
    const bool x_dominates = (2 * ax >= 3 * ay);
    // When both expressions are false the push is near a 45° diagonal —
    // suppress entirely so the stick doesn't trigger both axes at once.
    if (!y_dominates && !x_dominates) return;

    const DWORD now  = GetTickCount();
    const DWORD rate = static_cast<DWORD>(StickMap_RateMs());

    if (y_dominates && ay >= deadzone && StickMap_ZoomEnabled())
    {
        if (now - s_stick_last_y_tick >= rate)
        {
            s_stick_last_y_tick = now;
            // Stick up = ry positive = zoom IN by default. Invert
            // flips the sign so stick up becomes zoom out.
            bool zoom_in = (ry > 0);
            if (StickMap_InvertY()) zoom_in = !zoom_in;
            SendWheelTap(zoom_in ? WHEEL_DELTA : -WHEEL_DELTA);
        }
    }
    else if (x_dominates && ax >= deadzone)
    {
        const int scancode = (rx > 0) ? StickMap_RightScancode()
                                      : StickMap_LeftScancode();
        if (scancode != 0 && now - s_stick_last_x_tick >= rate)
        {
            s_stick_last_x_tick = now;
            SendScancodeSimple(static_cast<uint8_t>(scancode));
        }
    }
}

// Thread-safe snapshot of the latest chord-modifier state. Safe to call
// from the render thread. If the chord system is disabled, reports
// everything as not held.
HeldState GetHeldState()
{
    HeldState s{};
    s.enabled = s_cc_enabled.load(std::memory_order_relaxed);
    if (!s.enabled) return s;
    s.lt = s_held_lt.load(std::memory_order_relaxed);
    s.rt = s_held_rt.load(std::memory_order_relaxed);
    s.rb = s_held_rb.load(std::memory_order_relaxed);
    return s;
}

// Snapshot of every slot's last-fire counter and tick. The render
// thread uses this to detect new fires (counter change) and to drive
// the flash animation (tick-based elapsed time).
FireEvents GetFireEvents()
{
    FireEvents e{};
    for (int i = 0; i < 10; ++i)
    {
        const uint64_t p = s_slot_fire[i].load(std::memory_order_relaxed);
        e.last_counter[i] = static_cast<uint32_t>(p >> 32);
        e.last_tick[i]    = static_cast<uint32_t>(p & 0xFFFFFFFFu);
    }
    return e;
}

// Suppress the four face buttons (A / B / X / Y) in a DI state buffer so
// PSO sees them as NOT pressed while a chord trigger is held. Does NOT
// touch the bumpers — LB is the in-game camera control and RB is the
// chord Ctrl modifier, both of which need to remain visible to PSO.
//
// Two buffer formats are handled, in priority order:
//
//   1. Standard DIJOYSTATE / DIJOYSTATE2 — `cbData == sizeof(DIJOYSTATE)`
//      (80) or `cbData == sizeof(DIJOYSTATE2)` (272). Zero rgbButtons
//      [0..3] and leave the rest alone. Preferred when it applies,
//      because when a game uses one of the standard formats its rgodf[]
//      entries are tagged with DIDFT_ANYINSTANCE and the SetDataFormat
//      hook can't extract per-button offsets from them — so any custom
//      offsets we stashed would be empty and wrong.
//
//   2. Custom format — `cbData == s_joy_format_size` *and* we captured
//      real (non-0xFFFF) button offsets from SetDataFormat. Writes zero
//      to the captured byte offsets for button instances 0..3.
//
// Unknown buffer sizes are left completely untouched.
static void SuppressJoystickButtons(void *lpvData, DWORD cbData)
{
    if (!s_chord_suppress_active.load(std::memory_order_relaxed) ||
        lpvData == nullptr)
        return;

    // Standard DIJOYSTATE / DIJOYSTATE2 path first. When the game uses
    // c_dfDIJoystick / c_dfDIJoystick2, buttons live at the fixed
    // offsets defined by DIJOYSTATE::rgbButtons[0..127] (48 bytes in).
    if (cbData == sizeof(DIJOYSTATE) || cbData == sizeof(DIJOYSTATE2))
    {
        DIJOYSTATE2 *js = reinterpret_cast<DIJOYSTATE2 *>(lpvData);
        for (int i = 0; i < 4; ++i)
            js->rgbButtons[i] = 0;
        return;
    }

    // Custom-format path for games that register their own compact
    // DIDATAFORMAT (PSO's old 20-byte joystick format is the canonical
    // example). Only useful when SetDataFormat actually captured real
    // offsets — if every slot is still 0xFFFF the capture failed and
    // we silently skip rather than zero arbitrary bytes.
    if (s_joy_format_captured.load(std::memory_order_acquire) &&
        cbData == s_joy_format_size.load(std::memory_order_relaxed))
    {
        uint8_t *buf = static_cast<uint8_t *>(lpvData);
        for (int i = 0; i < 4; ++i)
        {
            const uint16_t ofs = s_joy_button_ofs[i];
            if (ofs != 0xFFFF && ofs < cbData)
                buf[ofs] = 0;
        }
    }
}

// ---------- Hooks ----------

typedef HRESULT (STDMETHODCALLTYPE *SetDataFormat_t)(
    IDirectInputDevice8A *, LPCDIDATAFORMAT);
static SetDataFormat_t s_real_SetDataFormat_joy = nullptr;

// Intercept IDirectInputDevice8::SetDataFormat on the joystick device.
// When PSO (or anything else) registers its custom data format, we walk
// the rgodf[] array to learn the byte offsets of every input object,
// then stash the per-button offsets so SuppressJoystickButtons can zero
// the correct bytes in PSO's compact buffer.
//
// Runs exactly once in practice (PSO calls SetDataFormat once at startup
// and never again), but the code is safe to call repeatedly — each call
// re-parses the format from scratch.
static HRESULT STDMETHODCALLTYPE Hook_SetDataFormat_Joystick(
    IDirectInputDevice8A *self, LPCDIDATAFORMAT lpdf)
{
    HRESULT hr = s_real_SetDataFormat_joy(self, lpdf);

    // The mouse and keyboard share this COM vtable with the joystick,
    // so this hook is also invoked for their SetDataFormat calls. Only
    // capture the format layout when the caller is a known joystick;
    // otherwise we overwrite the real joystick format with the last
    // keyboard call (256 bytes, scancode-indexed) and later zero out
    // DIK_1 / DIK_2 in the keyboard state buffer.
    if (SUCCEEDED(hr) && lpdf != nullptr && IsKnownJoystick(self))
    {
        // Reset first (publication-pattern writer side). Readers
        // that load-acquire s_joy_format_captured==true are
        // guaranteed to see the array writes that happened before
        // the store-release below.
        s_joy_format_captured.store(false, std::memory_order_relaxed);
        for (int i = 0; i < 16; ++i) s_joy_button_ofs[i] = 0xFFFF;
        s_joy_format_size.store(lpdf->dwDataSize, std::memory_order_relaxed);

        int logged = 0;
        const LPDIOBJECTDATAFORMAT objs = lpdf->rgodf;
        for (DWORD i = 0; i < lpdf->dwNumObjs; ++i)
        {
            const DIOBJECTDATAFORMAT &obj = objs[i];
            if (!(DIDFT_GETTYPE(obj.dwType) & DIDFT_BUTTON))
                continue;
            const DWORD instance = DIDFT_GETINSTANCE(obj.dwType);
            if (instance < 16 && obj.dwOfs < 0x10000)
            {
                s_joy_button_ofs[instance] =
                    static_cast<uint16_t>(obj.dwOfs);
                if (logged < 10)
                {
                    PSO_LOG("Hook_SetDataFormat_Joystick: button[%lu] -> ofs 0x%lX",
                            (unsigned long)instance,
                            (unsigned long)obj.dwOfs);
                    ++logged;
                }
            }
        }
        // Store-release publishes everything written above to any
        // thread that load-acquires this flag.
        s_joy_format_captured.store(true, std::memory_order_release);

        PSO_LOG("Hook_SetDataFormat_Joystick: dwDataSize=%lu dwNumObjs=%lu captured=yes",
                (unsigned long)lpdf->dwDataSize,
                (unsigned long)lpdf->dwNumObjs);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_GetDeviceState_Joystick(
    IDirectInputDevice8A *self, DWORD cbData, LPVOID lpvData)
{
    // Release any deferred palette-slot key whose ~30 ms down-hold has
    // elapsed. Runs on every incoming call (~60 Hz across all devices
    // that share this vtable) so latency from deadline-hit to actual
    // key-up is at most one poll. Safe to run for any device.
    DrainPendingKeyUp(/*force=*/false);

    HRESULT hr = s_real_GetDeviceState_joy(self, cbData, lpvData);

    // --- Mouse wheel rate limiter ---
    //
    // Vtable sharing means this same hook receives mouse polls from
    // PSO's DirectInput mouse device (it's the only input path that
    // survived hooking every other wheel candidate — WM_MOUSEWHEEL at
    // the WndProc layer did nothing because PSO bypasses messages).
    // When we recognise the mouse pointer, apply the wheel throttle
    // directly on the buffer: zero lZ if we let another non-zero lZ
    // through less than kWheelThrottleMs ago; otherwise normalize
    // small touchpad deltas up to one WHEEL_DELTA so the game's menu
    // accumulator crosses the 120-unit scroll threshold on the first
    // poll of each gesture (physical wheel clicks already arrive at
    // WHEEL_DELTA or a multiple and pass through unchanged).
    //
    // DIMOUSESTATE / DIMOUSESTATE2 both have { LONG lX; LONG lY;
    // LONG lZ; ... } so lZ is at +8. Guard on cbData >= 12 before
    // writing.
    if (SUCCEEDED(hr) && lpvData != nullptr && IsKnownMouse(self))
    {
        if (WheelFilter_Enabled() && cbData >= 12)
        {
            LONG *plZ = reinterpret_cast<LONG *>(
                static_cast<uint8_t *>(lpvData) + 8);
            const LONG raw_lz = *plZ;
            const int  mode   = WheelFilter_Mode();
            const DWORD now   = GetTickCount();

            // Mode 0 just logs. Modes 1 and 2 do real work.
            if (mode != 0)
            {
                // Delta synthesis — the actual fix.
                //
                // PSO's DI mouse sits in absolute-axis mode when
                // Windows promotes the touchpad to GUID_SysMouse
                // (no physical mouse plugged in). In that state lZ
                // is a cumulative position that plateaus at the
                // last accumulated value and only changes on real
                // input. Tracking the raw value ourselves gives us
                // a per-poll delta that works in BOTH modes:
                //   * Relative: DI resets lZ each call, so
                //     s_mouse_last_raw_lz stays 0 and delta == raw.
                //   * Absolute: raw is cumulative, delta is the
                //     real change since the previous poll.
                //
                // Plateau (delta == 0) → emit 0, PSO sees no
                //   movement, doesn't scroll. No quiet-gap rate
                //   limiting, no special timing logic.
                //
                // Non-zero delta → it's actual new input. Emit a
                //   sign-preserving ±WHEEL_DELTA (one clean click
                //   per poll of movement) so sustained gestures
                //   produce sustained clicks at PSO's poll rate
                //   (~30 Hz), which is PSO's scroll rate during a
                //   "held" wheel.
                //
                // This replaces the earlier quiet-gap approach
                // which rate-limited the filter to one click per
                // 200 ms — that killed continuous scroll because
                // sustained gestures only ever produced the first
                // click. The fix was a symptom-chaser: the real
                // bug was always the absolute-mode plateau, and
                // delta synthesis is the targeted fix for it.
                const LONG delta = raw_lz - s_mouse_last_raw_lz;
                s_mouse_last_raw_lz = raw_lz;

                bool blocked = false;
                if (delta == 0)
                {
                    // Plateau or quiet — no movement.
                    *plZ = 0;
                }
                else if (mode == 2)
                {
                    // block_all: drop every wheel event.
                    *plZ = 0;
                    blocked = true;
                }
                else // mode == 1, smart
                {
                    *plZ = (delta > 0) ? WHEEL_DELTA : -WHEEL_DELTA;
                }

                if (raw_lz != 0 || delta != 0)
                {
                    if (blocked)
                        s_stat_mouse_wheel_blocks.fetch_add(
                            1, std::memory_order_relaxed);
                    else
                        s_stat_mouse_wheel_passes.fetch_add(
                            1, std::memory_order_relaxed);
                }
            }

            if (WheelFilter_DebugLog() && raw_lz != 0)
            {
                const int n = s_wheel_debug_log_count.fetch_add(
                    1, std::memory_order_relaxed);
                if (n < kMaxWheelDebugLogLines)
                {
                    PSO_LOG("wheel-state: tick=%lu raw=%ld last=%ld "
                            "mode=%d out=%ld",
                            (unsigned long)now,
                            (long)raw_lz,
                            (long)s_mouse_last_raw_lz,
                            mode,
                            (long)*plZ);
                }
            }
        }
        return hr;
    }

    // Vtable sharing again: the mouse and keyboard devices are routed
    // through this hook too. Skip chord detection and suppression when
    // the caller is not a real joystick — otherwise chord edge tracking
    // gets clobbered by keyboard polls and suppression zeros bytes in
    // the keyboard state buffer, stealing our own SendInput'd keys.
    if (!IsKnownJoystick(self)) return hr;

    s_stat_getstate_joy_calls.fetch_add(1, std::memory_order_relaxed);
    s_last_joy_cbdata.store(cbData, std::memory_order_relaxed);

    if (SUCCEEDED(hr) && lpvData != nullptr)
    {
        // Chord detection runs every poll; uses XInput, doesn't depend on
        // the buffer format. Pass the buffer as a DIJOYSTATE2* fallback
        // source only when it's big enough to safely deref; otherwise
        // pass null so DetectAndFireChord uses XInput exclusively.
        const DIJOYSTATE2 *js = (cbData >= sizeof(DIJOYSTATE))
                                    ? reinterpret_cast<const DIJOYSTATE2 *>(lpvData)
                                    : nullptr;
        DetectAndFireChord(js);

        // Suppression is buffer-format-aware. Uses the SetDataFormat-
        // captured offsets when the cbData matches PSO's custom format;
        // falls back to standard DIJOYSTATE2 rgbButtons[] layout for
        // DIJOYSTATE(2) buffers; no-op for unknown sizes.
        SuppressJoystickButtons(lpvData, cbData);
    }
    return hr;
}

// Shared-vtable GetDeviceData hook. Only filters wheel records on
// mouse devices; joystick / keyboard buffered data passes straight
// through. GetDeviceData returns an array of DIDEVICEOBJECTDATA
// records between polls — one per input transition since the last
// read — so for the wheel that means one entry per WHEEL_DELTA or
// per touchpad smooth-scroll tick. The same throttle + normalize
// logic the state-poll hook uses applies here, but we walk the
// record array and rewrite wheel entries individually.
//
// DIDEVICEOBJECTDATA layout (x86, 20 bytes):
//   dwOfs       u32  input object offset (DIMOFS_Z = 8 for lZ)
//   dwData      u32  signed wheel delta reinterpreted as LONG
//   dwTimeStamp u32
//   dwSequence  u32
//   uAppData    u32
static HRESULT STDMETHODCALLTYPE Hook_GetDeviceData_Shared(
    IDirectInputDevice8A *self,
    DWORD cbObjectData,
    LPDIDEVICEOBJECTDATA rgdod,
    LPDWORD pdwInOut,
    DWORD dwFlags)
{
    HRESULT hr = s_real_GetDeviceData_shared(
        self, cbObjectData, rgdod, pdwInOut, dwFlags);
    if (!SUCCEEDED(hr) || !IsKnownMouse(self)) return hr;
    if (!WheelFilter_Enabled())                return hr;
    if (rgdod == nullptr || pdwInOut == nullptr || cbObjectData < 8)
        return hr;

    const DWORD count = *pdwInOut;
    if (count == 0) return hr;

    // DIGDD_PEEK returns a snapshot without consuming the queue, so
    // we MUST NOT mutate it in that case — our edits would bleed
    // into the next non-peek read and corrupt the event stream.
    const bool is_peek = (dwFlags & DIGDD_PEEK) != 0;
    if (is_peek) return hr;

    const DWORD quiet = static_cast<DWORD>(WheelFilter_ThrottleMs());
    uint8_t *base = reinterpret_cast<uint8_t *>(rgdod);

    // Two-pointer compaction. Drop any wheel record (dwOfs ==
    // DIMOFS_Z) the filter rejects. Magnitude-agnostic quiet-gap
    // matching the state-poll hook — see the comment in
    // Hook_GetDeviceState_Joystick for the rationale.
    const int mode = WheelFilter_Mode();
    DWORD write = 0;
    for (DWORD read = 0; read < count; ++read)
    {
        uint8_t *src = base + read * cbObjectData;
        const DWORD dw_ofs = *reinterpret_cast<DWORD *>(src + 0);

        bool drop = false;
        if (dw_ofs == DIMOFS_Z)
        {
            LONG *p_data = reinterpret_cast<LONG *>(src + 4);
            const LONG z_in = *p_data;
            if (z_in != 0)
            {
                const DWORD now = GetTickCount();

                if (mode == 2)
                {
                    drop = true;
                }
                else if (mode == 1)
                {
                    const DWORD gap =
                        (s_mouse_last_wheel_tick == 0)
                            ? 0xFFFFFFFFu
                            : (now - s_mouse_last_wheel_tick);
                    s_mouse_last_wheel_tick = now;
                    if (gap < quiet) drop = true;
                }

                if (drop)
                    s_stat_mouse_wheel_blocks.fetch_add(
                        1, std::memory_order_relaxed);
                else
                    s_stat_mouse_wheel_passes.fetch_add(
                        1, std::memory_order_relaxed);

                if (WheelFilter_DebugLog())
                {
                    const int n = s_wheel_debug_log_count.fetch_add(
                        1, std::memory_order_relaxed);
                    if (n < kMaxWheelDebugLogLines)
                    {
                        PSO_LOG("wheel-data:  tick=%lu lZ=%ld mode=%d "
                                "drop=%d rec=%u/%u",
                                (unsigned long)now,
                                (long)z_in,
                                mode,
                                drop ? 1 : 0,
                                (unsigned)read, (unsigned)count);
                    }
                }
            }
        }

        if (!drop)
        {
            if (write != read)
            {
                uint8_t *dst = base + write * cbObjectData;
                std::memmove(dst, src, cbObjectData);
            }
            ++write;
        }
    }
    *pdwInOut = write;

    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(
    IDirectInput8A *self, REFGUID rguid,
    LPDIRECTINPUTDEVICE8A *lplpDirectInputDevice, LPUNKNOWN pUnkOuter)
{
    HRESULT hr = s_real_CreateDevice(self, rguid, lplpDirectInputDevice, pUnkOuter);
    const uint32_t create_seq =
        s_stat_create_device_calls.fetch_add(1, std::memory_order_relaxed) + 1;

    if (!SUCCEEDED(hr) || !lplpDirectInputDevice || !*lplpDirectInputDevice)
    {
        PSO_LOG("Hook_CreateDevice: #%u failed hr=0x%lX",
                create_seq, (unsigned long)hr);
        return hr;
    }

    IDirectInputDevice8A *dev = *lplpDirectInputDevice;

    // Query the device's actual capabilities so we can identify it by
    // type instead of by GUID. PSO creates the keyboard device with an
    // INSTANCE GUID (not GUID_SysKeyboard), so the GUID-based filter in
    // the previous implementation let the keyboard through as
    // "joystick-or-other" and we ended up patching the wrong device.
    // GetCapabilities returns a DIDEVCAPS with dwDevType whose low byte
    // is one of DI8DEVTYPE_{KEYBOARD, MOUSE, JOYSTICK, GAMEPAD, DRIVING,
    // FLIGHT, 1STPERSON, DEVICECTRL, SCREENPOINTER, REMOTE, SUPPLEMENTAL}.
    DIDEVCAPS caps = {};
    caps.dwSize = sizeof(caps);
    const HRESULT cap_hr = dev->GetCapabilities(&caps);
    const BYTE devType = (cap_hr == DI_OK) ? LOBYTE(caps.dwDevType) : 0;

    const bool is_joystick_class =
        devType == DI8DEVTYPE_JOYSTICK  ||
        devType == DI8DEVTYPE_GAMEPAD   ||
        devType == DI8DEVTYPE_DRIVING   ||
        devType == DI8DEVTYPE_FLIGHT    ||
        devType == DI8DEVTYPE_1STPERSON ||
        devType == DI8DEVTYPE_SUPPLEMENTAL;

    const char *kind_str =
        devType == DI8DEVTYPE_KEYBOARD ? "keyboard" :
        devType == DI8DEVTYPE_MOUSE    ? "mouse" :
        is_joystick_class              ? "joystick" :
                                         "other";
    PSO_LOG("Hook_CreateDevice: #%u devType=0x%02X kind=%s hr=0x%lX dev=0x%p",
            create_seq,
            (unsigned)devType, kind_str,
            (unsigned long)hr, dev);

    // Register every joystick-class device pointer so IsKnownJoystick
    // can distinguish real joystick polls from the mouse and keyboard
    // polls that share this vtable.
    if (is_joystick_class && s_joystick_device_count < kMaxJoystickDevices)
    {
        s_joystick_devices[s_joystick_device_count++] = dev;
        PSO_LOG("Hook_CreateDevice: registered joystick #%d dev=0x%p",
                s_joystick_device_count, dev);
    }

    // Same game for mouse devices. The shared vtable means the
    // joystick vtable[9] patch also routes mouse polls through our
    // hook; IsKnownMouse lets the hook apply wheel-filter logic
    // only to real mouse calls.
    if (devType == DI8DEVTYPE_MOUSE && s_mouse_device_count < kMaxMouseDevices)
    {
        s_mouse_devices[s_mouse_device_count++] = dev;
        PSO_LOG("Hook_CreateDevice: registered mouse #%d dev=0x%p",
                s_mouse_device_count, dev);
    }

    // Patch the shared vtable on the first joystick OR mouse we see,
    // whichever comes first. Historically this only triggered for
    // joysticks (because the chord system only needed joystick
    // polls), but the wheel filter needs to run on mouse polls too —
    // and users without a controller would otherwise never get the
    // hook installed. Vtable is shared across device types, so one
    // patch covers both.
    const bool should_patch_vtable =
        (is_joystick_class || devType == DI8DEVTYPE_MOUSE) &&
        s_real_GetDeviceState_joy == nullptr;

    if (should_patch_vtable)
    {
        void **dev_vt = *reinterpret_cast<void ***>(dev);

        // vtable index 9 = GetDeviceState
        void *old_gds = nullptr;
        if (VtablePatch(dev_vt, 9,
                        reinterpret_cast<void *>(&Hook_GetDeviceState_Joystick),
                        &old_gds))
        {
            s_real_GetDeviceState_joy = reinterpret_cast<GetDeviceState_t>(old_gds);
            s_patched_joy_vtable      = dev_vt;
            s_orig_GetDeviceState_joy = old_gds;
            PSO_LOG("Hook_CreateDevice: patched shared vtable[9] via %s, real=0x%p",
                    kind_str, old_gds);
        }
        else
        {
            PSO_LOG("Hook_CreateDevice: VtablePatch FAILED for vtable[9]");
        }

        // vtable index 10 = GetDeviceData. Needed for the mouse-wheel
        // filter on the buffered-data path — PSO reads the wheel
        // through both GetDeviceState (polling) and GetDeviceData
        // (buffered events), and the state hook alone only caught
        // roughly half the touchpad burst. This patch rides the same
        // vtable so every device type routes through the buffered
        // hook; Hook_GetDeviceData_Shared gates on IsKnownMouse.
        void *old_gdd = nullptr;
        if (VtablePatch(dev_vt, 10,
                        reinterpret_cast<void *>(&Hook_GetDeviceData_Shared),
                        &old_gdd))
        {
            s_real_GetDeviceData_shared =
                reinterpret_cast<GetDeviceData_t>(old_gdd);
            s_orig_GetDeviceData_shared = old_gdd;
            PSO_LOG("Hook_CreateDevice: patched shared vtable[10] via %s, real=0x%p",
                    kind_str, old_gdd);
        }
        else
        {
            PSO_LOG("Hook_CreateDevice: VtablePatch FAILED for vtable[10]");
        }

        // vtable index 11 = SetDataFormat. Capturing this lets us learn
        // the byte layout of PSO's custom DIDATAFORMAT so the chord
        // suppression writes to the right offsets in PSO's compact
        // 20-byte buffer.
        void *old_sdf = nullptr;
        if (VtablePatch(dev_vt, 11,
                        reinterpret_cast<void *>(&Hook_SetDataFormat_Joystick),
                        &old_sdf))
        {
            s_real_SetDataFormat_joy = reinterpret_cast<SetDataFormat_t>(old_sdf);
            s_orig_SetDataFormat_joy = old_sdf;
            PSO_LOG("Hook_CreateDevice: patched joystick vtable[11], real=0x%p", old_sdf);
        }
        else
        {
            PSO_LOG("Hook_CreateDevice: VtablePatch FAILED for joystick vtable[11]");
        }
    }
    return hr;
}

static HRESULT WINAPI Hook_DirectInput8Create(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf,
    LPVOID *ppvOut, LPUNKNOWN punkOuter)
{
    HRESULT hr = s_real_DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    PSO_LOG("Hook_DirectInput8Create: hr=0x%lX ppvOut=0x%p",
            (unsigned long)hr, ppvOut ? *ppvOut : nullptr);

    if (SUCCEEDED(hr) && ppvOut && *ppvOut && s_real_CreateDevice == nullptr)
    {
        IDirectInput8A *di8 = reinterpret_cast<IDirectInput8A *>(*ppvOut);
        void **di8_vt = *reinterpret_cast<void ***>(di8);
        void *old_fn = nullptr;
        if (VtablePatch(di8_vt, 3, reinterpret_cast<void *>(&Hook_CreateDevice), &old_fn))
        {
            s_real_CreateDevice  = reinterpret_cast<CreateDevice_t>(old_fn);
            s_patched_di8_vtable = di8_vt;
            s_orig_CreateDevice  = old_fn;
            PSO_LOG("Hook_DirectInput8Create: patched IDirectInput8 vtable[3], real=0x%p", old_fn);
        }
        else
        {
            PSO_LOG("Hook_DirectInput8Create: VtablePatch FAILED for IDirectInput8 vtable[3]");
        }
    }
    return hr;
}

// ---------- Install / Uninstall ----------

bool Install()
{
    PSO_LOG("chords::Install: begin");
    if (s_hook_installed)
    {
        PSO_LOG("chords::Install: already installed");
        return true;
    }

    TryLoadXInput();

    // Cache QPC frequency once so DrainPendingKeyUp / SendScancodeTap
    // don't pay the syscall on every joystick poll.
    QueryPerformanceFrequency(&s_qpc_freq);

    const MH_STATUS init_rc = MH_Initialize();
    PSO_LOG("chords::Install: MH_Initialize=%d", (int)init_rc);
    if (init_rc != MH_OK && init_rc != MH_ERROR_ALREADY_INITIALIZED)
    {
        PSO_LOG("chords::Install: MH_Initialize failed, abort");
        return false;
    }

    HMODULE dinput8 = LoadLibraryA("dinput8.dll");
    PSO_LOG("chords::Install: dinput8.dll handle=0x%p", dinput8);
    if (dinput8 == nullptr) return false;

    void *target = reinterpret_cast<void *>(
        GetProcAddress(dinput8, "DirectInput8Create"));
    PSO_LOG("chords::Install: DirectInput8Create address=0x%p", target);
    if (target == nullptr) return false;

    const MH_STATUS create_rc = MH_CreateHook(target,
        reinterpret_cast<void *>(&Hook_DirectInput8Create),
        reinterpret_cast<void **>(&s_real_DirectInput8Create));
    PSO_LOG("chords::Install: MH_CreateHook=%d real=0x%p",
            (int)create_rc, (void*)s_real_DirectInput8Create);
    if (create_rc != MH_OK) return false;

    const MH_STATUS enable_rc = MH_EnableHook(target);
    PSO_LOG("chords::Install: MH_EnableHook=%d", (int)enable_rc);
    if (enable_rc != MH_OK) return false;

    s_hook_installed = true;
    PSO_LOG("chords::Install: success");
    return true;
}

void Uninstall()
{
    PSO_LOG("chords::Uninstall: begin (installed=%d)", s_hook_installed ? 1 : 0);
    if (!s_hook_installed) return;

    // Force-release any deferred palette-slot key so we never leave a
    // scancode stuck down across DLL unload.
    DrainPendingKeyUp(/*force=*/true);

    void *tmp;
    if (s_patched_joy_vtable && s_orig_GetDeviceState_joy)
    {
        VtablePatch(s_patched_joy_vtable, 9, s_orig_GetDeviceState_joy, &tmp);
        PSO_LOG("chords::Uninstall: restored shared vtable[9]");
    }
    if (s_patched_joy_vtable && s_orig_GetDeviceData_shared)
    {
        VtablePatch(s_patched_joy_vtable, 10, s_orig_GetDeviceData_shared, &tmp);
        PSO_LOG("chords::Uninstall: restored shared vtable[10]");
    }
    if (s_patched_joy_vtable && s_orig_SetDataFormat_joy)
    {
        VtablePatch(s_patched_joy_vtable, 11, s_orig_SetDataFormat_joy, &tmp);
        PSO_LOG("chords::Uninstall: restored shared vtable[11]");
    }
    if (s_patched_di8_vtable && s_orig_CreateDevice)
    {
        VtablePatch(s_patched_di8_vtable, 3, s_orig_CreateDevice, &tmp);
        PSO_LOG("chords::Uninstall: restored IDirectInput8 vtable[3]");
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    s_hook_installed            = false;
    s_real_DirectInput8Create   = nullptr;
    s_real_CreateDevice         = nullptr;
    s_real_GetDeviceState_joy   = nullptr;
    s_real_GetDeviceData_shared = nullptr;
    s_real_SetDataFormat_joy    = nullptr;
    s_orig_GetDeviceData_shared = nullptr;
    s_patched_di8_vtable        = nullptr;
    s_patched_joy_vtable        = nullptr;
    s_joy_format_captured.store(false, std::memory_order_release);
    for (int i = 0; i < kMaxJoystickDevices; ++i) s_joystick_devices[i] = nullptr;
    s_joystick_device_count = 0;
    for (int i = 0; i < kMaxMouseDevices; ++i) s_mouse_devices[i] = nullptr;
    s_mouse_device_count    = 0;
    PSO_LOG("chords::Uninstall: done");
}

// ---------- Persistence ----------

void ReadConfigKey(const char *key, const char *val)
{
    if (std::strcmp(key, "cc_enabled") == 0)
    {
        s_cc_enabled.store(std::atoi(val) != 0, std::memory_order_relaxed);
    }
    else if (std::strcmp(key, "cc_trigger_threshold") == 0)
    {
        s_cc_trigger_threshold.store(std::atoi(val), std::memory_order_relaxed);
    }
}

void WriteConfig(std::FILE *f)
{
    std::fprintf(f, "cc_enabled=%d\n",
                 s_cc_enabled.load(std::memory_order_relaxed) ? 1 : 0);
    std::fprintf(f, "cc_trigger_threshold=%d\n",
                 s_cc_trigger_threshold.load(std::memory_order_relaxed));
}

// ---------- Config panel ----------

bool RenderConfigPanelSection()
{
    bool dirty = false;

    ImGui::TextUnformatted("Controller chord remapping");

    // ImGui widgets want a raw pointer; stage a local copy of the
    // atomic value, let the widget edit it, and store it back if the
    // user changed something this frame. This is the standard way
    // to bridge between atomics and ImGui-style value editing.
    {
        bool enabled = s_cc_enabled.load(std::memory_order_relaxed);
        if (ImGui::Checkbox("Enable LT/RT + face button -> palette slots 1-0", &enabled))
        {
            s_cc_enabled.store(enabled, std::memory_order_relaxed);
            dirty = true;
        }
    }
    {
        int threshold = s_cc_trigger_threshold.load(std::memory_order_relaxed);
        if (ImGui::SliderInt("Trigger threshold", &threshold, 32, 200))
        {
            s_cc_trigger_threshold.store(threshold, std::memory_order_relaxed);
            dirty = true;
        }
    }
    ImGui::TextDisabled("Face buttons are suppressed while a trigger is held,");
    ImGui::TextDisabled("so the chord fires a palette slot without also attacking.");
    ImGui::TextDisabled("  LT + A/B/X/Y       ->  1 / 2 / 3 / 4");
    ImGui::TextDisabled("  RT + A/B/X/Y       ->  5 / 6 / 7 / 8");
    ImGui::TextDisabled("  LT+RT + A/B        ->  9 / 0   (requires XInput)");
    ImGui::TextDisabled("  LB held            ->  emits Ctrl+digit instead");
    ImGui::TextDisabled("                         (PSO palette set 2, slots 11-20)");

    ImGui::Separator();
    ImGui::Text("Hook installed: %s", s_hook_installed ? "yes" : "no");

    const uint32_t last_rc = s_last_xinput_rc.load(std::memory_order_relaxed);
    ImGui::Text("XInput loaded: %s",
                s_xinput_available
                    ? (last_rc == 0 ? "yes (controller connected)"
                                    : "yes (no controller)")
                    : "no");

    const uint32_t polls   = s_stat_getstate_joy_calls.load(std::memory_order_relaxed);
    const uint32_t cbdata  = s_last_joy_cbdata.load(std::memory_order_relaxed);
    ImGui::Text("Joystick polls: %u%s",
                polls,
                cbdata == sizeof(DIJOYSTATE)  ? "  (DIJOYSTATE)" :
                cbdata == sizeof(DIJOYSTATE2) ? "  (DIJOYSTATE2)" : "");

    const uint32_t fires   = s_stat_chord_fires.load(std::memory_order_relaxed);
    const uint32_t calls   = s_stat_sendinput_calls.load(std::memory_order_relaxed);
    const uint32_t fails   = s_stat_sendinput_fails.load(std::memory_order_relaxed);
    ImGui::Text("Chords fired: %u", fires);
    ImGui::Text("SendInput: %u ok, %u failed", calls - fails, fails);

    // ---- Live state diagnostic ----
    //
    // Shows the raw XInput + DI state we're reading right now so you
    // can verify (a) the controller is connected and we're getting its
    // values, (b) the trigger classifier is crossing the threshold when
    // you pull LT/RT, (c) the right buttons light up when you press
    // A/B/X/Y/LB/RB. Much faster than tail -f on pixelated_mods.log.
    ImGui::Separator();
    ImGui::TextUnformatted("Live controller state");

    // Format captured from SetDataFormat (if our hook caught it).
    // Load-acquire the publication flag — if it reads true, we're
    // guaranteed to see the array writes that the input thread did
    // before its matching store-release.
    if (s_joy_format_captured.load(std::memory_order_acquire))
    {
        ImGui::Text("Joystick format: dwDataSize=%lu",
                    (unsigned long)s_joy_format_size.load(std::memory_order_relaxed));
        ImGui::Text("Face button offsets:  A=0x%X  B=0x%X  X=0x%X  Y=0x%X",
                    s_joy_button_ofs[0], s_joy_button_ofs[1],
                    s_joy_button_ofs[2], s_joy_button_ofs[3]);
    }
    else
    {
        ImGui::TextDisabled("Joystick format NOT captured (SetDataFormat hook hasn't fired)");
    }

    // XInput state, polled fresh for the panel render.
    if (s_xinput_available && s_xinput_GetState != nullptr)
    {
        XSTATE s{};
        const DWORD rc = s_xinput_GetState(0, &s);
        if (rc == 0)
        {
            const WORD b = s.Gamepad.wButtons;
            const BYTE lt_val   = s.Gamepad.bLeftTrigger;
            const BYTE rt_val   = s.Gamepad.bRightTrigger;
            const int  threshold =
                s_cc_trigger_threshold.load(std::memory_order_relaxed);
            const bool lt_held  = lt_val >= threshold;
            const bool rt_held  = rt_val >= threshold;

            ImGui::Text("LT: %3u  %s    RT: %3u  %s",
                        lt_val, lt_held ? "HELD" : "....",
                        rt_val, rt_held ? "HELD" : "....");

            auto btn_row = [&](const char *label, WORD mask) {
                const bool down = (b & mask) != 0;
                ImGui::TextColored(
                    down ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                         : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                    "  %-3s %s", label, down ? "DOWN" : "....");
                ImGui::SameLine();
            };
            btn_row("A",  XBTN_A);
            btn_row("B",  XBTN_B);
            btn_row("X",  XBTN_X);
            btn_row("Y",  XBTN_Y);
            ImGui::NewLine();
            btn_row("LB", XBTN_LB);
            btn_row("RB", XBTN_RB);
            ImGui::NewLine();

            // Predict which digit would fire if any face button edge
            // occurred right now, using the same chord-map logic as
            // DetectAndFireChord. Helpful for verifying the user's
            // mental model matches what the add-on will actually do.
            const bool rb_mod = (b & XBTN_RB) != 0;
            const char *would_fire = "none";
            if (lt_held && rt_held)
            {
                if ((b & XBTN_A)) would_fire = rb_mod ? "Ctrl+9" : "9";
                else if ((b & XBTN_B)) would_fire = rb_mod ? "Ctrl+0" : "0";
            }
            else if (lt_held)
            {
                if      ((b & XBTN_A)) would_fire = rb_mod ? "Ctrl+1" : "1";
                else if ((b & XBTN_B)) would_fire = rb_mod ? "Ctrl+2" : "2";
                else if ((b & XBTN_X)) would_fire = rb_mod ? "Ctrl+3" : "3";
                else if ((b & XBTN_Y)) would_fire = rb_mod ? "Ctrl+4" : "4";
            }
            else if (rt_held)
            {
                if      ((b & XBTN_A)) would_fire = rb_mod ? "Ctrl+5" : "5";
                else if ((b & XBTN_B)) would_fire = rb_mod ? "Ctrl+6" : "6";
                else if ((b & XBTN_X)) would_fire = rb_mod ? "Ctrl+7" : "7";
                else if ((b & XBTN_Y)) would_fire = rb_mod ? "Ctrl+8" : "8";
            }
            ImGui::Text("Current chord combo -> %s", would_fire);
            ImGui::Text("Suppress active: %s",
                        s_chord_suppress_active.load(std::memory_order_relaxed)
                            ? "YES (zeroing face buttons)" : "no");
        }
        else
        {
            ImGui::TextDisabled("XInput controller disconnected (rc=0x%08lX)",
                                (unsigned long)rc);
        }
    }
    else
    {
        ImGui::TextDisabled("XInput not loaded");
    }

    return dirty;
}

} // namespace chords

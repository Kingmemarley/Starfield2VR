#include "nvidia/UpscalerAfrNvidiaModule.h"

#include <windows.h>
#include <tlhelp32.h>
#pragma comment(lib, "version.lib")

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include "mods/VR.hpp"
#include "nvidia/StreamlineAbi.h"
#include "mods/vr/LaserSight.hpp"
#include "utility/FunctionHook.hpp"

// See the header. Streamline's own calls are passed through untouched whenever per-eye DLSS is not in use.
namespace {
using slabi::BaseStructure;
using slabi::Constants;
using slabi::Feature;
using slabi::FrameToken;
using slabi::Result;
using slabi::ViewportHandle;

using SetTagT             = Result(const ViewportHandle&, const void*, uint32_t, void*);
using SetTagForFrameT     = Result(const FrameToken&, const ViewportHandle&, const void*, uint32_t, void*);
using SetConstantsT       = Result(const Constants&, const FrameToken&, const ViewportHandle&);
using EvaluateT           = Result(Feature, const FrameToken&, const BaseStructure**, uint32_t, void*);
using AllocateT           = Result(void*, Feature, const ViewportHandle&);
using FreeT               = Result(Feature, const ViewportHandle&);
using GetFeatureFunctionT = Result(Feature, const char*, void*&);
using DlssSetOptionsT     = Result(const ViewportHandle&, const void*);

// The right eye's viewport id is the game's + this. Ids at or above it are never remapped (they are ours, should Streamline
// call one of its own hooked functions with them).
constexpr uint32_t kOtherEye  = 0x4000;
constexpr uint32_t kMaxInputs = 16; // slEvaluateFeature inputs handled (the game passes its viewport, maybe a few more)
// A menu's view's id: the game's + this x its place (1..3).
constexpr uint32_t kViewStep = 0x10000;
constexpr int      kViews    = 4;
// Streamline's "out of video memory" warning: returned after a DLSS evaluation that worked (the budget is used up), not a failure.
constexpr Result   kWarnOutOfVram = 39;

bool Failed(Result r) {
    return r != 0 && r != kWarnOutOfVram;
}

// The hooks live as long as the process (never freed: a call may be inside one at any time).
FunctionHook* g_set_tag{};
FunctionHook* g_set_tag_for_frame{};
FunctionHook* g_set_constants{};
FunctionHook* g_evaluate{};
FunctionHook* g_allocate{};
FunctionHook* g_free{};
FunctionHook* g_get_feature_function{};
FunctionHook* g_dlss_set_options{};
// (fix147) DLSS's set-options function when it could not be hooked in place: Streamline's DLSS (sl.dlss.dll) was loaded where no
// free memory lies within 2 GB of it (fix146's test: at 0x1fb70ce0000, among the game's own reservations), so the hook's jump to
// its trampoline cannot reach, and the function's first instruction reads its own data relative to where it is (the stack check's
// cookie), which cannot be copied far away. The game gets the function from Streamline (slGetFeatureFunction) and keeps its
// address: it is handed this module's function in its place instead, which calls DLSS's own (this) as the hook's trampoline did.
std::atomic<DlssSetOptionsT*> g_dlss_direct{nullptr};
std::atomic<void*>            g_dlss_function{nullptr}; // (DLSS's own, as Streamline gave it)
std::atomic<bool>             g_dlss_redirected{false};
// Streamline's eErrorInvalidState (its results in order: 39 is eWarnOutOfVRAM, the one before it this).
constexpr Result kErrorInvalidState = 38;

std::atomic<bool> g_installed{false};
std::atomic<bool> g_hooks_ok{false};       // every hook a split needs is in place
std::atomic<bool> g_dlss_hooked{false};    // DLSS's set-options function is hooked
std::atomic<bool> g_dlss_hook_failed{false};
std::atomic<bool> g_enabled{true};         // the "Per-eye DLSS" setting
std::atomic<bool> g_history_fix{true};     // the game gives each eye its own previous camera
std::atomic<bool> g_options_ready{false};  // a right-eye instance has the game's DLSS options
std::atomic<bool> g_layout_ok{true};       // the game's calls look as expected (else per-eye stays off for the session)
std::atomic<bool> g_right_failed{false};   // the right eye's instance kept failing (off until the options change or the setting is turned on)
std::atomic<bool> g_last_left_ok{true};    // the game's own instance's latest evaluation succeeded
std::atomic<int>  g_right_failures{0};     // right-eye evaluations failing in a row (while the left one's succeed)
std::atomic<int>  g_resolve_attempts{0};
std::atomic<int64_t> g_next_resolve_us{0};
std::mutex        g_install_mutex;
std::mutex        g_hook_mutex;            // hooking DLSS's set-options function

int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// (fix179) How long the game's Streamline DLSS calls take (the menus' slow frames: a frame of 250-350 ms as the star map opens, where the
// game turns DLSS off and frees it; a helper's research found no wait for the graphics card in Streamline 2.2.0's free, so it is measured
// here): every call through these hooks added up (any thread; D3D12Component takes the sums once a present, take_call_time), and each one
// over 2 ms logged with what it was, for which id, how long, and how much of that it waited for another thread's DLSS call (the lock
// below). At most 80 such lines, then one in 100.
std::atomic<int64_t>  g_call_total_us{0};
std::atomic<int64_t>  g_call_longest_us{0};
std::atomic<uint32_t> g_call_count{0};
std::atomic<int>      g_slow_calls{0};
constexpr int64_t     kSlowCallUs = 2000;
// (fix179, the review) The present thread's own check (once a present: on_present's let-go) counted apart: its wait for the lock is the
// time a game thread's DLSS call held it (already counted as that call), so it is not added to the calls' sums; its own total and longest.
std::atomic<int64_t>  g_present_check_us{0};
std::atomic<int64_t>  g_present_check_longest_us{0};
// (fix179, the review) The ids last evaluated with the game's instance and the right eye's (under g_sl_mutex): an id evaluated that is not
// the last one has DLSS make its instance in that call (for the slow calls' lines). Cleared when the game frees that id (OnFree): the same id
// made again after a free is a new instance too.
uint32_t g_last_game_eval_id  = 0xFFFFFFFF;
uint32_t g_last_right_eval_id = 0xFFFFFFFF;

void AtomicMax(std::atomic<int64_t>& value, int64_t candidate) {
    int64_t current = value.load(std::memory_order_relaxed);
    while (candidate > current && !value.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

struct CallTimer {
    const char* what;
    uint32_t    id{0xFFFFFFFF};
    int64_t     start{NowUs()};
    int64_t     locked{0};      // (when the lock was got: 0 if none)
    const char* note{nullptr};  // (more about it, for its line: whose instance, made in it...)
    bool        present_check{false}; // (the present thread's own check: counted apart, not as a DLSS call)
    explicit CallTimer(const char* w, uint32_t i = 0xFFFFFFFF, bool check = false) : what(w), id(i), present_check(check) {}
    CallTimer(const CallTimer&)            = delete;
    CallTimer& operator=(const CallTimer&) = delete;
    // (Declared before the call's lock: this runs after the lock is let go, so its line is never written under it.)
    ~CallTimer() {
        const int64_t took = NowUs() - start;
        if (present_check) {
            g_present_check_us.fetch_add(took, std::memory_order_relaxed);
            AtomicMax(g_present_check_longest_us, took);
        } else {
            g_call_total_us.fetch_add(took, std::memory_order_relaxed);
            g_call_count.fetch_add(1, std::memory_order_relaxed);
            AtomicMax(g_call_longest_us, took);
        }
        if (took < kSlowCallUs) {
            return;
        }
        const int n = g_slow_calls.fetch_add(1, std::memory_order_relaxed);
        if (n < 80 || n % 100 == 0) {
            spdlog::info("[DLSS] (fix179) a slow Streamline call: {} for viewport {}{}{} took {:.1f} ms ({:.1f} ms of it waiting for another thread's "
                         "DLSS call to finish; thread {}; slow calls so far {})",
                what, id == 0xFFFFFFFF ? std::string{"(none)"} : std::to_string(id), note != nullptr ? ", " : "", note != nullptr ? note : "",
                took / 1000.0, locked > start ? (locked - start) / 1000.0 : 0.0, GetCurrentThreadId(), n + 1);
        }
    }
    void got_lock() { locked = NowUs(); }
};

// Per frame: split between the two instances or not, and which game ids may be split, decided once per frame (the first time
// a call for it comes) so its constants, tags and evaluation all go the same way; the reset bookkeeping and the statistics for
// the log (g_state_mutex).
std::mutex g_state_mutex;
struct FrameChoice {
    uint32_t frame{0};
    bool     valid{false};
    bool     split{false};
    std::array<uint32_t, 16> ids{}; // the game ids that may be split this frame (a copy: the list may change within the frame)
    uint32_t count{0};
};
std::array<FrameChoice, 8> g_choices{};
bool                       g_last_split{false};
bool                       g_last_swap_eyes{false};
bool                       g_last_native{false}; // (full stereo: the eyes are the frame's two views, not frames that take turns)
bool                       g_pending_reset[2]{true, true}; // reset the next frame's history: the game's instance (0), the right eye's (1)
std::atomic<bool>          g_right_unneeded{false}; // (fix165) the right eye's instance not needed now (mono, video memory short)
std::atomic<bool>          g_right_unneeded_menu{false}; // (fix173) ...because a menu is shown in mono (for the log)
// (fix165) The right eye's instance evaluated since it was last let go, and when last (us): it is let go only 2 s after its last evaluation,
// when the GPU has surely finished with it (Streamline frees it at once; freed while a queued frame still uses it, the device could be
// lost; the review).
std::atomic<bool>          g_right_live{false};
std::atomic<int64_t>       g_right_last_us{0};
std::atomic<uint32_t>      g_right_last_presents{0}; // (fix173) ...and the presents done then (VR::m_presents_done)
uint32_t                   g_forced_frame[2]{0xFFFFFFFF, 0xFFFFFFFF}; // ...the frame it was forced in (more calls for it get it too)
int                        g_reset_more[2]{0, 0}; // (fix160) frames after the next one to start over too (request_reset), each a new frame index
// Game ids whose right-eye id has the game's options (the newest last; an id freed by the game is taken out: the game makes a new
// id at each menu's open and close, so a list that only grew stopped per-eye DLSS after the fourth).
std::array<uint32_t, 16>   g_mirrored{};
uint32_t                   g_mirrored_count{0};

// The menu's views (each its own DLSS instance, behind a see-through menu): the thread's view (set around the game's steps that
// make DLSS calls), whether that is wanted now, and each view's instance (by its place, 1..3; under g_state_mutex).
thread_local int     t_view   = -1;
thread_local bool    t_tagged = false; // (this thread's view tagged its id in the step now running: its evaluation may use it)
std::atomic<bool>    g_views_enabled{false}; // the "Per-view DLSS" setting (off by default)
std::atomic<int>     g_dlss_model{2};        // (fix168) the "DLSS model" choice: 0 the game's, 1 E, 2 K, 3 NVIDIA's picks (fix153: on/off)
std::atomic<bool>    g_views_active{false}; // the game side wants it now
std::atomic<int64_t> g_last_dlss_ms{0};     // (the game's last DLSS evaluation, for full stereo's check)
std::atomic<bool>    g_views_failed{false}; // the menu's views' instances kept failing (off until the setting is turned on again)
std::atomic<int>     g_view_failures{0};
// Every Streamline DLSS call made one at a time: DLSS 2.2 keeps its current viewport in one global and its viewports' map without a
// lock, and the views' steps are recorded on several threads at once.
std::recursive_mutex g_sl_mutex;
struct ViewSlot {
    uint32_t game{0xFFFFFFFF};              // the game's id it is for (none yet: 0xFFFFFFFF, the game's ids may start at 0)
    bool     options{false};                // it has the game's DLSS options
    bool     evaluated{false};              // DLSS made its resources (freed when not used for a while)
    uint32_t constants_frame{0xFFFFFFFF};   // the frame its constants were last set for
    uint32_t last_frame{0xFFFFFFFF};        // the frame it was last evaluated in
};
std::array<ViewSlot, kViews> g_view_slots{};
uint32_t                     g_latest_frame{0};
// A menu's view's instance of a game id the game no longer uses: when the game replaces its DLSS object (a new size, mode, or DLSS
// turned on or off: at a menu's open and close, the full-size frame's resize) the views' next calls are already for the new id
// before the old one is freed (its free is queued). Freed with the game's, or a few seconds on.
struct Orphan {
    uint32_t game{0};
    int      view{0};
    uint32_t since{0}; // (the frame it was left in)
};
std::vector<Orphan> g_orphans{};

// (Under g_state_mutex.) The view's instance now for this game id; the one it had, if DLSS made it, kept to be freed.
void SlotFor(int view, uint32_t game) {
    auto& slot = g_view_slots[view];
    if (slot.game == game) {
        return;
    }
    if (slot.evaluated && g_orphans.size() < 64) {
        g_orphans.push_back(Orphan{slot.game, view, g_latest_frame});
    }
    // (Back to an id it had left: its instance is in use again, not to be freed.)
    std::erase_if(g_orphans, [&](const Orphan& o) { return o.game == game && o.view == view; });
    slot      = ViewSlot{};
    slot.game = game;
}
struct ViewStats {
    int evaluations[kViews]{};
    int constants{0};
    int not_ready{0}; // evaluations of a menu's view whose id did not have this frame's constants or tags (evaluated with its id anyway)
    int vram_warnings{0};
    int freed{0};
};
ViewStats g_view_stats{};
ViewportHandle g_vp_template{}; // (the game's viewport as it passes it: the ids made from it for freeing; "next" cleared)
bool           g_have_template{false};
struct Stats {
    int     evaluations[2]{};
    int     shared{0};          // DLSS evaluations with one instance for both eyes
    int     game_resets{0};
    int     forced_resets{0};
    int     errors{0};
    Result  last_error{0};
    int     token_mismatch{0};  // the frame token's eye differs from the eye of the frame the game reported rendering
    int     options_calls{0};
    float   jitter_min[2][2]{{1e9f, 1e9f}, {1e9f, 1e9f}};
    float   jitter_max[2][2]{{-1e9f, -1e9f}, {-1e9f, -1e9f}};
    int     jitter_n[2]{};
    // (fix151) The game's constants calls: in full stereo the left eye's view, the right eye's, a menu's view (routed); else (mono, AER);
    // not valid (no viewport of the game's); and how many reached the laser sight (its camera: fix149's test had none in full stereo).
    int     constants_left{0}, constants_right{0}, constants_routed{0}, constants_other{0}, constants_invalid{0}, constants_laser{0};
    int64_t since_us{0};
};
Stats    g_stats{};
int      g_first_logs{0};
uint32_t g_last_options_mode{0xFFFFFFFF}, g_last_options_w{0}, g_last_options_h{0};

bool ValidViewport(const ViewportHandle& vp) {
    return vp.structType == slabi::kViewportHandleType && vp.value < kOtherEye;
}

ViewportHandle OtherViewport(const ViewportHandle& vp) {
    ViewportHandle other;
    std::memcpy(&other, &vp, sizeof(other));
    other.value = vp.value + kOtherEye;
    return other;
}

ViewportHandle ViewViewport(const ViewportHandle& vp, int view) {
    ViewportHandle mine;
    std::memcpy(&mine, &vp, sizeof(mine));
    mine.value = vp.value + kViewStep * static_cast<uint32_t>(view);
    return mine;
}

bool ViewsUsable() {
    return g_views_enabled.load(std::memory_order_relaxed) && !g_views_failed.load(std::memory_order_relaxed) &&
           g_hooks_ok.load(std::memory_order_relaxed) && g_dlss_hooked.load(std::memory_order_relaxed);
}

// This thread's call is for a menu's view with an instance of its own (its place, else 0).
int RoutedView(const ViewportHandle& vp) {
    const int view = t_view;
    if (view < 1 || view >= kViews || !g_views_active.load(std::memory_order_acquire) || !ViewsUsable() || !ValidViewport(vp)) {
        return 0;
    }
    return view;
}

// Full stereo: both eyes are views of one frame, the left the world's (the frame's first), the right the game side's own (its
// second); a call is the right eye's when it is made for the second view (DlssViews' hooks say which view, on the thread drawing it).
bool NativeFrame(uint32_t frame) {
    return VR::get()->is_native_frame(frame);
}

bool RightView(uint32_t frame) {
    return NativeFrame(frame) && t_view == 1;
}

int SideOf(uint32_t frame) {
    if (NativeFrame(frame)) {
        return t_view == 1 ? 1 : 0;
    }
    return VR::get()->eye_for_frame(frame) == VRRuntime::Eye::RIGHT ? 1 : 0;
}

bool MirroredLocked(uint32_t id, uint32_t count) {
    const auto end = g_mirrored.begin() + std::min<uint32_t>(count, g_mirrored_count);
    return std::find(g_mirrored.begin(), end, id) != end;
}

bool Mirrored(uint32_t id) {
    std::scoped_lock _{g_state_mutex};
    return MirroredLocked(id, g_mirrored_count);
}

// A right-eye instance can be used (it has options, every hook is in place, the game's motion vectors are per eye).
bool CanSplit() {
    return g_enabled.load(std::memory_order_relaxed) && g_history_fix.load(std::memory_order_relaxed) && g_options_ready.load(std::memory_order_relaxed) &&
           g_layout_ok.load(std::memory_order_relaxed) && g_hooks_ok.load(std::memory_order_relaxed) && !g_right_failed.load(std::memory_order_relaxed);
}

// This frame's choice (g_state_mutex held).
const FrameChoice& ChoiceLocked(uint32_t frame) {
    auto& choice = g_choices[frame % g_choices.size()];
    if (choice.valid && choice.frame == frame) {
        return choice;
    }
    const auto& vr    = VR::get();
    // (Mono: one view every frame, so one instance follows it as on a monitor.)
    const bool  split = CanSplit() && vr->is_hmd_active() && !vr->is_mono_frame(frame);
    choice            = FrameChoice{frame, true, split};
    choice.ids        = g_mirrored;
    choice.count      = g_mirrored_count;
    // The histories so far are of the other arrangement: per-eye DLSS turned on or off, or the eyes swapped (F11 > VR).
    const bool swapped = vr->m_swap_eyes->value();
    // (Likewise full stereo on or off: its eyes' histories are the views', not the frames' that took turns.)
    const bool native  = vr->is_native_frame(frame);
    if (split != g_last_split || swapped != g_last_swap_eyes || native != g_last_native) {
        g_pending_reset[0] = g_pending_reset[1] = true;
        if (split != g_last_split) {
            static int told = 0;
            if (++told <= 10 || told % 100 == 0) {
                spdlog::info("[DLSS] {} from frame {}", split ? "each eye now has its own DLSS instance and history" : "both eyes share one DLSS instance again",
                    frame);
            }
        }
        g_last_split     = split;
        g_last_swap_eyes = swapped;
        g_last_native    = native;
    }
    return choice;
}

// Whether a call for this frame and game id goes to the right eye's instance (and which side's history it is).
struct Route {
    bool split{false};
    int  side{0};
    bool other{false};
};
Route RouteLocked(uint32_t frame, uint32_t id) {
    const auto& choice = ChoiceLocked(frame);
    Route       route;
    route.split = choice.split && std::find(choice.ids.begin(), choice.ids.begin() + std::min<uint32_t>(choice.count, 16), id) != choice.ids.begin() + std::min<uint32_t>(choice.count, 16);
    route.side  = route.split ? SideOf(frame) : 0;
    route.other = route.split && route.side == 1;
    return route;
}

void CountResult(Result r) {
    if (r == kWarnOutOfVram) {
        std::scoped_lock _{g_state_mutex};
        ++g_view_stats.vram_warnings;
    } else if (r != 0) {
        std::scoped_lock _{g_state_mutex};
        ++g_stats.errors;
        g_stats.last_error = r;
    }
}

Result OnDlssSetOptions(const ViewportHandle& vp, const void* options);

// (fix147) Readable and writable memory only (committed, not a guard page): the game's data is read 8 bytes at a time.
bool WritableRegion(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD kind = mbi.Protect & 0xFF;
    return kind == PAGE_READWRITE || kind == PAGE_WRITECOPY || kind == PAGE_EXECUTE_READWRITE || kind == PAGE_EXECUTE_WRITECOPY;
}

// (fix147) In this part of the game's data, each 8-byte value equal to DLSS's function changed to this module's (each at once, as the
// game may read it on another thread). (A read fault, as cannot be in committed memory: the rest of the part left alone.)
int SwapInPart(uint8_t* from, uint8_t* to, uintptr_t value, uintptr_t replacement, int room) {
    int swapped = 0;
    __try {
        for (auto* at = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(from) + 7) & ~uintptr_t{7}); at + 8 <= to && swapped < room; at += 8) {
            auto* slot = reinterpret_cast<volatile LONG64*>(at);
            if (static_cast<uintptr_t>(*slot) == value &&
                static_cast<uintptr_t>(InterlockedCompareExchange64(slot, static_cast<LONG64>(replacement), static_cast<LONG64>(value))) == value) {
                ++swapped;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return swapped;
}

// (fix147) The copies of DLSS's function's address the game already keeps (its own data: the place the address it got from Streamline
// was put, as Streamline's helpers keep it, in a static of their own) changed to this module's function. Their number.
int RedirectKeptCopies(void* function) {
    auto*      base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (base == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    const auto  value       = reinterpret_cast<uintptr_t>(function);
    const auto  replacement = reinterpret_cast<uintptr_t>(&OnDlssSetOptions);
    const auto* section     = IMAGE_FIRST_SECTION(nt);
    int         swapped     = 0;
    constexpr int kMost     = 16; // (more would not be the game's own copies of one function's address)
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections && swapped < kMost; ++i, ++section) {
        // (Its data: written to, not code.)
        if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0 || (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) {
            continue;
        }
        uint8_t*       at  = base + section->VirtualAddress;
        uint8_t* const end = at + section->Misc.VirtualSize;
        while (at < end && swapped < kMost) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(at, &mbi, sizeof(mbi)) == 0) {
                break;
            }
            uint8_t* const part_end = std::min(static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize, end);
            if (WritableRegion(mbi)) {
                swapped += SwapInPart(at, part_end, value, replacement, kMost - swapped);
            }
            at = part_end;
        }
    }
    return swapped;
}

// from_game: the game itself asked Streamline for the function just now (slGetFeatureFunction's hook): it is handed this module's
// function there should the in-place hook fail.
void HookDlssOptions(void* function, bool from_game) {
    std::scoped_lock _{g_hook_mutex};
    if (g_dlss_hooked.load() || g_dlss_hook_failed.load() || function == nullptr || function == reinterpret_cast<void*>(&OnDlssSetOptions)) {
        return;
    }
    auto* hook         = new FunctionHook(function, reinterpret_cast<void*>(&OnDlssSetOptions));
    g_dlss_set_options = hook; // (set before the hook goes in: the game may call it at once)
    if (hook->create()) {
        g_dlss_hooked = true;
        spdlog::info("[DLSS] hooked DLSS's set-options function ({:p})", function);
        return;
    }
    // (fix147) Not hooked in place (the hook never went in: nothing runs in it, its object kept, as every hook's is). The game is
    // handed this module's function in place of DLSS's: from now on where it asks Streamline for it, and in the copies of its address
    // it already keeps. This module's function calls DLSS's own, as the hook's trampoline did: the same options, the same results.
    g_dlss_set_options = nullptr;
    g_dlss_function.store(function, std::memory_order_release);
    g_dlss_direct.store(reinterpret_cast<DlssSetOptionsT*>(function), std::memory_order_release); // (before the game can have ours)
    g_dlss_redirected.store(true, std::memory_order_release);
    // (Asked for just now: the game has no copy yet, the one it is about to keep is handed ours. Else its copies searched for in its
    // data, about 45 MB, once.)
    const auto started = std::chrono::steady_clock::now();
    const int  swapped = from_game ? 0 : RedirectKeptCopies(function);
    const auto took    = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    // (Hooked only once the game has ours: handed it now, or a copy changed; else when it next asks for it, OnGetFeatureFunction. The
    // review: "hooked" with nothing redirected let the menus' views use instances with no options.)
    g_dlss_hooked      = from_game || swapped > 0;
    spdlog::info("[DLSS] (fix147) DLSS's set-options function ({:p}) could not be hooked in place (Streamline's DLSS is loaded too far from free "
                 "memory for the hook's jump): the game is handed the mod's own function in its place instead, which calls DLSS's ({})",
        function, from_game ? std::string{"the game asked for it just now: it gets the mod's"}
                            : std::format("the game had it before: {} copies of its address in the game's data changed, in {:.1f} ms", swapped, took));
    if (!from_game && swapped == 0) {
        spdlog::warn("[DLSS] (fix147) the game's own copy of DLSS's set-options function was not found in its data: per-eye DLSS starts only when "
                     "the game asks Streamline for the function again");
    }
}

// If the game asked for DLSS's set-options function before the hooks were in, it is looked up here, from a DLSS evaluation
// (DLSS is loaded then), once a second, a few times.
void TryResolveDlssOptions() {
    if (g_dlss_hooked.load(std::memory_order_relaxed) || g_dlss_hook_failed.load(std::memory_order_relaxed) || g_get_feature_function == nullptr ||
        g_resolve_attempts.load(std::memory_order_relaxed) >= 10) {
        return;
    }
    const int64_t now = NowUs();
    if (now < g_next_resolve_us.load(std::memory_order_relaxed)) {
        return;
    }
    g_next_resolve_us = now + 1'000'000;
    ++g_resolve_attempts;
    void*        function = nullptr;
    const Result r        = g_get_feature_function->get_original<GetFeatureFunctionT>()(slabi::kFeatureDLSS, "slDLSSSetOptions", function);
    if (r == 0 && function != nullptr) {
        HookDlssOptions(function, false);
    } else if (g_resolve_attempts.load() == 10) {
        spdlog::warn("[DLSS] DLSS's set-options function could not be found (Streamline result {}): per-eye DLSS is off", r);
    }
}

// --- The hooks ------------------------------------------------------------------------------------------------------------

// (fix153) Which DLSS the game runs: the game's own nvngx_dlss.dll (3.5.0 in Starfield 1.16.236's folder) or a newer one NGX loaded in
// its place (the NVIDIA app's override, from ProgramData\NVIDIA\NGX\models\dlss\...), found among the loaded modules by name and path;
// its file version (the override's folder number is its version too: 310 << 16 ...). Checked every 10 s until one is found, then
// every minute. DLSS 4's transformer model (preset K, 11) is asked for only from 310 on: the game's 3.5 has no such preset.
std::mutex        g_dlss_file_mtx;
std::string       g_dlss_file_text{"DLSS: not loaded yet"};
std::atomic<int>  g_dlss_major{0}; // (3 for 3.5.0; 310 for 310.x)
std::atomic<int>  g_dlss_minor{0}; // (fix167) 310.9 -> 9: DLSS 4.5's presets M and L from 310.5 on
int64_t           g_dlss_checked_ms{0};

uint64_t FileVersionOf(const wchar_t* path) {
    DWORD      handle = 0;
    const DWORD size  = GetFileVersionInfoSizeW(path, &handle);
    if (size == 0 || size > (1u << 20)) {
        return 0;
    }
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) {
        return 0;
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT               len  = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || info == nullptr || len < sizeof(VS_FIXEDFILEINFO)) {
        return 0;
    }
    return (static_cast<uint64_t>(info->dwFileVersionMS) << 32) | info->dwFileVersionLS;
}

std::string DlssVersionText() {
    std::scoped_lock _{g_dlss_file_mtx};
    return g_dlss_file_text;
}

// (The present thread, outside the Streamline lock: once a second until found, then no more (what is loaded stays loaded); the review.)
void CheckDlssFile() {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    static int tries = 0;
    // (fix167) Looked at again every 30 s once one is found (for 10 minutes), the newest kept: a newer one NGX loads later (the NVIDIA
    // app's override) was missed when the first found ended the search.
    // (At most 20 times once found: a module list taken on this thread costs a few ms, never every 30 s all session long.)
    static int    again = 0;
    const bool    found = g_dlss_major.load() > 0;
    const int64_t every = found ? 30000 : 1000;
    if (tries >= 600 || (found && again >= 20) || (g_dlss_checked_ms != 0 && now_ms - g_dlss_checked_ms < every)) {
        return; // (not in 10 minutes of looking: no DLSS, or no newer one)
    }
    ++tries;
    again += found ? 1 : 0;
    g_dlss_checked_ms = now_ms;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    uint64_t     best = 0;
    std::wstring best_path{};
    for (BOOL ok = Module32FirstW(snap, &entry); ok; ok = Module32NextW(snap, &entry)) {
        std::wstring path{entry.szExePath};
        std::wstring lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
        const bool own      = lower.find(L"nvngx_dlss.dll") != std::wstring::npos;
        const bool override = lower.find(L"\\ngx\\models\\dlss\\") != std::wstring::npos;
        if (!own && !override) {
            continue;
        }
        uint64_t version = FileVersionOf(path.c_str());
        // (An override without a version of its own: its folder's number, ...\versions\<major << 16 | ...>\files\....)
        if (version == 0 && override) {
            const auto at = lower.find(L"\\versions\\");
            if (at != std::wstring::npos) {
                const uint64_t folder = wcstoull(lower.c_str() + at + 10, nullptr, 10);
                version               = (folder >> 16) << 48;
            }
        }
        if (version > best) {
            best      = version;
            best_path = path;
        }
    }
    CloseHandle(snap);
    if (best == 0) {
        return;
    }
    const int major = static_cast<int>(best >> 48);
    const int minor = static_cast<int>((best >> 32) & 0xFFFF);
    if (major < g_dlss_major.load() || (major == g_dlss_major.load() && minor <= g_dlss_minor.load() && g_dlss_major.load() > 0)) {
        return; // (no newer one)
    }
    g_dlss_minor = minor;
    g_dlss_major = major;
    std::string text = std::format("DLSS: {}.{}.{}.{} ({}){}", major, (best >> 32) & 0xFFFF, (best >> 16) & 0xFFFF, best & 0xFFFF,
        [&] {
            std::string narrow{};
            for (const wchar_t c : best_path) {
                narrow.push_back(c < 128 ? static_cast<char>(c) : '?');
            }
            return narrow;
        }(),
        major > 310 || (major == 310 && minor >= 5) ? "; DLSS 4.5: presets M (performance), L (ultra performance), K (the others)"
        : major >= 310                              ? "; the transformer model (preset K) can be used"
                                                    : "; the transformer model needs DLSS 310 or newer (the NVIDIA app's override)");
    spdlog::info("[DLSS] (fix153) {}", text);
    // (Presets are read as DLSS makes an eye's instance (Streamline 2.2.0: only a mode or size change makes it again): found after the game
    // set its options, the transformer model comes with its next making, at a menu or a loading screen, both eyes at once.)
    if (major >= 310 && g_options_ready.load()) {
        spdlog::info("[DLSS] (fix153) found after the eyes' DLSS was made: the transformer model from the next menu or loading screen");
    }
    std::scoped_lock _{g_dlss_file_mtx};
    g_dlss_file_text = std::move(text);
}

Result OnDlssSetOptions(const ViewportHandle& vp, const void* options_in) {
    CallTimer timer{"DLSS's set-options (the game's id, the right eye's too)", vp.value}; // (fix179)
    std::scoped_lock sl{g_sl_mutex};
    timer.got_lock();
    // (fix153) DLSS 4's transformer model for every mode (the game sends preset C, an old CNN one, from its INI), when the DLSS loaded
    // is 310 or newer and the options are Streamline 2.2.0's (version 2: the presets at 0x3C-0x50, as the game's builder 0x142A72E60
    // writes them): a copy changed, the game's own left as it is.
    const void* options = options_in;
    alignas(8) uint8_t patched[0x58]{};
    const int          model = g_dlss_model.load(std::memory_order_relaxed);
    const int          major = g_dlss_major.load(std::memory_order_relaxed);
    const int          minor = g_dlss_minor.load(std::memory_order_relaxed);
    if (options_in != nullptr && model != 0 && major >= 310) {
        const auto* base = static_cast<const slabi::BaseStructure*>(options_in);
        if (base->structType == slabi::kDLSSOptionsType && base->structVersion == 2) {
            // (fix168) The chosen model (F11, "DLSS model"; a helper's research, NVIDIA's DLSS programming guide 310.6):
            //  1: the older CNN model, preset E (5): NVIDIA's guide gives it about a third of K's time; removed in 310.4, back (deprecated)
            //     from 310.6; before that, K (below);
            //  2: DLSS 4's transformer, preset K (11), in every mode (fix153);
            //  3: DLSS 4.5 as NVIDIA recommends (fix167, from 310.5): M (13) for performance, L (12) for ultra performance, K for the
            //     others; before 310.5, K.
            // The slots, as the game's builder writes them: 0x3C DLAA, 0x40 quality, 0x44 balanced, 0x48 performance, 0x4C ultra
            // performance, 0x50 ultra quality. Streamline 2.2.0 hands the numbers on as they are. The driver's own override (the NVIDIA
            // app's Model Presets: Recommended or a preset) wins over these (its profile comes before the game's request).
            constexpr uint32_t kPresetE = 5, kPresetK = 11, kPresetL = 12, kPresetM = 13;
            const bool         second = major > 310 || (major == 310 && minor >= 5);
            const bool         has_e  = major > 310 || (major == 310 && minor >= 6);
            // (Without E (310.0-310.5): K, the cheapest left; the game's C is gone from 310.4, DLSS's default (M in performance) then; the
            // review.)
            {
                std::memcpy(patched, options_in, sizeof(patched));
                for (size_t at = 0x3C; at <= 0x50; at += 4) {
                    const uint32_t preset = model == 1 && has_e      ? kPresetE
                                            : model <= 2 || !second ? kPresetK
                                            : at == 0x48            ? kPresetM
                                            : at == 0x4C            ? kPresetL
                                                                    : kPresetK;
                    std::memcpy(patched + at, &preset, sizeof(preset));
                }
                options = patched;
            }
            // (Told each time the choice changes: the DLSS step's time in the "[GpuTimer]" lines then says which model ran: about 1.0 ms an
            // eye for E (as the game's own 3.5), 2.3-2.5 for K, 3.4 for M at 1996x2150 on this laptop.)
            static int told_model = -1; // (the choice and the DLSS it was for: told again for a newer DLSS found later; the review)
            if (const int key = model * 1000000 + major * 1000 + minor; told_model != key) {
                told_model = key;
                spdlog::info("[DLSS] (fix168) the model asked for: {} (DLSS {}.{}; the game's presets were DLAA {}, quality {}, balanced {}, "
                             "performance {}, ultra performance {}, ultra quality {})",
                    options == options_in ? "the game's own presets"
                    : model == 1 && has_e ? "the older CNN model (preset E) in every mode"
                    : model == 1          ? "DLSS 4's transformer (preset K) in every mode (preset E needs DLSS 310.6 or newer)"
                    : model == 2 || !second
                        ? "DLSS 4's transformer (preset K) in every mode"
                        : "DLSS 4.5 as NVIDIA recommends: M for performance, L for ultra performance, K for the others",
                    major, minor, *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(options_in) + 0x3C),
                    *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(options_in) + 0x40),
                    *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(options_in) + 0x44),
                    *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(options_in) + 0x48),
                    *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(options_in) + 0x4C),
                    *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(options_in) + 0x50));
            }
        }
    }
    // DLSS's own function: through the in-place hook's trampoline, or (fix147: the game handed this function in its place) directly.
    DlssSetOptionsT* original = g_dlss_direct.load(std::memory_order_acquire);
    if (original == nullptr) {
        auto* hook = g_dlss_set_options;
        if (hook == nullptr) {
            return kErrorInvalidState; // (cannot be: this function is reached only by the hook or once DLSS's is known)
        }
        original = hook->get_original<DlssSetOptionsT>();
    }
    const Result r = original(vp, options);
    if (!ValidViewport(vp) || options == nullptr) {
        return r;
    }
    // The right eye's instance gets the same options (always, so per-eye DLSS can be turned on at any time).
    const auto   other = OtherViewport(vp);
    const Result r2    = original(other, options);
    // ...and each menu's view's (options are only data: an instance is made at its first evaluation). The game's "off", as it
    // lets go of its id, reaches them too.
    bool view_options[kViews]{};
    if (g_views_enabled.load(std::memory_order_relaxed)) {
        for (int view = 1; view < kViews; ++view) {
            view_options[view] = original(ViewViewport(vp, view), options) == 0;
        }
    }
    const auto*  start = static_cast<const slabi::DLSSOptionsStart*>(options);
    const bool   known = start->structType == slabi::kDLSSOptionsType;
    if (known && start->mode == 0) {
        timer.note = "to off"; // (fix179)
    }
    std::scoped_lock _{g_state_mutex};
    ++g_stats.options_calls;
    g_vp_template      = vp;
    g_vp_template.next = nullptr;
    g_have_template    = true;
    // (Only for the id a view's instance is for now: the game also sets "off" for its old id as it lets go of it, after the new
    // one is in use. A view's instance moves to a new id with its constants: OnSetConstants.)
    for (int view = 1; view < kViews; ++view) {
        auto& slot = g_view_slots[view];
        if (slot.game == 0xFFFFFFFF) {
            slot.game = vp.value;
        }
        if (slot.game == vp.value) {
            slot.options = view_options[view];
        }
    }
    if (r2 == 0) {
        if (!MirroredLocked(vp.value, g_mirrored_count)) {
            if (g_mirrored_count == g_mirrored.size()) {
                std::rotate(g_mirrored.begin(), g_mirrored.begin() + 1, g_mirrored.end()); // (the oldest out)
                --g_mirrored_count;
            }
            g_mirrored[g_mirrored_count++] = vp.value;
        }
        const bool changed = known && (start->mode != g_last_options_mode || start->outputWidth != g_last_options_w || start->outputHeight != g_last_options_h);
        if (changed && g_right_failed.exchange(false)) {
            g_right_failures = 0;
            spdlog::info("[DLSS] the game's DLSS options changed: per-eye DLSS is tried again");
        }
        if (!g_options_ready.exchange(true) || changed) {
            if (known) {
                static constexpr const char* kModes[] = {"off", "performance", "balanced", "quality", "ultra performance", "ultra quality", "DLAA"};
                g_last_options_mode = start->mode;
                g_last_options_w    = start->outputWidth;
                g_last_options_h    = start->outputHeight;
                spdlog::info("[DLSS] the game set DLSS to {} for {}x{} (viewport {}); the right eye's instance (viewport {}) has the same",
                    start->mode < 7 ? kModes[start->mode] : "an unknown mode", start->outputWidth, start->outputHeight, vp.value, other.value);
            } else {
                spdlog::info("[DLSS] the game set DLSS options (viewport {}); the right eye's instance (viewport {}) has the same", vp.value, other.value);
            }
        }
    } else {
        ++g_stats.errors;
        g_stats.last_error = r2;
        static int told    = 0;
        if (++told <= 5) {
            spdlog::warn("[DLSS] setting the right eye's DLSS options failed (Streamline result {})", r2);
        }
    }
    return r;
}

Result OnSetTag(const ViewportHandle& vp, const void* tags, uint32_t count, void* cmd) {
    CallTimer timer{"set tags", vp.value}; // (fix179)
    std::scoped_lock sl{g_sl_mutex};
    timer.got_lock();
    const auto original = g_set_tag->get_original<SetTagT>();
    // A menu's view: its own id's tags only (its DLSS step tags, then evaluates, on this thread).
    if (const int view = RoutedView(vp); view > 0) {
        const Result r = original(ViewViewport(vp, view), tags, count, cmd);
        t_tagged       = r == 0;
        return r;
    }
    // Full stereo: each eye's view tags its own pictures on the thread drawing it, the right eye's into its own id only, the left
    // eye's (and any not known to be a view's) into the game's only (not copied to the other: the right eye's own come after,
    // possibly at once on another thread).
    if (ValidViewport(vp) && CanSplit() && VR::get()->is_hmd_active() && Mirrored(vp.value) &&
        NativeFrame(VR::get()->m_render_frame_count.load())) {
        if (t_view == 1) {
            return original(OtherViewport(vp), tags, count, cmd);
        }
        vrmod::LaserSight::get().on_tags(tags, count, cmd);
        return original(vp, tags, count, cmd);
    }
    // (The game's own view's depth tag: for the gun's laser sight, LaserSight.)
    if (ValidViewport(vp)) {
        vrmod::LaserSight::get().on_tags(tags, count, cmd);
    }
    const Result r = original(vp, tags, count, cmd);
    // (No view known, while the menu's views have their own: as when the game lets go of its pictures, its tags cleared: theirs
    // too, so none of them keeps a picture the game makes again.)
    if (t_view < 0 && g_views_active.load(std::memory_order_acquire) && ViewsUsable() && ValidViewport(vp)) {
        for (int view = 1; view < kViews; ++view) {
            original(ViewViewport(vp, view), tags, count, cmd);
        }
    }
    // Tags (the game's color, depth and motion buffers) have no frame: both instances get them, so whichever eye is evaluated
    // next has this frame's. (Only while the right eye's instance can be used: a tag may be copied for each id.)
    if (CanSplit() && ValidViewport(vp) && VR::get()->is_hmd_active() && Mirrored(vp.value)) {
        original(OtherViewport(vp), tags, count, cmd);
    }
    return r;
}

Result OnSetTagForFrame(const FrameToken& frame, const ViewportHandle& vp, const void* tags, uint32_t count, void* cmd) {
    CallTimer timer{"set tags for a frame", vp.value}; // (fix179)
    std::scoped_lock sl{g_sl_mutex};
    timer.got_lock();
    const auto original = g_set_tag_for_frame->get_original<SetTagForFrameT>();
    if (const int view = RoutedView(vp); view > 0) {
        const Result r = original(frame, ViewViewport(vp, view), tags, count, cmd);
        t_tagged       = r == 0;
        return r;
    }
    if (ValidViewport(vp)) {
        const uint32_t index = frame;
        Route          route;
        {
            std::scoped_lock _{g_state_mutex};
            route = RouteLocked(index, vp.value);
        }
        if (route.other) {
            return original(frame, OtherViewport(vp), tags, count, cmd);
        }
    }
    return original(frame, vp, tags, count, cmd);
}

Result OnSetConstants(const Constants& values, const FrameToken& frame, const ViewportHandle& vp) {
    CallTimer timer{"set constants", vp.value}; // (fix179)
    std::scoped_lock sl{g_sl_mutex};
    timer.got_lock();
    const auto original = g_set_constants->get_original<SetConstantsT>();
    if (!ValidViewport(vp)) {
        std::scoped_lock _{g_state_mutex};
        ++g_stats.constants_invalid;
        return original(values, frame, vp);
    }
    const uint32_t index = frame;
    const bool     known = values.structType == slabi::kConstantsType;
    // (fix151) Each call counted by where it goes (fix149's test: in full stereo the laser got no camera at all, though the left eye's
    // DLSS ran), and full stereo's right eye's camera kept for the laser: its left eye's camera is it moved back by the eyes' distance.
    {
        const bool native = NativeFrame(index);
        std::scoped_lock _{g_state_mutex};
        if (RoutedView(vp) > 0) {
            ++g_stats.constants_routed;
        } else if (native) {
            ++(t_view == 1 ? g_stats.constants_right : g_stats.constants_left);
        } else {
            ++g_stats.constants_other;
        }
    }
    if (known && NativeFrame(index) && t_view == 1) {
        vrmod::LaserSight::get().on_right_constants(values, index);
    }
    // A menu's view (its scene setup sets them, on this thread): its own id's, its history started over when it was not evaluated
    // in the frame before (shown again, or new).
    if (const int view = RoutedView(vp); view > 0) {
        bool reset = false;
        {
            std::scoped_lock _{g_state_mutex};
            SlotFor(view, vp.value);
            auto& slot           = g_view_slots[view];
            // (Its options: copied with each of the game's; a new id's come after its first constants, in the same frame.)
            reset                = slot.last_frame == 0xFFFFFFFF || (index - slot.last_frame > 1 && slot.last_frame != index);
            slot.constants_frame = index;
            ++g_view_stats.constants;
        }
        Result r = 0;
        if (reset && known && values.reset != slabi::Boolean::eTrue) {
            auto&      flag  = const_cast<slabi::Boolean&>(values.reset);
            const auto saved = flag;
            flag             = slabi::Boolean::eTrue;
            r                = original(values, frame, ViewViewport(vp, view));
            flag             = saved;
        } else {
            r = original(values, frame, ViewViewport(vp, view));
        }
        CountResult(r);
        return r;
    }
    // (The game's own view's camera for this frame: for the gun's laser sight, LaserSight. Full stereo: the left eye's, the world's.)
    if (known && !RightView(index)) {
        vrmod::LaserSight::get().on_constants(values, index);
        std::scoped_lock _{g_state_mutex};
        ++g_stats.constants_laser;
    }
    Route          route;
    bool           force = false;
    {
        std::scoped_lock _{g_state_mutex};
        route          = RouteLocked(index, vp.value);
        const int side = route.side;
        if (known) {
            if (values.reset == slabi::Boolean::eTrue) {
                // A cut (a load, a teleport): the other eye's next frame starts over too. (Full stereo: each eye's view is told so by
                // the game itself, in the same frame.)
                ++g_stats.game_resets;
                if (!NativeFrame(index)) {
                    g_pending_reset[1 - side] = true;
                }
                g_pending_reset[side]     = false;
            } else if (g_pending_reset[side] || g_forced_frame[side] == index || g_reset_more[side] > 0) {
                force = true;
                if (g_forced_frame[side] != index) {
                    // (A new frame: the asked reset, or one of the frames after it, request_reset's.)
                    if (g_pending_reset[side]) {
                        g_pending_reset[side] = false;
                    } else if (g_reset_more[side] > 0) {
                        --g_reset_more[side];
                    }
                    g_forced_frame[side] = index;
                    ++g_stats.forced_resets;
                }
            }
            const float jx = values.jitterOffset.x, jy = values.jitterOffset.y;
            if (std::isfinite(jx) && std::isfinite(jy) && std::abs(jx) < 4.0f && std::abs(jy) < 4.0f) {
                g_stats.jitter_min[side][0] = std::min(g_stats.jitter_min[side][0], jx);
                g_stats.jitter_min[side][1] = std::min(g_stats.jitter_min[side][1], jy);
                g_stats.jitter_max[side][0] = std::max(g_stats.jitter_max[side][0], jx);
                g_stats.jitter_max[side][1] = std::max(g_stats.jitter_max[side][1], jy);
                ++g_stats.jitter_n[side];
            }
        }
    }
    const ViewportHandle target = route.other ? OtherViewport(vp) : vp;
    Result               r      = 0;
    if (force) {
        // The game's own constants, with its reset flag set for this call only.
        auto&      reset = const_cast<slabi::Boolean&>(values.reset);
        const auto saved = reset;
        reset            = slabi::Boolean::eTrue;
        r                = original(values, frame, target);
        reset            = saved;
    } else {
        r = original(values, frame, target);
    }
    CountResult(r);
    return r;
}

Result OnEvaluate(Feature feature, const FrameToken& frame, const BaseStructure** inputs, uint32_t count, void* cmd) {
    const auto original = g_evaluate->get_original<EvaluateT>();
    if (feature != slabi::kFeatureDLSS) {
        return original(feature, frame, inputs, count, cmd);
    }
    CallTimer timer{"evaluate DLSS"}; // (fix179)
    std::scoped_lock sl{g_sl_mutex};
    timer.got_lock();
    TryResolveDlssOptions();
    const uint32_t index = frame;
    // The game's viewport among the inputs (and where it is).
    const ViewportHandle* game_vp = nullptr;
    uint32_t              at      = 0;
    if (inputs != nullptr && count <= kMaxInputs) {
        for (uint32_t i = 0; i < count; ++i) {
            if (inputs[i] != nullptr && inputs[i]->structType == slabi::kViewportHandleType) {
                game_vp = static_cast<const ViewportHandle*>(inputs[i]);
                at      = i;
                break;
            }
        }
    }
    if (game_vp != nullptr) {
        timer.id = game_vp->value; // (fix179)
    }
    // A menu's view: with its own id (never the game's: the world's history would be built on it). Its options, this frame's
    // constants and this step's tags expected; counted when not there (Streamline then uses what it had for that id, or fails).
    if (const int view = game_vp != nullptr ? RoutedView(*game_vp) : 0; view > 0) {
        timer.note = "a menu's view's own instance"; // (fix179)
        const bool tagged = t_tagged;
        t_tagged          = false;
        {
            std::scoped_lock _{g_state_mutex};
            if (g_view_slots[view].game != game_vp->value) {
                ++g_view_stats.not_ready;
                SlotFor(view, game_vp->value); // (its instance is this id's from now on)
            }
            auto& slot = g_view_slots[view];
            if (!slot.options || slot.constants_frame != index || !tagged) {
                ++g_view_stats.not_ready;
            }
            slot.last_frame = index;
            slot.evaluated  = true;
            g_latest_frame  = index;
            ++g_view_stats.evaluations[view];
        }
        const BaseStructure* local[kMaxInputs]{};
        for (uint32_t i = 0; i < count; ++i) {
            local[i] = inputs[i];
        }
        const ViewportHandle mine = ViewViewport(*game_vp, view);
        local[at]                 = &mine;
        const Result r            = original(feature, frame, local, count, cmd);
        CountResult(r);
        // (Failing again and again: the menu's views go back to the game's own way, DLSS off behind menus: the game side asks.)
        if (Failed(r)) {
            if (g_view_failures.fetch_add(1) + 1 == 30) {
                g_views_failed = true;
                spdlog::error("[DLSS] a menu's view's own DLSS instance failed 30 times in a row (Streamline result {}): the menu's views no longer "
                              "have their own (DLSS off behind menus, as the game does)",
                    r);
            }
        } else {
            g_view_failures = 0;
        }
        return r;
    }
    {
        std::scoped_lock _{g_state_mutex};
        g_latest_frame = index;
        g_last_dlss_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        ++g_view_stats.evaluations[0];
    }
    // (The game's own view, DLSS about to read its depth: copied for the gun's laser sight while it is shown, LaserSight. Full
    // stereo: the left eye's, the world's view, whose camera the line is given in.)
    if (!RightView(index)) {
        vrmod::LaserSight::get().on_evaluate(cmd, index, reinterpret_cast<const void* const*>(inputs), count);
    }
    Route route;
    {
        const auto&    vr       = VR::get();
        const uint32_t rendered = vr->m_render_frame_count.load();
        std::scoped_lock _{g_state_mutex};
        if (game_vp != nullptr && ValidViewport(*game_vp)) {
            route = RouteLocked(index, game_vp->value);
        } else if (ChoiceLocked(index).split) {
            route.split = route.other = SideOf(index) == 1; // (split, but no viewport to remap: handled below)
            route.side  = SideOf(index);
        }
        if (route.split) {
            ++g_stats.evaluations[route.side];
        } else {
            ++g_stats.shared;
        }
        if (vr->eye_for_frame(rendered) != vr->eye_for_frame(index)) {
            ++g_stats.token_mismatch;
        }
        // (fix139) Foveated shading's eye check: did it give this frame's world view its pattern by this frame's turn? A miss: this
        // frame not marked but the next one already is (a step labelled a frame ahead). Not marked at all (its step not run yet, as
        // steps are recorded out of order, or no pattern given): counted apart.
        if (!RightView(index) && !vr->is_mono_frame(index) && !NativeFrame(index)) {
            if (vr->m_foveation_frames[index & 7].load(std::memory_order_relaxed) == index + 1) {
                vr->m_foveation_hits.fetch_add(1, std::memory_order_relaxed);
            } else if (vr->m_foveation_frames[(index + 1) & 7].load(std::memory_order_relaxed) == index + 2) {
                vr->m_foveation_misses.fetch_add(1, std::memory_order_relaxed);
            } else {
                vr->m_foveation_unmarked.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (g_first_logs < 4) {
            ++g_first_logs;
            spdlog::info("[DLSS] evaluated for frame {} (the frame the game reported rendering: {}), {} eye, {}", index, rendered,
                SideOf(index) == 1 ? "right" : "left",
                route.split ? (route.other ? "with the right eye's instance" : "with the game's instance") : "one instance for both eyes");
        }
    }
    // (fix179) For the slow calls' lines: an id evaluated for the first time (or again after it was freed) has DLSS make its instance in
    // this call (Streamline 2.2.0 makes it at its first evaluation: NGX's feature, its video memory).
    if (!route.other) {
        if (game_vp != nullptr && game_vp->value != g_last_game_eval_id) {
            g_last_game_eval_id = game_vp->value;
            timer.note     = "the game's instance, its first evaluation for this id (DLSS makes the instance in it)";
        } else {
            timer.note = "the game's instance";
        }
        const Result r = original(feature, frame, inputs, count, cmd);
        CountResult(r);
        if (route.split) {
            g_last_left_ok = !Failed(r);
        }
        return r;
    }
    if (game_vp == nullptr || !ValidViewport(*game_vp)) {
        // Not what was expected: per-eye DLSS goes off for the session (this frame's constants went to the right eye's id).
        if (g_layout_ok.exchange(false)) {
            spdlog::error("[DLSS] the game's DLSS call has no viewport among its {} inputs: per-eye DLSS is off", count);
        }
        const Result r = original(feature, frame, inputs, count, cmd);
        CountResult(r);
        return r;
    }
    // The right eye: the same call with its own viewport id in place of the game's.
    // (fix179: for its line, when slow: an instance let go or freed is made again in this call.)
    const bool right_new = !g_right_live.load(std::memory_order_relaxed) || game_vp->value != g_last_right_eval_id;
    g_last_right_eval_id = game_vp->value;
    timer.note = right_new ? "the right eye's instance, its first evaluation for this id or since it was let go (DLSS makes it in it)"
                           : "the right eye's instance";
    const BaseStructure* local[kMaxInputs]{};
    for (uint32_t i = 0; i < count; ++i) {
        local[i] = inputs[i];
    }
    const ViewportHandle other = OtherViewport(*game_vp);
    local[at]                  = &other;
    const Result r             = original(feature, frame, local, count, cmd);
    CountResult(r);
    g_right_last_us.store(NowUs(), std::memory_order_relaxed); // (fix165)
    g_right_last_presents.store(VR::get()->m_presents_done.load(std::memory_order_relaxed), std::memory_order_relaxed); // (fix173)
    g_right_live.store(true, std::memory_order_relaxed);
    // The right eye's instance keeps failing while the game's works (its picture would not be upscaled, showing the left
    // eye's): back to one instance until the options change or the setting is turned on again.
    if (Failed(r) && g_last_left_ok.load()) {
        if (g_right_failures.fetch_add(1) + 1 == 10) {
            g_right_failed = true;
            spdlog::error("[DLSS] the right eye's DLSS instance failed 10 times in a row (Streamline result {}): per-eye DLSS is off for now", r);
        }
    } else if (!Failed(r)) {
        g_right_failures = 0;
    }
    return r;
}

Result OnAllocate(void* cmd, Feature feature, const ViewportHandle& vp) {
    CallTimer timer{"allocate (the game's id, the right eye's too while it is used)", vp.value}; // (fix179)
    std::scoped_lock sl{g_sl_mutex};
    timer.got_lock();
    const auto   original = g_allocate->get_original<AllocateT>();
    const Result r        = original(cmd, feature, vp);
    // (Only while it is used: DLSS makes the right eye's resources at its first evaluation anyway.)
    if (feature == slabi::kFeatureDLSS && ValidViewport(vp) && CanSplit() && Mirrored(vp.value)) {
        original(cmd, feature, OtherViewport(vp));
    }
    return r;
}

Result OnFree(Feature feature, const ViewportHandle& vp) {
    CallTimer timer{"free (the game's id, the right eye's and the menu's views' with it)", vp.value}; // (fix179)
    std::scoped_lock sl{g_sl_mutex};
    timer.got_lock();
    const auto   original = g_free->get_original<FreeT>();
    const int64_t game_start = NowUs(); // (fix179: each free's own time, for its line)
    const Result r        = original(feature, vp);
    const int64_t game_us = NowUs() - game_start;
    if (feature == slabi::kFeatureDLSS && ValidViewport(vp)) {
        // (fix179, the review) This id's instances are gone: its next evaluation makes them again (for the slow calls' lines).
        if (g_last_game_eval_id == vp.value) {
            g_last_game_eval_id = 0xFFFFFFFF;
        }
        if (g_last_right_eval_id == vp.value) {
            g_last_right_eval_id = 0xFFFFFFFF;
        }
        bool mirrored = false;
        bool views[kViews]{};
        {
            std::scoped_lock _{g_state_mutex};
            mirrored = MirroredLocked(vp.value, g_mirrored_count);
            // (Taken out of the list: the game does not use a freed id again.)
            if (mirrored) {
                const auto end = g_mirrored.begin() + g_mirrored_count;
                (void)std::remove(g_mirrored.begin(), end, vp.value); // (the ids after it moved down one; the count below)
                --g_mirrored_count;
                // (fix173) No id left with a right eye's instance (it is freed with the game's just below): nothing for the let-go to free (the
                // test's log: it freed an id the right eye never used, "16387 25", 3 s after the game had freed both).
                if (g_mirrored_count == 0) {
                    g_right_live = false;
                }
            }
            // Its next frames start from nothing either way.
            g_pending_reset[0] = g_pending_reset[1] = true;
            for (int view = 1; view < kViews; ++view) {
                auto& slot  = g_view_slots[view];
                views[view] = slot.game == vp.value && slot.evaluated;
                if (slot.game == vp.value) {
                    slot = ViewSlot{};
                }
            }
            // (Those left when the game's next id came into use, as is usual: the game frees its old id a moment later.)
            std::erase_if(g_orphans, [&](const Orphan& o) {
                if (o.game != vp.value) {
                    return false;
                }
                views[o.view] = true;
                return true;
            });
        }
        int64_t right_us = -1;
        Result  right_r  = 0;
        if (mirrored) {
            const int64_t right_start = NowUs();
            right_r                   = original(feature, OtherViewport(vp));
            right_us                  = NowUs() - right_start;
        }
        // ...and the menu's views' instances made for it.
        int64_t views_us = 0;
        int     views_n  = 0;
        for (int view = 1; view < kViews; ++view) {
            if (views[view]) {
                const int64_t view_start = NowUs();
                original(feature, ViewViewport(vp, view));
                views_us += NowUs() - view_start;
                ++views_n;
                std::scoped_lock _{g_state_mutex};
                ++g_view_stats.freed;
            }
        }
        // (fix179) Each free's own time: the game's, the right eye's mirrored one (its result: 0 freed; 25, eErrorInvalidParameter: there was
        // none for that id), the menu's views'.
        const std::string views_text = views_n > 0 ? std::format(", the menu's views' {} {:.1f} ms", views_n, views_us / 1000.0) : std::string{};
        if (mirrored) {
            spdlog::info("[DLSS] the game freed its DLSS resources (viewport {}); the right eye's were freed too (fix179: the game's free took {:.1f} "
                         "ms, the right eye's {:.1f} ms (result {}){}; thread {})",
                vp.value, game_us / 1000.0, right_us / 1000.0, (int)right_r, views_text, GetCurrentThreadId());
        } else {
            static int told = 0;
            if (told++ < 40) {
                spdlog::info("[DLSS] (fix179) the game freed its DLSS resources (viewport {}) in {:.1f} ms; no right eye's instance for it{}; thread {}",
                    vp.value, game_us / 1000.0, views_text, GetCurrentThreadId());
            }
        }
    }
    return r;
}

Result OnGetFeatureFunction(Feature feature, const char* name, void*& function) {
    const Result r = g_get_feature_function->get_original<GetFeatureFunctionT>()(feature, name, function);
    // The game asks for DLSS's set-options function before its first use: hooked before it is called.
    if (r == 0 && feature == slabi::kFeatureDLSS && name != nullptr && function != nullptr && std::strcmp(name, "slDLSSSetOptions") == 0) {
        HookDlssOptions(function, true);
        // (fix147) Not hooked in place: the game gets this module's function instead of DLSS's (it calls DLSS's). (The game's
        // reference is where it keeps the address: Streamline's helpers pass their own static, read at each call after.)
        const void* mine = reinterpret_cast<void*>(&OnDlssSetOptions);
        if (g_dlss_redirected.load(std::memory_order_acquire) && (function == g_dlss_function.load(std::memory_order_acquire) || function == mine)) {
            function = reinterpret_cast<void*>(&OnDlssSetOptions);
            static std::atomic<int> told{0};
            if (told.fetch_add(1) < 4) {
                spdlog::info("[DLSS] (fix147) the game asked Streamline for DLSS's set-options function: it was handed the mod's own (which calls DLSS's)");
            }
        }
    }
    return r;
}

// The hook's pointer is set before the hook goes in (the game may call the function at once, from another thread); on a
// failure it is cleared again (the hook never went in, so nothing can be running in it).
void HookExport(HMODULE module, const char* name, void* detour, FunctionHook*& out, bool& ok) {
    auto* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (target == nullptr) {
        ok = false;
        spdlog::warn("[DLSS] Streamline has no {}", name);
        return;
    }
    auto* hook = new FunctionHook(target, detour);
    out        = hook;
    if (!hook->create()) {
        ok  = false;
        out = nullptr;
        spdlog::error("[DLSS] could not hook {}", name);
    }
}

bool Install() {
    std::scoped_lock _{g_install_mutex};
    if (g_installed.load()) {
        return true;
    }
    HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
    if (module == nullptr) {
        return false;
    }
    bool ok = true;
    HookExport(module, "slGetFeatureFunction", reinterpret_cast<void*>(&OnGetFeatureFunction), g_get_feature_function, ok);
    HookExport(module, "slSetTag", reinterpret_cast<void*>(&OnSetTag), g_set_tag, ok);
    HookExport(module, "slSetConstants", reinterpret_cast<void*>(&OnSetConstants), g_set_constants, ok);
    HookExport(module, "slEvaluateFeature", reinterpret_cast<void*>(&OnEvaluate), g_evaluate, ok);
    HookExport(module, "slAllocateResources", reinterpret_cast<void*>(&OnAllocate), g_allocate, ok);
    HookExport(module, "slFreeResources", reinterpret_cast<void*>(&OnFree), g_free, ok);
    if (GetProcAddress(module, "slSetTagForFrame") != nullptr) { // (Streamline 2.7 and later)
        HookExport(module, "slSetTagForFrame", reinterpret_cast<void*>(&OnSetTagForFrame), g_set_tag_for_frame, ok);
    }
    g_hooks_ok  = ok;
    g_installed = true;
    spdlog::info("[DLSS] Streamline hooked{} (per-eye DLSS {})", ok ? "" : " in part", ok ? "can be used" : "is off");
    return true;
}

const char* WhyNot() {
    if (!g_installed.load()) {
        return "Streamline is not loaded (no NVIDIA DLSS in this game session)";
    }
    if (!g_hooks_ok.load() || g_dlss_hook_failed.load()) {
        return "Streamline could not be hooked (see the log)";
    }
    if (!g_enabled.load()) {
        return "off in this menu";
    }
    if (!g_history_fix.load()) {
        return "the 'Nvidia DLSS and TAA Fix' option is off (each eye needs its own motion)";
    }
    if (VR::get()->is_mono()) {
        return "mono mode is on (one view every frame: one DLSS instance works as on a monitor)";
    }
    if (!g_dlss_hooked.load()) {
        return "DLSS is not in use (pick DLSS in the game's display settings; it needs an RTX card)";
    }
    if (!g_options_ready.load()) {
        return "waiting for the game to set DLSS options (change the DLSS quality once in the game's display settings)";
    }
    if (!g_layout_ok.load()) {
        return "the game's DLSS calls were not as expected (see the log)";
    }
    if (g_right_failed.load()) {
        return "the right eye's DLSS instance kept failing (see the log); tried again when the DLSS setting changes";
    }
    return nullptr;
}
} // namespace

UpscalerAfrNvidiaModule::UpscalerAfrNvidiaModule() {
    // Hooked as soon as Streamline is loaded (the game sets its DLSS options before the first frame is shown, and the rest of
    // the mod starts at that first frame).
    if (!Install()) {
        std::thread([] {
            for (int i = 0; i < 2400; ++i) { // two minutes
                if (Install()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            spdlog::info("[DLSS] Streamline was not loaded in the first two minutes: per-eye DLSS is not used");
        }).detach();
    }
}

std::optional<std::string> UpscalerAfrNvidiaModule::on_initialize() {
    g_enabled = m_per_eye->value();
    if (!g_installed.load()) {
        spdlog::info("[DLSS] Streamline is not loaded yet");
    }
    return std::nullopt;
}

void UpscalerAfrNvidiaModule::on_present() {
    CheckDlssFile(); // (fix153)
    const bool enabled = m_per_eye->value();
    if (enabled && !g_enabled.exchange(enabled) && g_right_failed.exchange(false)) {
        g_right_failures = 0; // turned on again: the right eye's instance is tried again
    }
    g_enabled = enabled;
    const bool views = m_per_view->value();
    if (views && !g_views_enabled.exchange(views) && g_views_failed.exchange(false)) {
        g_view_failures = 0;
    }
    g_views_enabled = views;
    g_dlss_model    = std::clamp(m_model->value(), 0, 3); // (a value out of range in the ini: as the menu shows it; the review)
    // A menu's view's instance not evaluated for a while (its menu closed, its preview gone): let go (its video memory).
    {
        // (fix179) The present thread waits here for the lock when a game thread is in a DLSS call: timed (logged over 2 ms).
        CallTimer        timer{"the present thread's check for instances to let go (it waits for the lock while a DLSS call runs)", 0xFFFFFFFF, true};
        std::scoped_lock sl{g_sl_mutex};
        timer.got_lock();
        ViewportHandle   stale[8]{};
        int              count = 0;
        ViewportHandle   right[8]{}; // (fix165)
        int              right_count = 0;
        {
            std::scoped_lock _{g_state_mutex};
            for (int view = 1; view < kViews; ++view) {
                auto& slot = g_view_slots[view];
                if (slot.evaluated && g_latest_frame - slot.last_frame > 120 && g_free != nullptr && g_have_template) {
                    stale[count]            = g_vp_template;
                    stale[count].value      = slot.game + kViewStep * static_cast<uint32_t>(view);
                    ++count;
                    slot.evaluated  = false;
                    slot.last_frame = 0xFFFFFFFF;
                    ++g_view_stats.freed;
                }
            }
            // (fix165) In mono with video memory over its budget: the right eye's instances let go (made again at their first evaluation),
            // only 2 s after the right eye was last evaluated (the GPU done with them).
            // (fix173: and 8 presents since, as the copies the GPU may still use are let go: now done as every mono menu opens, when frames
            // can be slow, video memory over its budget; the review.)
            if (g_right_unneeded.load() && g_right_live.load() && NowUs() - g_right_last_us.load() > 2'000'000 &&
                VR::get()->m_presents_done.load(std::memory_order_relaxed) - g_right_last_presents.load(std::memory_order_relaxed) >= 8 &&
                g_have_template && g_free != nullptr) {
                g_right_live = false;
                for (uint32_t i = 0; i < g_mirrored_count && right_count < 8; ++i) {
                    right[right_count]       = g_vp_template;
                    right[right_count].value = g_mirrored[i] + kOtherEye;
                    ++right_count;
                }
            }
        // (Instances of an id the game no longer uses, not freed with it a few seconds on: let go here.)
            std::erase_if(g_orphans, [&](const Orphan& o) {
                if (count >= 8 || !g_have_template || g_free == nullptr || g_latest_frame - o.since <= 240) {
                    return false;
                }
                stale[count]       = g_vp_template;
                stale[count].value = o.game + kViewStep * static_cast<uint32_t>(o.view);
                ++count;
                ++g_view_stats.freed;
                return true;
            });
        }
        for (int i = 0; i < count; ++i) {
            g_free->get_original<FreeT>()(slabi::kFeatureDLSS, stale[i]);
        }
        if (right_count > 0) {
            static int told = 0;
            std::string results;
            for (int i = 0; i < right_count; ++i) {
                const int64_t start = NowUs(); // (fix179: each free's time too)
                const Result  r     = g_free->get_original<FreeT>()(slabi::kFeatureDLSS, right[i]);
                results += std::format("{}{} {} ({:.1f} ms)", i == 0 ? "" : ", ", right[i].value, (int)r, (NowUs() - start) / 1000.0);
            }
            if (told++ < 20) {
                spdlog::info("[DLSS] (fix165) the right eye's instance let go ({}, not used for 2 s; viewport and result: {}): made again when two "
                             "pictures come back",
                    g_right_unneeded_menu.load() ? "(fix173) mono while a menu is shown" : "mono, video memory over its budget", results);
            }
        }
    }
    // A line every 30 s while DLSS runs.
    const int64_t now = NowUs();
    Stats         stats;
    ViewStats     view_stats;
    {
        std::scoped_lock _{g_state_mutex};
        if (g_stats.since_us == 0) {
            g_stats.since_us = now;
            return;
        }
        if (now - g_stats.since_us < 30'000'000) {
            return;
        }
        stats            = g_stats;
        g_stats          = Stats{};
        g_stats.since_us = now;
        view_stats       = g_view_stats;
        g_view_stats     = ViewStats{};
    }
    if (view_stats.evaluations[1] + view_stats.evaluations[2] + view_stats.evaluations[3] > 0 || view_stats.vram_warnings > 0 || view_stats.freed > 0) {
        spdlog::info("[DLSS] last 30 s, by view: the world's (or the only one) {}, the menu's views' with their own instances {} / {} / {} ({} not "
                     "ready: without this frame's constants or tags); their constants set {} times; instances let go {}; {} evaluations with the "
                     "video memory budget used up (Streamline's warning 39)",
            view_stats.evaluations[0], view_stats.evaluations[1], view_stats.evaluations[2], view_stats.evaluations[3], view_stats.not_ready,
            view_stats.constants, view_stats.freed, view_stats.vram_warnings);
    }
    const int evaluations = stats.evaluations[0] + stats.evaluations[1] + stats.shared;
    if (evaluations == 0) {
        return;
    }
    auto range = [&](int side) {
        if (stats.jitter_n[side] == 0) {
            return std::string{"none"};
        }
        return std::format("x {:+.2f}..{:+.2f}, y {:+.2f}..{:+.2f}", stats.jitter_min[side][0], stats.jitter_max[side][0], stats.jitter_min[side][1],
            stats.jitter_max[side][1]);
    };
    const char* why = WhyNot();
    spdlog::info("[DLSS] last {:.0f} s: {} evaluations ({} left / {} right with their own instances, {} shared); per-eye {}{}; resets: {} by the game, {} "
                 "passed to the other eye; jitter (pixels) left {}, right {}; {} frames whose token eye differed from the rendered frame's; {} option "
                 "calls; {} errors{}",
        static_cast<double>(now - stats.since_us) * 1e-6, evaluations, stats.evaluations[0], stats.evaluations[1], stats.shared, why ? "off: " : "on",
        why ? why : "", stats.game_resets, stats.forced_resets, range(0), range(1), stats.token_mismatch, stats.options_calls, stats.errors,
        stats.errors > 0 ? std::format(" (last: Streamline result {})", stats.last_error) : std::string{});
    spdlog::info("[DLSS] (fix151) the game's constants calls: full stereo's left eye {}, right eye {}, a menu's view {}, other frames {}, not a "
                 "viewport of the game's {}; given to the laser sight {}",
        stats.constants_left, stats.constants_right, stats.constants_routed, stats.constants_other, stats.constants_invalid, stats.constants_laser);
}

void UpscalerAfrNvidiaModule::on_draw_ui() {
    if (!ImGui::CollapsingHeader("Upscaler (AFR)")) {
        return;
    }
    m_per_eye->draw("Per-eye DLSS (each eye keeps its own DLSS history)");
    m_per_view->draw("Per-view DLSS behind see-through menus (test: the game did not start DLSS there, no anti-aliasing)");
    m_model->draw("DLSS model (from the next menu or loading screen)");
    ImGui::TextWrapped("Needs a DLSS 310 or newer (the NVIDIA app's override; the game's own 3.5 keeps its presets). The NVIDIA app's Model Presets "
                       "set to Recommended or a preset choose for the game: this choice then changes nothing.");
    ImGui::TextUnformatted(DlssVersionText().c_str());
    const char* why    = WhyNot();
    bool        in_use = false;
    {
        std::scoped_lock _{g_state_mutex};
        in_use = g_last_split;
    }
    if (why == nullptr) {
        ImGui::TextWrapped("Per-eye DLSS is %s.", in_use ? "in use" : "ready (used while the headset shows the game)");
    } else {
        ImGui::TextWrapped("Per-eye DLSS is not in use: %s.", why);
    }
    ImGui::TextWrapped("The game draws one eye per frame; DLSS builds each picture from the ones before, so with one instance each eye was "
                       "built on the other's (ghosting between the eyes). With this on, each eye has its own. Frame Generation still must be off.");
}

void UpscalerAfrNvidiaModule::on_config_load(const utility::Config& cfg, bool set_defaults) {
    m_per_eye->config_load(cfg, set_defaults);
    m_per_view->config_load(cfg, set_defaults);
    m_model->config_load(cfg, set_defaults);
    g_enabled       = m_per_eye->value();
    g_views_enabled = m_per_view->value();
    g_dlss_model    = std::clamp(m_model->value(), 0, 3);
}

void UpscalerAfrNvidiaModule::on_config_save(utility::Config& cfg) {
    m_per_eye->config_save(cfg);
    m_per_view->config_save(cfg);
    m_model->config_save(cfg);
}

void UpscalerAfrNvidiaModule::set_history_fix(bool on) {
    g_history_fix = on;
}

void UpscalerAfrNvidiaModule::set_right_unneeded(bool unneeded, bool menu) {
    g_right_unneeded_menu.store(unneeded && menu, std::memory_order_relaxed);
    g_right_unneeded.store(unneeded, std::memory_order_relaxed);
}

void UpscalerAfrNvidiaModule::take_call_time(uint32_t& calls, int64_t& total_us, int64_t& longest_us, int64_t& check_us, int64_t& check_longest_us) {
    calls            = g_call_count.exchange(0, std::memory_order_relaxed);
    total_us         = g_call_total_us.exchange(0, std::memory_order_relaxed);
    longest_us       = g_call_longest_us.exchange(0, std::memory_order_relaxed);
    check_us         = g_present_check_us.exchange(0, std::memory_order_relaxed);
    check_longest_us = g_present_check_longest_us.exchange(0, std::memory_order_relaxed);
}

void UpscalerAfrNvidiaModule::request_reset(const char* why, int frames) {
    uint64_t forced = 0;
    {
        std::scoped_lock _{g_state_mutex};
        forced = g_stats.forced_resets;
        g_pending_reset[0] = g_pending_reset[1] = true;
        for (auto& more : g_reset_more) {
            more = std::max(more, std::clamp(frames, 1, 8) - 1);
        }
    }
    static std::atomic<int> told{0};
    if (const int n = told.fetch_add(1) + 1; n <= 6 || n % 100 == 0) {
        spdlog::info("[DLSS] (fix160) both eyes' history cleared for their next {} frame(s): {} ({} time(s); forced resets so far {})", std::clamp(frames, 1, 8),
            why != nullptr ? why : "asked", n, forced);
    }
}

int UpscalerAfrNvidiaModule::set_view(int view) {
    const int before = t_view;
    t_view           = view;
    t_tagged         = false;
    return before;
}

void UpscalerAfrNvidiaModule::set_views_active(bool on) {
    if (g_views_active.exchange(on) != on) {
        static int told = 0;
        if (++told <= 12) {
            spdlog::info("[DLSS] the menu's views {}", on ? "now each have their own DLSS instance" : "no longer have their own DLSS instances");
        }
    }
}

bool UpscalerAfrNvidiaModule::views_ready() {
    // (The game's DLSS options seen: an id of the game's to give the views' ids the same.)
    return ViewsUsable() && g_options_ready.load(std::memory_order_relaxed);
}

bool UpscalerAfrNvidiaModule::eyes_ready() {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t at  = g_last_dlss_ms.load(std::memory_order_relaxed);
    return CanSplit() && g_dlss_hooked.load(std::memory_order_relaxed) && at != 0 && now - at < 1000;
}

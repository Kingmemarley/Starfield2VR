#include "VrSettingsTab.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <thread>

#include <safetyhook/easy.hpp>
#include <spdlog/spdlog.h>

#include <Framework.hpp>
#include <Mod.hpp>
#include "memory/GameCode.h"
#include "memory/ScanHelper.h"

namespace VrSettingsTab
{
    namespace
    {
        // ---- The game's Settings data (Starfield 1.16.236; RVAs, each checked at Install) ----
        // The menu's tab id for ours (the game's are 0-6; the game cuts a tab id to its low byte, and takes out a tab's rows by it), and
        // our rows' ids (low byte 0xC0-0xFF: the game's checkbox, slider and stepper handlers switch on the low byte, none of theirs
        // above 0x5A, so ours reach none of the game's settings).
        constexpr uint32_t kTabId   = 7;
        constexpr uint32_t kFirstId = 0x7C0;
        constexpr size_t   kMaxRows = 64;

        // BuildCategoryRows (0x14151BFB0, the command each opening of a tab runs: rbp = the model, rbx = the tab's id (a byte), rsi = the
        // rows' list (the model + 0x1E0); the list's write lock taken (0x14151BFE5), the tab's old rows taken out (0x14151BFFE), then
        // the tab's own builder (none above 6: ja to the lock's release and return). Hooked right after the old rows went: our rows
        // pushed there, under the game's lock, only for our tab.
        constexpr uintptr_t kRowsStartRva  = 0x151BFB0;
        constexpr uint8_t   kRowsStart[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20 };
        constexpr uintptr_t kRowsModelRva  = 0x151BFCA; // mov rbp,rdx
        constexpr uint8_t   kRowsModel[]   = { 0x48, 0x8B, 0xEA };
        constexpr uintptr_t kRowsTabRva    = 0x151BFCD; // mov rbx,rcx
        constexpr uint8_t   kRowsTab[]     = { 0x48, 0x8B, 0xD9 };
        constexpr uintptr_t kRowsListRva   = 0x151BFD0; // lea rcx,[rdx+1B8h] (then its slot 11: the list, into rsi)
        constexpr uint8_t   kRowsList[]    = { 0x48, 0x8D, 0x8A, 0xB8, 0x01, 0x00, 0x00 };
        constexpr uintptr_t kRowsListSetRva = 0x151BFDD; // mov rsi,rax
        constexpr uint8_t   kRowsListSet[] = { 0x48, 0x8B, 0xF0 };
        constexpr uintptr_t kRowsLockRva   = 0x151BFE5; // lea rcx,[rbp+1B0h] (the list's lock)
        constexpr uint8_t   kRowsLock[]    = { 0x48, 0x8D, 0x8D, 0xB0, 0x01, 0x00, 0x00 };
        // lea rcx,[rsi+28h]; mov rdx,rbx; call (take out the tab's rows); movzx eax,byte [rbx]; mov edi,..; mov r14d,..; cmp eax,6; ja
        constexpr uintptr_t kRowsAroundRva = 0x151BFF7;
        constexpr uint8_t   kRowsAround[]  = { 0x48, 0x8D, 0x4E, 0x28, 0x48, 0x8B, 0xD3, 0xE8, 0xED, 0xC0, 0x01, 0x00, 0x0F, 0xB6, 0x03, 0xBF, 0x00, 0xF0,
                                               0xBF, 0xFF, 0x41, 0xBE, 0x00, 0x00, 0xC0, 0xFF, 0x83, 0xF8, 0x06, 0x0F, 0x87, 0xE4, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kRowsHookRva   = 0x151C003;

        // BuildCategoryHeaders (0x14151AE50, run as Settings opens: rdi = the model, rsi = the tabs' list (the model + 0x110), r14 its
        // lock, taken; the list cleared, the game's 7 tabs pushed). Hooked right after the 7th push (0x14151B806): ours pushed there.
        constexpr uintptr_t kTabsStartRva  = 0x151AE50;
        constexpr uint8_t   kTabsStart[]   = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55 };
        constexpr uintptr_t kTabsListRva   = 0x151AE7A; // lea rcx,[rdx+0E8h]; mov rax,[rcx]; call [rax+58h]; mov rsi,rax
        constexpr uint8_t   kTabsList[]    = { 0x48, 0x8D, 0x8A, 0xE8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x58, 0x48, 0x8B, 0xF0 };
        constexpr uintptr_t kTabsLockRva   = 0x151AE91; // lea r14,[rdi+0E0h]
        constexpr uint8_t   kTabsLock[]    = { 0x4C, 0x8D, 0xB7, 0xE0, 0x00, 0x00, 0x00 };
        // lea rdx,[rsp+20h]; mov rcx,rsi; call PushHeader (the 7th tab); nop; mov [rbp-80h],r12; mov [rsp+60h],r12; mov [rsp+40h],r13
        constexpr uintptr_t kTabsAroundRva = 0x151B7FE;
        constexpr uint8_t   kTabsAround[]  = { 0x48, 0x8D, 0x54, 0x24, 0x20, 0x48, 0x8B, 0xCE, 0xE8, 0x65, 0x74, 0x01, 0x00, 0x90,
                                               0x4C, 0x89, 0x65, 0x80, 0x4C, 0x89, 0x64, 0x24, 0x60, 0x4C, 0x89, 0x6C, 0x24, 0x40 };
        constexpr uintptr_t kTabsHookRva   = 0x151B80B;

        // A row (GeneralSetting, 0x490 bytes on the stack as the game's builders make them; its values bound to the menu's names at
        // 0x14153B520): its constructor and destructor, and the push of a row into the list (it takes the row's values; the row is
        // destroyed after, as the game does).
        constexpr uintptr_t kRowCtorRva    = 0x1520D10;
        constexpr uint8_t   kRowCtor[]     = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20 };
        constexpr uintptr_t kRowDtorRva    = 0x1521160;
        constexpr uint8_t   kRowDtor[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8D, 0x05 };
        constexpr uintptr_t kPushRowRva    = 0x1530F50;
        constexpr uint8_t   kPushRow[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0xD9 };
        constexpr uintptr_t kPushRowSizeRva = 0x1530F6F; // add [rbx+40h],4E8h (a row in the list: 0x4E8 bytes)
        constexpr uint8_t   kPushRowSize[] = { 0x48, 0x81, 0x43, 0x40, 0xE8, 0x04, 0x00, 0x00 };
        constexpr uintptr_t kPushTabRva    = 0x1532C70;
        constexpr uint8_t   kPushTab[]     = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0xC2, 0x48, 0x8B, 0xD9 };
        constexpr uintptr_t kPushTabSizeRva = 0x1532C8F; // add [rbx+40h],0D8h (a tab in the list)
        constexpr uint8_t   kPushTabSize[] = { 0x48, 0x81, 0x43, 0x40, 0xD8, 0x00, 0x00, 0x00 };
        // The row's values as the menu's names are bound to them (lea r8,[rbx+60h] uID; +80h uType; +0A0h uCategory; +0C0h bEnabled).
        constexpr uintptr_t kBindIdRva     = 0x153B57B;
        constexpr uint8_t   kBindId[]      = { 0x4C, 0x8D, 0x43, 0x60 };
        constexpr uintptr_t kBindTypeRva   = 0x153B58F;
        constexpr uint8_t   kBindType[]    = { 0x4C, 0x8D, 0x83, 0x80, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kBindCatRva    = 0x153B5A6;
        constexpr uint8_t   kBindCat[]     = { 0x4C, 0x8D, 0x83, 0xA0, 0x00, 0x00, 0x00 };
        constexpr uintptr_t kBindEnRva     = 0x153B5BD;
        constexpr uint8_t   kBindEn[]      = { 0x4C, 0x8D, 0x83, 0xC0, 0x00, 0x00, 0x00 };
        // A stepper's choice added (it takes the fixed string, nulling ours), a fixed string made from text (as the game's assign does,
        // case kept) and let go.
        constexpr uintptr_t kAddOptionRva  = 0x14D68B0;
        constexpr uint8_t   kAddOption[]   = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x83, 0xC1, 0x28, 0x45, 0x33, 0xC0 };
        constexpr uintptr_t kMakeStringRva = 0x28CFB80;
        constexpr uint8_t   kMakeString[]  = { 0x44, 0x88, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54 };
        constexpr uintptr_t kReleaseStringRva = 0x28CEF30;
        constexpr uint8_t   kReleaseString[] = { 0x41, 0x55, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x83, 0x39, 0x00, 0x4C, 0x8B, 0xE9 };
        // A text value's constructor (a tab's name), and its set (slot 13 of its table: the table's slot checked).
        constexpr uintptr_t kTextCtorRva   = 0x254E600;
        constexpr uint8_t   kTextCtor[]    = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x33, 0xC0, 0x48 };
        constexpr uintptr_t kTextSetSlotRva = 0x4B1BE50;
        constexpr uintptr_t kTextSetRva    = 0x254E690;
        constexpr uint8_t   kTextSet[]     = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41 };
        // The tables of a whole-number value and of an on/off value (a tab's id and flags are made with them, as the game's do).
        constexpr uintptr_t kU32TableRva   = 0x4B076D8;
        constexpr uintptr_t kU32TableFirst = 0x38CAD0;
        constexpr uintptr_t kBoolTableRva  = 0x4B078A8;
        constexpr uintptr_t kBoolTableFirst = 0x38CAA0;
        // A tab's rows asked for again (posted: the rebuild runs in a later step), a tab reset to its defaults (for a tab above 6 only
        // that rebuild and the reply the menu waits for), a slider's value and text posted to its row.
        constexpr uintptr_t kRequestTabRva = 0x151BED0;
        constexpr uint8_t   kRequestTab[]  = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0xB6, 0xF2 };
        constexpr uintptr_t kResetTabRva   = 0x152BB90;
        constexpr uint8_t   kResetTab[]    = { 0x40, 0x56, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x89, 0x6C, 0x24, 0x58, 0x33, 0xF6, 0x4C, 0x89, 0x74, 0x24, 0x48, 0x48, 0x8B, 0xE9, 0x44, 0x0F, 0xB6 };
        constexpr uintptr_t kResetAboveRva = 0x152BBA5; // movzx r14d,dl; cmp r14d,6 (then ja: a tab above 6 only rebuilt, the reply sent)
        constexpr uint8_t   kResetAbove[]  = { 0x44, 0x0F, 0xB6, 0xF2, 0x41, 0x83, 0xFE, 0x06 };
        constexpr uintptr_t kPostSliderRva = 0x1537D50;
        constexpr uint8_t   kPostSlider[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x50 };
        // The events' readers (out, event): a checkbox {u8 on; u32 id @4; u8 gameplay @8}, a slider {float}, a stepper {u32 index}, a
        // tab {u32 id}.
        constexpr uint8_t   kDecodeHead[]  = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC };
        constexpr uintptr_t kDecodeCheckRva = 0x1536490;
        constexpr uintptr_t kDecodeSliderRva = 0x1535060;
        constexpr uintptr_t kDecodeStepperRva = 0x1535FF0;
        constexpr uintptr_t kDecodeTabRva  = 0x1536930;

        // The game's handlers (slot 1 of each event's table in the model: the model is the table's place minus its offset; each reads
        // its event with its reader, then runs, or queues, its work and returns 1).
        struct HandlerSpot
        {
            const char*    name;
            uintptr_t      slot;      // the slot's RVA
            uintptr_t      fn;        // the game's function there
            uintptr_t      model_at;  // its lea of the model
            const uint8_t* model_bytes;
            size_t         model_size;
            ptrdiff_t      model_offset;
        };
        constexpr uint8_t kCheckModel[]   = { 0x48, 0x8D, 0x8B, 0x78, 0xFF, 0xFF, 0xFF };
        constexpr uint8_t kSliderModel[]  = { 0x48, 0x8D, 0x4B, 0x88 };
        constexpr uint8_t kStepperModel[] = { 0x48, 0x8D, 0x4B, 0x80 };
        constexpr uint8_t kResetModel[]   = { 0x48, 0x8D, 0x8B, 0x70, 0xFF, 0xFF, 0xFF };
        // (fix190) Each spot's rvas as constants (its slot, the game's function there, its lea of the model), checked by them in Install.
        constexpr uintptr_t kCheckSlotRva    = 0x4C71830;
        constexpr uintptr_t kCheckFnRva      = 0x1518550;
        constexpr uintptr_t kCheckModelRva   = 0x151856E;
        constexpr uintptr_t kSliderSlotRva   = 0x4C717A0;
        constexpr uintptr_t kSliderFnRva     = 0x1511A60;
        constexpr uintptr_t kSliderModelRva  = 0x1511A7E;
        constexpr uintptr_t kStepperSlotRva  = 0x4C717B8;
        constexpr uintptr_t kStepperFnRva    = 0x1515D70;
        constexpr uintptr_t kStepperModelRva = 0x1515D8E;
        constexpr uintptr_t kResetSlotRva    = 0x4C71848;
        constexpr uintptr_t kResetFnRva      = 0x151A570;
        constexpr uintptr_t kResetModelRva   = 0x151A58E;
        constexpr HandlerSpot kCheckSpot{ "checkbox", kCheckSlotRva, kCheckFnRva, kCheckModelRva, kCheckModel, sizeof(kCheckModel), 0x88 };
        constexpr HandlerSpot kSliderSpot{ "slider", kSliderSlotRva, kSliderFnRva, kSliderModelRva, kSliderModel, sizeof(kSliderModel), 0x78 };
        constexpr HandlerSpot kStepperSpot{ "stepper", kStepperSlotRva, kStepperFnRva, kStepperModelRva, kStepperModel, sizeof(kStepperModel), 0x80 };
        constexpr HandlerSpot kResetSpot{ "defaults", kResetSlotRva, kResetFnRva, kResetModelRva, kResetModel, sizeof(kResetModel), 0x90 };
        // Their first bytes (the checkbox's, slider's and stepper's begin alike; the Defaults' with its own frame).
        constexpr uint8_t kHandlerHead[]  = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0x3D };
        constexpr uint8_t kResetHead[]    = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x3D };

        // The game's switches on a setting's id (the reason ours, low byte 0xC0-0xFF, reach none of its settings): the checkbox's
        // (cmp eax,53h; ja), the slider's (add eax,-4; cmp eax,4Eh; ja), the stepper's (add eax,-3; cmp eax,32h), the stepper's
        // VoiceLanguage (cmp dl,5Ah), the slider's volumes (the whole id 0x54-0x59).
        constexpr uintptr_t kCheckSwitchRva = 0x1518681;
        constexpr uint8_t   kCheckSwitch[]  = { 0x0F, 0xB6, 0xC0, 0x41, 0xBD, 0x01, 0x00, 0x00, 0x00, 0x45, 0x33, 0xFF, 0x83, 0xF8, 0x53, 0x0F, 0x87 };
        constexpr uintptr_t kSliderSwitchRva = 0x1511BA4;
        constexpr uint8_t   kSliderSwitch[] = { 0x40, 0x0F, 0xB6, 0xC6, 0x83, 0xC0, 0xFC, 0x4D, 0x8D, 0x74, 0x24, 0xFF, 0x83, 0xF8, 0x4E, 0x0F, 0x87 };
        constexpr uintptr_t kStepperSwitchRva = 0x1515EA9;
        constexpr uint8_t   kStepperSwitch[] = { 0x0F, 0xB6, 0xC2, 0x83, 0xC0, 0xFD, 0x41, 0xB9, 0x01, 0x00, 0x00, 0x00, 0x45, 0x33, 0xE4, 0x83, 0xF8, 0x32 };
        constexpr uintptr_t kStepperVoiceRva = 0x1517036;
        constexpr uint8_t   kStepperVoice[] = { 0x80, 0xFA, 0x5A };
        constexpr uintptr_t kSliderVolumeRva = 0x1512DEF;
        constexpr uint8_t   kSliderVolume[] = { 0x8D, 0x46, 0xAC, 0x83, 0xF8, 0x06, 0x73 };

        // A row's values (each a value of 0x20 bytes: its table, +8 its parent, +0x10 changed, +0x18 the value).
        constexpr size_t kRowSize     = 0x490;
        constexpr size_t kText        = 0x000;
        constexpr size_t kDescription = 0x020;
        constexpr size_t kId          = 0x060;
        constexpr size_t kType        = 0x080;
        constexpr size_t kCategory    = 0x0A0;
        constexpr size_t kEnabled     = 0x0C0;
        // ...and its nested data (a nested value: its data through its container at +0x28, slot 11): a slider's {fValue 0..1, +0x20 its
        // text}, a stepper's {uIndex, +0x20 its choices}, a checkbox's {bChecked}.
        constexpr size_t kSliderData  = 0x100;
        constexpr size_t kStepperData = 0x198;
        constexpr size_t kCheckData   = 0x288;
        // A tab (0x80 bytes): its name, +0x20 its id, +0x40 disabled, +0x60 a spinner shown.
        constexpr size_t kTabSize     = 0x80;

        using RowFn          = void (*)(void* row);
        using PushFn         = void (*)(void* list, void* item);
        using AddOptionFn    = void (*)(void* options, void** fixed_string);
        using MakeStringFn   = void (*)(void** out, const char* text, bool case_sensitive);
        using ReleaseStringFn = void (*)(void** fixed_string);
        using TextCtorFn     = void (*)(void* value);
        using TabFn          = void (*)(void* model, uint8_t tab);
        using PostSliderFn   = void (*)(void* shuttle, const void* refresh);
        using DecodeFn       = void* (*)(void* out, void* event);
        using HandlerFn      = int32_t (*)(void* sink, void* event);

        uintptr_t       g_base = 0;
        RowFn           g_row_ctor{ nullptr };
        RowFn           g_row_dtor{ nullptr };
        PushFn          g_push_row{ nullptr };
        PushFn          g_push_tab{ nullptr };
        AddOptionFn     g_add_option{ nullptr };
        MakeStringFn    g_make_string{ nullptr };
        ReleaseStringFn g_release_string{ nullptr };
        TextCtorFn      g_text_ctor{ nullptr };
        TabFn           g_request_tab{ nullptr };
        TabFn           g_reset_tab{ nullptr };
        PostSliderFn    g_post_slider{ nullptr };
        DecodeFn        g_decode_check{ nullptr };
        DecodeFn        g_decode_slider{ nullptr };
        DecodeFn        g_decode_stepper{ nullptr };
        DecodeFn        g_decode_tab{ nullptr };
        HandlerFn       g_check_original{ nullptr };
        HandlerFn       g_slider_original{ nullptr };
        HandlerFn       g_stepper_original{ nullptr };
        HandlerFn       g_reset_original{ nullptr };
        uintptr_t       g_u32_table = 0;
        uintptr_t       g_bool_table = 0;

        safetyhook::MidHook g_tabs_hook{};
        safetyhook::MidHook g_rows_hook{};

        // (Set once before Install, read only after.)
        std::vector<Row> g_rows;

        std::string           g_installed{ "not installed" };
        std::atomic<uint32_t> g_tabs_built{ 0 };
        std::atomic<uint32_t> g_rows_built{ 0 };
        std::atomic<uint32_t> g_changes{ 0 };
        std::atomic<uint32_t> g_thread_logged{ 0 }; // (a bit for each place)

        bool ReadPtr(uintptr_t at, uintptr_t& out)
        {
            __try {
                out = *reinterpret_cast<const uintptr_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // (fix190) The game's places by their 1.16.236 rva, for this build's game version (GameCode.h): its code checked there, and the
        // address (0 when not known for it).
        bool      SameCode(uintptr_t rva, const uint8_t* bytes, size_t n) { return GameCode::Same(rva, bytes, n); }
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        // A function of an object's table.
        template <class Fn>
        Fn Slot(void* object, int slot)
        {
            return reinterpret_cast<Fn>((*reinterpret_cast<uintptr_t* const*>(object))[slot]);
        }

        // A value written as the game's builders write one (the Interface tab's, 0x141521280): its parent told first when it asks
        // (its slot 5, then its slot 2 with the value), then the value set and marked changed (its slot 9) when it differs.
        void TellParent(uint8_t* value)
        {
            void* parent = *reinterpret_cast<void**>(value + 0x08);
            if (parent != nullptr && Slot<bool (*)(void*)>(parent, 5)(parent)) {
                Slot<void (*)(void*, void*)>(parent, 2)(parent, value);
            }
        }

        template <class T>
        void SetValue(uint8_t* value, T v)
        {
            TellParent(value);
            T* at = reinterpret_cast<T*>(value + 0x18);
            if (!(*at == v)) {
                *at = v;
                Slot<void (*)(void*, bool)>(value, 9)(value, true);
            }
        }

        void SetText(uint8_t* value, const char* text)
        {
            Slot<void (*)(void*, const char*)>(value, 13)(value, text);
        }

        // A row's nested data (its container's slot 11, as the game reaches it).
        uint8_t* DataOf(uint8_t* row, size_t nested)
        {
            void* container = row + nested + 0x28;
            return static_cast<uint8_t*>(Slot<void* (*)(void*)>(container, 11)(container));
        }

        bool Ours(uint32_t id)
        {
            return id >= kFirstId && id < kFirstId + g_rows.size() && g_rows[id - kFirstId].kind != Kind::Header;
        }

        bool Usable(const Row& r)
        {
            return !r.enabled || r.enabled();
        }

        // A slider's value as the menu's 0-1 (its bar moves in hundredths).
        float FractionOf(const Row& r)
        {
            const float lo = r.slider->range().x, hi = r.slider->range().y;
            return hi > lo ? std::clamp((r.slider->value() - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
        }

        std::string SliderText(const Row& r)
        {
            return r.show ? r.show(r.slider->value()) : std::format("{:.2f}", r.slider->value());
        }

        // A stepper's choice now: the combo's, or the one of the slider's values nearest its value.
        uint32_t IndexOf(const Row& r)
        {
            if (r.combo != nullptr) {
                const int n = static_cast<int>(r.options.size());
                return static_cast<uint32_t>(std::clamp(r.combo->value(), 0, std::max(n - 1, 0)));
            }
            if (r.slider != nullptr && !r.values.empty()) {
                size_t best = 0;
                for (size_t i = 1; i < r.values.size(); ++i) {
                    if (std::fabs(r.values[i] - r.slider->value()) < std::fabs(r.values[best] - r.slider->value())) {
                        best = i;
                    }
                }
                return static_cast<uint32_t>(best);
            }
            return 0;
        }

        // (The review) Each place's thread logged once: the tabs' and rows' builds and the handlers are expected on the game's one thread.
        void LogThread(uint32_t place, const char* where)
        {
            if ((g_thread_logged.fetch_or(place) & place) == 0) {
                spdlog::info("[VrTab] (fix186) {} first reached on thread {}", where, GetCurrentThreadId());
            }
        }

        // Our tab, made as the game makes its own (0x14151AEB7...): its name a text value, its id and two flags plain values with no
        // parent, marked changed; pushed, then its name let go (the rest has nothing to let go).
        void OnTabs(safetyhook::Context& ctx)
        {
            LogThread(1, "the tabs' build (Settings opened)");
            auto* list = reinterpret_cast<void*>(ctx.rsi);
            alignas(16) uint8_t tab[kTabSize]{};
            g_text_ctor(tab);
            for (size_t at : { size_t{ 0x20 }, size_t{ 0x40 }, size_t{ 0x60 } }) {
                *reinterpret_cast<uintptr_t*>(tab + at)  = at == 0x20 ? g_u32_table : g_bool_table;
                *reinterpret_cast<void**>(tab + at + 8)  = nullptr;
                tab[at + 0x10]                           = 1;
            }
            SetText(tab, "VR");
            SetValue<uint32_t>(tab + 0x20, kTabId);
            g_push_tab(list, tab);
            g_release_string(reinterpret_cast<void**>(tab + 0x18)); // (the push took it: nothing left to let go, as the game's own)
            // (The review) The menu asks for a tab's rows once per opening of Settings' panel (only Display again each time): our rows
            // built again now (a post, run after this), with the settings as they are now (changed in the F11 window meanwhile).
            g_request_tab(reinterpret_cast<void*>(ctx.rdi), static_cast<uint8_t>(kTabId));
            if (g_tabs_built.fetch_add(1) < 10) {
                spdlog::info("[VrTab] (fix186) Settings opened: the VR tab (id {}) added after the game's 7, its rows asked for", kTabId);
            }
        }

        void PushRow(void* list, size_t index)
        {
            const Row& r = g_rows[index];
            alignas(16) uint8_t row[kRowSize];
            g_row_ctor(row);
            SetText(row + kText, r.text.c_str());
            SetText(row + kDescription, r.description.c_str());
            SetValue<uint32_t>(row + kId, kFirstId + static_cast<uint32_t>(index));
            SetValue<uint32_t>(row + kType, static_cast<uint32_t>(r.kind));
            SetValue<uint32_t>(row + kCategory, kTabId);
            SetValue<bool>(row + kEnabled, Usable(r));
            switch (r.kind) {
            case Kind::Checkbox:
                SetValue<bool>(DataOf(row, kCheckData), r.toggle != nullptr && r.toggle->value());
                break;
            case Kind::Slider: {
                uint8_t* data = DataOf(row, kSliderData);
                SetValue<float>(data, FractionOf(r));
                SetText(data + 0x20, SliderText(r).c_str());
                break;
            }
            case Kind::Stepper: {
                uint8_t* data = DataOf(row, kStepperData);
                for (const auto& option : r.options) {
                    void* fixed = nullptr;
                    g_make_string(&fixed, option.c_str(), true);
                    g_add_option(data + 0x20, &fixed); // (it takes it: ours nulled)
                    g_release_string(&fixed);
                }
                SetValue<uint32_t>(data, IndexOf(r));
                break;
            }
            case Kind::Header:
                break;
            }
            g_push_row(list, row);
            g_row_dtor(row);
        }

        // A tab's rows built (each opening of a tab, our tab's too): the game took out the tab's old rows; for ours, ours pushed.
        void OnRows(safetyhook::Context& ctx)
        {
            const uint8_t tab = *reinterpret_cast<const uint8_t*>(ctx.rbx);
            if (tab != kTabId) {
                return;
            }
            LogThread(2, "the rows' build (the VR tab)");
            auto* list = reinterpret_cast<void*>(ctx.rsi);
            for (size_t i = 0; i < g_rows.size(); ++i) {
                PushRow(list, i);
            }
            if (g_rows_built.fetch_add(1) < 20) {
                spdlog::info("[VrTab] (fix186) the VR tab opened: its {} rows built", g_rows.size());
            }
        }

        void Changed(const Row& r, void* model, const std::string& how)
        {
            if (r.apply) {
                r.apply();
            }
            if (g_framework != nullptr) {
                g_framework->request_save_config();
            }
            if (r.rebuild && model != nullptr) {
                g_request_tab(model, static_cast<uint8_t>(kTabId)); // (other rows usable or not with it now)
            }
            if (g_changes.fetch_add(1) < 200) {
                spdlog::info("[VrTab] (fix186) {} -> {}", r.text, how);
            }
        }

        int32_t OnCheck(void* sink, void* event)
        {
            alignas(16) uint8_t out[32]{};
            g_decode_check(out, event);
            uint32_t id = 0;
            std::memcpy(&id, out + 4, sizeof(id));
            const bool on = out[0] != 0, gameplay = out[8] != 0;
            // (The game's own first, ours too: its work for an id it does not know is only the row's state set, ours.)
            const int32_t ret = g_check_original(sink, event);
            if (!gameplay && Ours(id)) {
                LogThread(4, "the checkbox handler (a VR row)");
                const Row& r = g_rows[id - kFirstId];
                if (r.kind == Kind::Checkbox && r.toggle != nullptr && r.toggle->value() != on) {
                    r.toggle->value() = on;
                    Changed(r, static_cast<uint8_t*>(sink) - kCheckSpot.model_offset, on ? "on" : "off");
                }
            }
            return ret;
        }

        int32_t OnStepper(void* sink, void* event)
        {
            alignas(16) uint8_t out[32]{};
            g_decode_stepper(out, event);
            uint32_t index = 0, id = 0;
            std::memcpy(&index, out, sizeof(index));
            std::memcpy(&id, out + 4, sizeof(id));
            const bool gameplay = out[8] != 0;
            const int32_t ret = g_stepper_original(sink, event);
            if (!gameplay && Ours(id)) {
                LogThread(8, "the stepper handler (a VR row)");
                const Row& r = g_rows[id - kFirstId];
                if (r.kind == Kind::Stepper && index < r.options.size() && index != IndexOf(r)) {
                    if (r.combo != nullptr) {
                        r.combo->value() = static_cast<int32_t>(index);
                    } else if (r.slider != nullptr && index < r.values.size()) {
                        r.slider->value() = r.values[index];
                    }
                    Changed(r, static_cast<uint8_t*>(sink) - kStepperSpot.model_offset, r.options[index]);
                }
            }
            return ret;
        }

        // A slider: ours handled here only (the game's work for an id it does not know would blank its text): the value set, and the row's
        // value and text posted to it as the game posts its own.
        int32_t OnSlider(void* sink, void* event)
        {
            alignas(16) uint8_t out[32]{};
            g_decode_slider(out, event);
            float    fraction = 0.0f;
            uint32_t id       = 0;
            std::memcpy(&fraction, out, sizeof(fraction));
            std::memcpy(&id, out + 4, sizeof(id));
            const bool gameplay = out[8] != 0;
            if (gameplay || !Ours(id) || g_rows[id - kFirstId].kind != Kind::Slider || g_rows[id - kFirstId].slider == nullptr) {
                return g_slider_original(sink, event);
            }
            LogThread(16, "the slider handler (a VR row)");
            const Row& r     = g_rows[id - kFirstId];
            auto*      model = static_cast<uint8_t*>(sink) - kSliderSpot.model_offset;
            const float lo = r.slider->range().x, hi = r.slider->range().y;
            const float f  = std::isfinite(fraction) ? std::clamp(fraction, 0.0f, 1.0f) : FractionOf(r);
            const float v  = lo + f * (hi - lo);
            const bool  changed = std::fabs(v - r.slider->value()) > 1e-6f;
            r.slider->value() = v;
            const std::string text = SliderText(r);
            // {u32 id; float value; a string {+0 its text (a pointer while +0xC, its capacity, is above 12), +8, +0xC capacity}}: the text
            // copied as it is posted.
            struct Refresh
            {
                uint32_t    id;
                float       value;
                const char* text;
                uint32_t    length;
                uint16_t    capacity;
                uint16_t    unused;
            };
            static_assert(sizeof(Refresh) == 0x18 && offsetof(Refresh, text) == 8 && offsetof(Refresh, capacity) == 0x14);
            const Refresh refresh{ id, f, text.c_str(), 0, 1024, 0 };
            void* shuttle = *reinterpret_cast<void**>(model + 0x08);
            if (shuttle != nullptr) {
                g_post_slider(shuttle, &refresh);
            }
            if (changed) {
                Changed(r, model, text);
            }
            return 1;
        }

        // Defaults (Y) on our tab: ours put back to their defaults here, then the game's own reset of a tab above 6 (its rows asked for
        // again and the reply the menu waits for), not the game's handler (it would also mark the display to be set up again on Back).
        int32_t OnReset(void* sink, void* event)
        {
            alignas(16) uint8_t out[32]{};
            g_decode_tab(out, event);
            uint32_t tab = 0;
            std::memcpy(&tab, out, sizeof(tab));
            if ((tab & 0xFF) != kTabId) {
                return g_reset_original(sink, event);
            }
            LogThread(32, "the Defaults handler (the VR tab)");
            auto* model = static_cast<uint8_t*>(sink) - kResetSpot.model_offset;
            for (const Row& r : g_rows) {
                if (r.toggle != nullptr) {
                    r.toggle->value() = r.toggle->default_value();
                }
                if (r.slider != nullptr) {
                    r.slider->value() = r.slider->default_value();
                }
                if (r.combo != nullptr) {
                    r.combo->value() = r.combo->default_value();
                }
            }
            for (const Row& r : g_rows) {
                if (r.apply) {
                    r.apply();
                }
            }
            if (g_framework != nullptr) {
                g_framework->request_save_config();
            }
            g_reset_tab(model, static_cast<uint8_t>(kTabId));
            spdlog::info("[VrTab] (fix186) Defaults on the VR tab: its settings put back to the mod's defaults");
            return 1;
        }

        // A handler's slot changed to ours, the game's there checked first (its function, its start, its lea of the model).
        bool PatchSlot(const HandlerSpot& spot, const uint8_t* head, size_t head_size, void* ours, HandlerFn& original)
        {
            // (fix190) Its places through GameCode (each spot's known and checked by its constants in Install first).
            const uintptr_t slot = spot.slot, fn_rva = spot.fn, model_at = spot.model_at;
            auto*     at = reinterpret_cast<uintptr_t*>(At(slot));
            uintptr_t fn = 0;
            if (!ReadPtr(reinterpret_cast<uintptr_t>(at), fn) || fn != At(fn_rva) || !SameCode(fn_rva, head, head_size) ||
                !SameCode(model_at, spot.model_bytes, spot.model_size)) {
                return false;
            }
            DWORD old = 0;
            if (!VirtualProtect(at, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
                return false;
            }
            original = reinterpret_cast<HandlerFn>(fn);
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(at), static_cast<LONG64>(reinterpret_cast<uintptr_t>(ours)));
            VirtualProtect(at, sizeof(uintptr_t), old, &old);
            return true;
        }

        void UnpatchSlot(const HandlerSpot& spot, HandlerFn original)
        {
            const uintptr_t slot = spot.slot; // (fix190) (known: patched only then)
            auto* at  = reinterpret_cast<uintptr_t*>(At(slot));
            DWORD old = 0;
            if (original != nullptr && VirtualProtect(at, sizeof(uintptr_t), PAGE_READWRITE, &old)) {
                InterlockedExchange64(reinterpret_cast<volatile LONG64*>(at), static_cast<LONG64>(reinterpret_cast<uintptr_t>(original)));
                VirtualProtect(at, sizeof(uintptr_t), old, &old);
            }
        }
    }

    void SetRows(std::vector<Row> rows)
    {
        if (rows.size() > kMaxRows) {
            spdlog::warn("[VrTab] (fix186) {} rows given, {} kept", rows.size(), kMaxRows);
            rows.resize(kMaxRows);
        }
        for (auto& r : rows) {
            // (A row without its setting is shown as a header.)
            if ((r.kind == Kind::Checkbox && r.toggle == nullptr) || (r.kind == Kind::Slider && r.slider == nullptr) ||
                (r.kind == Kind::Stepper && r.combo == nullptr && (r.slider == nullptr || r.values.size() != r.options.size())) ||
                (r.kind == Kind::Stepper && r.options.empty())) {
                spdlog::warn("[VrTab] (fix186) the row \"{}\" has no setting: shown as a heading", r.text);
                r.kind = Kind::Header;
            }
        }
        g_rows = std::move(rows);
    }

    void Install()
    {
        g_base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        // (fix190) Each run checked by its own constants (GameCode; the port tool reads them here).
        struct Check
        {
            uintptr_t   rva;
            bool        same;
            const char* what;
        };
        const Check checks[] = {
            { kRowsStartRva, SameCode(kRowsStartRva, kRowsStart, sizeof(kRowsStart)), "the rows' build" },
            { kRowsModelRva, SameCode(kRowsModelRva, kRowsModel, sizeof(kRowsModel)), "the rows' build (its model)" },
            { kRowsTabRva, SameCode(kRowsTabRva, kRowsTab, sizeof(kRowsTab)), "the rows' build (its tab)" },
            { kRowsListRva, SameCode(kRowsListRva, kRowsList, sizeof(kRowsList)), "the rows' build (its list)" },
            { kRowsListSetRva, SameCode(kRowsListSetRva, kRowsListSet, sizeof(kRowsListSet)), "the rows' build (its list kept)" },
            { kRowsLockRva, SameCode(kRowsLockRva, kRowsLock, sizeof(kRowsLock)), "the rows' build (its lock)" },
            { kRowsAroundRva, SameCode(kRowsAroundRva, kRowsAround, sizeof(kRowsAround)), "the rows' build (where ours go)" },
            { kTabsStartRva, SameCode(kTabsStartRva, kTabsStart, sizeof(kTabsStart)), "the tabs' build" },
            { kTabsListRva, SameCode(kTabsListRva, kTabsList, sizeof(kTabsList)), "the tabs' build (its list)" },
            { kTabsLockRva, SameCode(kTabsLockRva, kTabsLock, sizeof(kTabsLock)), "the tabs' build (its lock)" },
            { kTabsAroundRva, SameCode(kTabsAroundRva, kTabsAround, sizeof(kTabsAround)), "the tabs' build (where ours goes)" },
            { kRowCtorRva, SameCode(kRowCtorRva, kRowCtor, sizeof(kRowCtor)), "a row's constructor" },
            { kRowDtorRva, SameCode(kRowDtorRva, kRowDtor, sizeof(kRowDtor)), "a row's destructor" },
            { kPushRowRva, SameCode(kPushRowRva, kPushRow, sizeof(kPushRow)), "a row's push" },
            { kPushRowSizeRva, SameCode(kPushRowSizeRva, kPushRowSize, sizeof(kPushRowSize)), "a row's size" },
            { kPushTabRva, SameCode(kPushTabRva, kPushTab, sizeof(kPushTab)), "a tab's push" },
            { kPushTabSizeRva, SameCode(kPushTabSizeRva, kPushTabSize, sizeof(kPushTabSize)), "a tab's size" },
            { kBindIdRva, SameCode(kBindIdRva, kBindId, sizeof(kBindId)), "a row's id" },
            { kBindTypeRva, SameCode(kBindTypeRva, kBindType, sizeof(kBindType)), "a row's type" },
            { kBindCatRva, SameCode(kBindCatRva, kBindCat, sizeof(kBindCat)), "a row's tab" },
            { kBindEnRva, SameCode(kBindEnRva, kBindEn, sizeof(kBindEn)), "a row's usable flag" },
            { kAddOptionRva, SameCode(kAddOptionRva, kAddOption, sizeof(kAddOption)), "a stepper's choice added" },
            { kMakeStringRva, SameCode(kMakeStringRva, kMakeString, sizeof(kMakeString)), "a fixed string made" },
            { kReleaseStringRva, SameCode(kReleaseStringRva, kReleaseString, sizeof(kReleaseString)), "a fixed string let go" },
            { kTextCtorRva, SameCode(kTextCtorRva, kTextCtor, sizeof(kTextCtor)), "a text value's constructor" },
            { kTextSetRva, SameCode(kTextSetRva, kTextSet, sizeof(kTextSet)), "a text value's set" },
            { kRequestTabRva, SameCode(kRequestTabRva, kRequestTab, sizeof(kRequestTab)), "a tab's rows asked for" },
            { kResetTabRva, SameCode(kResetTabRva, kResetTab, sizeof(kResetTab)), "a tab's reset" },
            { kResetAboveRva, SameCode(kResetAboveRva, kResetAbove, sizeof(kResetAbove)), "a tab's reset (above 6)" },
            { kPostSliderRva, SameCode(kPostSliderRva, kPostSlider, sizeof(kPostSlider)), "a slider's refresh" },
            { kDecodeCheckRva, SameCode(kDecodeCheckRva, kDecodeHead, sizeof(kDecodeHead)), "the checkbox event's reader" },
            { kDecodeSliderRva, SameCode(kDecodeSliderRva, kDecodeHead, sizeof(kDecodeHead)), "the slider event's reader" },
            { kDecodeStepperRva, SameCode(kDecodeStepperRva, kDecodeHead, sizeof(kDecodeHead)), "the stepper event's reader" },
            { kDecodeTabRva, SameCode(kDecodeTabRva, kDecodeHead, sizeof(kDecodeHead)), "the tab event's reader" },
            { kCheckSwitchRva, SameCode(kCheckSwitchRva, kCheckSwitch, sizeof(kCheckSwitch)), "the checkbox handler's switch" },
            { kSliderSwitchRva, SameCode(kSliderSwitchRva, kSliderSwitch, sizeof(kSliderSwitch)), "the slider handler's switch" },
            { kStepperSwitchRva, SameCode(kStepperSwitchRva, kStepperSwitch, sizeof(kStepperSwitch)), "the stepper handler's switch" },
            { kStepperVoiceRva, SameCode(kStepperVoiceRva, kStepperVoice, sizeof(kStepperVoice)), "the stepper handler's language" },
            { kSliderVolumeRva, SameCode(kSliderVolumeRva, kSliderVolume, sizeof(kSliderVolume)), "the slider handler's volumes" },
        };
        if (g_base == 0) {
            g_installed = "the game's code not found: no VR tab";
            spdlog::warn("[VrTab] {}", g_installed);
            return;
        }
        for (const auto& c : checks) {
            if (!c.same) {
                g_installed = std::format("{} is not as in Starfield 1.16.236 (rva {:x}): no VR tab", c.what, c.rva);
                spdlog::warn("[VrTab] (fix186) {}", g_installed);
                return;
            }
        }
        // (fix190) The places it uses without a check of their own code (the text set's slot, the value tables and their first functions,
        // the handlers' slots, the two hooked places in the runs checked above): known in this game version, or no VR tab.
        if (!GameCode::Known({ kTextSetSlotRva, kU32TableRva, kU32TableFirst, kBoolTableRva, kBoolTableFirst, kCheckSlotRva, kSliderSlotRva,
                kStepperSlotRva, kResetSlotRva, kRowsHookRva, kTabsHookRva })) {
            g_installed = "the game's places it uses are not known in this game version (GameCode): no VR tab";
            spdlog::warn("[VrTab] (fix190) {}", g_installed);
            return;
        }
        uintptr_t text_set = 0, u32_first = 0, bool_first = 0;
        if (!ReadPtr(At(kTextSetSlotRva), text_set) || text_set != At(kTextSetRva) || !ReadPtr(At(kU32TableRva), u32_first) ||
            u32_first != At(kU32TableFirst) || !ReadPtr(At(kBoolTableRva), bool_first) || bool_first != At(kBoolTableFirst)) {
            g_installed = "the game's value tables are not as in Starfield 1.16.236: no VR tab";
            spdlog::warn("[VrTab] (fix186) {}", g_installed);
            return;
        }
        if (g_rows.empty()) {
            g_installed = "no rows given: no VR tab";
            spdlog::warn("[VrTab] (fix186) {}", g_installed);
            return;
        }
        g_row_ctor       = reinterpret_cast<RowFn>(At(kRowCtorRva));
        g_row_dtor       = reinterpret_cast<RowFn>(At(kRowDtorRva));
        g_push_row       = reinterpret_cast<PushFn>(At(kPushRowRva));
        g_push_tab       = reinterpret_cast<PushFn>(At(kPushTabRva));
        g_add_option     = reinterpret_cast<AddOptionFn>(At(kAddOptionRva));
        g_make_string    = reinterpret_cast<MakeStringFn>(At(kMakeStringRva));
        g_release_string = reinterpret_cast<ReleaseStringFn>(At(kReleaseStringRva));
        g_text_ctor      = reinterpret_cast<TextCtorFn>(At(kTextCtorRva));
        g_request_tab    = reinterpret_cast<TabFn>(At(kRequestTabRva));
        g_reset_tab      = reinterpret_cast<TabFn>(At(kResetTabRva));
        g_post_slider    = reinterpret_cast<PostSliderFn>(At(kPostSliderRva));
        g_decode_check   = reinterpret_cast<DecodeFn>(At(kDecodeCheckRva));
        g_decode_slider  = reinterpret_cast<DecodeFn>(At(kDecodeSliderRva));
        g_decode_stepper = reinterpret_cast<DecodeFn>(At(kDecodeStepperRva));
        g_decode_tab     = reinterpret_cast<DecodeFn>(At(kDecodeTabRva));
        g_u32_table      = At(kU32TableRva);
        g_bool_table     = At(kBoolTableRva);

        // (The handlers first: with them ours and the game's both handled, our rows not yet shown. Each slot checked before any changes.)
        // (fix190) Each spot by its constants (GameCode; the port tool reads them here): its slot, the game's function there, that
        // function's start and its lea of the model.
        uintptr_t now = 0;
        const struct
        {
            const HandlerSpot* spot;
            uintptr_t          slot;
            uintptr_t          fn;
            bool               same;
        } spots[] = {
            { &kCheckSpot, At(kCheckSlotRva), At(kCheckFnRva),
                SameCode(kCheckFnRva, kHandlerHead, sizeof(kHandlerHead)) && SameCode(kCheckModelRva, kCheckModel, sizeof(kCheckModel)) },
            { &kSliderSpot, At(kSliderSlotRva), At(kSliderFnRva),
                SameCode(kSliderFnRva, kHandlerHead, sizeof(kHandlerHead)) && SameCode(kSliderModelRva, kSliderModel, sizeof(kSliderModel)) },
            { &kStepperSpot, At(kStepperSlotRva), At(kStepperFnRva),
                SameCode(kStepperFnRva, kHandlerHead, sizeof(kHandlerHead)) && SameCode(kStepperModelRva, kStepperModel, sizeof(kStepperModel)) },
            { &kResetSpot, At(kResetSlotRva), At(kResetFnRva),
                SameCode(kResetFnRva, kResetHead, sizeof(kResetHead)) && SameCode(kResetModelRva, kResetModel, sizeof(kResetModel)) },
        };
        for (const auto& s : spots) {
            if (!ReadPtr(s.slot, now) || now != s.fn || !s.same) {
                g_installed = std::format("the game's {} handler is not as in Starfield 1.16.236 (or another mod has it): no VR tab", s.spot->name);
                spdlog::warn("[VrTab] (fix186) {}", g_installed);
                return;
            }
        }
        const bool patched = PatchSlot(kCheckSpot, kHandlerHead, sizeof(kHandlerHead), reinterpret_cast<void*>(&OnCheck), g_check_original) &&
                             PatchSlot(kSliderSpot, kHandlerHead, sizeof(kHandlerHead), reinterpret_cast<void*>(&OnSlider), g_slider_original) &&
                             PatchSlot(kStepperSpot, kHandlerHead, sizeof(kHandlerHead), reinterpret_cast<void*>(&OnStepper), g_stepper_original) &&
                             PatchSlot(kResetSpot, kResetHead, sizeof(kResetHead), reinterpret_cast<void*>(&OnReset), g_reset_original);
        auto undo = [] {
            UnpatchSlot(kCheckSpot, g_check_original);
            UnpatchSlot(kSliderSpot, g_slider_original);
            UnpatchSlot(kStepperSpot, g_stepper_original);
            UnpatchSlot(kResetSpot, g_reset_original);
        };
        if (!patched) {
            undo();
            g_installed = "the game's change handlers could not be hooked: no VR tab";
            spdlog::warn("[VrTab] (fix186) {}", g_installed);
            return;
        }
        // (The rows before the tab: a tab shown always finds its rows.)
        g_rows_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kRowsHookRva)), &OnRows);
        if (!g_rows_hook) {
            undo();
            g_installed = "could not hook the rows' build: no VR tab";
            spdlog::warn("[VrTab] (fix186) {}", g_installed);
            return;
        }
        g_tabs_hook = safetyhook::create_mid(reinterpret_cast<void*>(At(kTabsHookRva)), &OnTabs);
        if (!g_tabs_hook) {
            g_installed = "could not hook the tabs' build: no VR tab (its rows hooked but never asked for)";
            spdlog::warn("[VrTab] (fix186) {}", g_installed);
            return;
        }
        g_installed = "hooked";
        spdlog::info("[VrTab] (fix186) a VR tab (id {}) in the game's Settings with {} rows (ids {:#x}-{:#x}): the tabs' build (rva {:x}), the rows' "
                     "build (rva {:x}) and the checkbox, slider, stepper and Defaults handlers hooked",
            kTabId, g_rows.size(), kFirstId, kFirstId + g_rows.size() - 1, kTabsHookRva, kRowsHookRva);
    }

    std::string Status()
    {
        return std::format("VR tab in the game's Settings (fix186 test): {}; shown {} times, opened {} times, {} changes", g_installed,
            g_tabs_built.load(), g_rows_built.load(), g_changes.load());
    }
}

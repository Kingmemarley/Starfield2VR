#include "MenuClean.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <memory/FunctionHook.h>
#include <utility/RTTI.hpp>

#include "BuildConsole.h"
#include "MenuLayer.h"
#include "MessagesBlock.h"
#include "RttiNames.h"
#include <glm/glm.hpp>
#include <mods/VR.hpp>
#include "RE/S/ScaleformGFxMovie.h"
#include "memory/ScanHelper.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"

namespace MenuClean
{
    namespace
    {
        // ---- Scaleform: the movie's ActionScript root (CommonLibSF's and CommonLibF4's ASMovieRootBase) ----

        // A Scaleform value as Starfield has it (GFx::Value, 0x30 bytes: CommonLibSF's; the game's GetVariable writes its type at +0x18,
        // as read in its code; fix107 had CommonLibF4's 0x20-byte one: the type read from the wrong place, and the game wrote 16 bytes
        // past it). Only plain ones are made or read here (a number, true or false), which hold nothing to be let go of.
        struct GfxValue
        {
            void*    prev{ nullptr };             // 00 (the list of managed values: not used by plain ones)
            void*    next{ nullptr };             // 08
            void*    object_interface{ nullptr }; // 10
            uint32_t type{ 0 };                   // 18
            uint32_t pad{ 0 };
            uint64_t data{ 0 }; // 20
            uint64_t aux{ 0 };  // 28
        };
        static_assert(sizeof(GfxValue) == 0x30);
        constexpr uint32_t kTypeBoolean = 2, kTypeInt = 3, kTypeUInt = 4, kTypeNumber = 5, kTypeMask = 0x0F, kManagedBit = 0x40;
        constexpr uint32_t kSetSticky = 1; // (Movie::SetVarType: kept, and given again to a part the timeline makes anew)

        constexpr size_t kMovieRootOffset = 0x18; // Movie: RefCountBase (0x10), StateBag (0x8), then Ptr<ASMovieRootBase>
        constexpr const char* kRootKind   = "Scaleform::GFx::ASMovieRootBase";
        constexpr const char* kRootCoded  = ".?AVASMovieRootBase@GFx@Scaleform@@";
        constexpr size_t kAdvanceSlot     = 0x18; // MovieImpl: Advance (SetViewport, 0xc, and GetMouseState, 0x24, are used already)
        constexpr size_t kSetVariableSlot = 0x31, kGetVariableSlot = 0x32, kIsAvailableSlot = 0x37, kInvokeArgsSlot = 0x3A;

        using SetVariableFn = bool (*)(void* root, const char* path, const GfxValue* value, uint32_t set_type);
        using GetVariableFn = bool (*)(void* root, GfxValue* out, const char* path);
        using IsAvailableFn = bool (*)(void* root, const char* path);
        using InvokeArgsFn  = bool (*)(void* root, const char* path, GfxValue* result, const char* format, va_list args);

        struct RootFns
        {
            uintptr_t set{ 0 };
            uintptr_t get{ 0 };
            uintptr_t available{ 0 };
            uintptr_t invoke{ 0 };
            uintptr_t release{ 0 }; // the game's own letting go of a managed value (found in GetVariable, which calls it); 0: not found
        };
        using ReleaseFn = void (*)(GfxValue* value);

        bool InGame(uintptr_t at)
        {
            const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
            return at >= base && at < base + MemoryScan::mod_size;
        }

        bool ReadPointer(uintptr_t at, uintptr_t& out)
        {
            __try {
                out = *reinterpret_cast<const uintptr_t*>(at);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool DerivesFrom(const void* object, const char* type_name)
        {
            __try {
                return utility::rtti::derives_from(object, type_name);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Of that kind (or one made from it), by its coded type name (read from the game's type information: RttiNames), or by its
        // plain one through kananlib, which compares "class Scaleform::GFx::ASMovieRootBase" (fix106 gave kananlib the coded name,
        // and nothing matched). Only an object whose type table is in the game is looked at.
        bool IsKind(const void* object, const char* coded_name, const char* plain_name)
        {
            uintptr_t table = 0;
            if (object == nullptr || !ReadPointer(reinterpret_cast<uintptr_t>(object), table) || !InGame(table)) {
                return false;
            }
            if (RttiNames::HasBase(object, coded_name)) {
                return true;
            }
            const std::string as_class  = std::string{ "class " } + plain_name;
            const std::string as_struct = std::string{ "struct " } + plain_name;
            return DerivesFrom(object, as_class.c_str()) || DerivesFrom(object, as_struct.c_str());
        }

        // (Each call on its own, with an exception handler: a part of the menu gone, or the root not what it should be, is only a
        // failed call.)
        bool CallAvailable(const RootFns& f, void* root, const char* path, bool& out)
        {
            __try {
                out = reinterpret_cast<IsAvailableFn>(f.available)(root, path);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool CallRelease(const RootFns& f, GfxValue& value)
        {
            __try {
                reinterpret_cast<ReleaseFn>(f.release)(&value);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool CallGetRaw(const RootFns& f, void* root, const char* path, GfxValue& out, bool& ok)
        {
            __try {
                ok = reinterpret_cast<GetVariableFn>(f.get)(root, &out, path);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        std::atomic<bool> g_managed_seen{ false };

        // A value read from the movie. A managed one (an object: not what is read here, but a path may name one) is let go at once
        // by the game's own function (it is in the movie's list of managed values, which a value left on this stack would spoil);
        // without that function it is not read at all (the menu backgrounds are then left as they are).
        bool CallGet(const RootFns& f, void* root, const char* path, GfxValue& out, bool& ok)
        {
            if (!CallGetRaw(f, root, path, out, ok)) {
                return false;
            }
            if ((out.type & 0x40) != 0) {
                const bool let_go = f.release != 0 && CallRelease(f, out);
                if (!g_managed_seen.exchange(true)) {
                    spdlog::warn("[MenuClean] {} is an object, not a plain value ({})", path, let_go ? "let go" : "could not be let go");
                }
                out = GfxValue{};
                ok  = false;
            }
            return true;
        }

        bool CallSetBool(const RootFns& f, void* root, const char* path, bool on, bool& ok)
        {
            __try {
                GfxValue value{};
                value.type = kTypeBoolean;
                value.data = on ? 1 : 0;
                ok         = reinterpret_cast<SetVariableFn>(f.set)(root, path, &value, kSetSticky);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool CallSetFalse(const RootFns& f, void* root, const char* path, bool& ok)
        {
            return CallSetBool(f, root, path, false, ok);
        }

        // (fix178) A number set in the movie (x, y, a scale).
        bool CallSetNumber(const RootFns& f, void* root, const char* path, double number, bool& ok)
        {
            __try {
                GfxValue value{};
                value.type = kTypeNumber;
                std::memcpy(&value.data, &number, sizeof(number));
                ok = reinterpret_cast<SetVariableFn>(f.set)(root, path, &value, kSetSticky);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // (fix178) An ActionScript function with two whole numbers ("%d %d", each an int in an 8-byte slot, as CallWithInt's one), its
        // result not wanted.
        bool CallWithInts(const RootFns& f, void* root, const char* path, int a, int b, bool& ok)
        {
            __try {
                alignas(8) uint64_t slots[3]{ static_cast<uint64_t>(static_cast<int64_t>(a)), static_cast<uint64_t>(static_cast<int64_t>(b)), 0 };
                ok = reinterpret_cast<InvokeArgsFn>(f.invoke)(root, path, nullptr, "%d %d", reinterpret_cast<va_list>(slots));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // An ActionScript function with one whole number ("%d": Scaleform's InvokeArgs reads it from the argument list, an int in
        // an 8-byte slot), its result not wanted (null: allowed).
        bool CallWithInt(const RootFns& f, void* root, const char* path, int argument, bool& ok)
        {
            __try {
                alignas(8) uint64_t slots[2]{ static_cast<uint64_t>(static_cast<int64_t>(argument)), 0 };
                ok = reinterpret_cast<InvokeArgsFn>(f.invoke)(root, path, nullptr, "%d", reinterpret_cast<va_list>(slots));
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The movie's definition (its file: which movie this is; a new movie made where an old one was has another).
        bool MovieDefOf(void* movie, void*& def)
        {
            __try {
                def = reinterpret_cast<RE::Scaleform::GFx::MovieImpl*>(movie)->GetMovieDef();
                return def != nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool UrlOf(void* def, const char*& url)
        {
            __try {
                url = reinterpret_cast<RE::Scaleform::GFx::MovieDef*>(def)->GetFileURL();
                return url != nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // ---- What is taken out of which menu (the game's interface files of 1.16.236 and a tested modlist's, read from the files:
        // fix123's research; the parts counts written for 1.10.32 had not matched any real file) ----

        struct Recipe
        {
            const char*                file;   // the movie's file name, lower case, without folder, "_lrg" or extension
            std::array<int, 2>         parts;  // the parts its clip has, all in place, in the versions known (0: none; both 0: none removed by place)
            // Each version's mark, told apart by it (a menu's count may change within its own timeline, or be one part off the other
            // version's): a named part it has ("name"), or has not ("!name"); "@lrg": its large file (else "!@lrg"). Null: none.
            std::array<const char*, 2> marks;
            std::array<const char*, 3> checks; // named parts that must be there too
            std::array<int, 2>         remove; // places of the parts removed by place, highest first (-1: none)
            std::array<const char*, 2> hide;   // named parts hidden
            const char*                what;   // for the log
            const char*                clip{ nullptr }; // its clip when not root1.Menu_mc (fix133: the container's and barter's are in a wrapper)
        };
        // (Places counted from the bottom of the clip: its timeline's layers from the lowest, each a part in every frame.)
        constexpr Recipe kRecipes[] = {
            // 84 parts (1.16, and a tested modlist's HUD Pack and Undelayed Menus versions): 0 Animated3DReveal_mc (a dark-blue block over the
            // 3D view while it opens and changes), 1 and 2 dividers, 3 the black edges (a bitmap, four times, faded in to half), 4 a disc,
            // 5 PlayerPaperDoll_mc ... 83 MenuHider_mc.
            { "datamenu", { 84, 0 }, { nullptr, nullptr }, { "Animated3DReveal_mc", "PlayerPaperDoll_mc", "MenuHider_mc" }, { 3, -1 },
                { "Animated3DReveal_mc", nullptr }, "its dark edges and the dark block it opens with" },
            // 16 parts (1.16, a tested modlist's HUD Pack; 15 in the large file): 0 a dark-blue block faded out as it opens, 1 the black edges,
            // 2 PreviewSceneRect_mc ... (12 QuantityVignette_mc, shown for the quantity box: left as it is).
            { "inventorymenu", { 16, 15 }, { "!@lrg", "@lrg" }, { "PreviewSceneRect_mc", "ItemList_mc", "ButtonBar_mc" }, { 1, 0 }, { nullptr, nullptr },
                "its dark edges and the dark block it opens with" },
            // 10 parts: 0 a dark-blue picture over the whole screen, 1 PreviewSceneRect_mc, 2 Vignette_mc (black edges)...
            { "missionmenu", { 10, 0 }, { nullptr, nullptr }, { "PreviewSceneRect_mc", "MissionsList_mc", "Vignette_mc" }, { 0, -1 },
                { "Vignette_mc", nullptr }, "its dark-blue background and dark edges" },
            // 34 parts as it opens (a tested modlist's Expanded Power Menu, with SelectPowersList_mc; 35 as it closes), 35 in 1.16 (with
            // FavoriteEssenceButton_mc): 0 the dark-blue "hologram" picture with stars, 1 a black edge, 2 SelectionInfo_mc...
            { "powersmenu", { 34, 35 }, { "SelectPowersList_mc", "FavoriteEssenceButton_mc" }, { "SelectionInfo_mc", "ButtonBar_mc", nullptr }, { 1, 0 },
                { nullptr, nullptr }, "its dark-blue hologram background and dark edges" },
            // 12 parts: 0 Background_mc (the full background), 1 GameLogo_mc ... 10 BlackLoading_mc (left as it is).
            { "pausemenu", { 0, 0 }, { nullptr, nullptr }, { "Background_mc", nullptr, nullptr }, { -1, -1 }, { "Background_mc", nullptr }, "its background" },
            { "shipcrewmenu", { 0, 0 }, { nullptr, nullptr }, { "Vignette_mc", nullptr, nullptr }, { -1, -1 }, { "Vignette_mc", nullptr }, "its dark edges" },
            // (Its picture is shown and hidden by the menu itself as it changes page: hidden again at each look.)
            { "researchmenu", { 0, 0 }, { nullptr, nullptr }, { "Background_mc", nullptr, nullptr }, { -1, -1 }, { "Background_mc", nullptr },
                "its background picture" },
            // (fix133, a helper's research of a tested modlist's files: HUD Pack's and Undelayed Menus' (large) versions as 1.16's.) 12 parts,
            // in a wrapper that fades it in (root1.FilterHolder_mc): 0 the black edges (a bitmap, four times, at half), 1
            // PreviewSceneRect_mc (the item's 3D view: the dark blue behind the item is that view's own background, cut out by its
            // depth), 2 ButtonBar_mc...
            { "containermenu", { 12, 0 }, { nullptr, nullptr }, { "PreviewSceneRect_mc", "ItemList_mc", "ButtonBar_mc" }, { 0, -1 }, { nullptr, nullptr },
                "its dark edges", "root1.FilterHolder_mc.Menu_mc" },
            // 13 parts, as the container's (8 VendorCreditsHeader_mc), in root1.MenuWrapper_mc.
            { "bartermenu", { 13, 0 }, { nullptr, nullptr }, { "PreviewSceneRect_mc", "ItemList_mc", "VendorCreditsHeader_mc" }, { 0, -1 },
                { nullptr, nullptr }, "its dark edges", "root1.MenuWrapper_mc.Menu_mc" },
            // The workbenches (1.16, Compact Crafting UI, 120 FPS - Smooth UI's large files; 13 or 10 parts): 0 a dark block seen only while
            // it opens, 1 CutBackground_mc (the world behind it darkened to 40%, its panels cut out of it): hidden by name.
            { "weaponscraftingmenu", { 0, 0 }, { nullptr, nullptr }, { "CutBackground_mc", nullptr, nullptr }, { -1, -1 }, { "CutBackground_mc", nullptr },
                "its darkening of the world behind it" },
            { "armorcraftingmenu", { 0, 0 }, { nullptr, nullptr }, { "CutBackground_mc", nullptr, nullptr }, { -1, -1 }, { "CutBackground_mc", nullptr },
                "its darkening of the world behind it" },
            { "drugscraftingmenu", { 0, 0 }, { nullptr, nullptr }, { "CutBackground_mc", nullptr, nullptr }, { -1, -1 }, { "CutBackground_mc", nullptr },
                "its darkening of the world behind it" },
            { "industrialcraftingmenu", { 0, 0 }, { nullptr, nullptr }, { "CutBackground_mc", nullptr, nullptr }, { -1, -1 },
                { "CutBackground_mc", nullptr }, "its darkening of the world behind it" },
            { "foodcraftingmenu", { 0, 0 }, { nullptr, nullptr }, { "CutBackground_mc", nullptr, nullptr }, { -1, -1 }, { "CutBackground_mc", nullptr },
                "its darkening of the world behind it" },
        };

        // The movies that may be see-through, looked at (their clip's parts logged) but nothing taken out; with their clip when not
        // Menu_mc. The Skills menu: its parts are on its root itself (18 in a tested modlist's Jedi Mojo version, 17 in 1.16: 0
        // PreviewSceneRect_mc, 1 a blue background, 2 the black edges, 3.. the skill tabs...); its script places parts by number
        // (addChildAt, setChildIndex, getChildAt), which removing two could break: looked at first, its live parts logged.
        struct LookedAt
        {
            const char* file;
            const char* clip;
        };
        // (fix163: the terminal, the mission board, the slate and the Status screen logged too, while see-through: their backgrounds, if any
        // cover the world, found by name for a later build; a helper's research.)
        constexpr LookedAt kLookedAt[] = { { "skillsmenu", "root1" }, { "genesisterminalmenu", "_root.TerminalBase_mc" },
            { "missionboard", "_root.Menu_mc" }, { "dataslatemenu", "root1.Menu_mc" }, { "statusmenu", "root1.Menu_mc" } };

        // (fix143) The hand scanner (MonocleMenu): only its circles and text, in the headset (no blue tint, only
        // the white circles and text). The mod "Sci Fi HUD - Holographic Scanner" (Nexus 13847's option) adds two pictures over
        // the whole screen to the game's file, in root1.Menu_mc, named only on its timeline (each name ends in a line break): SCIFI, a teal
        // picture at 59% (the blue tint; its lowest part, depth 1) and Visor, a blue helmet-visor frame with glowing edges (its highest,
        // depth 501). Both files have Gradient_mc, a black vignette (clear in the middle, half black at the edges). All three are taken
        // out: Gradient_mc and the two pictures hidden by name when their names are found; else the two removed by place (the lowest and
        // the highest) when the clip has two parts more than the game's own (14 parts at frame 1 and from frame 10, 15 at frames 2-9 as it
        // opens) and the game's named parts are all there. Looked at on every second advance while it shows (the timeline puts them back
        // as the scanner opens again). Not with the setting off, nor on the flat screen.
        constexpr const char* kScannerFile    = "monoclemenu";
        constexpr const char* kScannerClip    = "root1.Menu_mc";
        constexpr const char* kScannerNamed[] = { "Gradient_mc", "ScanMapMarkerHolder_mc", "OuterCircle_mc", "ButtonBar_mc", "NearbyPOIDisplay_mc" };
        constexpr const char* kScannerMod[]   = { "SCIFI\n", "Visor\n" };

        // (fix177) Build mode (WorkshopMenu) in the headset, with the setting BuildConsole: its centre reticle (Reticle_mc, the game's aim at
        // the view's middle; the right hand aims since fix176) and its corner brackets (ScreenBorderFrameContainer_mc, stretched to the box it
        // is drawn in) hidden; whether its action or choice card is up read for the console (ActionCard_mc, InteractiveMessage_mc: their
        // timelines stop at frame 1 and 10 hidden and 6 shown, open from 2 and close from 7: up at frames 2 to 9); the item card's height
        // logged a few times. Looked at on every second advance (the menu shows its reticle again in some modes).
        constexpr const char* kWorkshopFile   = "workshopmenu";
        constexpr const char* kWorkshopClip   = "root1.Menu_mc";
        constexpr const char* kWorkshopHide[] = { "Reticle_mc", "ScreenBorderFrameContainer_mc" };
        // (fix178) The build menu's button hints: the bar shown (ButtonBar_mc or SecondaryButtonBar_mc, right-justified at their x: their
        // background from x minus its width to x), read for the build console's hints panel (trimmed to it, centred).
        constexpr const char* kWorkshopBars[] = { "ButtonBar_mc", "SecondaryButtonBar_mc" };

        // (fix178) The game's messages (HUDMessagesMenu) arranged below your view's middle (MessagesBlock.h). Its menu clip and the parts placed
        // here; what they were (the movie's own: put back when not arranged).
        constexpr const char* kMessagesFile = "hudmessagesmenu";
        constexpr const char* kMessagesClip = "root1.Menu_mc";
        struct MessagesState
        {
            void*  movie{ nullptr };   // the movie arranged (another one, made anew: untouched)
            bool   applied{ false };
            bool   refused{ false };   // (not as expected: left alone)
            double menu[3]{};          // the menu clip's x, y, scale as found
            double game_safe[2]{};     // the movie's own margins (SafeX, SafeY), as the game last gave them
            double flyout[2]{};        // SkillChallengeFlyout_mc's x, y as found (not pinned by the movie)
            double messages_y{ 0.0 };  // Messages_mc's y as found (only its x is pinned)
            double subtitles_scale{ 1.0 };
            int    repins{ 0 };
            bool   just_laid_out{ false }; // (its SetSafeRect called last look: its margin should read ours now)
            double ours_safe{ -1.0 };      // (the margin last given by this)
            double cache[11]{ -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9, -1e9 }; // (the values last set, by slot)
            int    looks{ 0 };
            int    misses{ 0 };            // (...and did not: five times, put back and left alone)
            int    logs{ 0 };
            float  logged_scale{ 0.0f }, logged_top{ 0.0f }, logged_safe{ 0.0f };
            int    logged_stack{ -1 };
        };
        std::mutex    g_msg_mtx{};
        MessagesState g_msg{};

        std::string FileKey(const char* url)
        {
            std::string s{ url, strnlen(url, 200) };
            if (const auto slash = s.find_last_of("/\\"); slash != std::string::npos) {
                s = s.substr(slash + 1);
            }
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (const auto dot = s.find_last_of('.'); dot != std::string::npos) {
                s.resize(dot);
            }
            if (s.size() > 4 && s.compare(s.size() - 4, 4, "_lrg") == 0) {
                s.resize(s.size() - 4);
            }
            return s;
        }

        struct MovieState
        {
            void*         def{ nullptr }; // its definition when this was made
            const Recipe* recipe{ nullptr };
            bool          looked_at_only{ false };
            const char*   clip{ nullptr }; // its clip when not Menu_mc (its recipe's, or as looked at)
            std::string   name; // its file, for the log
            uint64_t      advances{ 0 };
            std::chrono::steady_clock::time_point last{};
            int  tries{ 0 }; // looks that found no menu clip
            bool done_report{ false };
            int  version{ -1 }; // which of its recipe's part counts it had when first seen whole (only that one is acted on after)
            bool scanner{ false };      // (fix143) the hand scanner's movie
            int  scanner_names{ 0 };    // ...the Sci Fi HUD's two pictures found by name (1), not (-1), not looked for yet (0)
            bool scanner_logged{ false }; // ...what was taken out logged (once while it shows)
            bool workshop{ false };       // (fix177) build mode's movie
            bool workshop_logged{ false }; // ...what was found by name logged (once while it shows)
            int  workshop_card_logs{ 0 }; // ...its item card's height logged (a few times while it shows)
            bool workshop_hidden{ false }; // ...its reticle and corner brackets hidden by this (shown again when the console is not shown)
            int  workshop_restores{ 0 };  // ...(shown again: logged a few times)
            bool messages{ false };       // (fix178) the game's messages' movie (HUDMessagesMenu)
        };

        std::unique_ptr<FunctionHook> g_hook{};
        std::mutex                    g_mtx{};
        std::unordered_map<void*, MovieState> g_movies{};
        std::unordered_map<void*, int>        g_roots{}; // 1 checked and usable, -1 not usable
        std::unordered_map<std::string, int>  g_logged{}; // lines logged per file
        std::atomic<bool>     g_disabled{ false };
        std::atomic<int>      g_bad_roots{ 0 };
        std::atomic<uint64_t> g_removed{ 0 };
        std::atomic<uint64_t> g_hidden{ 0 };
        std::atomic<uint64_t> g_left{ 0 }; // menus not as published: left as they are
        std::mutex            g_status_mtx{};
        std::string           g_status{ "not installed yet" };

        void SetStatus(std::string text)
        {
            std::scoped_lock _{ g_status_mtx };
            g_status = std::move(text);
        }

        bool LogFor(const std::string& file, int most = 3)
        {
            std::scoped_lock _{ g_mtx };
            return g_logged[file]++ < most;
        }

        bool GetFunctions(void* root, RootFns& f)
        {
            uintptr_t table = 0;
            if (root == nullptr || !ReadPointer(reinterpret_cast<uintptr_t>(root), table) || !InGame(table)) {
                return false;
            }
            if (!(ReadPointer(table + kSetVariableSlot * 8, f.set) && ReadPointer(table + kGetVariableSlot * 8, f.get) &&
                  ReadPointer(table + kIsAvailableSlot * 8, f.available) && ReadPointer(table + kInvokeArgsSlot * 8, f.invoke) &&
                  InGame(f.set) && InGame(f.get) && InGame(f.available) && InGame(f.invoke))) {
                return false;
            }
            // GetVariable lets go of what its value held first: "test al, 1; je +8; mov rcx, rdx; call <release>" (1.16.236), in its
            // first bytes. (Found once: every root has the same GetVariable.)
            static std::atomic<uintptr_t> s_get{ 0 };
            static std::atomic<uintptr_t> s_release{ 0 };
            if (s_get.load() == f.get) {
                f.release = s_release.load();
                return f.release != 0;
            }
            f.release = 0;
            static constexpr uint8_t kCallRelease[] = { 0xA8, 0x01, 0x74, 0x08, 0x48, 0x8B, 0xCA, 0xE8 };
            for (uintptr_t at = f.get; at < f.get + 0x60; ++at) {
                bool same = true;
                for (size_t i = 0; i < sizeof(kCallRelease) && same; ++i) {
                    uintptr_t word = 0;
                    same = ReadPointer(at + i, word) && static_cast<uint8_t>(word) == kCallRelease[i];
                }
                if (same) {
                    int32_t distance = 0;
                    uintptr_t word   = 0;
                    if (ReadPointer(at + sizeof(kCallRelease), word)) {
                        distance              = static_cast<int32_t>(static_cast<uint32_t>(word));
                        const uintptr_t target = at + sizeof(kCallRelease) + 4 + static_cast<intptr_t>(distance);
                        f.release              = InGame(target) ? target : 0;
                    }
                    break;
                }
            }
            s_release.store(f.release);
            s_get.store(f.get);
            if (f.release == 0) {
                // (Without it a value read could not be let go safely: nothing is read, the menu backgrounds are left as they are.)
                spdlog::warn("[MenuClean] the game's letting go of a Scaleform value not found in its GetVariable: menu backgrounds left as they are");
            }
            return f.release != 0;
        }

        bool Available(const RootFns& f, void* root, const std::string& path)
        {
            bool out = false;
            return CallAvailable(f, root, path.c_str(), out) && out;
        }

        // A number read from the movie (-1: none).
        double Number(const RootFns& f, void* root, const std::string& path)
        {
            GfxValue v{};
            bool     ok = false;
            if (!CallGet(f, root, path.c_str(), v, ok) || !ok) {
                return -1.0;
            }
            if ((v.type & kManagedBit) != 0) {
                return -1.0; // (not a plain number: not read)
            }
            switch (v.type & kTypeMask) {
            case kTypeInt:
                return static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(v.data)));
            case kTypeUInt:
                return static_cast<double>(static_cast<uint32_t>(v.data));
            case kTypeNumber: {
                double d = 0.0;
                std::memcpy(&d, &v.data, sizeof(d));
                return d;
            }
            default:
                return -1.0;
            }
        }

        // (fix177) A number read from the movie: false when there is none (any value, -1 too, is then a real one).
        bool ReadNumber(const RootFns& f, void* root, const std::string& path, double& out)
        {
            GfxValue v{};
            bool     ok = false;
            if (!CallGet(f, root, path.c_str(), v, ok) || !ok || (v.type & kManagedBit) != 0) {
                return false;
            }
            switch (v.type & kTypeMask) {
            case kTypeInt:
                out = static_cast<double>(static_cast<int32_t>(static_cast<uint32_t>(v.data)));
                return true;
            case kTypeUInt:
                out = static_cast<double>(static_cast<uint32_t>(v.data));
                return true;
            case kTypeNumber:
                std::memcpy(&out, &v.data, sizeof(out));
                return std::isfinite(out);
            default:
                return false;
            }
        }

        // What the movie gave for a path, as it is (for the log, when it is not what was expected).
        std::string RawValue(const RootFns& f, void* root, const std::string& path)
        {
            GfxValue v{};
            bool     ok = false;
            if (!CallGet(f, root, path.c_str(), v, ok)) {
                return "the call failed";
            }
            return fmt::format("answered {}, type {:#x}, value {:#x}", ok, v.type, v.data);
        }

        // True or false read from the movie (-1: none).
        int Flag(const RootFns& f, void* root, const std::string& path)
        {
            GfxValue v{};
            bool     ok = false;
            if (!CallGet(f, root, path.c_str(), v, ok) || !ok || (v.type & kTypeMask) != kTypeBoolean) {
                return -1;
            }
            return (v.data & 0xFF) != 0 ? 1 : 0;
        }

        void MarkRoot(void* root, int state)
        {
            std::scoped_lock _{ g_mtx };
            if (g_roots.size() > 256) {
                g_roots.clear();
            }
            g_roots[root] = state;
        }

        void BadRoot(const char* name, const std::string& why)
        {
            const int bad = ++g_bad_roots;
            spdlog::warn("[MenuClean] {}: its Flash player does not answer as expected ({}): left as it is{}", name, why,
                bad >= 3 ? " (three such: menu backgrounds are left as they are until the game restarts)" : "");
            if (bad >= 3) {
                g_disabled = true;
                SetStatus("Menu backgrounds: left as they are (the game's Flash player is not as expected)");
            }
        }

        // The movie's ActionScript root: the object at the movie's +0x18 (CommonLibSF's and CommonLibF4's Movie), checked to be one
        // (its type) before any of its functions is called. Were it not there, the movie's first 0x100 bytes are looked through for
        // it once (and the place found kept for every movie); what is at +0x18 then is logged.
        std::atomic<int> g_root_offset{ -1 }; // (-1 not found yet, -2 not found at all)

        void* FindRoot(void* movie, const char* name)
        {
            const uintptr_t m      = reinterpret_cast<uintptr_t>(movie);
            const int       offset = g_root_offset.load();
            if (offset == -2) {
                return nullptr;
            }
            if (offset >= 0) {
                uintptr_t root = 0;
                return ReadPointer(m + static_cast<uintptr_t>(offset), root) && IsKind(reinterpret_cast<void*>(root), kRootCoded, kRootKind)
                           ? reinterpret_cast<void*>(root)
                           : nullptr;
            }
            uintptr_t expected = 0;
            ReadPointer(m + kMovieRootOffset, expected);
            if (IsKind(reinterpret_cast<void*>(expected), kRootCoded, kRootKind)) {
                g_root_offset = static_cast<int>(kMovieRootOffset);
                spdlog::info("[MenuClean] the menus' ActionScript root found at the movie's +{:#x}, as expected ({})", kMovieRootOffset, name);
                return reinterpret_cast<void*>(expected);
            }
            for (uintptr_t at = 8; at < 0x100; at += 8) {
                uintptr_t candidate = 0;
                if (ReadPointer(m + at, candidate) && candidate != 0 && IsKind(reinterpret_cast<void*>(candidate), kRootCoded, kRootKind)) {
                    g_root_offset = static_cast<int>(at);
                    spdlog::info("[MenuClean] the menus' ActionScript root found at the movie's +{:#x}, not +{:#x} (there: {}) ({})", at,
                        kMovieRootOffset, RttiNames::Describe(reinterpret_cast<void*>(expected)), name);
                    return reinterpret_cast<void*>(candidate);
                }
            }
            g_root_offset = -2;
            g_disabled    = true;
            spdlog::warn("[MenuClean] {}: no ActionScript root ({}) found in its movie (at +{:#x}: {}; the movie itself: {}): menu backgrounds "
                         "are left as they are until the game restarts",
                name, kRootKind, kMovieRootOffset, RttiNames::Describe(reinterpret_cast<void*>(expected)), RttiNames::Describe(movie));
            SetStatus("Menu backgrounds: left as they are (the menus' ActionScript root not found)");
            return nullptr;
        }

        // The root's calls checked once: they answer as they should (a part that is there and one that is not answer differently;
        // the menu clip's part count is a number). 1 usable; 0 not yet (no menu clip made yet, or a movie without one); -1 not usable.
        // The clips a menu's parts may be in: its recipe's own, else the menu clip Menu_mc.
        std::vector<std::string> ClipCandidates(const char* clip)
        {
            if (clip != nullptr) {
                return { clip };
            }
            return { "root1.Menu_mc", "root.Menu_mc" };
        }

        int RootUsable(void* root, RootFns& f, const char* name, const char* own_clip)
        {
            int known = 0; // 0 not checked, 1 usable, -1 not usable
            {
                std::scoped_lock _{ g_mtx };
                if (const auto it = g_roots.find(root); it != g_roots.end()) {
                    known = it->second;
                }
            }
            if (known < 0 || !GetFunctions(root, f)) {
                return -1;
            }
            if (known == 1) {
                return 1;
            }
            std::string clip;
            for (const auto& candidate : ClipCandidates(own_clip)) {
                if (Available(f, root, candidate)) {
                    clip = candidate;
                    break;
                }
            }
            if (clip.empty()) {
                return 0; // (looked at again later)
            }
            const bool   absent_ok = !Available(f, root, clip + ".vrNoSuchPart_7f3a");
            const double parts     = Number(f, root, clip + ".numChildren");
            const bool   usable    = absent_ok && parts >= 1.0 && parts <= 2000.0;
            MarkRoot(root, usable ? 1 : -1);
            if (!usable && own_clip != nullptr) {
                // (A clip of the menu's own (its root itself): this menu left as it is, the others still looked at.)
                if (LogFor(std::string{ name } + "#own", 1)) {
                    spdlog::info("[MenuClean] {}: its clip {} does not answer as expected (a missing part reported {}, its part count {}): this "
                                 "menu left as it is",
                        name, clip, absent_ok ? "missing" : "there", parts);
                }
            } else if (!usable) {
                BadRoot(name, fmt::format("a missing part reported {}, {}'s part count {} ({})", absent_ok ? "missing" : "there", clip, parts,
                                  RawValue(f, root, clip + ".numChildren")));
            } else if (LogFor("#root", 1)) {
                spdlog::info("[MenuClean] the menus' Flash player answers as expected ({}: {}'s parts counted, {})", name, clip, parts);
            }
            return usable ? 1 : -1;
        }

        std::string MenuClip(const RootFns& f, void* root, const char* own_clip)
        {
            for (const auto& candidate : ClipCandidates(own_clip)) {
                if (Available(f, root, candidate)) {
                    return candidate;
                }
            }
            return {};
        }

        // One look at a menu: its painted backgrounds taken out when its clip is as published. (On the movie's own thread, right
        // after the game advanced it.)
        void Look(void* movie, MovieState& st)
        {
            void* root = FindRoot(movie, st.name.c_str());
            if (root == nullptr) {
                return;
            }
            RootFns   f{};
            const int usable = RootUsable(root, f, st.name.c_str(), st.clip);
            if (usable <= 0) {
                if (usable == 0 && ++st.tries == 120 && LogFor(st.name + "#none")) {
                    spdlog::info("[MenuClean] {}: no clip {} found in it: nothing taken out", st.name, st.clip != nullptr ? st.clip : "Menu_mc");
                }
                return;
            }
            const std::string clip = MenuClip(f, root, st.clip);
            if (clip.empty()) {
                ++st.tries;
                return;
            }
            const double parts = Number(f, root, clip + ".numChildren");
            if (st.looked_at_only) {
                if (!st.done_report) {
                    st.done_report = true;
                    if (LogFor(st.name, 1)) {
                        // (With the named parts a later version may need: there or not.)
                        std::string names{};
                        for (const char* n : { "PreviewSceneRect_mc", "Tab_mc", "SkillDescription_mc", "SkillTab6", "InspectPopup_mc", "ButtonBar_mc" }) {
                            names += fmt::format("{}{} {}", names.empty() ? "" : ", ", n, Available(f, root, clip + "." + n) ? "there" : "not there");
                        }
                        spdlog::info("[MenuClean] {}: its clip {} has {} parts (nothing taken out of this menu; {})", st.name, clip, parts, names);
                    }
                }
                return;
            }
            const Recipe& r = *st.recipe;
            int removed = 0;
            if (r.parts[0] > 0 || r.parts[1] > 0) {
                bool named = true;
                for (const char* n : r.checks) {
                    if (n != nullptr && !Available(f, root, clip + "." + n)) {
                        named = false;
                    }
                }
                int to_remove = 0;
                for (const int p : r.remove) {
                    to_remove += p >= 0 ? 1 : 0;
                }
                // (Which version of the menu it is: the count it has when first seen whole, kept from then on. A part its timeline makes
                // anew after one was taken out could otherwise give the other version's count, with other parts at those places.)
                // (Or, a menu shown again with its parts already taken out: the count left, when it is no version's whole count.)
                // (Each version only with its mark: by a part only it has, or its file.)
                const auto marked = [&](int v) {
                    const char* m = r.marks[v];
                    if (m == nullptr) {
                        return true;
                    }
                    const bool negate = m[0] == '!';
                    if (negate) {
                        ++m;
                    }
                    bool has = false;
                    if (std::strcmp(m, "@lrg") == 0) {
                        std::string lower = st.name;
                        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        has = lower.find("_lrg.") != std::string::npos;
                    } else {
                        has = Available(f, root, clip + "." + m);
                    }
                    return has != negate;
                };
                if (st.version < 0 && named) {
                    for (int v = 0; v < 2; ++v) {
                        if (r.parts[v] > 0 && parts == static_cast<double>(r.parts[v]) && marked(v)) {
                            st.version = v;
                        }
                    }
                    for (int v = 0; v < 2 && st.version < 0; ++v) {
                        if (r.parts[v] > 0 && parts == static_cast<double>(r.parts[v] - to_remove) &&
                            parts != static_cast<double>(r.parts[0]) && parts != static_cast<double>(r.parts[1]) && marked(v)) {
                            st.version = v;
                        }
                    }
                }
                const int  whole = st.version >= 0 ? r.parts[st.version] : 0;
                if (named && whole > 0 && parts == static_cast<double>(whole)) {
                    // (Highest place first: the lower ones keep their places.)
                    for (const int p : r.remove) {
                        bool ok = false;
                        if (p >= 0 && CallWithInt(f, root, (clip + ".removeChildAt").c_str(), p, ok) && ok) {
                            ++removed;
                        }
                    }
                    g_removed += static_cast<uint64_t>(removed);
                } else if ((st.version < 0 || !named) && !st.done_report && st.advances > 30) {
                    // (Not any version known, once it has settled: changed by an update or a mod. A version known with a count between
                    // whole and taken out: a part its timeline made anew, taken out when the clip is whole again.)
                    st.done_report = true;
                    ++g_left;
                    if (LogFor(st.name + "#left", 2)) {
                        spdlog::info("[MenuClean] {}: its clip {} has {} parts{} ({} expected): nothing taken out by place (a different "
                                     "version of this menu)",
                            st.name, clip, parts, named ? "" : ", not all the named parts expected",
                            r.parts[1] > 0 ? fmt::format("{} or {}", r.parts[0], r.parts[1]) : fmt::format("{}", r.parts[0]));
                    }
                }
            }
            int hidden = 0;
            for (const char* n : r.hide) {
                if (n == nullptr) {
                    continue;
                }
                const std::string visible = clip + "." + n + ".visible";
                if (Flag(f, root, visible) == 1) {
                    bool ok = false;
                    if (CallSetFalse(f, root, visible.c_str(), ok) && ok) {
                        ++hidden;
                    }
                }
            }
            g_hidden += static_cast<uint64_t>(hidden);
            if ((removed > 0 || hidden > 0) && LogFor(st.name)) {
                spdlog::info("[MenuClean] {}: {} taken out for the see-through menu ({} {} parts{}; {} part(s) removed, {} hidden)", st.name, r.what,
                    clip, parts, removed > 0 ? ", whole" : "", removed, hidden);
            }
            if (removed > 0 || hidden > 0) {
                SetStatus(fmt::format("Menu backgrounds: taken out of see-through menus (last: {})", st.name));
            }
        }

        // (fix143) The setting on, in the headset.
        bool ScannerWanted()
        {
            static auto vr = VR::get();
            return GameFlow::gStore.internalSettings.scannerPlain && vr != nullptr && vr->is_hmd_active();
        }

        // (fix143) One look at the hand scanner: its tint, visor frame and dark edges taken out (kScannerFile's notes). On the movie's
        // own thread, right after the game advanced it.
        void LookScanner(void* movie, MovieState& st)
        {
            void* root = FindRoot(movie, st.name.c_str());
            if (root == nullptr) {
                return;
            }
            RootFns f{};
            if (RootUsable(root, f, st.name.c_str(), kScannerClip) <= 0) {
                return;
            }
            const std::string clip{ kScannerClip };
            if (!Available(f, root, clip)) {
                return;
            }
            int hidden = 0, removed = 0;
            const auto hide = [&](const std::string& part) {
                const std::string visible = clip + "." + part + ".visible";
                bool              ok      = false;
                if (Flag(f, root, visible) == 1 && CallSetFalse(f, root, visible.c_str(), ok) && ok) {
                    ++hidden;
                }
            };
            // The dark edges (both files' Gradient_mc): hidden by name, kept hidden (sticky).
            hide("Gradient_mc");
            // The Sci Fi HUD's two pictures: by their names, looked for until found (fix143's review: they may come after the first look).
            if (st.scanner_names <= 0) {
                int found = 0;
                for (const char* n : kScannerMod) {
                    found += Available(f, root, clip + "." + n) ? 1 : 0;
                }
                const bool first = st.scanner_names == 0;
                st.scanner_names = found > 0 ? 1 : -1;
                if ((first || found > 0) && LogFor(st.name + "#names", 2)) {
                    spdlog::info("[MenuClean] {}: the Sci Fi HUD scanner's tint and visor frame {} by name ({} of 2){}", st.name,
                        found > 0 ? "found" : "not found", found, found > 0 ? ": hidden by name" : ": taken out by place when they are there");
                }
            }
            double parts = Number(f, root, clip + ".numChildren");
            double frame = -1.0;
            if (st.scanner_names > 0) {
                for (const char* n : kScannerMod) {
                    hide(n);
                }
            } else {
                // By place: two parts more than the game's own clip has at this frame, with its named parts all there: the lowest (the
                // tint) and the highest (the visor frame) are the Sci Fi HUD's. (Only both at once: one more is the game's own line as
                // it opens, in a file whose opening is longer.)
                bool named = true;
                for (const char* n : kScannerNamed) {
                    named = named && Available(f, root, clip + "." + n);
                }
                frame = Number(f, root, clip + ".currentFrame");
                if (named && parts >= 1.0 && frame >= 1.0) {
                    const double whole = frame >= 2.0 && frame <= 9.0 ? 15.0 : 14.0;
                    if (parts - whole == 2.0) { // (exactly the two: any other count is not the Sci Fi HUD's, fix143's review)
                        bool ok = false;
                        if (CallWithInt(f, root, (clip + ".removeChildAt").c_str(), static_cast<int>(parts) - 1, ok) && ok) {
                            ++removed;
                        }
                        const double left = Number(f, root, clip + ".numChildren");
                        ok                = false;
                        if (left - whole >= 1.0 && CallWithInt(f, root, (clip + ".removeChildAt").c_str(), 0, ok) && ok) {
                            ++removed;
                        }
                    }
                }
            }
            g_hidden += static_cast<uint64_t>(hidden);
            g_removed += static_cast<uint64_t>(removed);
            if ((hidden > 0 || removed > 0) && !st.scanner_logged) {
                st.scanner_logged = true;
                if (LogFor(st.name + "#scanner", 10)) {
                    spdlog::info("[MenuClean] {}: its tint, visor frame and dark edges taken out ({} {} parts at frame {}; {} hidden, {} removed by place)",
                        st.name, clip, parts, frame, hidden, removed);
                }
                SetStatus(fmt::format("Menu backgrounds: the hand scanner's tint and edges taken out (last: {})", st.name));
            }
        }

        // (fix177) One look at build mode's menu (kWorkshopFile's notes). On the movie's own thread, right after the game advanced it.
        void LookWorkshop(void* movie, MovieState& st)
        {
            void* root = FindRoot(movie, st.name.c_str());
            if (root == nullptr) {
                return;
            }
            RootFns f{};
            if (RootUsable(root, f, st.name.c_str(), kWorkshopClip) <= 0) {
                return;
            }
            const std::string clip{ kWorkshopClip };
            if (!Available(f, root, clip)) {
                return;
            }
            // (The review) Hidden only while build mode is shown on the console (not back in the view, not the large-text menu, not with the
            // setting off); shown again once otherwise, if this hid them. (Looked at after every advance meanwhile: a reticle the menu shows
            // again in its advance is hidden before the frame is drawn, no flicker.)
            const bool hide   = BuildConsole::ConsoleShown();
            int        hidden = 0, restored = 0;
            bool       found[2]{};
            for (int i = 0; i < 2; ++i) {
                const std::string part = clip + "." + kWorkshopHide[i];
                found[i]               = Available(f, root, part);
                bool ok                = false;
                if (!found[i]) {
                    continue;
                }
                const int visible = Flag(f, root, part + ".visible");
                if (hide && visible == 1 && CallSetBool(f, root, (part + ".visible").c_str(), false, ok) && ok) {
                    ++hidden;
                } else if (!hide && st.workshop_hidden && visible == 0 && CallSetBool(f, root, (part + ".visible").c_str(), true, ok) && ok) {
                    ++restored;
                }
            }
            if (hide && hidden > 0) {
                st.workshop_hidden = true;
            } else if (!hide && st.workshop_hidden) {
                st.workshop_hidden = false;
                if (st.workshop_restores++ < 3 && LogFor(st.name + "#restore", 10)) {
                    spdlog::info("[MenuClean] (fix177) {}: build mode not on the console now: its centre reticle and corner brackets shown again ({})",
                        st.name, restored);
                }
            }
            g_hidden += static_cast<uint64_t>(hidden);
            if (!BuildConsole::LookWanted()) {
                return; // (only shown again: the setting off)
            }
            // (The review) Where the menu was laid out: its list's y (TR-pinned 4 units above the 50-unit border: 46 in a 16:9 box, about -448
            // in fix175's tall one), else its item card's (50 / about -444).
            {
                double y     = 0.0;
                bool   known = ReadNumber(f, root, clip + ".BuildPopupContainer_mc.y", y) || ReadNumber(f, root, clip + ".BuildItemCard_mc.y", y);
                BuildConsole::NoteMenuLayout(known, static_cast<float>(y));
            }
            // (fix178) The button hints' bar shown: its background's left and right (the bar right-justified at its x, its background from x
            // minus the background's width to x; without one, the bar's own width), both bars together if both show.
            {
                bool   any = false, known = false;
                double left = 1e9, right = -1e9;
                for (const char* bar : kWorkshopBars) {
                    const std::string part = clip + "." + bar;
                    double            x = 0.0, w = 0.0;
                    if (!ReadNumber(f, root, part + ".x", x)) {
                        continue;
                    }
                    known = true;
                    if (Flag(f, root, part + ".visible") == 0) {
                        continue;
                    }
                    if (!(Flag(f, root, part + ".Background_mc.visible") != 0 && ReadNumber(f, root, part + ".Background_mc.width", w) && w > 1.0) &&
                        !(ReadNumber(f, root, part + ".width", w) && w > 1.0)) {
                        continue;
                    }
                    any   = true;
                    left  = std::min(left, x - w);
                    right = std::max(right, x);
                }
                BuildConsole::NoteHintsBar(known, any, static_cast<float>(left), static_cast<float>(right));
            }
            // The action and choice cards: up while either is between its opening and its hidden frames (and not hidden by name).
            bool known = false, shown = false;
            for (const char* card : { "ActionCard_mc", "InteractiveMessage_mc" }) {
                const std::string part  = clip + "." + card;
                const double      frame = Number(f, root, part + ".currentFrame");
                if (frame < 1.0) {
                    continue;
                }
                known = true;
                if (frame >= 2.0 && frame <= 9.0 && Flag(f, root, part + ".visible") != 0) {
                    shown = true;
                }
            }
            BuildConsole::NoteCentreCards(known, shown);
            if (!st.workshop_logged) {
                st.workshop_logged = true;
                if (LogFor(st.name + "#workshop", 10)) {
                    spdlog::info("[MenuClean] (fix177) {}: its centre reticle {} and corner brackets {} ({} hidden now); its action and choice cards {}",
                        st.name, found[0] ? "found by name" : "not found", found[1] ? "found by name" : "not found", hidden,
                        known ? "read (their frames)" : "not read (shown on the console always)");
                }
            }
            // (The item card's real height, a few times, to check its crop: the research's 30 to 830 units.)
            if (st.workshop_card_logs < 3 && st.advances > 60 && st.advances % 120 == 61 && LogFor(st.name + "#card", 12)) {
                ++st.workshop_card_logs;
                const std::string card = clip + ".BuildItemCard_mc";
                spdlog::info("[MenuClean] (fix177) {}: its item card at {:.0f},{:.0f}, {:.0f} x {:.0f} units, shown {}; the action cards {}", st.name,
                    Number(f, root, card + ".x"), Number(f, root, card + ".y"), Number(f, root, card + ".width"), Number(f, root, card + ".height"),
                    Flag(f, root, card + ".visible"), shown ? "up" : "not up");
            }
        }

        // (fix178) One look at the game's messages (HUDMessagesMenu: kMessagesFile's notes, MessagesBlock.h): arranged below your view's middle
        // while wanted, put back as the movie had them otherwise. On the movie's own thread, right after the game advanced it.
        void LookMessages(void* movie, MovieState& st)
        {
            const bool want = MessagesBlock::Wanted();
            std::scoped_lock _{ g_msg_mtx };
            auto&            m = g_msg;
            if (m.movie != movie) {
                // (Another movie, made anew: as the movie made it, nothing of ours on it.)
                if (m.applied && m.logs < 40) {
                    ++m.logs;
                    spdlog::info("[Messages] (fix178) {}: the messages' movie made anew: arranged again from its own places", st.name);
                }
                const int logs = m.logs;
                m              = MessagesState{};
                m.movie        = movie;
                m.logs         = logs;
            }
            if (m.refused || (!want && !m.applied)) {
                return;
            }
            void* root = FindRoot(movie, st.name.c_str());
            if (root == nullptr) {
                return;
            }
            RootFns f{};
            if (RootUsable(root, f, st.name.c_str(), kMessagesClip) <= 0) {
                return;
            }
            const std::string clip{ kMessagesClip };
            const auto        num = [&](const std::string& path, double& out) { return ReadNumber(f, root, path, out); };
            const auto        set = [&](const std::string& path, double value) {
                double now = 0.0;
                if (num(path, now) && std::abs(now - value) < 0.01) {
                    return false; // (already)
                }
                bool ok = false;
                return CallSetNumber(f, root, path.c_str(), value, ok) && ok;
            };
            const auto repin = [&](double safe_x, double safe_y) {
                bool ok = false;
                CallWithInts(f, root, (clip + ".SetSafeRect").c_str(), static_cast<int>(std::lround(safe_x)), static_cast<int>(std::lround(safe_y)), ok);
                return ok;
            };
            double     safe[2]{};
            const bool safe_known = num(clip + ".SafeX", safe[0]) && num(clip + ".SafeY", safe[1]);
            // (Laid out again with our margin last look, and it does not read ours: the call did not take. Five times: put back, left alone.)
            bool       give_up    = false;
            const bool after_ours = m.just_laid_out;
            if (m.applied && m.just_laid_out && want) {
                const auto plan_now = MessagesBlock::Current();
                if (plan_now.valid && (!safe_known || std::abs(safe[0] - std::round(plan_now.safe_x)) > 0.5)) {
                    give_up = ++m.misses >= 5;
                } else {
                    m.misses = 0;
                }
            }
            m.just_laid_out = false;
            if (!want || give_up) {
                // Put back: the menu clip where it was, the parts placed here as they were, the movie's own margins (it lays its parts out
                // again with them).
                set(clip + ".scaleX", m.menu[2]);
                set(clip + ".scaleY", m.menu[2]);
                set(clip + ".x", m.menu[0]);
                set(clip + ".y", m.menu[1]);
                set(clip + ".SkillChallengeFlyout_mc.x", m.flyout[0]);
                set(clip + ".SkillChallengeFlyout_mc.y", m.flyout[1]);
                set(clip + ".Messages_mc.y", m.messages_y);
                set(clip + ".BottomCenterGroup_mc.scaleX", m.subtitles_scale);
                set(clip + ".BottomCenterGroup_mc.scaleY", m.subtitles_scale);
                const bool ok = repin(m.game_safe[0], m.game_safe[1]);
                m.applied     = false;
                m.refused     = m.refused || give_up;
                MessagesBlock::NoteApplied(false, MessagesBlock::Plan{});
                if (m.logs < 40) {
                    ++m.logs;
                    spdlog::info("[Messages] (fix178) {}: the messages put back where the HUD has them (its margins {:.0f},{:.0f}{}){}", st.name,
                        m.game_safe[0], m.game_safe[1], ok ? "" : "; laying it out again not answered",
                        give_up ? ": its margins did not take our value five times, left alone from now on" : "");
                }
                return;
            }
            const auto plan = MessagesBlock::Current();
            if (!plan.valid) {
                return;
            }
            if (!m.applied) {
                // As published (HUDMessagesMenu.gfx as loaded): its menu clip unscaled, its parts there by name, its margins readable. Else left
                // alone (another version of the movie, a mod's).
                double     at[3]{};
                const bool place = num(clip + ".x", at[0]) && num(clip + ".y", at[1]) && num(clip + ".scaleX", at[2]);
                bool       parts = true;
                for (const char* n : { "Messages_mc", "SkillChallengeFlyout_mc", "LevelUpHolder_mc", "BottomCenterGroup_mc", "XPGroup_mc" }) {
                    parts = parts && Available(f, root, clip + "." + n);
                }
                double flyout[2]{}, messages_y = 0.0, subtitles = 1.0;
                const bool read = num(clip + ".SkillChallengeFlyout_mc.x", flyout[0]) && num(clip + ".SkillChallengeFlyout_mc.y", flyout[1]) &&
                                  num(clip + ".Messages_mc.y", messages_y) && num(clip + ".BottomCenterGroup_mc.scaleX", subtitles);
                if (!place || !parts || !safe_known || !read || std::abs(at[2] - 1.0) > 0.01 || std::abs(subtitles - 1.0) > 0.01) {
                    m.refused = true;
                    spdlog::warn("[Messages] (fix178) {}: not as expected (its menu clip {}, scale {:.2f}; its parts {}; its margins {}; its own "
                                 "places {}): the messages left where the HUD has them",
                        st.name, place ? "found" : "not found", at[2], parts ? "found by name" : "not all found", safe_known ? "read" : "not readable",
                        read ? "read" : "not readable");
                    return;
                }
                m.menu[0]         = at[0];
                m.menu[1]         = at[1];
                m.menu[2]         = at[2];
                m.game_safe[0]    = safe[0];
                m.game_safe[1]    = safe[1];
                m.flyout[0]       = flyout[0];
                m.flyout[1]       = flyout[1];
                m.messages_y      = messages_y;
                m.subtitles_scale = subtitles;
            } else if (safe_known && !after_ours && std::abs(safe[0] - m.ours_safe) > 0.5) {
                // (The game laid the movie out again with its own margins (its box's size changed): kept as its own, arranged again below.)
                m.game_safe[0] = safe[0];
                m.game_safe[1] = safe[1];
                ++m.repins;
            }
            // The menu clip larger and moved; the subtitles kept at their size; then the movie lays its parts out again with the new margin
            // (only when something changed, or the game laid it out with its own).
            // (What was last set is kept: set again only when it changes, the movie read back every 30th look, after a lay-out and as it
            // starts. A few reads a look otherwise.)
            const bool relaid = !safe_known || std::abs(safe[0] - std::round(plan.safe_x)) > 0.5;
            bool       verify = !m.applied || relaid || ++m.looks % 30 == 0;
            const auto setc   = [&](int slot, const char* part, double value) {
                if (!verify && std::abs(m.cache[slot] - value) < 0.01) {
                    return false;
                }
                m.cache[slot] = value;
                return set(clip + part, value);
            };
            const double s     = plan.scale;
            bool         moved = false;
            moved |= setc(0, ".scaleX", s);
            moved |= setc(1, ".scaleY", s);
            moved |= setc(2, ".x", plan.origin[0]);
            moved |= setc(3, ".y", plan.origin[1]);
            setc(4, ".BottomCenterGroup_mc.scaleX", m.subtitles_scale / s);
            setc(5, ".BottomCenterGroup_mc.scaleY", m.subtitles_scale / s);
            if (moved || !m.applied || relaid) {
                m.just_laid_out = true;
                m.ours_safe     = std::round(plan.safe_x);
                verify          = true; // (the movie's pinned parts placed anew: the level-up's own place among them)
                if (!repin(plan.safe_x, m.game_safe[1]) && m.logs < 40) {
                    ++m.logs;
                    spdlog::warn("[Messages] (fix178) {}: laying it out again (its SetSafeRect) not answered", st.name);
                }
            }
            // The notifications' column's top at the block's top; under it, as they show, the skill challenge flyout and the level-up.
            const auto local_x = [&](double gx) { return (gx - plan.origin[0]) / s; };
            const auto local_y = [&](double gy) { return (gy - plan.origin[1]) / s; };
            setc(6, ".Messages_mc.y", local_y(plan.top));
            double cursor = plan.top, height = 0.0;
            if (num(clip + ".Messages_mc.height", height) && height > 1.0) {
                cursor += height * s + 12.0;
            }
            double       frame     = 0.0;
            const bool   flyout_up = num(clip + ".SkillChallengeFlyout_mc.currentFrame", frame) && frame >= 2.0 && frame <= 118.0;
            const double flyout_at = cursor;
            setc(7, ".SkillChallengeFlyout_mc.x", local_x(plan.right_col[0]));
            setc(8, ".SkillChallengeFlyout_mc.y", local_y(flyout_at));
            if (flyout_up) {
                cursor += 74.0 * s + 12.0;
            }
            // (The level-up: its drawing 79 units below and 175 either side of its own place.)
            double       level_frame = 0.0;
            const bool   level_up    = num(clip + ".LevelUpHolder_mc.LevelUpWidget_mc.currentFrame", level_frame) && level_frame >= 2.0;
            const double level_x     = (plan.right_col[0] + plan.right_col[1]) * 0.5;
            setc(9, ".LevelUpHolder_mc.x", local_x(level_x));
            setc(10, ".LevelUpHolder_mc.y", local_y(cursor - 79.0 * s));
            m.applied = true;
            MessagesBlock::NoteApplied(true, plan);
            // Where each ends up (when that changes): degrees from your view's middle, and their text's height.
            const int stack = (flyout_up ? 1 : 0) + (level_up ? 2 : 0);
            if (m.logs < 40 && (std::abs(m.logged_scale - plan.scale) > 0.01f || std::abs(m.logged_top - plan.top) > 1.0f ||
                                   std::abs(m.logged_safe - plan.safe_x) > 1.0f || m.logged_stack != stack)) {
                ++m.logs;
                m.logged_scale = plan.scale;
                m.logged_top   = plan.top;
                m.logged_safe  = plan.safe_x;
                m.logged_stack = stack;
                const float d  = plan.deg_per_unit;
                const auto  az = [&](double x) { return std::format("{:.0f}", (x - 960.0) * d); };
                const auto  el = [&](double y) { return std::format("{:.0f}", (540.0 - y) * d); };
                spdlog::info("[Messages] (fix178) {}: arranged below your view's middle (the movie {:.2f} times larger: its 18-unit text {:.2f} degrees; "
                             "degrees from the middle, + right / + up): the mission updates from {} to {} across, from {} down; the notifications from {} "
                             "to {} across, from {} down{}; the skill challenge flyout {} at {} across, {} down; the level-up {} at {} across; the "
                             "subtitles kept at their size at the bottom ({} up), the XP and the location's name in the middle; the margins {:.0f} (the "
                             "game's {:.0f},{:.0f}; laid out again by the game {} times)",
                    st.name, plan.scale, 18.0f * plan.scale * d, az(plan.left_col[0]), az(plan.left_col[1]), el(plan.top), az(plan.right_col[0]),
                    az(plan.right_col[1]), el(plan.top), height > 1.0 ? std::format(" (now {:.0f} units tall)", height * s) : std::string{},
                    flyout_up ? "up" : "not up", az(plan.right_col[0]), el(flyout_at), level_up ? "up" : "not up", az(level_x),
                    el(plan.shown_bottom - 150.0), plan.safe_x, m.game_safe[0], m.game_safe[1], m.repins);
            }
        }

        void AfterAdvance(void* movie)
        {
            if (g_disabled.load() || movie == nullptr) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            void*      def = nullptr;
            if (!MovieDefOf(movie, def)) {
                return;
            }
            MovieState st{};
            bool       look = false;
            {
                std::scoped_lock _{ g_mtx };
                auto it = g_movies.find(movie);
                // (Not advanced for a while: a menu opened again; or another movie made where one was.)
                if (it != g_movies.end() && (now - it->second.last > std::chrono::milliseconds(500) || it->second.def != def)) {
                    g_movies.erase(it);
                    it = g_movies.end();
                }
                if (it == g_movies.end()) {
                    if (g_movies.size() > 128) {
                        g_movies.clear();
                    }
                    it = g_movies.emplace(movie, MovieState{}).first;
                    it->second.def  = def;
                    const char* url = nullptr;
                    if (UrlOf(def, url)) {
                        it->second.name    = std::string{ url, strnlen(url, 200) };
                        const auto key     = FileKey(url);
                        for (const auto& r : kRecipes) {
                            if (key == r.file) {
                                it->second.recipe = &r;
                                it->second.clip   = r.clip;
                            }
                        }
                        for (const auto& l : kLookedAt) {
                            if (key == l.file) {
                                it->second.looked_at_only = true;
                                it->second.clip           = l.clip;
                            }
                        }
                        it->second.scanner = key == kScannerFile;
                        it->second.workshop = key == kWorkshopFile; // (fix177)
                        it->second.messages = key == kMessagesFile; // (fix178)
                    }
                }
                auto& s = it->second;
                s.last  = now;
                ++s.advances;
                if (s.recipe == nullptr && !s.looked_at_only && !s.scanner && !s.workshop && !s.messages) {
                    return;
                }
                // Each frame while it opens, then every third, then every thirtieth (a part the timeline makes anew is taken out
                // again). (The scanner and (fix177) build mode: each frame while it opens, then every second.)
                // ((fix177) Build mode on the console: after every advance, its reticle kept hidden in each frame drawn.)
                // ((fix178) The messages' movie, always open: every second advance.)
                look = s.scanner || s.workshop || s.messages ? s.advances <= 120 || s.advances % 2 == 1 || (s.workshop && BuildConsole::ConsoleShown())
                                               : s.advances <= 30 || (s.advances <= 300 && s.advances % 3 == 1) || s.advances % 30 == 1;
                if (look) {
                    st = s;
                }
            }
            // (fix143) The hand scanner: not a see-through menu, its own setting.
            if (look && st.scanner) {
                if (!ScannerWanted()) {
                    return;
                }
                LookScanner(movie, st);
                std::scoped_lock _{ g_mtx };
                if (const auto it = g_movies.find(movie); it != g_movies.end()) {
                    it->second.scanner_names  = st.scanner_names;
                    it->second.scanner_logged = st.scanner_logged;
                }
                return;
            }
            // (fix178) The game's messages: arranged below your view's middle (its own setting), or put back.
            if (st.messages) {
                if (look) {
                    LookMessages(movie, st);
                }
                return;
            }
            // (fix177) Build mode's menu: not a see-through menu, the build console's setting.
            if (look && st.workshop) {
                // (Looked at with the setting on in the headset; or to show again what this hid, the setting turned off.)
                if (!BuildConsole::LookWanted() && !st.workshop_hidden) {
                    return;
                }
                LookWorkshop(movie, st);
                std::scoped_lock _{ g_mtx };
                if (const auto it = g_movies.find(movie); it != g_movies.end()) {
                    it->second.workshop_logged    = st.workshop_logged;
                    it->second.workshop_card_logs = st.workshop_card_logs;
                    it->second.workshop_hidden    = st.workshop_hidden;
                    it->second.workshop_restores  = st.workshop_restores;
                }
                return;
            }
            if (st.workshop) {
                return; // (never a see-through menu's recipe)
            }
            if (!look || !MenuLayer::SeeThroughWanted()) {
                return;
            }
            Look(movie, st);
            {
                std::scoped_lock _{ g_mtx };
                if (const auto it = g_movies.find(movie); it != g_movies.end()) {
                    it->second.tries       = st.tries;
                    it->second.done_report = st.done_report;
                    it->second.version     = st.version;
                }
            }
        }

        float AdvanceDetour(void* movie, float delta, uint32_t catch_up, bool capture)
        {
            using func_t         = float(void*, float, uint32_t, bool);
            static auto original = g_hook->get_original<func_t>();
            const float result   = original(movie, delta, catch_up, capture);
            try {
                AfterAdvance(movie);
            } catch (...) {
            }
            return result;
        }
    }

    void Install()
    {
        // The movie's type table (MovieImpl's own: its complete object locator says offset 0), as the viewport hook's.
        uintptr_t table = 0;
        for (const uintptr_t t : utility::rtti::find_vtables(MemoryScan::mod, ".?AVMovieImpl@GFx@Scaleform@@")) {
            uintptr_t locator = 0;
            uint32_t  offset  = 1;
            if (ReadPointer(t - 8, locator) && locator != 0) {
                uintptr_t word = 0;
                if (ReadPointer(locator, word)) {
                    offset = static_cast<uint32_t>(word >> 32); // (the locator: signature, then the offset)
                }
            }
            if (offset == 0) {
                table = t;
                break;
            }
        }
        uintptr_t advance = 0;
        if (table == 0 || !ReadPointer(table + kAdvanceSlot * 8, advance) || !InGame(advance)) {
            spdlog::warn("[MenuClean] the game's Flash movies (MovieImpl) not found: menus keep their painted backgrounds");
            SetStatus("Menu backgrounds: left as they are (the game's Flash movies not found)");
            return;
        }
        g_hook = std::make_unique<FunctionHook>(advance, reinterpret_cast<uintptr_t>(&AdvanceDetour));
        if (!g_hook->create()) {
            spdlog::warn("[MenuClean] could not hook the game's Flash movies' Advance: menus keep their painted backgrounds");
            SetStatus("Menu backgrounds: left as they are (could not hook the game's Flash movies)");
            return;
        }
        const auto base = reinterpret_cast<uintptr_t>(MemoryScan::mod);
        spdlog::info("[MenuClean] the game's Flash movies' Advance hooked (MovieImpl type table rva {:x}, function 0x18 rva {:x})", table - base,
            advance - base);
        SetStatus("Menu backgrounds: ready (taken out of see-through menus)");
    }

    std::string Status()
    {
        std::scoped_lock _{ g_status_mtx };
        return fmt::format("{} [parts removed {}, hidden {}, menus left as they are {}]", g_status, g_removed.load(), g_hidden.load(), g_left.load());
    }
}

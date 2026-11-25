#include "BuildConsole.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <numeric>
#include <shared_mutex>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/spdlog.h>

#include <ModSettings.h>
#include "CreationEngineCameraManager.h"
#include "MessagesBlock.h"
#include "RE/S/ScaleformGFxMovie.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"
#include <mods/VR.hpp>

namespace BuildConsole
{
    namespace
    {
        // The movies' stage (WorkshopMenu.swf and HUDMessagesMenu.gfx: 1920 x 1080, filling a 16:9 viewport exactly).
        constexpr float kStageW = 1920.0f;
        constexpr float kStageH = 1080.0f;
        // The notifications' movie in a box below the build menu's, the very box the HUD has in play (the HUDScale setting's share of the eye picture,
        // taller than wide), only moved; made smaller (the same shape) when the layer is too short for it, not below half (else build mode as
        // before). Its shape kept because the game lays a movie's parts out only when its own sizes change (the viewport routine at
        // 0x1425563BD-0x14255660E: SetSafeRect is called only when the safe rect it works out from the game's own sizes differs): the HUD's
        // movies, always open, keep the places they were pinned to in play (its subtitles at that tall box's bottom); in a box of another
        // shape some would fall outside it. (Build mode's menu is made anew as it opens, and laid out then in its console box.)
        constexpr float kMessagesFitMin = 0.5f;
        // The panels: low in front of your seat this far from your eyes (the research: 0.8 to 1 m), those with your head a little nearer (in
        // front of them when they cross); text 18 of the stage's units tall (the list's, the hints', the notifications') this many degrees
        // (Microsoft's comfortable 0.65-0.8 degrees).
        constexpr float kRoomM     = 0.90f;
        constexpr float kHeadM     = 0.75f;
        constexpr float kTextDeg   = 0.70f;
        constexpr float kTextUnits = 18.0f;
        // A new opening: none drawn in the last second. Put again in front of you when your head moved this far (standing up, a step) or
        // the view was recentred.
        constexpr int64_t kNewAfterMs  = 1000;
        constexpr float   kHeadMovedM  = 0.5f;
        // Not shown this long into an opening (and not for this long since): build mode in the view as before for that opening (fix175's
        // way); this many openings so: for the session. The cut refused this many times in a row: likewise.
        constexpr int64_t kShownWithinMs   = 800;
        constexpr int64_t kShownGapMs      = 600;
        constexpr int     kFallbacksToStop = 3;
        constexpr int     kRefusalsToStop  = 30;
        // This opening's picture saved for the log this long into it (once).
        constexpr int64_t kSnapAfterMs = 2000;

        enum class Movie : uint8_t { Workshop, Messages };
        enum class Where : uint8_t { Room, Head };

        // Each part of the console: its crop of its movie's stage (the research's, from the movies' own layout: scratchpad buildmenu_rs; with
        // from_bottom its v0 and v1 from the bottom of the part of the stage its box shows, where the movie pins it), where it goes (the room:
        // degrees right and up from the console's straight ahead at your eyes' level; your head: from your view's middle; with hud_place
        // where the HUD has it in play), its text as the console's or as the HUD's own size, and whether it is shown only while the action or
        // choice card is up.
        struct PanelDef
        {
            const char* name;
            Movie       movie;
            float       u0, v0, u1, v1;
            Where       where;
            float       az, el; // degrees: its middle's right (+) or left, its top's (top) or middle's up (+) or down
            bool        top;
            bool        hud_scale;
            bool        hud_place;
            bool        from_bottom;
            bool        centre_card;
            int         column{ -1 }; // (fix178) its row in the column low in the middle (0 the top one), centred on straight ahead
        };
        // The room's (the order they are drawn in: the room's first, your head's over them):
        // - the item card (BuildItemCard_mc, InspectItemCard_mc, AreaResourcesCard_mc at the stage's top left: 333 wide, 30 to 830 tall as the
        //   item has sections), 20 degrees left, from 5 above your eyes' level down;
        // - the categories (LB [..] RB), the list and its pop-ups and colour pop-up (BuildPopupContainer_mc, ColorPopup_mc, top right; the
        //   pop-ups open 300 units left of the list, the list itself on the right of this crop at 21 degrees right), from 5 above down;
        // - (fix178) one column low in the middle under them, centred straight ahead, from about 28 to 38 degrees below: the button hints
        //   (ButtonBar_mc or SecondaryButtonBar_mc, right-justified at x 1870, y 867-915: only the bar shown, read each look), then the
        //   footer's two boxes (FooterContainer_mc: Info_mc the mode, the outpost's or ship's name and the build limit; Stats_mc cargo, crew,
        //   power and production) one under the other. (fix177 had the hints across the middle with the bar at its right end, and the footer's
        //   two halves side by side under them; the lower rows read better stacked in the lower middle.)
        // Your head's:
        // - the action and choice cards (ActionCard_mc, InteractiveMessage_mc: modify mode's actions, the game's choices), where the game
        //   puts them, just right of your view's middle (they are about what you aim at), only while one is up;
        // - the build warnings (DisplayMessage_mc: red "cannot place" and the like), a line 5 degrees below your view's middle ((fix178) 7
        //   before: the messages' block now starts 8 below);
        // - the notifications (HUDMessagesMenu's Messages_mc, the stacked messages pinned to the stage's right, its y kept at 100), a strip from
        //   10 degrees below your view's middle, at the HUD's own size (as sharp as the HUD's own text: its box is the HUD's), bigger in view
        //   than at the edge where they were (0.48 degrees there). (fix178) With the messages arranged below your view's middle (MessagesBlock):
        //   that whole block instead, just where it is in play (notifications, mission updates, flyout, level-up), and no mission updates' part;
        // - the subtitles (BottomCenterGroup_mc, pinned to the box's bottom middle) and the mission updates (QuestUpdate_mc, pinned left) just
        //   where the HUD has them in play. (The rest of HUDMessagesMenu, the location's name, XP and level-up in the middle, is not shown while
        //   building.)
        constexpr PanelDef kPanels[] = {
            { "the item card", Movie::Workshop, 40, 40, 400, 925, Where::Room, -20.5f, 5.0f, true, false, false, false, false },
            { "the categories and list", Movie::Workshop, 1280, 36, 1905, 862, Where::Room, 15.8f, 5.0f, true, false, false, false, false },
            // (fix178: one column low in the middle, top to bottom: the
            // button hints (the bar shown only, read each look: the console's hints panel trimmed to it), the footer's name and build limit,
            // its cargo, crew and power; each trimmed to its box (no divider line above it), centred on straight ahead.)
            { "the button hints", Movie::Workshop, 600, 862, 1890, 924, Where::Room, 0.0f, 0.0f, true, false, false, false, false, 0 },
            { "the footer's name and build limit", Movie::Workshop, 46, 938, 694, 1030, Where::Room, 0.0f, 0.0f, true, false, false, false, false, 1 },
            { "the footer's cargo, crew and power", Movie::Workshop, 1026, 938, 1884, 1030, Where::Room, 0.0f, 0.0f, true, false, false, false, false, 2 },
            { "the action and choice cards", Movie::Workshop, 990, 330, 1370, 620, Where::Head, 8.6f, 2.5f, false, false, false, false, true },
            // ((fix178) 5 below the middle, not 7: the messages' block now starts 8 below.)
            { "the build warnings", Movie::Workshop, 480, 740, 1440, 812, Where::Head, 0.0f, -5.0f, false, false, false, false, false },
            { "the notifications", Movie::Messages, 1400, 60, 1920, 340, Where::Head, 3.0f, -10.0f, true, true, false, false, false },
            { "the subtitles", Movie::Messages, 380, -300, 1580, 0, Where::Head, 0.0f, 0.0f, false, true, true, true, false },
            { "the mission updates", Movie::Messages, 0, 60, 520, 420, Where::Head, 0.0f, 0.0f, false, true, true, false, false },
        };
        constexpr int kPanelCount = static_cast<int>(sizeof(kPanels) / sizeof(kPanels[0]));
        static_assert(kPanelCount <= vrmod::D3D12Component::ConsoleCut::kMax && kPanelCount <= runtimes::OpenXR::kConsoleQuads);
        constexpr int kListPanel   = 1;
        constexpr int kCardsPanel  = 5;
        constexpr int kHintsPanel  = 2;
        constexpr int kFooterLeft  = 3;
        constexpr int kFooterRight = 4;
        // (The list's crop overlaps the cards' at x 1280-1370, y 330-620 in the stage: while a card is up, the list's left edge is moved to
        // the cards' right one, so no piece of a card shows on the list's panel.)
        constexpr float kListTrimU0 = 1372.0f;
        // (fix178) The column low in the middle: its top row's top this many degrees below your eyes' level (just under the list's last row,
        // about 27 below, and the item card's usual bottom), its rows this far apart. (Its last row ends about 38 below: the three rows need
        // about 10 degrees at 0.7-degree text, under the card and the list, which stay where they are.)
        constexpr float kColumnTopDeg = -27.6f;
        constexpr float kColumnGapDeg = 0.6f;
        // (fix178) In the messages' block (MessagesBlock): the notifications' part is the whole block, where it is in play; the mission updates
        // have no part of their own (they are in it).
        constexpr int kNotificationsPanel = 7;
        constexpr int kMissionsPanel      = 9;

        // The boxes and the packed picture for one size of interface layer (the render side; made again when the layer's size changes).
        struct Layout
        {
            int      buffer[2]{ 0, 0 };
            uint32_t backbuffer[2]{ 0, 0 }; // (what the HUD's box is worked out from, with its scale)
            float    hud_scale{ 0.0f };
            bool     ok{ false };
            int      box[2][4]{};      // [movie] left, top, width, height (the layer's pixels)
            float    scale[2]{};       // [movie] pixels per stage unit
            float    origin[2][2]{};   // [movie] where the stage's 0,0 is in the layer (the stage centred in its box)
            float    hud_play_w{ 0.0f }; // the HUD's box's width in play (pixels of the eye picture), for the HUD's own size in view
            float    crop[kPanelCount][4]{}; // each part's crop of the stage (units), from_bottom worked out
            bool     enabled[kPanelCount]{};   // (fix178) the parts this layout has (the mission updates not, with the messages' block)
            bool     hud_place[kPanelCount]{}; // (fix178) where the HUD has it in play
            bool     block{ false };           // (fix178) made for the messages' block, with this region
            float    block_region[4]{};
            D3D12_BOX from[kPanelCount]{};
            uint32_t to[kPanelCount][2]{};
            uint32_t size[2]{ 0, 0 }; // the packed picture
        };
        // (The render side: the layout in use, when each movie was last placed, this opening.)
        std::mutex g_mtx{};
        Layout     g_layout{};
        int64_t    g_workshop_ms{ 0 };
        int64_t    g_messages_ms{ 0 };
        int64_t    g_opening_start_ms{ 0 };
        uint32_t   g_opening{ 0 };
        bool       g_snapped{ false }; // (this opening's picture asked for)
        int64_t              g_clock_start_ms{ 0 }; // (this opening's 0.8 s counted from here: moved on while the headset does not draw)
        std::atomic<bool>    g_fallback{ false };
        std::atomic<int>     g_fallbacks{ 0 };
        std::atomic<bool>    g_stopped{ false };
        std::atomic<int>     g_refusals{ 0 };
        std::atomic<uint32_t> g_cuts{ 0 };
        std::atomic<uint32_t> g_opening_cuts{ 0 };
        std::atomic<const char*> g_problem{ "none seen" }; // (the last thing that kept it from being shown, for the fallback's line)
        std::atomic<int64_t> g_workshop_seen_ms{ 0 }; // (g_workshop_ms, read without the lock: the HUD's movies in play look only at this)
        std::atomic<int> g_layout_logs{ 0 };
        std::atomic<int> g_opening_logs{ 0 };
        std::atomic<int> g_lrg_logs{ 0 };
        // (MenuClean: the action or choice card up, when last read.)
        std::atomic<bool>    g_cards_known{ false };
        std::atomic<bool>    g_cards_shown{ false };
        std::atomic<int64_t> g_cards_ms{ 0 };
        // (The review) Where build mode's menu was laid out. The game lays a movie out (its SetSafeRect) only when the safe rect it works out
        // from its own sizes differs from the movie's (0x14255641B-0x14255644F): at a new movie's first viewport and when the layer's size
        // changes. Whether the console's box was the one given at the last of those (render side, with g_mtx); the movie and its layer's size
        // at its last viewport; when it was last given one at all (on the console or not).
        void*   g_menu_movie{ nullptr };
        int     g_menu_buffer[2]{ 0, 0 };
        int64_t g_menu_call_ms{ 0 };
        bool    g_menu_in_box{ false };
        // (MenuClean, as it reads the menu: its list's y, about 46 when laid out in a 16:9 box, about -448 in fix175's tall one.)
        std::atomic<bool>    g_layout_known{ false };
        std::atomic<float>   g_layout_y{ 0.0f };
        std::atomic<int64_t> g_layout_ms{ 0 };
        std::atomic<int64_t> g_layout_wrong_since{ 0 };
        // (Render side: this opening's frames placed on the console, and since it was last shown; the shown time seen last.)
        int     g_opening_frames{ 0 };
        int     g_frames_unshown{ 0 };
        int64_t g_shown_seen_ms{ 0 };
        // (The review) Another movie drawn inside the boxes while the console cuts: logged once each; the game's fader (a fade to or from black,
        // which must show whole) stops the cut while it is drawn.
        std::atomic<int64_t> g_overlay_ms{ 0 };
        std::mutex           g_others_mtx{};
        std::vector<uint32_t> g_others_logged{};
        // (fix178, MenuClean) The build menu's hints bar shown: its left and right (stage units), when last read.
        std::atomic<bool>    g_hints_known{ false };
        std::atomic<bool>    g_hints_any{ false };
        std::atomic<float>   g_hints_left{ 0.0f };
        std::atomic<float>   g_hints_right{ 0.0f };
        std::atomic<int64_t> g_hints_ms{ 0 };
        std::atomic<int>     g_hints_logs{ 0 };
        std::atomic<float>   g_hints_logged_width{ -1.0f };

        // (The game's thread: where the console is.)
        struct Room
        {
            bool      placed{ false };
            uint32_t  opening{ 0 };
            glm::vec3 eyes{ 0.0f };          // your eyes when it was placed (stage)
            glm::vec3 forward{ 0.0f, 0.0f, -1.0f }; // the way your head faced then, level
            glm::vec3 origin{ 0.0f };        // the standing origin then (a recenter moves it)
            int64_t   since_ms{ 0 };
            int64_t   last_us{ 0 };
            int       moves{ 0 };
            bool      logged{ false };       // (this opening's panels logged)
            // Time your view rested on each of the room's panels, the cards' time up, frames shown (for the log as it closes).
            float     looked[kPanelCount]{};
            float     cards_up{ 0.0f };
            float     open_s{ 0.0f };
            uint32_t  frames{ 0 };
        };
        Room g_room{};
        int  g_close_logs{ 0 };
        int  g_move_logs{ 0 };
        int  g_place_logs{ 0 };

        int64_t NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // (The review) The HUD's movies, the notifications' box and the cut follow build mode's placement on the console within half a second:
        // the HUD's viewport is set before build mode's in each frame, so a shorter window (fix177's first, two frames) put the HUD back in its
        // play box after one slow frame, under the boxes the cut then cleared (its parts flashed between them, or landed on the panels). After
        // build mode closes the HUD comes back that much later (the boxes still cleared meanwhile: nothing stale is shown).
        constexpr int64_t kPartnersWindowMs = 500;

        // The console could be used at all: the setting on, in the headset (OpenXR), not stopped or refused by the runtime. (Not whether it
        // is shown this moment: a menu over build mode, a conversation and the menu panel are Wanted's.)
        bool Enabled()
        {
            static auto vr = VR::get();
            if (!GameFlow::gStore.internalSettings.buildConsole || g_stopped.load() || vr == nullptr || !vr->is_hmd_active() ||
                !vr->get_runtime()->is_openxr() || ModSettings::showFlatScreenDisplay()) {
                return false;
            }
            const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
            return xr != nullptr && !xr->console_refused.load();
        }

        bool Wanted()
        {
            static auto vr = VR::get();
            if (!Enabled() || ModSettings::g_internalSettings.showQuadDisplay) {
                return false;
            }
            const auto xr = vr->m_openxr; // (a copy: it may be made anew on another thread)
            // (Not with a menu over build mode, its message box say: that is on the menu panel. Nor in a conversation, which has its own cut.)
            return xr != nullptr && !xr->panel_mode && !GameFlow::isShowingMenu() && !GameFlow::isInDialogue();
        }

        // ---- The packed picture: each part where its top is lowest, then leftmost (a "skyline"), the tallest first; tried at a few widths,
        // the smallest kept. A 2-pixel gap round each (the headset's filtering never reaches a neighbour). ----
        struct PackItem
        {
            int w{ 0 }, h{ 0 };
            int x{ 0 }, y{ 0 };
        };

        bool PackAt(std::vector<PackItem>& items, int width, int& used_w, int& used_h)
        {
            struct Seg
            {
                int x, w, y;
            };
            std::vector<Seg> sky{ { 0, width, 0 } };
            std::vector<size_t> order(items.size());
            std::iota(order.begin(), order.end(), size_t{ 0 });
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return items[a].h != items[b].h ? items[a].h > items[b].h : items[a].w > items[b].w;
            });
            used_w = used_h = 0;
            for (const size_t idx : order) {
                auto& it = items[idx];
                if (it.w > width) {
                    return false;
                }
                int best_y = INT_MAX, best_x = 0;
                for (size_t i = 0; i < sky.size(); ++i) {
                    const int x = sky[i].x;
                    if (x + it.w > width) {
                        break;
                    }
                    int y = 0, covered = 0;
                    for (size_t j = i; j < sky.size() && covered < it.w; ++j) {
                        y = std::max(y, sky[j].y);
                        covered += sky[j].w;
                    }
                    if (y < best_y) {
                        best_y = y;
                        best_x = x;
                    }
                }
                if (best_y == INT_MAX) {
                    return false;
                }
                it.x = best_x;
                it.y = best_y;
                const int x0 = best_x, x1 = best_x + it.w, top = best_y + it.h;
                std::vector<Seg> next{};
                for (const auto& s : sky) {
                    const int s0 = s.x, s1 = s.x + s.w;
                    if (s1 <= x0 || s0 >= x1) {
                        next.push_back(s);
                        continue;
                    }
                    if (s0 < x0) {
                        next.push_back({ s0, x0 - s0, s.y });
                    }
                    if (s1 > x1) {
                        next.push_back({ x1, s1 - x1, s.y });
                    }
                }
                next.push_back({ x0, it.w, top });
                std::sort(next.begin(), next.end(), [](const Seg& a, const Seg& b) { return a.x < b.x; });
                sky.clear();
                for (const auto& s : next) {
                    if (!sky.empty() && sky.back().y == s.y && sky.back().x + sky.back().w == s.x) {
                        sky.back().w += s.w;
                    } else {
                        sky.push_back(s);
                    }
                }
                used_w = std::max(used_w, x1);
                used_h = std::max(used_h, top);
            }
            return true;
        }

        bool Pack(std::vector<PackItem>& items, uint32_t& out_w, uint32_t& out_h)
        {
            int widest = 0;
            for (const auto& it : items) {
                widest = std::max(widest, it.w);
            }
            if (widest <= 0) {
                return false;
            }
            std::vector<PackItem> best{};
            int64_t best_area = INT64_MAX;
            int     best_w = 0, best_h = 0;
            for (int width = widest; width <= std::max(widest * 3, 4096); width += 32) {
                auto trial = items;
                int  w = 0, h = 0;
                if (!PackAt(trial, width, w, h)) {
                    continue;
                }
                const int64_t area = static_cast<int64_t>(w) * h;
                if (area < best_area) {
                    best_area = area;
                    best      = std::move(trial);
                    best_w    = w;
                    best_h    = h;
                }
            }
            if (best.empty() || best_w <= 0 || best_h <= 0 || best_w > 8192 || best_h > 8192) {
                return false;
            }
            items = std::move(best);
            out_w = static_cast<uint32_t>(best_w);
            out_h = static_cast<uint32_t>(best_h);
            return true;
        }

        // The boxes in a layer of this size: the build menu's 16:9 and as wide as the layer at its top, the notifications' the HUD's own box
        // below it, centred; each part's pixels in the layer and its place in the packed picture. (backbuffer, hud_scale: as the HUD's box is
        // worked out in play, CreationEngineCameraManager.)
        // ((fix178) block: the messages arranged below your view's middle (MessagesBlock): the notifications' part is then the whole block,
        // where it is in play, and the mission updates have none of their own.)
        Layout MakeLayout(int buffer_w, int buffer_h, const uint32_t backbuffer[2], float hud_scale, const MessagesBlock::Plan* block)
        {
            Layout l{};
            l.buffer[0]     = buffer_w;
            l.buffer[1]     = buffer_h;
            l.backbuffer[0] = backbuffer[0];
            l.backbuffer[1] = backbuffer[1];
            l.hud_scale     = hud_scale;
            l.block         = block != nullptr;
            if (block != nullptr) {
                for (int k = 0; k < 4; ++k) {
                    l.block_region[k] = block->region[k];
                }
            }
            const float sw = static_cast<float>(buffer_w) / kStageW;
            const int   wh = static_cast<int>(std::lround(static_cast<float>(buffer_w) * kStageH / kStageW));
            const int   avail = buffer_h - wh;
            // The HUD's box in play, and how much smaller it must be here (the same shape).
            const int   play_w = std::min(static_cast<int>(static_cast<float>(backbuffer[0]) * hud_scale), buffer_w);
            const int   play_h = std::min(static_cast<int>(static_cast<float>(backbuffer[1]) * hud_scale), buffer_h);
            const float fit    = play_w > 0 && play_h > 0 ? std::min({ 1.0f, static_cast<float>(avail) / play_h, static_cast<float>(buffer_w) / play_w }) : 0.0f;
            if (buffer_w < 640 || wh <= 0 || avail <= 0 || !(fit >= kMessagesFitMin)) {
                return l; // (not ok: a layer as wide as tall or wider, the flat screen's; or the HUD's box too big below it)
            }
            const int mw = std::max(1, static_cast<int>(std::floor(play_w * fit)));
            const int mh = std::max(1, static_cast<int>(std::floor(play_h * fit)));
            l.box[0][0]     = 0;
            l.box[0][1]     = 0;
            l.box[0][2]     = buffer_w;
            l.box[0][3]     = wh;
            l.scale[0]      = sw;
            l.origin[0][0]  = 0.0f;
            l.origin[0][1]  = 0.0f;
            l.box[1][0]     = (buffer_w - mw) / 2;
            l.box[1][1]     = wh + (avail - mh) / 2;
            l.box[1][2]     = mw;
            l.box[1][3]     = mh;
            // (Shown whole in its box, centred: the movie's scale mode 1, as the probe logs.)
            l.scale[1]      = std::min(static_cast<float>(mw) / kStageW, static_cast<float>(mh) / kStageH);
            l.origin[1][0]  = l.box[1][0] + (mw - kStageW * l.scale[1]) * 0.5f;
            l.origin[1][1]  = l.box[1][1] + (mh - kStageH * l.scale[1]) * 0.5f;
            l.hud_play_w    = static_cast<float>(play_w);
            // The bottom of the part of the stage the notifications' box shows (stage units): its subtitles are pinned to it.
            const float shown_bottom = kStageH + (mh - kStageH * l.scale[1]) * 0.5f / l.scale[1];
            constexpr int kGap = 2;
            std::vector<PackItem> items{};
            std::vector<int>      packed{};
            bool                  block_ok = block != nullptr; // ((the review) its crop inside the box: else the old parts kept)
            for (int i = 0; i < kPanelCount; ++i) {
                const auto& p   = kPanels[i];
                const int   m   = p.movie == Movie::Workshop ? 0 : 1;
                const auto* b   = l.box[m];
                const float s   = l.scale[m];
                const float ox  = l.origin[m][0];
                const float oy  = l.origin[m][1];
                // (The layer's pixels of a crop, clamped to the movie's box.)
                const auto pixels = [&](float cu0, float cv0, float cu1, float cv1, int& x0, int& y0, int& x1, int& y1) {
                    x0 = std::clamp(static_cast<int>(std::floor(ox + cu0 * s)), b[0], b[0] + b[2]);
                    x1 = std::clamp(static_cast<int>(std::ceil(ox + cu1 * s)), b[0], b[0] + b[2]);
                    y0 = std::clamp(static_cast<int>(std::floor(oy + cv0 * s)), b[1], b[1] + b[3]);
                    y1 = std::clamp(static_cast<int>(std::ceil(oy + cv1 * s)), b[1], b[1] + b[3]);
                    return x1 > x0 && y1 > y0;
                };
                float       u0 = p.u0, u1 = p.u1;
                float       v0  = p.from_bottom ? shown_bottom + p.v0 : p.v0;
                float       v1  = p.from_bottom ? shown_bottom + p.v1 : p.v1;
                l.enabled[i]    = true;
                l.hud_place[i]  = p.hud_place;
                if (block_ok && i == kNotificationsPanel) {
                    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
                    if (pixels(block->region[0], block->region[1], block->region[2], block->region[3], bx0, by0, bx1, by1)) {
                        u0             = block->region[0];
                        v0             = block->region[1];
                        u1             = block->region[2];
                        v1             = block->region[3];
                        l.hud_place[i] = true;
                    } else {
                        block_ok = false; // (outside the box: the notifications' and mission updates' own parts, as before)
                    }
                } else if (block_ok && i == kMissionsPanel) {
                    l.enabled[i] = false;
                    continue;
                }
                l.crop[i][0]    = u0;
                l.crop[i][1]    = v0;
                l.crop[i][2]    = u1;
                l.crop[i][3]    = v1;
                int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                if (!pixels(u0, v0, u1, v1, x0, y0, x1, y1)) {
                    return l;
                }
                l.from[i] = D3D12_BOX{ static_cast<UINT>(x0), static_cast<UINT>(y0), 0, static_cast<UINT>(x1), static_cast<UINT>(y1), 1 };
                items.push_back(PackItem{ x1 - x0 + kGap, y1 - y0 + kGap });
                packed.push_back(i);
            }
            if (!Pack(items, l.size[0], l.size[1])) {
                return l;
            }
            for (size_t k = 0; k < packed.size(); ++k) {
                l.to[packed[k]][0] = static_cast<uint32_t>(items[k].x);
                l.to[packed[k]][1] = static_cast<uint32_t>(items[k].y);
            }
            l.ok = true;
            return l;
        }

        // (fix178) The messages' block wanted in this layout: arranged lately (MessagesBlock), and its region.
        bool BlockNow(MessagesBlock::Plan& plan)
        {
            return MessagesBlock::Applied(plan);
        }

        bool SameBlock(const Layout& l, bool block, const MessagesBlock::Plan& plan)
        {
            if (l.block != block) {
                return false;
            }
            if (!block) {
                return true;
            }
            for (int k = 0; k < 4; ++k) {
                if (std::abs(l.block_region[k] - plan.region[k]) > 2.0f) {
                    return false;
                }
            }
            return true;
        }

        // A direction (degrees right and up) in a frame (its forward and up, level): a unit vector.
        glm::vec3 Direction(const glm::vec3& forward, const glm::vec3& up, float az_deg, float el_deg)
        {
            const glm::vec3 right = glm::normalize(glm::cross(forward, up));
            const float     az    = glm::radians(az_deg);
            const float     el    = glm::radians(el_deg);
            return glm::normalize(std::cos(el) * (std::cos(az) * forward + std::sin(az) * right) + std::sin(el) * up);
        }

        // A quad at `at` facing `eyes` (its front, +z, toward them), kept upright.
        glm::quat Facing(const glm::vec3& at, const glm::vec3& eyes, const glm::vec3& up)
        {
            glm::vec3 z = eyes - at;
            z           = glm::length(z) > 1e-4f ? glm::normalize(z) : glm::vec3{ 0.0f, 0.0f, 1.0f };
            glm::vec3 x = glm::cross(up, z);
            x           = glm::length(x) > 1e-4f ? glm::normalize(x) : glm::vec3{ 1.0f, 0.0f, 0.0f };
            const glm::vec3 y = glm::cross(z, x);
            return glm::normalize(glm::quat_cast(glm::mat3{ x, y, z }));
        }

        // The HUD's own size in view, as a tangent per stage unit: its box's width in play (pixels of the eye picture) across the eye's view.
        float HudTanPerUnit(const Layout& l)
        {
            static auto vr = VR::get();
            Vector4f    f{};
            {
                std::shared_lock _{ vr->get_runtime()->eyes_mtx };
                f = vr->get_runtime()->eye_frustums[1];
            }
            const float across = f[1] > f[0] ? f[1] - f[0] : 2.75f;
            const float share  = l.backbuffer[0] > 0 ? l.hud_play_w / static_cast<float>(l.backbuffer[0]) : 0.4f;
            return std::clamp(share * across / kStageW, 0.0003f, 0.0011f);
        }

        // A movie's file (its definition's URL), read under a guard.
        bool MovieUrl(void* movie, const char*& url)
        {
            __try {
                auto* def = reinterpret_cast<RE::Scaleform::GFx::MovieImpl*>(movie)->GetMovieDef();
                url       = def != nullptr ? def->GetFileURL() : nullptr;
                return url != nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // The part of a movie's stage its viewport shows (as it was last set: the frame before's), read under a guard.
        bool ShownPart(void* movie, float out[4])
        {
            __try {
                const auto v = reinterpret_cast<RE::Scaleform::GFx::MovieImpl*>(movie)->GetVisibleFrameRect();
                out[0]       = v.x;
                out[1]       = v.y;
                out[2]       = v.z;
                out[3]       = v.w;
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }
        // (A few frames into each opening, the first openings: the part of the stage each movie shows, logged once each.)
        std::atomic<uint32_t> g_shown_logged[2]{};
        std::atomic<int>      g_shown_logs{ 0 };

        void LogShownPart(void* movie, int which, uint32_t opening, int64_t since_ms)
        {
            if (movie == nullptr || since_ms < 500 || g_shown_logged[which].load() == opening || g_shown_logs.load() >= 20) {
                return;
            }
            g_shown_logged[which] = opening;
            ++g_shown_logs;
            float r[4]{};
            if (ShownPart(movie, r)) {
                spdlog::info("[Workshop] (fix177) build console: {} shows {:.0f},{:.0f} to {:.0f},{:.0f} of its stage ({})", which == 0 ? "the build menu" : "the notifications' movie",
                    r[0], r[1], r[2], r[3], which == 0 ? "0,0 to 1920,1080 expected: its parts at their designed places" : "as the HUD's box in play");
            }
        }

        bool CardsUp(bool& known)
        {
            known = g_cards_known.load() && NowMs() - g_cards_ms.load() < 500;
            return !known || g_cards_shown.load();
        }
    }

    void Install()
    {
        spdlog::info("[Workshop] (fix177) build mode as a console (setting BuildConsole): its item card, list, button hints and footer on panels "
                     "low in front of your seat, its action cards, warnings and notifications near your view's middle");
    }

    bool LookWanted()
    {
        static auto vr = VR::get();
        return GameFlow::gStore.internalSettings.buildConsole && vr != nullptr && vr->is_hmd_active() && !ModSettings::showFlatScreenDisplay();
    }

    bool ConsoleShown()
    {
        const int64_t seen = g_workshop_seen_ms.load(std::memory_order_relaxed);
        return seen != 0 && NowMs() - seen <= 200 && !g_fallback.load() && Enabled();
    }

    void NoteCentreCards(bool known, bool shown)
    {
        g_cards_known = known;
        g_cards_shown = shown;
        g_cards_ms    = NowMs();
    }

    void NoteHintsBar(bool known, bool any, float left, float right)
    {
        g_hints_known = known;
        g_hints_any   = any;
        g_hints_left  = left;
        g_hints_right = right;
        g_hints_ms    = NowMs();
        // (Its width when it changes, the first few times: the hints panel follows it, centred.)
        const float width = any ? right - left : 0.0f;
        if (known && std::abs(width - g_hints_logged_width.load()) > 4.0f && g_hints_logs.fetch_add(1) < 20) {
            g_hints_logged_width = width;
            spdlog::info("[Workshop] (fix178) build console: the button hints' bar {} (the hints panel trimmed to it, centred straight ahead, its "
                         "row {:.1f} degrees below your eyes' level, {:.1f} degrees wide)",
                any ? std::format("from {:.0f} to {:.0f} of the stage", left, right) : std::string{ "not shown" }, -kColumnTopDeg,
                any ? glm::degrees(2.0f * std::atan((width + 16.0f) * 0.5f * std::tan(glm::radians(kTextDeg / kTextUnits)))) : 0.0f);
        }
    }

    void NoteMenuLayout(bool known, float list_y)
    {
        const int64_t now = NowMs();
        g_layout_known    = known;
        g_layout_y        = list_y;
        g_layout_ms       = now;
        // (Laid out for a taller box than 16:9: its list well above the stage's top.)
        if (known && list_y < -150.0f) {
            int64_t none = 0;
            g_layout_wrong_since.compare_exchange_strong(none, now);
        } else {
            g_layout_wrong_since = 0;
        }
    }

    void NoteMovie(void* movie, const Scaleform::Gfx::Viewport* viewport)
    {
        // (Only while the console cuts: in play, one look at a number.)
        const int64_t seen = g_workshop_seen_ms.load(std::memory_order_relaxed);
        if (movie == nullptr || viewport == nullptr || seen == 0) {
            return;
        }
        const int64_t now = NowMs();
        if (now - seen > kPartnersWindowMs || g_fallback.load()) {
            return;
        }
        const char* url = nullptr;
        if (!MovieUrl(movie, url) || url == nullptr) {
            return;
        }
        const uint32_t hash = djb2Hash(url);
        if (hash == "Interface/WorkshopMenu.swf"_DJB || hash == "Interface/HUDMessagesMenu.gfx"_DJB || hash == "Interface/HUDMenu.gfx"_DJB ||
            hash == "Interface/HUDMenu_LRG.gfx"_DJB) {
            return;
        }
        Layout l{};
        {
            std::scoped_lock _{ g_mtx };
            l = g_layout;
        }
        if (!l.ok || viewport->bufferWidth != l.buffer[0] || viewport->bufferHeight != l.buffer[1] || viewport->width <= 0 || viewport->height <= 0) {
            return;
        }
        // Inside either box (the parts of the layer the cut clears)?
        bool inside = false;
        for (int m = 0; m < 2; ++m) {
            const auto* b = l.box[m];
            inside = inside || (viewport->left < b[0] + b[2] && b[0] < viewport->left + viewport->width && viewport->top < b[1] + b[3] &&
                                   b[1] < viewport->top + viewport->height);
        }
        if (!inside) {
            return;
        }
        // The game's fader: a fade to or from black must show whole, so the cut stops while it is drawn.
        const bool fader = hash == "Interface/FaderMenu.swf"_DJB || hash == "Interface/FaderMenu_LRG.swf"_DJB;
        if (fader) {
            g_overlay_ms = now;
        }
        bool first = false;
        {
            std::scoped_lock _{ g_others_mtx };
            if (g_others_logged.size() < 32 && std::find(g_others_logged.begin(), g_others_logged.end(), hash) == g_others_logged.end()) {
                g_others_logged.push_back(hash);
                first = true;
            }
        }
        if (first) {
            spdlog::info("[Workshop] (fix177) build console: {} drawn at {},{} {}x{} of the {}x{} layer, inside the console's boxes: {}", url,
                viewport->left, viewport->top, viewport->width, viewport->height, viewport->bufferWidth, viewport->bufferHeight,
                fader ? "the cut stops while it is drawn (its fade shown whole)" : "cleared with them (or caught on a panel)");
        }
    }

    bool PlaceMovie(void* movie, uint32_t movie_hash, Scaleform::Gfx::Viewport* viewport, int buffer_w, int buffer_h)
    {
        static auto vr = VR::get();
        const bool  workshop = movie_hash == "Interface/WorkshopMenu.swf"_DJB;
        const bool  large    = movie_hash == "Interface/WorkshopMenu_LRG.swf"_DJB;
        const bool  messages = movie_hash == "Interface/HUDMessagesMenu.gfx"_DJB;
        const bool  hud      = movie_hash == "Interface/HUDMenu.gfx"_DJB || movie_hash == "Interface/HUDMenu_LRG.gfx"_DJB;
        if (viewport == nullptr || buffer_w <= 0 || buffer_h <= 0 || !(workshop || large || messages || hud)) {
            return false;
        }
        if (large) {
            // (Its parts' places not measured: the large-text movie has its own bounds and offsets.)
            if (GameFlow::gStore.internalSettings.buildConsole && g_lrg_logs.fetch_add(1) < 3) {
                spdlog::info("[Workshop] (fix177) the large-text build menu (WorkshopMenu_LRG): its parts' places are not known to the console: "
                             "build mode in the view as before (fix175's way)");
            }
            return false;
        }
        const int64_t now = NowMs();
        if (!workshop) {
            // The notifications' movie and the HUD: while build mode is placed on the console (within half a second: kPartnersWindowMs; the
            // cut clears the notifications' box as long). (In play: one look at a number, nothing more.)
            if (const int64_t seen = g_workshop_seen_ms.load(std::memory_order_relaxed); seen == 0 || now - seen > kPartnersWindowMs || g_fallback.load()) {
                return false;
            }
            Layout   l{};
            uint32_t opening = 0;
            int64_t  start   = 0;
            {
                std::scoped_lock _{ g_mtx };
                if (g_workshop_ms == 0 || now - g_workshop_ms > kPartnersWindowMs || !g_layout.ok || g_layout.buffer[0] != buffer_w ||
                    g_layout.buffer[1] != buffer_h) {
                    return false;
                }
                if (messages) {
                    g_messages_ms = now;
                }
                l       = g_layout;
                opening = g_opening;
                start   = g_opening_start_ms;
            }
            if (!Wanted()) {
                return false;
            }
            if (messages) {
                viewport->left   = l.box[1][0];
                viewport->top    = l.box[1][1];
                viewport->width  = l.box[1][2];
                viewport->height = l.box[1][3];
                LogShownPart(movie, 1, opening, now - start);
            } else {
                // (The HUD: drawn nowhere, left of the layer, as the conversation's movie behind you (fix144): its parts would only have been
                // cleared, or caught in the console's crops.)
                viewport->left   = -4 * buffer_w;
                viewport->top    = 0;
                viewport->width  = buffer_w / 2;
                viewport->height = buffer_w * 9 / 32;
            }
            return true;
        }
        // Build mode's menu. A new opening: a new movie, or none given a viewport for a second. Where the game lays it out: at a new movie's
        // first viewport and when the layer's size changes (the review: only then is its SetSafeRect called). There it is always given the
        // console's box while the console can be used at all (whatever is up a moment: a menu, a conversation, the panel), so it is laid out
        // for it; a frame drawn there that is not cut shows it once, high in your view.
        const auto&    bb        = vr->get_backbuffer_size();
        const uint32_t backbuffer[2]{ bb[0], bb[1] };
        const float    hud_scale = GameFlow::gStore.hudSettings.hudScale;
        const bool     enabled   = Enabled();
        // ((fix178) The messages arranged below your view's middle: the console's notifications' part follows it.)
        MessagesBlock::Plan block_plan{};
        const bool          block = BlockNow(block_plan);
        bool           fresh = false, layout_event = false, in_box = false, made = false;
        Layout         l{};
        uint32_t       opening = 0;
        int64_t        start   = 0;
        {
            std::scoped_lock _{ g_mtx };
            fresh        = movie != g_menu_movie || now - g_menu_call_ms > kNewAfterMs;
            layout_event = fresh || buffer_w != g_menu_buffer[0] || buffer_h != g_menu_buffer[1];
            if (fresh) {
                g_opening_start_ms = now;
                g_clock_start_ms   = now;
                ++g_opening;
                g_snapped        = false;
                g_opening_frames = 0;
                g_frames_unshown = 0;
                g_shown_seen_ms  = 0;
            }
            g_menu_movie     = movie;
            g_menu_buffer[0] = buffer_w;
            g_menu_buffer[1] = buffer_h;
            g_menu_call_ms   = now;
            if (layout_event) {
                g_menu_in_box = enabled;
            }
            in_box = g_menu_in_box;
            if (enabled && (g_layout.buffer[0] != buffer_w || g_layout.buffer[1] != buffer_h || g_layout.backbuffer[0] != backbuffer[0] ||
                               g_layout.backbuffer[1] != backbuffer[1] || g_layout.hud_scale != hud_scale || !SameBlock(g_layout, block, block_plan))) {
                g_layout = MakeLayout(buffer_w, buffer_h, backbuffer, hud_scale, block ? &block_plan : nullptr);
                made     = true;
            }
            l       = g_layout;
            opening = g_opening;
            start   = g_opening_start_ms;
        }
        if (fresh) {
            g_fallback     = false;
            g_opening_cuts = 0;
            g_problem      = "none seen";
            g_layout_wrong_since = 0;
            g_layout_known = false;
        }
        if (const int n = made ? g_layout_logs.fetch_add(1) : 99; n < 8) {
            if (l.ok) {
                spdlog::info("[Workshop] (fix177) build console: the build menu in a 16:9 box {}x{} at {},{} of the {}x{} interface layer ({:.2f} px per "
                             "stage unit); its notifications (HUDMessagesMenu) in the HUD's own box {}x{} (the HUD's scale {:.2f} of the {}x{} eye picture), "
                             "moved to {},{} ({:.2f} px per unit); the HUD (HUDMenu) not drawn meanwhile; its {} parts packed in a {}x{} picture ({:.1f} MB)",
                    l.box[0][2], l.box[0][3], l.box[0][0], l.box[0][1], buffer_w, buffer_h, l.scale[0], l.box[1][2], l.box[1][3], hud_scale, backbuffer[0],
                    backbuffer[1], l.box[1][0], l.box[1][1], l.scale[1], kPanelCount, l.size[0], l.size[1],
                    l.size[0] * static_cast<double>(l.size[1]) * 4.0 / 1048576.0);
                for (int i = 0; i < kPanelCount && n < 2; ++i) {
                    if (!l.enabled[i]) {
                        continue;
                    }
                    const auto& p = kPanels[i];
                    const auto& b = l.from[i];
                    spdlog::info("[Workshop] (fix177)   {}: the {}'s stage {:.0f},{:.0f}-{:.0f},{:.0f} = the layer's {},{}-{},{}, at {},{} of the picture", p.name,
                        p.movie == Movie::Workshop ? "build menu" : "notifications' movie", l.crop[i][0], l.crop[i][1], l.crop[i][2], l.crop[i][3], b.left,
                        b.top, b.right, b.bottom, l.to[i][0], l.to[i][1]);
                }
            } else {
                spdlog::info("[Workshop] (fix177) build console: the {}x{} interface layer is too short for the build menu's box and the HUD's "
                             "below it (the HUD's scale {:.2f} of {}x{}): build mode in a 16:9 box only here (not cut)",
                    buffer_w, buffer_h, hud_scale, backbuffer[0], backbuffer[1]);
            }
        }
        // Where the console's box is for this layer (or, too short for both boxes, a 16:9 box as wide as it: laid out as on the console).
        const auto put_in_box = [&]() {
            if (l.ok && l.buffer[0] == buffer_w && l.buffer[1] == buffer_h) {
                viewport->left   = l.box[0][0];
                viewport->top    = l.box[0][1];
                viewport->width  = l.box[0][2];
                viewport->height = l.box[0][3];
                return true;
            }
            const int h      = std::min(buffer_h, static_cast<int>(std::lround(static_cast<float>(buffer_w) * kStageH / kStageW)));
            viewport->left   = 0;
            viewport->top    = (buffer_h - h) / 2;
            viewport->width  = buffer_w;
            viewport->height = h;
            return false;
        };
        if (!enabled) {
            return false;
        }
        if (layout_event && fresh && g_opening_logs.fetch_add(1) < 40) {
            spdlog::info("[Workshop] (fix177) build console: an opening; the build menu laid out in the console's 16:9 box ({}x{} layer{})", buffer_w,
                buffer_h, Wanted() ? "" : "; not shown yet: a menu, a conversation or the menu panel up");
        }
        if (!Wanted()) {
            // (Up a moment: build mode where it was, its parts as laid out; at a layout, the console's box all the same.)
            if (layout_event) {
                put_in_box();
                return true;
            }
            return false;
        }
        if (!in_box) {
            // (Laid out for the view's box at its last layout (the setting turned on while building, say): its parts would be off the
            // console's box: build mode in the view for this opening. Not counted toward stopping.)
            if (!g_fallback.exchange(true) && g_opening_logs.fetch_add(1) < 40) {
                spdlog::info("[Workshop] (fix177) build console: the build menu was laid out for the view's box, not the console's (the console "
                             "could not be used as it was laid out): build mode in the view for this opening");
            }
            return false;
        }
        if (g_fallback.load()) {
            return false;
        }
        // (The review) Laid out for another box after all (MenuClean reads its list's place: well above the stage's top for a taller box) for
        // a moment: build mode in the view for this opening, not counted toward stopping.
        if (const int64_t wrong = g_layout_wrong_since.load(); wrong != 0 && now - wrong > 200 && now - g_layout_ms.load() < 500) {
            g_fallback = true;
            if (g_opening_logs.fetch_add(1) < 40) {
                spdlog::warn("[Workshop] (fix177) build console: the build menu's parts are laid out for a taller box (its list at y {:.0f}, 46 "
                             "expected): build mode in the view for this opening",
                    g_layout_y.load());
            }
            return false;
        }
        if (!l.ok) {
            g_problem = "the interface layer too short for both boxes";
            put_in_box();
            return true;
        }
        // Not shown on the console: only an opening never shown counts (0.8 s and two dozen frames in, while the headset draws and no fade is
        // up); one shown that stops being shown for 240 frames goes back in the view too (not counted toward stopping). A hitch (no frames),
        // the headset not drawing (shouldRender false), a fade, a menu a moment over it: none of these.
        if (const auto xr = vr->m_openxr; xr != nullptr) {
            const bool    drawing  = xr->frame_state.shouldRender == XR_TRUE && now - g_overlay_ms.load() > 150;
            const int64_t shown_ms = xr->console_shown_ms.load();
            bool          give_up  = false;
            bool          counted  = false;
            int           frames   = 0;
            {
                std::scoped_lock _{ g_mtx };
                if (!drawing) {
                    g_clock_start_ms = now; // (its 0.8 s counted from when the headset draws again)
                    g_frames_unshown = 0;
                } else {
                    ++g_opening_frames;
                    if (shown_ms != g_shown_seen_ms) {
                        g_shown_seen_ms  = shown_ms;
                        g_frames_unshown = 0;
                    } else {
                        ++g_frames_unshown;
                    }
                }
                frames = g_opening_frames;
                if (drawing && shown_ms < start && now - g_clock_start_ms > kShownWithinMs && frames > 24) {
                    give_up = counted = true;
                } else if (drawing && shown_ms >= start && g_frames_unshown > 240) {
                    give_up = true;
                }
            }
            if (give_up) {
                g_fallback  = true;
                const int n = counted ? ++g_fallbacks : g_fallbacks.load();
                spdlog::warn("[Workshop] (fix177) build console {} (cut {} times, its images {}, {} frames placed; the last problem seen: {}): build "
                             "mode in the view as before (fix175's way) for this opening{}",
                    counted ? std::format("never shown {} ms into this opening ({} ms while the headset drew)", now - start, now - g_clock_start_ms)
                            : std::string{ "shown, then not for 240 frames" },
                    g_opening_cuts.load(), xr->console_swapchain != XR_NULL_HANDLE ? "made" : "not made", frames, g_problem.load(),
                    counted && n >= kFallbacksToStop ? "; from now on for every opening" : "");
                if (counted && n >= kFallbacksToStop) {
                    g_stopped = true;
                }
                return false;
            }
        }
        put_in_box();
        {
            std::scoped_lock _{ g_mtx };
            g_workshop_ms = now;
        }
        g_workshop_seen_ms.store(now, std::memory_order_relaxed);
        LogShownPart(movie, 0, opening, now - start);
        return true;
    }

    bool CutWanted()
    {
        int64_t ms = 0;
        {
            std::scoped_lock _{ g_mtx };
            ms = g_workshop_ms;
        }
        // (Not while the game's fader is drawn inside the boxes: its fade shown whole.)
        const int64_t now = NowMs();
        return ms != 0 && now - ms <= kPartnersWindowMs && now - g_overlay_ms.load() > 100 && !g_fallback.load() && Wanted();
    }

    bool CutBoxes(uint32_t layer_w, uint32_t layer_h, vrmod::D3D12Component::ConsoleCut& cut)
    {
        Layout  l{};
        int64_t ms = 0;
        {
            std::scoped_lock _{ g_mtx };
            l  = g_layout;
            ms = g_workshop_ms;
        }
        if (!l.ok || ms == 0 || NowMs() - ms > kPartnersWindowMs || g_fallback.load() || !Wanted()) {
            return false;
        }
        if (static_cast<uint32_t>(l.buffer[0]) != layer_w || static_cast<uint32_t>(l.buffer[1]) != layer_h) {
            static int told = 0;
            g_problem = "the interface layer not the size the build menu was placed for";
            if (told++ < 5) {
                spdlog::info("[Workshop] (fix177) the interface layer is {}x{}, build mode was placed for {}x{}: not cut this time", layer_w, layer_h,
                    l.buffer[0], l.buffer[1]);
            }
            return false;
        }
        cut       = vrmod::D3D12Component::ConsoleCut{};
        cut.size[0] = l.size[0];
        cut.size[1] = l.size[1];
        cut.parts   = 0;
        for (int i = 0; i < kPanelCount; ++i) {
            if (!l.enabled[i]) {
                continue; // ((fix178) a part this layout leaves out)
            }
            cut.from[cut.parts]  = l.from[i];
            cut.to[cut.parts][0] = l.to[i][0];
            cut.to[cut.parts][1] = l.to[i][1];
            ++cut.parts;
        }
        cut.clears = 2;
        for (int m = 0; m < 2; ++m) {
            cut.clear[m] = D3D12_RECT{ l.box[m][0], l.box[m][1], l.box[m][0] + l.box[m][2], l.box[m][1] + l.box[m][3] };
        }
        // (This opening's picture saved for the log once, a while into it.)
        {
            std::scoped_lock _{ g_mtx };
            if (!g_snapped && NowMs() - g_opening_start_ms > kSnapAfterMs) {
                g_snapped = true;
                cut.snap  = true;
            }
        }
        return true;
    }

    void NoteCut(int result)
    {
        if (result > 0) {
            g_refusals = 0;
            g_cuts.fetch_add(1, std::memory_order_relaxed);
            g_opening_cuts.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (result < 0) {
            g_problem = "the cut refused (the interface layer not found, or not one it can be done with)";
            if (++g_refusals >= kRefusalsToStop && !g_stopped.exchange(true)) {
                spdlog::warn("[Workshop] (fix177) the build menu could not be cut out of the interface layer {} times: build mode stays in the view "
                             "(fix175's way) until the game restarts",
                    kRefusalsToStop);
            }
        }
    }

    void Update()
    {
        static auto   vr  = VR::get();
        const int64_t now = NowMs();
        Layout        l{};
        int64_t       ms = 0;
        uint32_t      opening = 0;
        {
            std::scoped_lock _{ g_mtx };
            l       = g_layout;
            ms      = g_workshop_ms;
            opening = g_opening;
        }
        const auto xr     = vr->m_openxr; // (a copy: it may be made anew on another thread)
        auto&      r      = g_room;
        const bool active = Wanted() && xr != nullptr && l.ok && ms != 0 && now - ms <= 200 && !g_fallback.load();
        if (!active) {
            // (Closed, or back in the view: what it showed, for the log.)
            if (r.placed && (now - ms > kNewAfterMs || g_fallback.load() || !Wanted())) {
                r.placed = false;
                if (g_close_logs < 40) {
                    ++g_close_logs;
                    const uint32_t dropped = xr != nullptr ? xr->console_dropped.load() : 0;
                    spdlog::info("[Workshop] (fix177) build console closed ({:.1f} s, shown {} frames, put again in front of you {} times; your view on "
                                 "the item card {:.1f} s, the list {:.1f} s, the hints {:.1f} s, the footer {:.1f} s; the action or choice card up {:.1f} s; "
                                 "cut {} times in all; quads left out for the layer limit {})",
                        r.open_s, r.frames, r.moves, r.looked[0], r.looked[kListPanel], r.looked[kHintsPanel], r.looked[kFooterLeft] + r.looked[kFooterRight],
                        r.cards_up, g_cuts.load(), dropped);
                }
            }
            return;
        }
        // Your eyes and the way your head faces (stage, y up).
        const glm::mat4 head    = tracking::HeadPose();
        const glm::vec3 eyes{ head[3] };
        const glm::vec3 forward = -glm::normalize(glm::vec3{ head[2] });
        if (!std::isfinite(eyes.x) || !std::isfinite(forward.x)) {
            return;
        }
        glm::vec3 level{ forward.x, 0.0f, forward.z };
        level                  = glm::length(level) > 1e-3f ? glm::normalize(level) : glm::vec3{ 0.0f, 0.0f, -1.0f };
        const glm::vec3 origin = tracking::StandingOriginPosition();
        const int64_t   nus    = NowUs();
        const float     dt     = r.last_us != 0 ? std::clamp(static_cast<float>(nus - r.last_us) * 1e-6f, 0.0f, 0.1f) : 0.0f;
        r.last_us              = nus;
        // Put in front of you: as an opening starts; again when the view was recentred or your head moved far from where it was put (stood
        // up, a step). Not as your head turns (it stays where it is in the room), nor on stick turns (the room turns round you, and the
        // console with you), nor as you walk (the room comes with you).
        const char* why = nullptr;
        if (!r.placed || r.opening != opening) {
            r          = Room{};
            r.placed   = true;
            r.opening  = opening;
            r.since_ms = now;
            r.last_us  = nus;
            r.eyes     = eyes;
            r.forward  = level;
            r.origin   = origin;
        } else if (glm::length(origin - r.origin) > 0.01f) {
            why = "the view was recentred";
        } else if (glm::length(glm::vec2{ eyes.x - r.eyes.x, eyes.z - r.eyes.z }) > kHeadMovedM || std::abs(eyes.y - r.eyes.y) > kHeadMovedM) {
            why = "your head moved far from where it was put";
        }
        if (why != nullptr) {
            r.eyes    = eyes;
            r.forward = level;
            r.origin  = origin;
            r.logged  = false; // (its new places logged)
            ++r.moves;
            if (g_move_logs < 30) {
                ++g_move_logs;
                spdlog::info("[Workshop] (fix177) build console put again in front of you ({})", why);
            }
        }
        r.open_s += dt;
        ++r.frames;
        bool       cards_known = false;
        const bool cards       = CardsUp(cards_known);
        if (cards_known && cards) {
            r.cards_up += dt;
        }
        const float     hud_tan = HudTanPerUnit(l);
        const glm::vec3 up{ 0.0f, 1.0f, 0.0f };
        runtimes::OpenXR::Console c{};
        c.ms = now;
        std::string placed_log{};
        // (fix178) The column low in the middle: its rows one under another from here down (degrees from your eyes' level), this far apart.
        float column_top = kColumnTopDeg;
        // (fix178) The hints' bar shown (MenuClean, read lately): the hints panel trimmed to it.
        const bool  hints_fresh = g_hints_known.load() && now - g_hints_ms.load() < 500;
        const bool  hints_any   = !hints_fresh || g_hints_any.load();
        const float hints_left = g_hints_left.load(), hints_right = g_hints_right.load();
        for (int i = 0; i < kPanelCount; ++i) {
            const auto& p = kPanels[i];
            // (The cards only while one is up; the list's left edge at the cards' right one meanwhile. A part this layout leaves out: none.)
            if ((p.centre_card && !cards) || !l.enabled[i]) {
                continue;
            }
            const float u0c = l.crop[i][0], v0c = l.crop[i][1], u1c = l.crop[i][2], v1c = l.crop[i][3];
            const bool  room      = p.where == Where::Room;
            const bool  hud_place = l.hud_place[i];
            const bool  column    = p.column >= 0;
            const float distance  = room ? kRoomM : kHeadM;
            const float tan_unit  = p.hud_scale ? hud_tan : std::tan(glm::radians(kTextDeg / kTextUnits)); // (its size in view, per stage unit)
            const float mpu       = distance * tan_unit;                                                     // (metres per stage unit)
            // The part shown: the list trimmed while a card is up; the hints to the bar shown (none shown: no hints panel, its row kept).
            float u0 = u0c, u1 = u1c;
            if (i == kListPanel && cards && cards_known) {
                u0 = std::max(u0, kListTrimU0);
            }
            const float h_m = (v1c - v0c) * mpu;
            const float h_deg = glm::degrees(2.0f * std::atan(h_m * 0.5f / distance));
            float       row_top = column_top;
            if (column) {
                column_top -= h_deg + kColumnGapDeg;
            }
            if (i == kHintsPanel && hints_fresh) {
                if (!hints_any) {
                    continue;
                }
                u0 = std::clamp(hints_left - 6.0f, u0c, u1c - 20.0f);
                u1 = std::clamp(hints_right + 10.0f, u0 + 20.0f, u1c);
            }
            // Its plane: the crop's middle `distance` along its direction, facing the eyes. (Where the HUD has it: the crop's middle as seen in
            // the HUD's box in play, from the stage's middle. (fix178) A column row: the part shown, centred straight ahead, at its row.)
            float az = p.az, el_c = p.top ? p.el - h_deg * 0.5f : p.el;
            if (hud_place) {
                az   = glm::degrees(std::atan(((u0c + u1c) * 0.5f - kStageW * 0.5f) * hud_tan));
                el_c = glm::degrees(std::atan((kStageH * 0.5f - (v0c + v1c) * 0.5f) * hud_tan));
            }
            if (column) {
                az   = 0.0f;
                el_c = row_top - h_deg * 0.5f;
            }
            const glm::vec3 base  = room ? r.eyes : glm::vec3{ 0.0f };
            const glm::vec3 ahead = room ? r.forward : glm::vec3{ 0.0f, 0.0f, -1.0f };
            const glm::vec3 at    = base + Direction(ahead, up, az, el_c) * distance;
            const glm::quat turn  = Facing(at, base, up);
            // Its offset on that plane (none for a column row: the part shown is what is centred), and its pixels in the picture.
            const float     du   = column ? 0.0f : ((u0 + u1) * 0.5f - (u0c + u1c) * 0.5f) * mpu;
            const glm::vec3 xs   = turn * glm::vec3{ 1.0f, 0.0f, 0.0f };
            const glm::vec3 pos  = at + xs * du;
            const int       m    = p.movie == Movie::Workshop ? 0 : 1;
            const float     s    = l.scale[m];
            const auto&     b    = l.from[i];
            const int       full = static_cast<int>(b.right - b.left);
            const int       skip = std::clamp(static_cast<int>(std::lround((u0 - u0c) * s)), 0, full - 1);
            const int       keep = std::clamp(static_cast<int>(std::lround((u1 - u0) * s)), 1, full - skip);
            auto&           q    = c.quads[c.count++];
            q.space              = room ? 0 : 1;
            q.pose.orientation   = { turn.x, turn.y, turn.z, turn.w };
            q.pose.position      = { pos.x, pos.y, pos.z };
            q.size[0]            = static_cast<float>(keep) / s * mpu;
            q.size[1]            = h_m;
            q.rect[0]            = static_cast<int32_t>(l.to[i][0]) + skip;
            q.rect[1]            = static_cast<int32_t>(l.to[i][1]);
            q.rect[2]            = keep;
            q.rect[3]            = static_cast<int32_t>(b.bottom - b.top);
            // Your view on it (the room's: for the log as it closes).
            if (room) {
                const glm::vec3 n     = turn * glm::vec3{ 0.0f, 0.0f, 1.0f };
                const float     along = glm::dot(forward, n);
                if (along < -1e-3f) {
                    const float     t   = glm::dot(pos - eyes, n) / along;
                    const glm::vec3 hit = eyes + forward * t;
                    const glm::vec3 lq  = glm::inverse(turn) * (hit - pos);
                    if (t > 0.0f && std::abs(lq.x) <= q.size[0] * 0.5f && std::abs(lq.y) <= q.size[1] * 0.5f) {
                        r.looked[i] += dt;
                    }
                }
            }
            if (!r.logged) {
                const glm::vec3 d    = room ? pos - r.eyes : pos;
                const glm::vec3 a    = room ? r.forward : glm::vec3{ 0.0f, 0.0f, -1.0f };
                const glm::vec3 side = glm::normalize(glm::cross(a, up));
                const float     len  = std::max(glm::length(d), 1e-4f);
                const float     el   = glm::degrees(std::asin(std::clamp(d.y / len, -1.0f, 1.0f)));
                const float     side_deg = glm::degrees(std::atan2(glm::dot(d, side), glm::dot(d, a)));
                placed_log += std::format("; {} (stage {:.0f},{:.0f}-{:.0f},{:.0f}): {:.2f} x {:.2f} m, its middle {:.0f} degrees {}, {:.0f} degrees {} {}, "
                                          "{:.2f} m away ({}), its {:.0f}-unit text {:.2f} degrees tall",
                    p.name, u0c, v0c, u1c, v1c, q.size[0], q.size[1], std::abs(side_deg), side_deg >= 0.0f ? "right" : "left", std::abs(el),
                    el >= 0.0f ? "above" : "below", room ? "your eyes' level" : "your view's middle", len, room ? "in the room" : "with your head",
                    kTextUnits, glm::degrees(std::atan(kTextUnits * tan_unit)));
            }
        }
        if (!r.logged) {
            r.logged = true;
        }
        if (!placed_log.empty() && g_place_logs < 40) {
            ++g_place_logs;
            spdlog::info("[Workshop] (fix177) build console put in front of you (your eyes {:.2f} m up; the action cards {}){}", eyes.y,
                cards_known ? "read from the menu" : "not read yet: shown always", placed_log);
        }
        xr->set_console(c);
    }

    std::string Status()
    {
        return std::format("Build mode as a console: {}; cut {} times, back in the view {} times", g_stopped.load() ? "stopped (not shown, or refused)"
                                                                                                                  : (GameFlow::gStore.internalSettings.buildConsole ? "on" : "off"),
            g_cuts.load(), g_fallbacks.load());
    }
}

#include "HandActions.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include <glm/gtc/matrix_transform.hpp>
#include <mods/VR.hpp>
#include <spdlog/spdlog.h>

#include <ModSettings.h>

#include "Climbing.h"
#include "CreationEngineSingletonManager.h"
#include "FavoritesWheel.h"
#include "HandGrab.h"
#include "LeftWeapon.h"
#include "Haptics.h"
#include "MeleeThrow.h"
#include "Physical.h"
#include "NodeTreeDump.h"
#include "body/BodyIK.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"

// Stage space throughout (OpenXR: metres, +Y up, the headset looking along -Z), hands relative to the head so leaning or
// moving the chair does not count as a hand moving.
namespace HandActions
{
    namespace
    {
        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool SafeRead(void* dst, uintptr_t src, size_t n)
        {
            SIZE_T done = 0;
            return src != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &done) && done == n;
        }

        bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        // Grips as the hand grab reads them (the same physical controller, the same thresholds).
        constexpr float kGripOn  = 0.75f;
        constexpr float kGripOff = 0.40f;
        // A push is made with the grip held and the palm facing out, away from you, as in a Force push (the wrist bent back).
        // Reaching for something, pointing the gravity gloves or punching keeps the palm sideways or down, so none of them is
        // a push. The palm's way is the grip pose's: its +X leaves the palm of a left hand and the back of a right hand.
        constexpr float kPalmOutDot  = 0.34f; // held for a push: the palm within 70 degrees of where you face (level)
        // (fix175) After fix174 powers went off with the left temple + left grip and as the scanner opened; the log: every one a
        // palm push, some with the palm 55-57 degrees off the push (deliberate pushes: 29-32), one 0.1 s after the grip was
        // squeezed to grab, one as the hand came away after the favorites wheel closed. Now: within 40 degrees (was 60); the grip held 0.3 s
        // before the push starts; the push starting at least 0.3 m from your eyes (not from the temples); none for 1 s after the wheel.
        // (fix184) The grip held first no more: powers stopped working with grip + push (the logs since fix175: 28
        // pushes refused for "the grip squeezed under 0.3 s before it", no power at all: the grip is squeezed as the hand pushes). The grip
        // squeezed with the push is enough; only one squeezed well after the push began (the push mostly made already) is not a power.
        constexpr float   kPalmPushDot      = 0.766f;  // and at the push, within 40 degrees of the way the hand pushed
        constexpr int64_t kPushGripLateUs   = 250'000; // (fix184) the grip squeezed at most this long after the push started
        constexpr float   kPushFromHeadMin  = 0.30f;   // the push starts at least this far from your eyes
        constexpr int64_t kPushAfterWheelMs = 1000;    // no push this soon after the favorites wheel closed

        // ---- Powers: a grip held and the hand pushed out ----
        constexpr int64_t kPushWindowUs   = 350'000; // the push happens within this long
        constexpr float   kPushDistance   = 0.22f;   // the hand moves at least this far (relative to the head)...
        constexpr float   kPushForwardDot = 0.34f;   // ...within 70 degrees of where you face (level)...
        constexpr float   kPushReach      = 0.25f;   // ...and ends at least this far in front of your eyes
        constexpr int64_t kPowerPressUs   = 200'000; // LB + RB held this long
        constexpr int64_t kPowerAimUs     = 700'000; // the aim follows the push this long (the cast comes a moment after the press)
        constexpr int64_t kPowerCooldownUs = 1'000'000;
        constexpr int64_t kAfterThrowUs   = 600'000; // no power from the arms swinging out with a grenade throw

        // ---- Grenades: behind the right shoulder (HIGGS: 25 cm right, 7 back, 10 down, 16 cm across; a slow hand) ----
        constexpr float   kShoulderRight  = 0.20f;
        constexpr float   kShoulderDown   = 0.10f;
        constexpr float   kShoulderBack   = 0.10f;
        constexpr float   kShoulderRadius = 0.18f;
        constexpr float   kShoulderFront  = 0.04f;  // the hand must be at least this far behind your eyes (not at the headset's side)
        constexpr float   kShoulderLeave  = 0.06f;  // leaving the zone by this much readies the grenade
        constexpr float   kTakeSpeed      = 2.0f;   // m/s: a hand faster than this is passing by, not reaching back
        constexpr int64_t kMinHoldUs      = 250'000; // the grenade button is held at least this long
        constexpr int64_t kButtonQuietUs  = 150'000; // and stays up this long after (its release reaches the game)
        constexpr int64_t kThrowWaitUs    = 2'500'000; // the game's throw comes within this long after letting go
        constexpr float   kThrowMinSpeed  = 1.0f;   // slower: the game's own direction is kept
        constexpr int     kLaunchLogs     = 3;      // launch data written to the log for the first few throws (for the next builds)
        // (fix189) The throw's speeds as the hand grab's: none faster than any hand moves (a tracking glitch's speed, the fastest of the
        // last four, sent a grenade its way), and no speed from a jump of over 0.3 m between two poses at more than that.
        constexpr float   kMaxHandSpeed   = 15.0f;  // m/s
        constexpr float   kPoseJump       = 0.30f;  // m

        struct Sample
        {
            int64_t   us{ 0 };
            int64_t   arrived{ 0 }; // when the hand's pose in this sample first came (in stereo a new pose every other frame)
            glm::vec3 rel{ 0.0f }; // the hand's grip point minus the head
            bool      grip{ false }; // held for a push: the grip squeezed, the trigger let go
            bool      valid{ false };
        };
        // (fix189) What a push that made no power came closest to (for the log): the longest palm-out move out in front.
        struct NearPush
        {
            float distance{ 0.0f };
            float peak{ 0.0f };
        };
        constexpr float kNearPushDistance = 0.12f; // a move this long counts as a try, logged if it was too short or too slow
        constexpr int kSamples = 64;

        struct HandState
        {
            Sample  samples[kSamples]{};
            int     next{ 0 };
            bool    grip{ false };
            int64_t grip_since{ 0 };    // (fix175) when the grip was squeezed
            int64_t reject_until{ 0 };  // (fix175) a push just rejected: not looked at again until this
            bool    push_grip{ false }; // the grip held with the palm facing out
            NearPush near_push{};       // (fix189) this squeeze's closest try so far (0 m: none), told as the grip or palm leaves the push
            glm::vec3 palm{ 0.0f };     // the way the palm faces (out of it), stage space
            int64_t last_push{ 0 };
            glm::vec3 velocity{ 0.0f };     // the controller's, stage space
            glm::vec3 recent[4]{};          // the last few velocities, for the throw
            int       recent_next{ 0 };
            glm::vec3 last_at{ 0.0f };      // for a speed from the poses when the runtime gives none
            int64_t   last_at_us{ 0 };
            glm::vec3 pose_velocity{ 0.0f };
        };
        HandState g_hands[2]{};

        // Powers.
        std::atomic<int64_t> g_power_until{ 0 };
        std::mutex           g_aim_mutex;
        glm::mat4            g_push_aim{ 1.0f };
        int64_t              g_push_aim_until{ 0 };
        std::atomic<int>     g_pushes{ 0 };

        // Grenades.
        enum class Nade { Idle, Taken, Readied };
        Nade                 g_nade{ Nade::Idle };
        std::atomic<bool>    g_nade_active{ false };  // taken or readied (for other threads)
        std::atomic<bool>    g_zone_ready{ false };   // the right hand is in the zone and could take one now
        std::atomic<bool>    g_rb_down{ false };
        std::atomic<int64_t> g_rb_since{ 0 };
        std::atomic<int64_t> g_rb_until{ 0 };
        std::atomic<int64_t> g_thrown_us{ 0 };      // when the last grenade left the hand
        std::atomic<int64_t> g_pending_since{ 0 };  // a thrown grenade the game has not launched yet: since when (0: none)
        std::atomic<int>     g_takes{ 0 };
        std::atomic<int>     g_throws{ 0 };
        std::atomic<int>     g_redirected{ 0 };
        bool                 g_told_none{ false };

        // (fix173) The grips as LB and RB in the hand scanner and the star map (ModConfig taps them): each grip squeezed there, until let go;
        // when ModConfig last tapped for each (its grip read as the runtime's button, which may come a little before or after this one's).
        std::atomic<bool>    g_grip_shoulder[2]{ false, false };
        std::atomic<int64_t> g_grip_tap_us[2]{ 0, 0 };
        std::atomic<bool>    g_scanner_closing{ false }; // an LB went to the game in the scanner: closing it
        constexpr int        kModeWithinMs  = 150;       // the scanner or the star map drawn this lately: the grips are LB and RB
        constexpr int64_t    kTapLatchUs    = 300'000;   // a grip squeezed this soon after ModConfig's tap for it is that tap's
        int                  g_grip_shoulder_logs{ 0 };
        int                  g_scanner_power_logs{ 0 };
        int                  g_not_push_logs{ 0 }; // (fix175)
        int                  g_near_push_logs{ 0 }; // (fix189)

        std::mutex g_throw_mutex;
        struct Throw
        {
            int64_t   us{ 0 };
            glm::vec3 stage_velocity{ 0.0f };
            glm::vec3 stage_point{ 0.0f };
            bool      pending{ false };
            int       logs{ 0 };       // launch data written to the log: grenades...
            int       other_logs{ 0 }; // ...and other launches while one was awaited (to compare)
        } g_throw{};

        int ControllerIndex(int hand)
        {
            static auto vr = VR::get();
            return hand == 0 ? vr->get_left_controller_index() : vr->get_right_controller_index();
        }

        // Where you face, level (stage space).
        glm::vec3 LevelForward(const glm::mat4& head)
        {
            glm::vec3 f = -glm::vec3{ head[2] };
            f.y         = 0.0f;
            const float length = glm::length(f);
            return length > 1e-3f ? f / length : glm::vec3{ 0.0f, 0.0f, -1.0f };
        }

        // An aim rotation (as tracking's: columns right, up, back) looking along `forward`, kept upright.
        glm::mat4 LookAlong(const glm::vec3& forward)
        {
            const glm::vec3 back = -glm::normalize(forward);
            glm::vec3       up{ 0.0f, 1.0f, 0.0f };
            glm::vec3       right = glm::cross(up, back);
            if (glm::length(right) < 1e-3f) {
                right = glm::vec3{ 1.0f, 0.0f, 0.0f };
            }
            right = glm::normalize(right);
            up    = glm::cross(back, right);
            glm::mat4 out{ 1.0f };
            out[0] = glm::vec4{ right, 0.0f };
            out[1] = glm::vec4{ up, 0.0f };
            out[2] = glm::vec4{ back, 0.0f };
            return out;
        }

        // Somewhere a hand can act: in first person with the headset and controllers, no menu, conversation, pilot's seat or
        // flat screen. (fix175: nor build mode, no menu by the count since fix175: no palm power or grenade while building.)
        bool InPlay()
        {
            static auto vr = VR::get();
            return vr->is_hmd_active() && vr->is_using_controllers() && GameFlow::isInFirstPerson() && !GameFlow::isShowingMenu() &&
                   !GameFlow::isWorkshopShown() && !GameFlow::isInDialogue() && !GameFlow::isPilotingShip() && !ModSettings::showFlatScreenDisplay() &&
                   CreationEngineSingletonManager::GetPlayerRef() != nullptr;
        }

        // A push out with a held grip in this hand's recent samples: the direction it pushed (true), relative to the head. (fix175) Also
        // where and when it started (`start_rel`, `start_us`).
        // (fix189) `closest` (none found): the longest move of 0.12 m or more since the squeeze, too short or too slow, for the log. (A review
        // found the push counts only from the squeeze, so a squeeze partway through a push leaves less than kPushDistance after it, and
        // fix184's refusal of a grip squeezed well after the push began never fires. Left as fix184 has it until fix184 has been tried;
        // the log now says when a try came close.)
        bool FindPush(const HandState& h, const glm::vec3& forward, float min_speed, glm::vec3& direction, float& distance, float& peak, glm::vec3& start_rel,
            int64_t& start_us, NearPush& closest)
        {
            closest              = NearPush{};
            const int     newest = (h.next + kSamples - 1) % kSamples;
            const Sample& now    = h.samples[newest];
            if (!now.valid || !now.grip || glm::dot(now.rel, forward) < kPushReach) {
                return false;
            }
            for (int back = 1; back < kSamples; ++back) {
                const Sample& start = h.samples[(newest - back + kSamples) % kSamples];
                if (!start.valid || !start.grip || now.us - start.us > kPushWindowUs) {
                    break; // the grip was not held from there on, or too long ago
                }
                const glm::vec3 moved  = now.rel - start.rel;
                const float     length = glm::length(moved);
                if (length < kNearPushDistance) {
                    continue;
                }
                const glm::vec3 along = moved / length;
                if (glm::dot(along, forward) < kPushForwardDot) {
                    continue;
                }
                // Its fastest moment along the push, from one new pose to the next (in stereo the poses are new every other frame:
                // a frame repeating the last pose does not count, and each move is over the time between the poses' arrivals).
                float         fastest = 0.0f;
                const Sample* last    = &start;
                for (int i = back - 1; i >= 0; --i) {
                    const Sample& b = h.samples[(newest - i + kSamples) % kSamples];
                    if (b.arrived == last->arrived) {
                        continue;
                    }
                    const float dt = static_cast<float>(b.arrived - last->arrived) * 1e-6f;
                    if (dt > 0.002f) {
                        fastest = std::max(fastest, glm::dot(b.rel - last->rel, along) / dt);
                    }
                    last = &b;
                }
                if (length >= kPushDistance && fastest >= min_speed) {
                    direction = along;
                    distance  = length;
                    peak      = fastest;
                    start_rel = start.rel;
                    start_us  = start.us;
                    return true;
                }
                if (length > closest.distance) {
                    closest = NearPush{ length, fastest }; // (fix189) too short or too slow
                }
            }
            return false;
        }

        // Where the right shoulder's zone is and how far the hand is into it.
        struct Shoulder
        {
            float from_centre{ 1e9f };
            float back{ 0.0f }; // how far behind your eyes the hand is (negative: in front)
        };
        Shoulder RightShoulder(const glm::mat4& head, const glm::vec3& hand)
        {
            static auto     vr      = VR::get();
            const glm::vec3 forward = LevelForward(head);
            const glm::vec3 up{ 0.0f, 1.0f, 0.0f };
            // With the controllers swapped (left-handed), the hand that is "right" is the physical left one: its shoulder.
            const float     side   = vr->get_right_controller_index() == 1 ? -1.0f : 1.0f;
            const glm::vec3 right  = glm::normalize(glm::cross(forward, up)) * side;
            const glm::vec3 eyes{ head[3] };
            const glm::vec3 centre = eyes + right * kShoulderRight - up * kShoulderDown - forward * kShoulderBack;
            return Shoulder{ glm::length(hand - centre), -glm::dot(hand - eyes, forward) };
        }

        void EndGrenade(const char* why, int64_t now)
        {
            if (g_nade != Nade::Idle) {
                spdlog::info("[Hands] grenade: {}", why);
            }
            if (g_rb_down.load()) {
                // A press the game sees whole. (fix189) Not into a menu: one opening within 0.25 s of readying the grenade got the rest
                // of the hold as an RB press of its own; let go at once there.
                g_rb_until = GameFlow::isShowingMenu() ? now : std::max(now, g_rb_since.load() + kMinHoldUs);
            }
            g_nade        = Nade::Idle;
            g_nade_active = false;
            g_rb_down     = false;
        }

        // A pointer in launch data to a weapon form (type 0x30 at +0x2e) whose weapon type (its instance data +0x248, general
        // data +0x50, type +0x38, as the hand grab reads the equipped weapons) is a grenade (10) or mine (11): where it is, or
        // -1. `forms` (optional) lists every weapon form found, for the log.
        int FindThrownWeapon(const std::uint8_t* bytes, uint32_t* thrown_type, std::string* forms)
        {
            constexpr uint8_t kFormTypeWeapon = 0x30;
            int               found           = -1;
            for (size_t at = 0; at + 8 <= kLaunchBytes; at += 8) {
                uintptr_t form = 0;
                std::memcpy(&form, bytes + at, sizeof(form));
                uint8_t type = 0;
                if (form < 0x10000 || form >= 0x7FFF'FFFF'0000ull || (form & 7) != 0 || !SafeRead(&type, form + 0x2e, sizeof(type)) ||
                    type != kFormTypeWeapon) {
                    continue;
                }
                uint32_t  id = 0, weapon_type = 0xffffffffu;
                uintptr_t instance = 0, general = 0;
                SafeRead(&id, form + 0x28, sizeof(id));
                if (SafeRead(&instance, form + 0x248, sizeof(instance)) && instance != 0 && SafeRead(&general, instance + 0x50, sizeof(general)) &&
                    general != 0) {
                    SafeRead(&weapon_type, general + 0x38, sizeof(weapon_type));
                }
                if (forms != nullptr) {
                    *forms += fmt::format(" +{:x}: weapon {:08x} type {}", at, id, static_cast<int>(weapon_type));
                }
                if (found < 0 && (weapon_type == 10 || weapon_type == 11)) {
                    found = static_cast<int>(at);
                    if (thrown_type != nullptr) {
                        *thrown_type = weapon_type;
                    }
                    if (forms == nullptr) {
                        break;
                    }
                }
            }
            return found;
        }

        std::string Hex(const std::uint8_t* bytes, size_t n)
        {
            std::string out;
            for (size_t i = 0; i < n; ++i) {
                out += fmt::format("{:02x}{}", bytes[i], (i + 1) % 8 == 0 ? " " : "");
            }
            return out;
        }
    }

    void Update()
    {
        static auto vr       = VR::get();
        const auto& settings = GameFlow::gStore.internalSettings;
        const int64_t now    = NowUs();
        const glm::mat4 head = tracking::HeadPose();
        const glm::vec3 eyes{ head[3] };
        const bool      play = InPlay() && Finite(eyes);
        const glm::vec3 forward = LevelForward(head);

        // Samples: each hand's grip and where it is from the head.
        bool      pressed[2]{ false, false };
        glm::vec3 hand_at[2]{};
        for (int hand = 0; hand < 2; ++hand) {
            auto&           h    = g_hands[hand];
            const glm::mat4 grip = tracking::GripPose(hand == 0);
            hand_at[hand]        = glm::vec3{ grip[3] };
            const bool  left_pose = ControllerIndex(hand) == 1; // the physical left controller
            const float value     = vr->get_finger_state(left_pose).grip;
            const bool  down      = h.grip ? value > kGripOff : value > kGripOn;
            pressed[hand]         = down && !h.grip;
            if (pressed[hand]) {
                h.grip_since = now;
            }
            h.grip                = down;
            const glm::vec3 x{ grip[0] };
            h.palm      = glm::length(x) > 1e-4f ? glm::normalize(x) * (left_pose ? 1.0f : -1.0f) : glm::vec3{ 0.0f };
            h.push_grip = down && Finite(h.palm) && glm::dot(h.palm, forward) >= kPalmOutDot;
            // The hand's speed: the runtime's, or from the poses when it gives none (they change once per new pose).
            if (Finite(hand_at[hand]) && hand_at[hand] != h.last_at) {
                const float dt = static_cast<float>(now - h.last_at_us) * 1e-6f;
                h.pose_velocity = h.last_at_us != 0 && dt > 0.002f && dt < 0.2f ? (hand_at[hand] - h.last_at) / dt : glm::vec3{ 0.0f };
                if (glm::length(hand_at[hand] - h.last_at) > kPoseJump && glm::length(h.pose_velocity) > kMaxHandSpeed) {
                    h.pose_velocity = glm::vec3{ 0.0f }; // (fix189) a tracking jump, no hand's move
                }
                h.last_at       = hand_at[hand];
                h.last_at_us    = now;
            } else if (now - h.last_at_us > 50'000) {
                h.pose_velocity = glm::vec3{ 0.0f };
            }
            h.velocity = glm::vec3{ vr->get_velocity(static_cast<uint32_t>(ControllerIndex(hand))) };
            if (!Finite(h.velocity) || glm::length(h.velocity) < 1e-5f) {
                h.velocity = Finite(h.pose_velocity) ? h.pose_velocity : glm::vec3{ 0.0f };
            }
            if (const float fast = glm::length(h.velocity); fast > kMaxHandSpeed && std::isfinite(fast)) {
                h.velocity *= kMaxHandSpeed / fast; // (fix189)
            }
            h.recent[h.recent_next] = h.velocity;
            h.recent_next           = (h.recent_next + 1) % 4;
            Sample& s = h.samples[h.next];
            s.us      = now;
            s.arrived = h.last_at_us;
            s.rel     = hand_at[hand] - eyes;
            s.grip    = h.push_grip;
            s.valid   = play && Finite(hand_at[hand]);
            h.next    = (h.next + 1) % kSamples;
        }

        const bool hands_free = HandGrab::HandsFree(); // no weapon out, or only fists: the right hand is free too

        // ---- (fix173) The grips as LB and RB in the hand scanner and the star map ----
        // A grip squeezed there taps its button for the game (ModConfig) and is that button's until let go, even after the scanner closes (its
        // LB tap closes it): no grab, gravity gloves' pull or knockdown with it (HandGrab::LeaveGrip), no melee swing (MarkBusy), no throw
        // readied (MeleeThrow::HoldOffGrip), as the favorites wheel takes its grips; no power push or grenade either (below). Before, in the
        // scanner the left grip grabbed and threw, the right one aimed (LT) or grabbed.
        // Only a squeeze made there (a grip already held as the scanner opens keeps doing what it did; the review), or one ModConfig has just
        // tapped for (its button may cross a little before this one's threshold, as the tap's LB is already closing the scanner).
        {
            const bool shoulders = GripsAreShoulders();
            for (int hand = 0; hand < 2; ++hand) {
                const bool left   = hand == 0;
                const bool tapped = now - g_grip_tap_us[hand].load() < kTapLatchUs;
                if (!g_hands[hand].grip) {
                    g_grip_shoulder[hand] = false;
                } else if (((pressed[hand] && shoulders) || tapped) && !g_grip_shoulder[hand].load()) {
                    g_grip_shoulder[hand] = true;
                    if (g_grip_shoulder_logs < 20) {
                        ++g_grip_shoulder_logs;
                        spdlog::info("[Hands] (fix173) the {} grip squeezed in the {}: it is {} there (no grab, throw or aim with it until let go)",
                            left ? "left" : "right", GripsPlace(), left ? "LB" : "RB");
                    }
                }
                if (g_grip_shoulder[hand].load()) {
                    HandGrab::LeaveGrip(left);
                    HandGrab::MarkBusy(left);
                    MeleeThrow::HoldOffGrip(left);
                }
            }
        }

        // ---- Grenade from behind the right shoulder ----
        {
            auto&          right    = g_hands[1];
            const Shoulder shoulder = RightShoulder(head, hand_at[1]);
            const bool     in_zone  = shoulder.from_centre < kShoulderRadius && shoulder.back > kShoulderFront;
            // (A weapon you picked up and drew: behind the shoulder its grip puts it in the bag instead. A melee weapon out with
            // throwing on: that hand holds it, and a grip squeezed behind the shoulder starts an overhand throw.)
            const bool     melee_hand = MeleeThrow::Available(false);
            // (fix173: not with the right grip as RB, in the scanner and the star map.)
            const bool     can_take   = settings.shoulderGrenade && play && !HandGrab::HoldingWith(false) && HandGrab::ThrowableEquipped() &&
                                  !HandGrab::PickedWeaponInHands() && !melee_hand && !g_grip_shoulder[1].load();
            g_zone_ready            = can_take && in_zone;
            if (g_nade != Nade::Idle && !(settings.shoulderGrenade && play)) {
                EndGrenade("dropped (a menu, a conversation, or the setting turned off)", now);
            }
            switch (g_nade) {
            case Nade::Idle:
                if (pressed[1] && in_zone && settings.shoulderGrenade && play && glm::length(right.velocity) < kTakeSpeed) {
                    if (!can_take) {
                        if (!HandGrab::HoldingWith(false) && !HandGrab::PickedWeaponInHands() && !melee_hand && !g_grip_shoulder[1].load() && !g_told_none) {
                            g_told_none = true;
                            spdlog::info("[Hands] grenade: none equipped (equip one in your inventory, Throwables)");
                        }
                        break;
                    }
                    g_nade        = Nade::Taken;
                    g_nade_active = true;
                    HandGrab::LeaveGrip(false);
                    // (Test: what hangs on the hands before the game readies the grenade, to compare with while it does.)
                    static bool logged_taken = false;
                    if (!logged_taken) {
                        logged_taken = true;
                        NodeTreeDump::request_hands("a grenade taken from your shoulder, before the game readies it");
                    }
                    haptics::MeleePulse(false, 0.4f, 0.05f);
                    const int n = g_takes.fetch_add(1) + 1;
                    if (n <= 20 || n % 10 == 0) {
                        spdlog::info("[Hands] grenade taken from behind your right shoulder (bring your hand forward to ready it, let go as you throw)");
                    }
                }
                break;
            case Nade::Taken:
                if (!right.grip) {
                    EndGrenade("put back (let go at the shoulder)", now);
                    haptics::MeleePulse(false, 0.2f, 0.03f);
                } else if (shoulder.from_centre > kShoulderRadius + kShoulderLeave || shoulder.back < kShoulderFront - kShoulderLeave) {
                    g_nade     = Nade::Readied;
                    g_rb_since = now;
                    g_rb_down  = true;
                }
                break;
            case Nade::Readied:
                {
                    // (Test: where the game puts the grenade's model while it readies it: the hands' anchor points, a moment in and
                    // a little later.)
                    static int logged_ready = 0;
                    const int64_t held_for  = now - g_rb_since.load();
                    if ((logged_ready == 0 && held_for > 350'000) || (logged_ready == 1 && held_for > 900'000)) {
                        ++logged_ready;
                        NodeTreeDump::request_hands(logged_ready == 1 ? "the game readying the grenade (its button held 0.35 s)"
                                                                       : "the game readying the grenade (its button held 0.9 s)");
                    }
                }
                if (!right.grip) {
                    // The throw: the fastest of the last few hand speeds (as HIGGS throws), where the hand let go.
                    glm::vec3 fastest{ 0.0f };
                    for (const auto& v : right.recent) {
                        if (glm::length(v) > glm::length(fastest)) {
                            fastest = v;
                        }
                    }
                    {
                        std::scoped_lock _{ g_throw_mutex };
                        g_throw.us             = now;
                        g_throw.stage_velocity = fastest;
                        g_throw.stage_point    = hand_at[1];
                        g_throw.pending        = true;
                    }
                    g_pending_since = now;
                    g_rb_until    = std::max(now, g_rb_since.load() + kMinHoldUs);
                    g_rb_down     = false;
                    g_nade        = Nade::Idle;
                    g_nade_active = false;
                    g_thrown_us   = now;
                    HandGrab::MarkBusy(false);
                    const int n = g_throws.fetch_add(1) + 1;
                    if (n <= 20 || n % 10 == 0) {
                        spdlog::info("[Hands] grenade thrown at {:.1f} m/s", glm::length(fastest));
                    }
                }
                break;
            }
            if (g_nade != Nade::Idle) {
                HandGrab::MarkBusy(false); // the throw is not a melee swing
            }
        }

        // ---- Powers: a grip held and that hand pushed out ----
        // (Not while a grenade is in hand or just thrown: the arms swing out with the throw, and the power's buttons would merge
        // with the grenade's.)
        const bool grenade_busy = g_nade != Nade::Idle || PressGrenade() || now - g_thrown_us.load() < kAfterThrowUs;
        // (fix173) Nor while the hand scanner is open (powers could fire while using the scanner): the hands reach out and squeeze
        // there (the scanner's own grips now, ScannerGrips), and a power's LB + RB in the scanner is no scan. Logged the first times a push
        // would have counted.
        const bool scanner = GameFlow::isScannerOpen();
        if (settings.pushPowers && play && !grenade_busy) {
            const float min_speed = std::clamp(settings.pushPowerSpeed, 0.8f, 4.0f);
            for (int hand = 0; hand < 2; ++hand) {
                auto&      h    = g_hands[hand];
                const bool left = hand == 0;
                // Not the hand holding the weapon, something grabbed, or the gun's support grip.
                // (fix154) Nor the left hand while it holds its own weapon (dual wielding).
                // (fix179) Nor while you climb (either hand on a wall: a palm shoved at the rock is no power).
                const bool free = (left || hands_free) && !HandGrab::HoldingWith(left) && !Physical::HoldingPerson(left) && !(left && body::IsSupportHandOnGun()) && !(left && LeftWeapon::Held()) &&
                                  !Climbing::Active();
                // (fix189) A try that came close (a palm-out move of 0.12 m or more out in front, too short or too slow) and made no power:
                // told once its grip or palm leaves the push (the first 20), so a push that did nothing says why.
                if (!h.push_grip && h.near_push.distance > 0.0f && g_near_push_logs < 20) {
                    ++g_near_push_logs;
                    const NearPush& n = h.near_push;
                    spdlog::info("[Hands] (fix189) {} palm pushed out {:.2f} m, {:.1f} m/s at its fastest: no power ({}; a power needs {:.2f} m within {:.2f} s "
                                 "and {:.1f} m/s)",
                        left ? "left" : "right", n.distance, n.peak,
                        n.distance < kPushDistance ? (n.peak < min_speed ? "too short and too slow" : "too short") : "too slow", kPushDistance,
                        kPushWindowUs * 1e-6, min_speed);
                }
                // (fix173: nor a grip that is LB or RB, squeezed in the scanner or the star map, until let go.)
                if (!free || !h.push_grip || g_grip_shoulder[hand].load()) {
                    h.near_push = NearPush{}; // (fix189)
                    continue;
                }
                if (GameFlow::isWeaponDrawn()) {
                    HandGrab::MarkBusy(left); // a free hand held for a push (palm out): its push is a power, not a melee swing
                }
                if (now - h.last_push < kPowerCooldownUs || now - g_power_until.load() < kPowerCooldownUs - kPowerPressUs || now < h.reject_until) {
                    continue;
                }
                glm::vec3 direction{}, start_rel{};
                float     distance = 0.0f, peak = 0.0f;
                int64_t   start_us = 0;
                NearPush  closest{};
                if (!FindPush(h, forward, min_speed, direction, distance, peak, start_rel, start_us, closest)) {
                    if (closest.distance > h.near_push.distance) {
                        h.near_push = closest; // (fix189)
                    }
                    continue;
                }
                h.near_push = NearPush{}; // (fix189) this try is answered below: a power, or why not
                // (fix175) Not a deliberate push: logged the first times (why), and not counted again for this push.
                const char* not_push = glm::dot(h.palm, direction) < kPalmPushDot ? "the palm more than 40 degrees off the push"
                                       : h.grip_since - start_us > kPushGripLateUs ? "(fix184) the grip squeezed well after the push began"
                                       : glm::length(start_rel) < kPushFromHeadMin  ? "it started at your head"
                                       : FavoritesWheel::OpenOrClosedWithin(kPushAfterWheelMs) ? "the favorites wheel just closed"
                                                                                                 : nullptr;
                if (not_push != nullptr) {
                    h.reject_until = now + kPushWindowUs; // (the review: only this push muted, not the power's whole 1 s cooldown)
                    if (g_not_push_logs < 20) {
                        ++g_not_push_logs;
                        spdlog::info("[Hands] (fix175) {} palm pushed out ({:.2f} m, {:.1f} m/s, the palm {:.0f} degrees from the push): no power ({})",
                            left ? "left" : "right", distance, peak, glm::degrees(std::acos(std::clamp(glm::dot(h.palm, direction), -1.0f, 1.0f))), not_push);
                    }
                    continue;
                }
                if (scanner) {
                    h.last_push = now; // (the same push not counted again a frame later)
                    if (g_scanner_power_logs < 10) {
                        ++g_scanner_power_logs;
                        spdlog::info("[Hands] (fix173) {} palm pushed out with the hand scanner open: no power there", left ? "left" : "right");
                    }
                    continue;
                }
                // Aim: where the palm faces (as a Force push is aimed), which is along the push.
                const glm::vec3 aim = h.palm;
                h.last_push              = now;
                g_power_until            = now + kPowerPressUs;
                {
                    std::scoped_lock _{ g_aim_mutex };
                    g_push_aim       = LookAlong(aim);
                    g_push_aim_until = now + kPowerAimUs;
                }
                HandGrab::LeaveGrip(left); // the gravity gloves stop aiming with this grip (no pull on the way back)
                HandGrab::MarkBusy(left);
                haptics::MeleePulse(left, 0.5f, 0.08f);
                const int n = g_pushes.fetch_add(1) + 1;
                if (n <= 20 || n % 10 == 0) {
                    spdlog::info("[Hands] {} palm pushed out ({:.2f} m, {:.1f} m/s, the palm {:.0f} degrees from the push): power (LB + RB), aimed where "
                                 "the palm faces",
                        left ? "left" : "right", distance, peak, glm::degrees(std::acos(std::clamp(glm::dot(h.palm, direction), -1.0f, 1.0f))));
                }
                break;
            }
        } else {
            g_hands[0].near_push = g_hands[1].near_push = NearPush{}; // (fix189) no powers now: no try told later
        }
    }

    bool PowerAimActive()
    {
        std::scoped_lock _{ g_aim_mutex };
        return NowUs() < g_push_aim_until;
    }

    bool IsThrownLaunch(const std::uint8_t* bytes) { return bytes != nullptr && FindThrownWeapon(bytes, nullptr, nullptr) >= 0; }

    bool AimOverride(glm::mat4& rotation)
    {
        std::scoped_lock _{ g_aim_mutex };
        if (NowUs() >= g_push_aim_until) {
            return false;
        }
        rotation = g_push_aim;
        return true;
    }

    bool RightGripTaken() { return g_nade_active.load() || g_zone_ready.load(); }

    bool GrenadeInHand() { return g_nade_active.load(); }

    bool PressGrenade() { return g_rb_down.load() || NowUs() < g_rb_until.load(); }

    bool OwnsGrenadeButton()
    {
        // Until the game has launched the grenade (or gave up on it): a press another way meanwhile would ready another.
        const int64_t now     = NowUs();
        const int64_t pending = g_pending_since.load();
        return g_nade_active.load() || PressGrenade() || now < g_rb_until.load() + kButtonQuietUs || (pending != 0 && now - pending < kThrowWaitUs);
    }

    bool PressPower() { return NowUs() < g_power_until.load(); }

    bool GripsAreShoulders()
    {
        // (Not in the alternative layout: its left grip with a trigger is LB or RB already, and a tap of LB first would close the scanner;
        // the review.)
        const auto& settings = GameFlow::gStore.internalSettings;
        if (!settings.scannerGrips || settings.alternativeJoyLayout) {
            return false;
        }
        // Drawn in the last kModeWithinMs, not the 0.3 s isScannerOpen allows: a grip squeezed just after the scanner or the star map closed
        // is play's again (RB there is the grenade; the review). Nor once an LB went to the game in the scanner (NoteScannerLb: it closes
        // it; its closing frames are still drawn), until it is gone.
        const bool scanner = GameFlow::isScannerDrawnWithin(kModeWithinMs);
        if (!scanner) {
            g_scanner_closing = false;
        }
        // (The scanner under a menu, the pause menu over it: that menu's own controls. The star map is a menu: its own here.)
        // (fix175) Build mode too (no menu by the count: the pause menu over it has its own controls): LB and RB are its categories there
        // (the game's control map: Workshop context), and with the hand grab held off there the grips had nothing else to do.
        return (scanner && !GameFlow::isShowingMenu() && !g_scanner_closing.load()) || GameFlow::isStarMapShown(kModeWithinMs) ||
               (GameFlow::isWorkshopShown(kModeWithinMs) && !GameFlow::isShowingMenu());
    }

    const char* GripsPlace()
    {
        return GameFlow::isStarMapShown(kModeWithinMs) ? "star map" : GameFlow::isWorkshopShown(kModeWithinMs) ? "build mode" : "hand scanner";
    }

    bool LeftGripWaits()
    {
        // (The hand scanner's left grip stays at once: its LB closes it, and no combo of the left grip means anything there.)
        return GameFlow::isStarMapShown(kModeWithinMs) || GameFlow::isWorkshopShown(kModeWithinMs);
    }

    bool GripIsShoulder(bool left) { return g_grip_shoulder[left ? 0 : 1].load(); }

    void NoteScannerLb()
    {
        static int logs = 0; // (the gamepad poll's own count)
        if (GameFlow::isScannerDrawnWithin(kModeWithinMs) && !g_scanner_closing.exchange(true) && logs < 20) {
            ++logs;
            spdlog::info("[Hands] (fix173) LB went to the game in the hand scanner (it closes it): the grips are the hand grab's again");
        }
    }

    void NoteGripTap(bool left) { g_grip_tap_us[left ? 0 : 1] = NowUs(); }

    bool ThrowPending()
    {
        std::scoped_lock _{ g_throw_mutex };
        if (g_throw.pending && NowUs() - g_throw.us > kThrowWaitUs) {
            g_throw.pending = false;
            g_pending_since = 0;
            spdlog::info("[Hands] grenade: the game threw nothing within {:.1f} s of letting go", kThrowWaitUs * 1e-6);
        }
        return g_throw.pending;
    }

    void OnPlayerLaunch(std::uint8_t* launch_data, const std::uint8_t* before)
    {
        // Starfield 1.16.236: the launch data's pitch, roll and yaw at +0x24 (as the gun's shots use them, MuzzleFire). Which
        // projectile this is: the grenade's or mine's weapon form in the launch data (FindThrownWeapon).
        constexpr size_t kAngles = 0x24;
        if (launch_data == nullptr) {
            return;
        }
        std::uint8_t after[kLaunchBytes]{};
        if (!SafeRead(after, reinterpret_cast<uintptr_t>(launch_data), sizeof(after))) {
            return;
        }
        uint32_t    thrown_type = 0;
        std::string forms;
        const int   thrown_at = FindThrownWeapon(after, &thrown_type, &forms);

        Throw throw_now{};
        {
            std::scoped_lock _{ g_throw_mutex };
            throw_now = g_throw;
            if (thrown_at >= 0) {
                g_throw.pending = false; // this is the grenade
                g_pending_since = 0;
            }
            int& logs = thrown_at >= 0 ? g_throw.logs : g_throw.other_logs;
            if (logs < (thrown_at >= 0 ? kLaunchLogs : 2)) {
                ++logs;
                spdlog::info("[Hands] a launch {:.2f} s after the grenade left your hand: {} (weapon forms:{})", (NowUs() - throw_now.us) * 1e-6,
                    thrown_at >= 0 ? fmt::format("the {} (its form at +{:x})", thrown_type == 10 ? "grenade" : "mine", thrown_at) : std::string{ "not a grenade" },
                    forms.empty() ? std::string{ " none" } : forms);
                for (size_t row = 0; row < kLaunchBytes; row += 0x40) {
                    spdlog::info("[Hands]   +{:02x} before {}", row, Hex(before + row, 0x40));
                    spdlog::info("[Hands]   +{:02x} after  {}", row, Hex(after + row, 0x40));
                }
            }
        }
        if (thrown_at < 0) {
            return;
        }
        // Along the hand's throw (a gentle one keeps the game's own direction).
        if (glm::length(throw_now.stage_velocity) < kThrowMinSpeed) {
            return;
        }
        glm::vec3 world_point{}, world_direction{};
        if (!tracking::StageToWorld(throw_now.stage_point, throw_now.stage_velocity, world_point, world_direction) || glm::length(world_direction) < 0.5f) {
            return;
        }
        const float two_pi = 6.28318530718f;
        float       angles[3]{ -std::asin(std::clamp(world_direction.z, -1.0f, 1.0f)), 0.0f,
            std::fmod(std::atan2(world_direction.x, world_direction.y) + two_pi, two_pi) };
        std::memcpy(launch_data + kAngles, angles, sizeof(angles));
        const int n = g_redirected.fetch_add(1) + 1;
        if (n <= 20 || n % 10 == 0) {
            spdlog::info("[Hands] grenade sent along your throw (pitch {:.0f}, heading {:.0f} degrees)", glm::degrees(-angles[0]), glm::degrees(angles[2]));
        }
    }

    std::string Status()
    {
        const auto& s = GameFlow::gStore.internalSettings;
        std::string out;
        if (s.pushPowers) {
            out += fmt::format("Powers pushed: {}. ", g_pushes.load());
        }
        if (s.shoulderGrenade) {
            out += fmt::format("Grenades taken: {}, thrown: {}, sent along your throw: {}{}.", g_takes.load(), g_throws.load(), g_redirected.load(),
                g_nade_active.load() ? " (one in your hand now)" : "");
        }
        return out.empty() ? std::string{ "Off." } : out;
    }
}

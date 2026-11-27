#include "GunSight.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>
#include <mods/VR.hpp>
#include <spdlog/spdlog.h>

#include <ModSettings.h>

#include "CreationEngineCameraManager.h"
#include "CreationEngineConstants.h"
#include "GloveHighlight.h"
#include "HandGrab.h"
#include "MuzzleFire.h"
#include "body/BodyIK.h"
#include "models/GameFlow.h"
#include "models/ModSettingsStore.h"
#include "vr/TrackingSpace.h"

namespace GunSight
{
    namespace
    {
        constexpr float kDotDegrees = 1.2f; // the dot's width as seen from the controller (about a hand at arm's length / 6)

        bool     g_shown{ false };
        bool     g_told_barrel{ false };
        bool     g_told_aim{ false };
        uint32_t g_weapon{ 0 };
    }

    namespace
    {
        constexpr float kPointerDegrees = 1.0f; // the gloves' pointing dot

        // The gravity gloves' marker: a ring around what they aim at (a faint white glow over it once locked on), or a small dot along their line
        // when nothing is found yet. Placed from your head at the target, so it sits on it whatever line the gloves use.
        bool ShowGloveMark(VRRuntime::Hand hand)
        {
            static auto vr       = VR::get();
            const auto& settings = GameFlow::gStore.internalSettings;
            HandGrab::GloveMark mark;
            // (fix175: nor in build mode, where the gloves do not aim.)
            if (!settings.gloveMarker || !vr->is_hmd_active() || !vr->is_using_controllers() || !GameFlow::isInFirstPerson() || GameFlow::isShowingMenu() ||
                GameFlow::isWorkshopShown() || GameFlow::isInDialogue() || GameFlow::isPilotingShip() || ModSettings::showFlatScreenDisplay() ||
                !HandGrab::AimMark(mark)) {
                return false;
            }
            const glm::vec3 head{ tracking::HeadPose()[3] };
            glm::vec3       at{}, edge{};
            // The ring's radius in tracked metres: from a point on it (the radius is in the game world's units).
            if (!tracking::WorldToStage(mark.point, at) || !tracking::WorldToStage(mark.point + glm::vec3{ 0.0f, 0.0f, mark.radius }, edge)) {
                return false;
            }
            const glm::vec3 to       = at - head;
            const float     distance = glm::length(to);
            if (!std::isfinite(distance) || distance < 0.2f || !std::isfinite(head.x)) {
                return false;
            }
            // (Highlighted on the object itself by the game: no ring or glow picture over it as well; the line's dot still.)
            if ((mark.kind == 1 || mark.kind == 2) && GloveHighlight::Showing()) {
                return false;
            }
            const float radius  = glm::length(edge - at);
            float       degrees = kPointerDegrees;
            int         sprite  = 3;
            if (mark.kind == 1 || mark.kind == 2) {
                // The glow (locked on) fades out toward its edge: drawn a little larger so it covers the object.
                const float cover = mark.kind == 2 ? 1.3f : 1.0f;
                degrees           = std::clamp(glm::degrees(2.0f * std::atan2(radius * cover, distance)), 1.5f, 40.0f);
                sprite            = mark.kind;
            }
            const float o[3]{ head.x, head.y, head.z };
            const float d[3]{ to.x / distance, to.y / distance, to.z / distance };
            vr->set_reticle(true, hand, distance, degrees, o, d, sprite);
            static bool told = false;
            if (!told) {
                told = true;
                spdlog::info("[GunSight] the gravity gloves' marker is shown (a ring on what they aim at, a faint white glow once locked on; a dot along "
                             "their line before)");
            }
            return true;
        }
    }

    namespace
    {
        constexpr float kLaserDotDegrees = 0.7f; // the laser's dot, seen from the gun (small, as a real one)
        bool            g_laser_logged{ false };
        bool            g_laser_reading_logged{ false };
        // (fix146) The last reading of where the line meets what is drawn (room metres along it), and when.
        std::chrono::steady_clock::time_point g_last_reading_at{};
        float                                 g_last_reading{ 0.0f };
        bool                                  g_last_reading_hit{ false };
        bool                                  g_had_reading{ false };
        std::chrono::steady_clock::time_point g_laser_called_at{};
        std::chrono::steady_clock::time_point g_laser_started_at{};

        // The laser (fix129, as Resident Evil 4 VR's): a short red beam out of the gun (15 cm, fix131: about 6 inches)
        // and a dot where the shots' line meets what is drawn. Where is read from the game's depth picture (VR's LaserSight: as DLSS
        // runs, a frame or two late), as a distance along the line, put on the line as it is now. The line is handed over from your
        // eye (the camera the frame is drawn with): its start as an offset in the eye's own axes (right, up, ahead) and its way the
        // same, in the game's units, since the renderer's positions are not the world's (fix130's log: 20.7 m apart). Nothing met
        // within 80 m (the sky): the beam alone. No reading (no DLSS: TAA, FSR, its depth not handed over): the dot at the gun
        // sight's distance. Returns false if the line cannot be had (the old dot then).
        // (fix146) beam: the beam shown (not while a held gun's barrel is not known yet: the gun would point beside it, the review's symptom
        // "aiming incorrectly then fixing itself"); the dot alone then. The dot's distance kept from the last reading for half a second when
        // one is late (it went to the gun sight's 15 m, a jump), and no dot before the first reading of a laser shown anew.
        bool ShowLaser(const glm::vec3& origin, const glm::vec3& direction, bool by_barrel, const float* barrel_world, const float* muzzle_world,
            float distance, VRRuntime::Hand hand, bool beam)
        {
            static auto vr = VR::get();
            (void)barrel_world;
            (void)muzzle_world;
            // The game's units for a metre of the room (as the shown body is placed: the tracking scale, the camera's parent's).
            glm::vec3 world_origin{}, world_direction{};
            float     scale = 1.0f;
            if (!tracking::StageToWorld(origin, direction, world_origin, world_direction, &scale) || !std::isfinite(scale) || scale < 0.1f ||
                scale > 10.0f) {
                return false;
            }
            // The line from your eye: in its own axes (the head's: x right, y up, -z ahead in the room).
            // (fix148) From the eye this frame's camera is at (the head's middle in mono, an eye in AER and full stereo: as the camera
            // manager places it, get_camera_eye_transform), not the head's middle: the beam is now drawn into the picture from there, and
            // half an eye's distance off it would stand beside the muzzle (the dot's reading a little truer too).
            const glm::mat4 head    = tracking::HeadPose();
            const glm::mat4 eye_off = CreationEngineCameraManager::Get()->get_camera_eye_transform();
            glm::vec3       eye     = glm::vec3{ head * glm::vec4{ glm::vec3{ eye_off[3] }, 1.0f } };
            if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z) || glm::length(eye - glm::vec3{ head[3] }) > 0.1f) {
                eye = glm::vec3{ head[3] };
            }
            const glm::vec3 right   = glm::normalize(glm::vec3{ head[0] });
            const glm::vec3 up      = glm::normalize(glm::vec3{ head[1] });
            const glm::vec3 ahead   = -glm::normalize(glm::vec3{ head[2] });
            const glm::vec3 from    = origin - eye;
            const float     lo[3]{ glm::dot(from, right) * scale, glm::dot(from, up) * scale, glm::dot(from, ahead) * scale };
            const float     ld[3]{ glm::dot(direction, right), glm::dot(direction, up), glm::dot(direction, ahead) };
            if (!std::isfinite(lo[0]) || !std::isfinite(ld[0]) || !std::isfinite(eye.x)) {
                return false;
            }
            vr->set_laser_ray(true, lo, ld, vr->m_engine_frame_count.load()); // (this frame's head pose: read with its camera)
            if (!g_laser_logged) {
                g_laser_logged = true;
                spdlog::info("[GunSight] the laser is shown (its line from the {}): where it meets what is drawn read from the game's depth as DLSS "
                             "runs; its beam out of the gun ({:.2f} m, fading; drawn into the pictures, hidden by what is nearer)",
                    by_barrel ? "gun's muzzle along its barrel" : "head along your right controller's aim",
                    std::clamp(GameFlow::gStore.internalSettings.gunLaserBeamLength, 0.05f, 2.0f));
            }
            bool  hit   = false;
            float along = 0.0f;
            const bool fresh = vr->laser_hit(hit, along);
            const auto clock_now = std::chrono::steady_clock::now();
            // (A laser shown anew, after a moment without: its readings looked for afresh.)
            if (clock_now - g_laser_called_at > std::chrono::milliseconds(300)) {
                g_laser_started_at = clock_now;
                g_had_reading      = false;
            }
            g_laser_called_at = clock_now;
            if (fresh) {
                g_last_reading_at  = clock_now;
                g_last_reading_hit = hit;
                g_last_reading     = along / scale;
                g_had_reading      = true;
            }
            const bool recent = g_had_reading && clock_now - g_last_reading_at < std::chrono::milliseconds(500);
            if (fresh != g_laser_reading_logged) {
                g_laser_reading_logged = fresh;
                spdlog::info("[GunSight] the laser's {}", fresh ? "readings come in: its dot sits where it meets what is drawn"
                                                                : "readings stopped (no DLSS, or its depth not handed over): its dot at the gun sight's distance");
            }
            // Where the dot is (the reading's distance is in the game's units): on the shots' line where it meets what is drawn; else
            // at the gun sight's distance (nothing met: no dot, but the beam still points that way).
            const float at = fresh && hit ? along / scale : (recent && g_last_reading_hit ? g_last_reading : distance);
            // The beam: out of the gun along its barrel (the line's own start), or out of your right controller toward the dot (the
            // shots' line starts at your head then: a beam along the controller would point beside it, fix131's review).
            glm::vec3 beam_start = origin, beam_way = direction;
            if (!by_barrel) {
                const glm::mat4 aim = tracking::AimPose(false);
                beam_start          = glm::vec3{ aim[3] };
                const glm::vec3 to  = origin + direction * std::max(at, 0.2f) - beam_start;
                beam_way            = glm::length(to) > 1e-3f ? glm::normalize(to) : -glm::normalize(glm::vec3{ aim[2] });
            }
            // (fix148) Its length a setting (0.30 m: about 6 inches seen, fading on from there to nothing), to the dot when that
            // is nearer. Drawn into the pictures in the eye's axes (as the line), hidden by the gun and anything nearer; the headset's strip
            // the same, only when it could not be (no depth).
            const float     shown    = std::clamp(GameFlow::gStore.internalSettings.gunLaserBeamLength, 0.05f, 2.0f);
            const float     length   = fresh && hit ? std::clamp(at, 0.05f, shown) : shown;
            const glm::vec3 beam_end = beam_start + beam_way * length;
            const float     s[3]{ beam_start.x, beam_start.y, beam_start.z };
            const float     e[3]{ beam_end.x, beam_end.y, beam_end.z };
            const glm::vec3 beam_from = beam_start - eye;
            const float     bs[3]{ glm::dot(beam_from, right) * scale, glm::dot(beam_from, up) * scale, glm::dot(beam_from, ahead) * scale };
            const float     bd[3]{ glm::dot(beam_way, right), glm::dot(beam_way, up), glm::dot(beam_way, ahead) };
            if (beam && std::isfinite(beam_start.x) && std::isfinite(beam_end.x) && std::isfinite(bs[0]) && std::isfinite(bd[0])) {
                vr->set_laser_beam(true, s, e);
                vr->set_laser_beam_view(true, bs, bd, length * scale, vr->m_engine_frame_count.load());
            } else {
                vr->set_laser_beam(false);
                vr->set_laser_beam_view(false);
            }
            // The dot.
            const float o[3]{ origin.x, origin.y, origin.z };
            const float d[3]{ direction.x, direction.y, direction.z };
            // (No dot: it meets nothing; the last reading, kept, met nothing; or no reading yet in the laser's first second. No reading for
            // longer: the gun sight's distance, as before.)
            const bool starting = !g_had_reading && clock_now - g_laser_started_at < std::chrono::seconds(1);
            if ((fresh && !hit) || (!fresh && recent && !g_last_reading_hit) || (!fresh && !recent && starting)) {
                vr->set_reticle(false, hand, distance, kLaserDotDegrees);
            } else {
                vr->set_reticle(true, hand, std::max(at, 0.2f), kLaserDotDegrees, o, d, runtimes::OpenXR::sprite_laser_dot);
            }
            return true;
        }
    }

    void Update(bool aiming_with_hand)
    {
        MuzzleFire::Update(); // (fix195) the log's summary of a cutter's trigger pull, once it has ended
        static auto vr       = VR::get();
        const auto& settings = GameFlow::gStore.internalSettings;
        const bool  show     = settings.gunSight && aiming_with_hand && vr->is_hmd_active() && vr->is_using_controllers() && GameFlow::isWeaponDrawn() &&
                          GameFlow::isInFirstPerson() && !GameFlow::isShowingMenu() && !GameFlow::isWorkshopShown() && // (fix175: nor build mode)
                          !GameFlow::isInDialogue() && !GameFlow::isPilotingShip() &&
                          !ModSettings::showFlatScreenDisplay() && HandGrab::GunOut();
        const int   right_index = vr->get_right_controller_index();
        const auto  hand        = right_index == 1 ? VRRuntime::Hand::LEFT : VRRuntime::Hand::RIGHT;
        const float distance    = std::clamp(settings.gunSightDistance, 3.0f, 60.0f);

        if (!show) {
            // (No laser either: its beam hidden, the game's depth no longer copied.)
            vr->set_laser_ray(false);
            vr->set_laser_beam(false);
            vr->set_laser_beam_view(false);
            // No gun sight: while the gravity gloves aim, the same layer shows what they aim at (they never aim with a gun out).
            if (ShowGloveMark(hand)) {
                g_shown = true;
                return;
            }
            if (g_shown) {
                vr->set_reticle(false, hand, distance, kDotDegrees);
                g_shown = false;
            }
            return;
        }

        // Another weapon: until it fires, assume its shots follow the barrel (MuzzleFire says otherwise after a shot).
        if (const uint32_t weapon = HandGrab::WeaponInHandsId(); weapon != g_weapon) {
            g_weapon = weapon;
            MuzzleFire::ResetShotsFollowBarrel();
        }

        // The ray the shots take: from the shown body's muzzle along its barrel (MuzzleFire fires them that way), else from the
        // game camera (your head) along the right controller's aim (the camera turns with it: the game's own shots).
        const glm::mat4 aim       = tracking::AimPose(false);
        glm::vec3       origin    = glm::vec3{ tracking::HeadPose()[3] };
        glm::vec3       direction = -glm::normalize(glm::vec3{ aim[2] });
        float           barrel_world[3]{}, muzzle_world[3]{};
        glm::vec3       barrel{}, muzzle{};
        const bool      barrel_mode = ModConstants::shootFromMuzzle && MuzzleFire::ShotsFollowBarrel() && body::IsActive();
        // (fix146) The muzzle where the gun is drawn this frame: as the body placed it in your hand, in your hand's own axes, put with this
        // frame's aim pose (running or turning, the gun and its line stay together). Else (no gun placed lately) the muzzle as last drawn,
        // put in the room with this frame's camera, as before.
        bool            from_hand = false;
        bool            by_barrel = false;
        if (float in_aim[3]{}, way_in_aim[3]{}; barrel_mode && body::GetMuzzleInAim(in_aim, way_in_aim)) {
            const glm::vec3 fwd   = -glm::normalize(glm::vec3{ aim[2] });
            const glm::vec3 up    = glm::normalize(glm::vec3{ aim[1] });
            glm::vec3       right = glm::cross(fwd, up);
            if (glm::length(right) > 1e-4f) {
                right               = glm::normalize(right);
                const glm::vec3 top = glm::cross(right, fwd);
                muzzle              = glm::vec3{ aim[3] } + right * in_aim[0] + fwd * in_aim[1] + top * in_aim[2];
                barrel              = glm::normalize(right * way_in_aim[0] + fwd * way_in_aim[1] + top * way_in_aim[2]);
                from_hand           = std::isfinite(muzzle.x) && std::isfinite(barrel.x);
                by_barrel           = from_hand;
            }
        }
        glm::vec3 old_muzzle{};
        const bool old_line = barrel_mode && body::GetBodyAimForward(barrel_world) && body::GetBodyMuzzleWorld(muzzle_world) &&
                              tracking::WorldToStage(glm::vec3{ muzzle_world[0], muzzle_world[1], muzzle_world[2] }, old_muzzle);
        if (!by_barrel && old_line &&
            tracking::WorldToStageDirection(glm::vec3{ barrel_world[0], barrel_world[1], barrel_world[2] }, barrel)) {
            muzzle    = old_muzzle;
            by_barrel = true;
        }
        if (by_barrel) {
            origin    = muzzle;
            direction = barrel;
        }
        if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) || !std::isfinite(direction.x) ||
            !std::isfinite(direction.y) || !std::isfinite(direction.z)) {
            vr->set_laser_ray(false);
            vr->set_laser_beam(false); // (not left where it last was)
            vr->set_laser_beam_view(false);
            return;
        }
        // (fix146) Each change of the line logged with its reason (the first few), and every 10 s, while it comes from the hand, how far the
        // muzzle as last drawn would have put it (the old way: behind as you run).
        {
            const int mode = from_hand ? 2 : by_barrel ? 1 : 0;
            static int last_mode = -1, mode_logs = 0;
            if (mode != last_mode && mode_logs < 20) {
                ++mode_logs;
                spdlog::info("[GunSight] the dot's line: {} (shots along the barrel {}, the body shown {}, the gun's barrel known {})",
                    mode == 2   ? "from the gun's muzzle as drawn in your hand this frame"
                    : mode == 1 ? "from the gun's muzzle as last drawn"
                                : "from your head along your right controller's aim, as the game's own shots go",
                    MuzzleFire::ShotsFollowBarrel(), body::IsActive(), body::BarrelKnown());
            }
            last_mode = mode;
            static auto offset_logged = std::chrono::steady_clock::time_point{};
            if (const auto now = std::chrono::steady_clock::now(); from_hand && old_line && now - offset_logged > std::chrono::seconds(10)) {
                offset_logged = now;
                spdlog::info("[GunSight] the laser's start from your hand; the muzzle as last drawn would put it {:.3f} m away from it", glm::length(old_muzzle - muzzle));
            }
        }
        if (by_barrel && !g_told_barrel) {
            g_told_barrel = true;
            spdlog::info("[GunSight] the dot follows the gun's barrel as your hand holds it ({:.0f} m out)", distance);
        } else if (!by_barrel && !g_told_aim) {
            g_told_aim = true;
            spdlog::info("[GunSight] the dot follows your right controller's aim from your head, as the game's own shots go ({:.0f} m out)", distance);
        }
        const float o[3]{ origin.x, origin.y, origin.z };
        const float d[3]{ direction.x, direction.y, direction.z };
        // (fix146) The beam only once the held gun's barrel is known (before, the gun is drawn on the game's hold estimate, beside the beam).
        // (Logged when it waits long: a gun whose muzzle is not read waits for its first shot; the review.)
        {
            static auto waiting_since = std::chrono::steady_clock::time_point{};
            static int  wait_logs     = 0;
            const auto  now_wait      = std::chrono::steady_clock::now();
            if (!by_barrel || body::BarrelKnown()) {
                waiting_since = {};
            } else if (waiting_since == std::chrono::steady_clock::time_point{}) {
                waiting_since = now_wait;
            } else if (now_wait - waiting_since > std::chrono::seconds(3) && wait_logs < 5) {
                ++wait_logs;
                waiting_since = now_wait + std::chrono::hours(1); // (once a wait)
                spdlog::info("[GunSight] the laser's beam waits for this gun's barrel to be known (its muzzle not read): the dot alone until its first shot");
            }
        }
        if (settings.gunLaser && ShowLaser(origin, direction, by_barrel, barrel_world, muzzle_world, distance, hand, !by_barrel || body::BarrelKnown())) {
            g_shown = true;
            return;
        }
        vr->set_laser_ray(false);
        vr->set_laser_beam(false);
        vr->set_laser_beam_view(false);
        vr->set_reticle(true, hand, distance, kDotDegrees, o, d, 0);
        g_shown = true;
    }
}

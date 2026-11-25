#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// The plan behind ViewHistory (no game or D3D12 code: tested on its own).
//
// While a full-screen menu is open, the game draws all of the frame's views (with the world kept drawn behind a see-through menu:
// the world, then the menu's 3D views) with one set of working pictures: the pictures a view keeps from one frame to the next
// (its anti-aliasing history, its previous depth, its light's history...) are the same pictures for every view. Each view then
// finds in them what the view drawn before it left, not its own last frame: the world the menu's views', the menu's views the
// world's (fix120b's test: the world in the player's preview).
//
// From a few frames of the steps' uses (which step used which picture, in which state, in the frame's order), the plan finds, for
// each view, the pictures it reads from an earlier frame (its first use of it in a frame is not a write) that it also writes itself,
// and that other views write in between: those are its history. For each, two actions:
//   save: right after the view's last use of it (its content then is what the view leaves for itself);
//   load: right after the last use of it by anything else before the view's next read (the view's saved content put back in it
//         then: the other views, and the frame's own steps, see in it what they did before).
// Both at steps whose state for it is known, on a direct command list, that run once a frame and belong to one view; each picture
// with one save place and one load place in frames of a parity. Pictures a view only reads (the frame's shared inputs), writes
// before reading (its working pictures), that nothing else writes, or that the frame's own steps write between the view's frames
// (the game's per-frame data), or with a step in between whose uses were not all seen, are left alone. (Where one view's history is
// saved and another's put back at the same step, the save is done first: ViewHistory.)
namespace ViewHistoryPlan
{
    // A use's group: the view it belongs to (its place in the frame: 0 the world's, then the menu's), or the frame's own steps.
    enum Group : uint8_t
    {
        kWorld = 0,    // a step of the world's view
        kMenu  = 1,    // a step of the menu's first view (the next ones: 2, 3...)
        kFrame = 0xff, // a step of the frame outside the views
    };

    // D3D12 resource states.
    constexpr uint32_t kWriteStates = 0x4 | 0x8 | 0x10 | 0x100 | 0x400 | 0x1000; // render target, UAV, depth write, stream out, copy/resolve dest
    constexpr uint32_t kUav         = 0x8;

    struct Use
    {
        uint32_t  frame{ 0 };  // the frame's number (consecutive frames: consecutive numbers)
        int32_t   order{ 0 };  // the step's place in the frame (the frame's steps in their order)
        uint8_t   group{ kFrame };
        void*     pass{ nullptr };
        int32_t   item{ 0 };   // where the step has it: its list's index, or another kind of place (fetched again at run time)
        int32_t   role{ 0 };   // the same place in every frame (one history: one role, though its picture may alternate)
        uintptr_t resource{ 0 }; // 0: not a use but a mark: item kRan (the step ran: once each time) or kBlind (its uses not all seen)
        uint32_t  state{ 0 };  // the D3D12 state the step uses it in; 0: not known
        bool      direct{ false };
        bool      buffer{ false }; // a buffer (its first use as unordered access may read what it had: counted as a read)
        bool      single{ true };  // the step runs once a frame, in one view (else never an action's place)
        // Untracked, one of a view's persistent pictures: it rests in unordered access (its state word, given back after its last
        // tracked use). Its state, when not known, taken as that in a frame where nothing else uses it with a tracked state (Plan).
        bool      resting{ false };
        bool      read_only{ false }; // (set by the plan: a resting picture read, its state taken as unordered access)
    };

    constexpr int32_t kRan   = -1;
    constexpr int32_t kBlind = -2;

    enum Op : uint8_t
    {
        kSave = 0,
        kLoad = 1,
    };

    struct Action
    {
        uint32_t  parity{ 0 };   // in the frames of this parity (frame & 1)
        void*     pass{ nullptr };
        int32_t   item{ 0 };
        uintptr_t resource{ 0 }; // expected there (else not done)
        uint32_t  state{ 0 };    // ...in this state
        Op        op{ kSave };
        uint64_t  role{ 0 };     // the view's history it is (one saved copy of each for each eye)
        uint32_t  ahead{ 0 };    // load: the content for the frame this many frames on (its eye's copy)
        uint8_t   view{ kWorld };
    };

    struct Kept
    {
        uintptr_t resource{ 0 };
        void*     first_read{ nullptr }; // the view's step that reads it from the frame before
        void*     save_pass{ nullptr };
        int32_t   save_item{ 0 };
        void*     load_pass{ nullptr };
        int32_t   load_item{ 0 };
        uint8_t   view{ kWorld };
    };

    // One of a view's histories left alone (for the log): why, and where it would have been saved and put back.
    struct Skip
    {
        uintptr_t   resource{ 0 };
        std::string why{};
        void*       save_pass{ nullptr };
        uint32_t    save_state{ 0 };
        void*       load_pass{ nullptr };
        uint32_t    load_state{ 0 };
        uint8_t     view{ kWorld };
    };

    struct Result
    {
        std::vector<Action>        actions{};
        std::vector<Kept>          kept{};
        std::map<std::string, int> skipped{}; // pictures left alone, by why (and view)
        std::vector<Skip>          notes{};   // ...those a view reads from an earlier frame and other views write in between
        std::set<std::pair<uintptr_t, uint8_t>> needed{}; // (each picture a view reads from an earlier frame, others using it in between)
        int                        resources{ 0 };
    };

    // (A state the game's flags can give that is not one D3D12 state: two kinds of write, or a write with a read, is not known.)
    inline bool Known(uint32_t state)
    {
        if (state == 0) {
            return false;
        }
        const uint32_t writes = state & kWriteStates;
        return writes == 0 || (writes == state && (writes & (writes - 1)) == 0);
    }
    inline bool Writes(uint32_t state) { return Known(state) && (state & kWriteStates) != 0; }


    inline uint64_t RoleKey(const void* pass, int32_t role)
    {
        return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pass)) << 8) ^ static_cast<uint64_t>(static_cast<uint32_t>(role) & 0xff);
    }

    // One step's use of one picture (its places merged: one state for it in a step).
    struct StepUse
    {
        uint32_t frame{ 0 };
        int32_t  order{ 0 };
        uint8_t  group{ kFrame };
        void*    pass{ nullptr };
        int32_t  item{ 0 };
        int32_t  role{ 0 };
        uint32_t state{ 0 };
        bool     direct{ false };
        bool     buffer{ false };
        bool     single{ true };
        bool     ambiguous{ false }; // two places in different states
        bool     read_only{ false }; // a resting picture read in unordered access (WithRestingStates): a read, though its state is a write's
    };

    // (A step's use writes the picture: its state a write's, unless it is a resting picture read in unordered access.)
    inline bool WritesUse(const StepUse& s) { return Writes(s.state) && !s.read_only; }

    using Taken = std::set<std::tuple<uint32_t, void*, int32_t, int>>; // (parity, pass, item, op): one action at each place

    // The histories of one view (mine), added to `result`; `taken`: the places already used (by this view or others).
    inline void PlanView(const std::vector<Use>& uses, uint8_t mine, bool every_view, Result& result, Taken& taken)
    {
        // Each picture's uses, each step once, in the frames' order; and the steps not fully seen.
        std::map<uintptr_t, std::vector<StepUse>>           by_resource{};
        std::vector<std::tuple<uint32_t, int32_t, uint8_t>> blind{};
        std::set<void*>                                     again{}; // steps that ran more than once in a frame
        {
            std::map<std::pair<void*, uint32_t>, int> runs{};
            for (const auto& u : uses) {
                if (u.pass != nullptr && u.resource == 0 && u.item == kRan && ++runs[{ u.pass, u.frame }] == 2) {
                    again.insert(u.pass);
                }
            }
        }
        {
            std::map<std::tuple<uintptr_t, uint32_t, int32_t, void*>, StepUse> merged{};
            for (const auto& u : uses) {
                if (u.pass == nullptr) {
                    continue;
                }
                if (u.resource == 0) {
                    if (u.item == kBlind) {
                        blind.emplace_back(u.frame, u.order, u.group);
                    }
                    continue;
                }
                const bool single = u.single && again.count(u.pass) == 0;
                const auto key = std::make_tuple(u.resource, u.frame, u.order, u.pass);
                auto       it  = merged.find(key);
                if (it == merged.end()) {
                    merged.emplace(key, StepUse{ u.frame, u.order, u.group, u.pass, u.item, u.role, u.state, u.direct, u.buffer, single, false, u.read_only });
                } else {
                    it->second.ambiguous |= it->second.state != u.state;
                    it->second.single &= single;
                    it->second.read_only &= u.read_only;
                }
            }
            for (const auto& [key, s] : merged) {
                by_resource[std::get<0>(key)].push_back(s);
            }
        }
        result.resources = std::max(result.resources, static_cast<int>(by_resource.size()));
        uint32_t first_frame = UINT32_MAX;
        for (const auto& u : uses) {
            if (u.resource != 0) {
                first_frame = std::min(first_frame, u.frame);
            }
        }
        const auto before = [](uint32_t fa, int32_t oa, uint32_t fb, int32_t ob) { return fa != fb ? fa < fb : oa < ob; };
        const auto other  = [&](uint8_t group) { return group != mine && group != kFrame; };
        const auto skip   = [&](const std::string& why) { ++result.skipped[mine == kWorld ? why : why + " (a menu view's)"]; };
        for (auto& [resource, seq] : by_resource) {
            std::sort(seq.begin(), seq.end(), [](const StepUse& a, const StepUse& b) {
                return a.frame != b.frame ? a.frame < b.frame : a.order < b.order;
            });
            bool mine_writes = false, other_writes = false, mine_uses = false, other_uses = false;
            for (const auto& s : seq) {
                if (s.group == mine) {
                    mine_uses = true;
                    mine_writes |= WritesUse(s);
                } else if (other(s.group)) {
                    other_uses = true;
                    other_writes |= WritesUse(s) || !Known(s.state);
                }
            }
            if (!mine_uses || !other_uses) {
                continue; // (not shared by this view and others)
            }
            if (!other_writes) {
                skip("only read by the other views");
                continue;
            }
            if (!mine_writes) {
                skip("only read by the view (the frame's own input)");
                continue;
            }
            std::vector<Action> found{};
            Kept                kept{};
            bool                needed = false;
            std::string         why{};
            Skip                note{};
            // (Where it would be saved and put back in every pair of frames, not only those that need it: the same each time.)
            std::set<std::tuple<uint32_t, void*, int32_t>> save_places[2]{}, load_places[2]{};
            for (size_t i = 0; i < seq.size() && why.empty(); ++i) {
                const auto& r = seq[i];
                if (r.group != mine || r.frame == first_frame) {
                    continue;
                }
                // (The view's first use of it in its frame, not a write: it reads what it had.)
                bool first = true;
                for (size_t j = i; j-- > 0 && seq[j].frame == r.frame;) {
                    if (seq[j].group == mine) {
                        first = false;
                        break;
                    }
                }
                const bool reads_first = !WritesUse(r) || (r.buffer && r.state == kUav);
                if (!first || !reads_first) {
                    continue;
                }
                // Its last use by the view before, and the others' uses in between.
                size_t s_at = SIZE_MAX;
                for (size_t j = i; j-- > 0;) {
                    if (seq[j].group == mine) {
                        s_at = j;
                        break;
                    }
                }
                if (s_at == SIZE_MAX) {
                    continue; // (nothing of the view's before it in these frames)
                }
                // (Another view using it in between, even only reading it, when the other views keep their own histories too: their own
                // content may be put back in it there. Else, and for the frame's own steps: only their writes.)
                bool   other_used = false, frame_wrote = false;
                size_t u_at = SIZE_MAX;
                for (size_t j = s_at + 1; j < i; ++j) {
                    const bool writes = WritesUse(seq[j]) || !Known(seq[j].state);
                    if (seq[j].group == kFrame) {
                        frame_wrote |= writes;
                    } else {
                        other_used |= every_view || writes;
                    }
                    u_at = j;
                }
                {
                    const auto& s0 = seq[s_at];
                    save_places[s0.frame & 1].insert({ 0u, s0.pass, s0.item });
                    if (u_at != SIZE_MAX) {
                        load_places[seq[u_at].frame & 1].insert({ r.frame - seq[u_at].frame, seq[u_at].pass, seq[u_at].item });
                    }
                }
                if (u_at == SIZE_MAX || !other_used) {
                    continue; // (not used by the other views in between)
                }
                const auto& s = seq[s_at];
                const auto& u = seq[u_at];
                // (What the view leaves in it: written by the view in that frame, at or before its last use.)
                bool mine_wrote = false;
                for (size_t j = s_at + 1; j-- > 0 && seq[j].frame == s.frame;) {
                    if (seq[j].group == mine && WritesUse(seq[j])) {
                        mine_wrote = true;
                        break;
                    }
                }
                if (!mine_wrote) {
                    continue; // (not the view's own in this frame's pattern)
                }
                needed = true;
                if (note.resource == 0) {
                    note = Skip{ resource, {}, s.pass, s.state, u.pass, u.state, mine }; // (for the log: the first frame that needs it)
                }
                // (Another view writes it first in this frame, before the view reads it: that view's output of this frame, which the view
                // reads (as a menu's room view may show the others'), not the view's own history: left alone.)
                for (size_t j = i; j-- > 0 && seq[j].frame == r.frame && why.empty();) {
                    if (!other(seq[j].group) || !WritesUse(seq[j])) {
                        continue;
                    }
                    bool writer_first = true;
                    for (size_t k = j; k-- > 0 && seq[k].frame == r.frame;) {
                        if (seq[k].group == seq[j].group) {
                            writer_first = false;
                            break;
                        }
                    }
                    if (writer_first) {
                        why = "written first by another view in the frame, before the view reads it (that view's output, not a history)";
                    }
                }
                if (!why.empty()) {
                    break;
                }
                if (frame_wrote) {
                    why = "written by the frame's own steps between the view's frames";
                    break;
                }
                for (const auto& [bf, bo, bg] : blind) {
                    // (Not seen: a step of the others' in between, or one of the view's after its last seen use in that frame.)
                    if ((bg != mine && before(s.frame, s.order, bf, bo) && before(bf, bo, r.frame, r.order)) ||
                        (bg == mine && bf == s.frame && bo > s.order)) {
                        why = "a step in between whose uses could not all be seen";
                        break;
                    }
                }
                if (!why.empty()) {
                    break;
                }
                if (s.ambiguous || u.ambiguous || !Known(s.state) || !Known(u.state)) {
                    why = "its state not known where it would be saved or put back";
                    break;
                }
                if (!s.direct || !u.direct) {
                    why = "saved or put back on a compute command list";
                    break;
                }
                if (!s.single || !u.single) {
                    why = "saved or put back at a step that runs more than once a frame, or where its state is not tracked or the step attaches it";
                    break;
                }
                const uint64_t role = RoleKey(s.pass, s.role);
                found.push_back(Action{ s.frame & 1, s.pass, s.item, resource, s.state, kSave, role, 0, mine });
                found.push_back(Action{ u.frame & 1, u.pass, u.item, resource, u.state, kLoad, role, r.frame - u.frame, mine });
                if (kept.resource == 0) {
                    kept = Kept{ resource, r.pass, s.pass, s.item, u.pass, u.item, mine };
                }
            }
            if (!needed) {
                skip("not read from an earlier frame by the view");
                continue;
            }
            result.needed.insert({ resource, mine });
            if (!why.empty()) {
                skip(why);
                note.why = why;
                result.notes.push_back(note);
                continue;
            }
            if (save_places[0].size() > 1 || save_places[1].size() > 1 || load_places[0].size() > 1 || load_places[1].size() > 1) {
                skip("saved or put back at different steps in different frames");
                note.why = "saved or put back at different steps in different frames";
                result.notes.push_back(note);
                continue;
            }
            // (The same action found in two frames of a parity is one; one save place and one load place in frames of a parity, the
            // same each time: else the pattern is not steady.)
            std::vector<Action> unique{};
            bool                steady = true;
            for (const auto& a : found) {
                bool same = false;
                for (const auto& b : unique) {
                    if (a.parity == b.parity && a.op == b.op) {
                        same = true;
                        steady &= a.pass == b.pass && a.item == b.item && a.state == b.state && a.role == b.role && a.ahead == b.ahead;
                    }
                }
                if (!same) {
                    unique.push_back(a);
                }
            }
            if (!steady) {
                skip("saved or put back at different steps in different frames");
                note.why = "saved or put back at different steps in different frames";
                result.notes.push_back(note);
                continue;
            }
            bool clash = false;
            for (const auto& a : unique) {
                clash |= taken.count({ a.parity, a.pass, a.item, static_cast<int>(a.op) }) != 0;
            }
            if (clash) {
                skip("two different actions at one place");
                note.why = "two different actions at one place";
                result.notes.push_back(note);
                continue;
            }
            for (const auto& a : unique) {
                taken.insert({ a.parity, a.pass, a.item, static_cast<int>(a.op) });
                result.actions.push_back(a);
            }
            result.kept.push_back(kept);
        }
    }

    // Every view's histories: the world's first, then each menu view's (in the frame's order).
    // every_view: each of the menu's views keeps its own histories too (else only the world's: as before fix123).
    // The resting pictures' untracked uses with no state known: in unordered access, in a frame where every use of the picture is
    // untracked (nothing then moves it from where it rests). Else left not known.
    inline std::vector<Use> WithRestingStates(const std::vector<Use>& uses)
    {
        std::set<std::pair<uintptr_t, uint32_t>> moved{}; // (picture, frame): a use of it there that is not a resting one
        bool                                     any = false;
        for (const auto& u : uses) {
            if (u.resource == 0) {
                continue;
            }
            if (!u.resting) {
                moved.insert({ u.resource, u.frame });
            } else if (u.state == 0) {
                any = true;
            }
        }
        std::vector<Use> out = uses;
        if (!any) {
            return out;
        }
        for (auto& u : out) {
            if (u.resource != 0 && u.resting && u.state == 0 && moved.count({ u.resource, u.frame }) == 0) {
                u.state  = kUav;
                u.read_only = true; // (the step reads it: its state was not known as it asks for a read; in unordered access, a read)
            }
        }
        return out;
    }

    // planned: the views (bits by group) whose histories are kept (a view whose picture is not shown left out; the world always).
    inline Result Plan(const std::vector<Use>& given, bool every_view = true, uint32_t planned = 0xffffffffu)
    {
        const std::vector<Use> uses = WithRestingStates(given);
        std::set<uint8_t>      views{};
        for (const auto& u : uses) {
            if (u.group != kFrame && (u.group == kWorld || (every_view && u.group < 32 && ((planned >> u.group) & 1u) != 0))) {
                views.insert(u.group);
            }
        }
        Result result{};
        Taken  taken{};
        for (const uint8_t view : views) {
            PlanView(uses, view, every_view, result, taken);
        }
        // A picture one view needs kept and cannot keep (its places not usable): the menu's views' copies of it dropped too, with
        // the other picture of the same history (else the one left out reads, instead of what it would have, another view's copy put
        // back in it). The world's kept: its copy is put back after every other use, as before fix123. Again until nothing changes
        // (a history dropped for one view leaves its other picture out for it).
        if (every_view) {
            for (bool changed = true; changed;) {
                changed = false;
                std::set<uintptr_t> partial{};
                for (const auto& [resource, view] : result.needed) {
                    const bool kept = std::any_of(result.kept.begin(), result.kept.end(), [&](const Kept& k) {
                        return k.resource == resource && k.view == view;
                    });
                    if (!kept) {
                        partial.insert(resource);
                    }
                }
                std::set<uint64_t> roles{};
                for (const auto& a : result.actions) {
                    if (a.view != kWorld && partial.count(a.resource) != 0) {
                        roles.insert(a.role);
                    }
                }
                if (roles.empty()) {
                    break;
                }
                std::set<std::pair<uintptr_t, uint8_t>> gone{};
                for (const auto& a : result.actions) {
                    if (roles.count(a.role) != 0) {
                        gone.insert({ a.resource, a.view });
                    }
                }
                std::erase_if(result.actions, [&](const Action& a) { return roles.count(a.role) != 0; });
                const size_t before = result.kept.size();
                std::erase_if(result.kept, [&](const Kept& k) { return gone.count({ k.resource, k.view }) != 0; });
                if (before != result.kept.size()) {
                    result.skipped["left alone: another view that reads it from an earlier frame cannot keep its own (a menu view's)"] +=
                        static_cast<int>(before - result.kept.size());
                    changed = true;
                }
            }
        }
        return result;
    }
}

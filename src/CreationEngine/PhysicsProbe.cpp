#include "PhysicsProbe.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <spdlog/spdlog.h>

#include "CreationEngineSingletonManager.h"
#include "Framework.hpp"
#include "HandGrab.h"
#include "memory/GameCode.h"

// Everything here runs on the probe's own thread and only reads (see PhysicsProbe.h).
namespace PhysicsProbe
{
    namespace
    {
        int64_t NowUs()
        {
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Only memory that can be read without side effects: committed, and neither no-access nor a guard page (a read of
        // another thread's stack guard page would take its guard away, and that thread's stack could then not grow).
        bool Readable(uintptr_t src, size_t n)
        {
            const uintptr_t end = src + n;
            if (end < src) {
                return false;
            }
            for (uintptr_t at = src; at < end;) {
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT || info.Protect == 0 ||
                    (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
                    return false;
                }
                const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
                if (next <= at) {
                    return false;
                }
                at = next;
            }
            return true;
        }

        bool SafeRead(void* dst, uintptr_t src, size_t n)
        {
            SIZE_T done = 0;
            return n == 0 || (src != 0 && Readable(src, n) && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(src), dst, n, &done) &&
                                 done == n);
        }

        template <typename T>
        bool Read(uintptr_t src, T& out)
        {
            return SafeRead(&out, src, sizeof(T));
        }

        // As much of [src, src + n) as can be read from its start (in 0x100 byte pieces once a whole read fails).
        size_t ReadSome(void* dst, uintptr_t src, size_t n)
        {
            if (SafeRead(dst, src, n)) {
                return n;
            }
            size_t done = 0;
            while (done < n) {
                const size_t step = std::min<size_t>(0x100, n - done);
                if (!SafeRead(static_cast<uint8_t*>(dst) + done, src + done, step)) {
                    break;
                }
                done += step;
            }
            return done;
        }

        uint32_t U32(const uint8_t* p, size_t at)
        {
            uint32_t v = 0;
            std::memcpy(&v, p + at, sizeof(v));
            return v;
        }

        uint64_t U64(const uint8_t* p, size_t at)
        {
            uint64_t v = 0;
            std::memcpy(&v, p + at, sizeof(v));
            return v;
        }

        bool Finite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        // Could be a pointer to the heap (or anywhere): above the first 64 KB, in user space, 8-aligned.
        bool Pointer(uintptr_t p) { return p >= 0x10000 && p < 0x7FFF'FFFF'0000ull && (p & 7) == 0; }

        struct Range
        {
            uintptr_t begin{ 0 };
            uintptr_t end{ 0 };
            bool      contains(uintptr_t a, size_t n = 1) const { return a >= begin && a < end && n <= end - a; }
        };

        template <typename F>
        void ForChunks(uintptr_t begin, uintptr_t end, size_t overlap, F&& f)
        {
            constexpr size_t     kChunk = 1 << 20;
            std::vector<uint8_t> buffer(kChunk);
            for (uintptr_t p = begin; p < end;) {
                const size_t n = static_cast<size_t>(std::min<uintptr_t>(kChunk, end - p));
                if (SafeRead(buffer.data(), p, n)) {
                    f(buffer.data(), n, p);
                }
                if (n < kChunk || n <= overlap) {
                    break;
                }
                p += n - overlap;
            }
        }

        // ---- The game's image ----

        struct Image
        {
            uintptr_t            base{ 0 };
            size_t               size{ 0 };
            Range                text{}, rdata{}, data{};
            std::string          sections{};
            std::vector<uint8_t> rdata_copy{};
        };
        Image g_image{};

        bool InImage(uintptr_t a) { return a >= g_image.base && a < g_image.base + g_image.size; }

        // (fix190) A place of the game by its 1.16.236 rva, for this build's game version (GameCode.h): its address (0 when not known for
        // it: the dump then leaves it out, as it is not in the game's code).
        uintptr_t At(uintptr_t rva) { return GameCode::At(rva); }

        bool ReadImage(Image& image)
        {
            image.base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            IMAGE_DOS_HEADER dos{};
            if (!image.base || !Read(image.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
                return false;
            }
            IMAGE_NT_HEADERS64 nt{};
            if (!Read(image.base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
                return false;
            }
            image.size            = nt.OptionalHeader.SizeOfImage;
            const uintptr_t first = image.base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
            for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
                IMAGE_SECTION_HEADER s{};
                if (!Read(first + i * sizeof(s), s)) {
                    return false;
                }
                char name[9]{};
                std::memcpy(name, s.Name, 8);
                const Range r{ image.base + s.VirtualAddress, image.base + s.VirtualAddress + std::max<DWORD>(s.Misc.VirtualSize, 1) };
                image.sections += fmt::format(" {} {:x}-{:x}", name, r.begin - image.base, r.end - image.base);
                if (std::strcmp(name, ".text") == 0) {
                    image.text = r;
                } else if (std::strcmp(name, ".rdata") == 0) {
                    image.rdata = r;
                } else if (std::strcmp(name, ".data") == 0) {
                    image.data = r;
                }
            }
            if (image.text.end <= image.text.begin || image.rdata.end <= image.rdata.begin || image.data.end <= image.data.begin) {
                return false;
            }
            image.rdata_copy.resize(image.rdata.end - image.rdata.begin);
            ForChunks(image.rdata.begin, image.rdata.end, 0,
                [&](const uint8_t* b, size_t n, uintptr_t p) { std::memcpy(image.rdata_copy.data() + (p - image.rdata.begin), b, n); });
            return true;
        }

        // The game's function (by its unwind table) that contains an address: [begin, end).
        bool FunctionRange(uintptr_t address, uintptr_t& begin, uintptr_t& end)
        {
            DWORD64 image_base = 0;
            const auto entry   = RtlLookupFunctionEntry(address, &image_base, nullptr);
            if (entry == nullptr || image_base != g_image.base) {
                return false;
            }
            begin = g_image.base + entry->BeginAddress;
            end   = g_image.base + entry->EndAddress;
            return end > begin;
        }

        // MSVC x64 RTTI.
        struct ObjectLocator
        {
            uint32_t signature;
            uint32_t offset;
            uint32_t cd_offset;
            int32_t  type_descriptor;
            int32_t  class_descriptor;
            int32_t  self;
        };

        // ".?AVb@a@@" -> "a::b"; templates and lambdas are left as they are.
        std::string ShortName(std::string_view name)
        {
            if (name.starts_with(".?AV") || name.starts_with(".?AU")) {
                name.remove_prefix(4);
            }
            if (name.ends_with("@@")) {
                name.remove_suffix(2);
            }
            if (name.find_first_of("?$<") != std::string_view::npos) {
                return std::string{ name };
            }
            std::vector<std::string_view> parts;
            for (size_t from = 0;;) {
                const size_t at = name.find('@', from);
                parts.push_back(name.substr(from, at == std::string_view::npos ? std::string_view::npos : at - from));
                if (at == std::string_view::npos) {
                    break;
                }
                from = at + 1;
            }
            std::string out;
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                if (!out.empty()) {
                    out += "::";
                }
                out += *it;
            }
            return out;
        }

        std::unordered_map<uintptr_t, std::string> g_type_names{}; // type table -> its type's name ("" none)

        std::string TableName(uintptr_t vtable)
        {
            if (const auto it = g_type_names.find(vtable); it != g_type_names.end()) {
                return it->second;
            }
            std::string   name;
            uintptr_t     locator = 0;
            ObjectLocator l{};
            if (vtable >= 8 && g_image.rdata.contains(vtable - 8, 16) && Read(vtable - 8, locator) && g_image.rdata.contains(locator, sizeof(l)) &&
                Read(locator, l) && l.signature == 1 && l.self == static_cast<int32_t>(locator - g_image.base)) {
                char raw[256]{};
                if (ReadSome(raw, g_image.base + static_cast<uint32_t>(l.type_descriptor) + 0x10, sizeof(raw) - 1) > 4) {
                    name = ShortName(raw);
                    if (l.offset != 0) {
                        name += fmt::format(" (at +{:x} in its object)", l.offset);
                    }
                }
            }
            if (g_type_names.size() < 100'000) {
                g_type_names.emplace(vtable, name);
            }
            return name;
        }

        // The type of a C++ object with virtual functions (by its type table's RTTI), else "".
        std::string TypeOf(uintptr_t object)
        {
            uintptr_t vtable = 0;
            if (!Pointer(object) || !Read(object, vtable)) {
                return {};
            }
            return TableName(vtable);
        }

        std::string Rva(uintptr_t a) { return InImage(a) ? fmt::format("{:x}", a - g_image.base) : fmt::format("{:x} (not in the game)", a); }

        // ---- The dump file (SFVRDMP1: header, then records: 1 code at its rva, 2 memory at its address, 3 note) ----

        class Dump
        {
        public:
            static constexpr size_t kMaxFunction  = 0x3000;
            static constexpr size_t kLeafWindow   = 0x100; // a function without an unwind entry (a leaf): this much from its start
            static constexpr int    kMaxFunctions = 9000;
            static constexpr size_t kCodeBudget   = 24u << 20;
            static constexpr size_t kDataBudget   = 4u << 20;

            bool Open(const std::filesystem::path& path)
            {
                m_out.open(path, std::ios::binary | std::ios::trunc);
                if (!m_out) {
                    return false;
                }
                m_out.write("SFVRDMP1", 8);
                const uint64_t base = g_image.base, size = g_image.size;
                m_out.write(reinterpret_cast<const char*>(&base), 8);
                m_out.write(reinterpret_cast<const char*>(&size), 8);
                return true;
            }

            void Note(const std::string& text) { Record(3, "note", 0, text.data(), text.size()); }

            // The function containing `address` (by the unwind table, at most kMaxFunction bytes), saved once; its start and
            // code for the caller (false: not a function, or over budget the first time).
            bool Code(const std::string& label, uintptr_t address, uintptr_t* begin_out = nullptr, std::vector<uint8_t>* code_out = nullptr)
            {
                if (!g_image.text.contains(address)) {
                    return false;
                }
                uintptr_t   begin = 0, end = 0;
                const char* leaf  = "";
                if (!FunctionRange(address, begin, end)) {
                    begin = address;
                    end   = address + kLeafWindow;
                    leaf  = " (no unwind entry: a window from here)";
                }
                end = std::min<uintptr_t>({ end, begin + kMaxFunction, g_image.text.end });
                std::vector<uint8_t> code(end - begin);
                if (!SafeRead(code.data(), begin, code.size())) {
                    return false;
                }
                if (!m_code.contains(begin)) {
                    if (m_functions >= kMaxFunctions || m_code_bytes + code.size() > kCodeBudget) {
                        ++m_skipped;
                        return false;
                    }
                    m_code.insert(begin);
                    ++m_functions;
                    m_code_bytes += code.size();
                    Record(1, label + leaf, begin - g_image.base, reinterpret_cast<const char*>(code.data()), code.size());
                }
                if (begin_out != nullptr) {
                    *begin_out = begin;
                }
                if (code_out != nullptr) {
                    *code_out = std::move(code);
                }
                return true;
            }

            // A stretch of code from `address` (a function whose unwind entry covers only its first part, its body following in
            // chained parts), saved after anything found by the unwind table at the same start (the reader keeps the last).
            void Window(const std::string& label, uintptr_t address, size_t size)
            {
                if (!g_image.text.contains(address) || m_code_bytes + size > kCodeBudget) {
                    return;
                }
                size = std::min<size_t>(size, g_image.text.end - address);
                std::vector<uint8_t> code(size);
                if (!SafeRead(code.data(), address, code.size())) {
                    return;
                }
                m_code_bytes += code.size();
                Record(1, label + " (a window from here)", address - g_image.base, reinterpret_cast<const char*>(code.data()), code.size());
            }

            // Memory as it is now (as much as can be read).
            void Data(const std::string& label, uintptr_t address, size_t size)
            {
                if (m_data_bytes + size > kDataBudget) {
                    return;
                }
                std::vector<char> raw(size);
                const size_t      got = ReadSome(raw.data(), address, size);
                if (got == 0) {
                    return;
                }
                m_data_bytes += got;
                Record(2, label, address, raw.data(), got);
            }

            void Flush() { m_out.flush(); }

            int    functions() const { return m_functions; }
            int    skipped() const { return m_skipped; }
            size_t code_bytes() const { return m_code_bytes; }

        private:
            void Record(uint32_t kind, const std::string& label, uint64_t where, const char* data, size_t size)
            {
                const auto label_size = static_cast<uint32_t>(label.size());
                const auto data_size  = static_cast<uint32_t>(size);
                m_out.write(reinterpret_cast<const char*>(&kind), 4);
                m_out.write(reinterpret_cast<const char*>(&label_size), 4);
                m_out.write(label.data(), label_size);
                m_out.write(reinterpret_cast<const char*>(&where), 8);
                m_out.write(reinterpret_cast<const char*>(&data_size), 4);
                m_out.write(data, data_size);
            }

            std::ofstream                 m_out{};
            std::unordered_set<uintptr_t> m_code{};
            int                           m_functions{ 0 };
            int                           m_skipped{ 0 };
            size_t                        m_code_bytes{ 0 };
            size_t                        m_data_bytes{ 0 };
        };

        // What a function calls (a call or jump to the start of another function).
        std::vector<uintptr_t> Callees(const std::vector<uint8_t>& code, uintptr_t begin)
        {
            std::vector<uintptr_t> out;
            for (size_t i = 0; i + 5 <= code.size(); ++i) {
                if (code[i] != 0xE8 && code[i] != 0xE9) {
                    continue;
                }
                int32_t rel = 0;
                std::memcpy(&rel, &code[i + 1], sizeof(rel));
                const uintptr_t target = begin + i + 5 + static_cast<intptr_t>(rel);
                if (!g_image.text.contains(target) || (target >= begin && target < begin + code.size())) {
                    continue;
                }
                uintptr_t  b = 0, e = 0;
                uint8_t    before = 0;
                const bool start  = FunctionRange(target, b, e) ? b == target
                                                                 : (target & 0xF) == 0 && Read(target - 1, before) && (before == 0xCC || before == 0xC3 || before == 0x90);
                if (!start || std::find(out.begin(), out.end(), target) != out.end()) {
                    continue;
                }
                out.push_back(target);
            }
            return out;
        }

        // A function and what it calls, breadth first, `depth` levels down, at most `cap` functions.
        void Expand(Dump& dump, const std::string& label, uintptr_t root, int depth, int cap)
        {
            struct Item
            {
                uintptr_t   address;
                int         level;
                std::string label;
            };
            std::deque<Item>              queue{ { root, 0, label } };
            std::unordered_set<uintptr_t> seen{};
            int                           visited = 0;
            while (!queue.empty() && visited < cap) {
                Item item = std::move(queue.front());
                queue.pop_front();
                uintptr_t            begin = 0;
                std::vector<uint8_t> code;
                if (!dump.Code(item.label, item.address, &begin, &code) || !seen.insert(begin).second) {
                    continue;
                }
                ++visited;
                if (item.level >= depth) {
                    continue;
                }
                for (const uintptr_t callee : Callees(code, begin)) {
                    if (!seen.contains(callee)) {
                        queue.push_back({ callee, item.level + 1, fmt::format("{} > {:x}", item.label, callee - g_image.base) });
                    }
                }
            }
        }

        // One pass over the game's code: where one of `tables` is loaded (lea r64,[rip+x]: constructors, code that makes
        // that object) and where one of `functions` is called (call rel32). Instruction addresses, at most 64 each.
        void ScanText(const std::unordered_set<uintptr_t>& tables, const std::unordered_set<uintptr_t>& functions,
            std::unordered_map<uintptr_t, std::vector<uintptr_t>>& loads, std::unordered_map<uintptr_t, std::vector<uintptr_t>>& calls)
        {
            auto add = [](std::vector<uintptr_t>& list, uintptr_t at) {
                if (list.size() < 64 && std::find(list.begin(), list.end(), at) == list.end()) {
                    list.push_back(at);
                }
            };
            ForChunks(g_image.text.begin, g_image.text.end, 16, [&](const uint8_t* b, size_t n, uintptr_t p) {
                for (size_t i = 0; i + 7 <= n; ++i) {
                    if (b[i] == 0xE8 && !functions.empty()) {
                        int32_t rel = 0;
                        std::memcpy(&rel, b + i + 1, sizeof(rel));
                        const uintptr_t t = p + i + 5 + static_cast<intptr_t>(rel);
                        if (functions.contains(t)) {
                            add(calls[t], p + i);
                        }
                    }
                    if ((b[i] == 0x48 || b[i] == 0x4C) && b[i + 1] == 0x8D && (b[i + 2] & 0xC7) == 0x05 && !tables.empty()) {
                        int32_t rel = 0;
                        std::memcpy(&rel, b + i + 3, sizeof(rel));
                        const uintptr_t t = p + i + 7 + static_cast<intptr_t>(rel);
                        if (tables.contains(t)) {
                            add(loads[t], p + i);
                        }
                    }
                }
            });
        }

        // The functions containing some instructions, each once (in a leaf function, without an unwind entry, a little before
        // the instruction stands in for its start).
        std::vector<uintptr_t> Containing(const std::vector<uintptr_t>& sites, size_t cap)
        {
            std::vector<uintptr_t> out;
            for (const uintptr_t at : sites) {
                uintptr_t b = 0, e = 0;
                if (!FunctionRange(at, b, e)) {
                    b = std::max<uintptr_t>(g_image.text.begin, at - 0x40);
                }
                if (std::find(out.begin(), out.end(), b) == out.end()) {
                    out.push_back(b);
                    if (out.size() >= cap) {
                        break;
                    }
                }
            }
            return out;
        }

        std::string List(const std::vector<uintptr_t>& functions)
        {
            std::string text;
            for (const uintptr_t f : functions) {
                text += " " + Rva(f);
            }
            return text.empty() ? std::string{ " none" } : text;
        }

        // ---- The physics types (static) ----

        enum class Kind
        {
            kNone,
            kCommand, // the physics command queue's commands: their code two levels down, who queues them and their callers
            kType,    // the main physics types: their functions and what those call, constructors
            kMinor,   // step listeners, filter templates, script functors: their functions, who makes them
        };

        // Main types by exact name (namespaced ones match too: ".?AVname@outer@@").
        constexpr const char* kTypes[] = { "bhkWorld", "bhkWorldM", "hknpBSWorld", "hknpWorld", "bhkNPCollisionObject", "bhkNPCollisionObjectUnlinked",
            "bhkNPCollisionProxyObject", "bhkPhysicsSystem", "hknpPhysicsSystem", "bhkCharacterController", "bhkCharProxyController",
            "bhkCharRigidBodyController", "bhkCharacterProxy", "bhkCharacterRigidBody", "hknpBSCharacterProxy", "hknpBSCharacterRigidBody",
            "hknpCharacterProxy", "hknpCharacterRigidBody", "bhkCollisionFilter", "hknpCollisionFilter", "hknpGroupCollisionFilter",
            "hknpPairCollisionFilter", "hknpBSCustomCollisionFilter", "hknpBSIgnoreBodyCollisionFilter", "hknpCapsuleShape", "hknpSphereShape",
            "hknpBoxShape", "hknpConvexShape", "hknpBSMouseSpringAction", "hknpAction", "hknpUnaryAction", "hknpSpringAction", "hknpActionManager",
            "bhkIWorldStepListener", "hknpApiCommandProcessor", "hknpInternalCommandProcessor", "hknpBSCommandStreamWriter",
            "hknpBSMotionPropertiesLibrary", "hknpMotionPropertiesLibrary", "hknpShapeManager" };
        constexpr const char* kMinorKeys[] = { "WorldStepListener", "hknpGroupCollisionFilterBase", "ApplyHavokImpulse", "SetMotionTypeFunctor",
            // Ray and shape casts (v3): code that makes a hit collector on its stack calls the cast with it (for a gun sight that
            // sits on what the gun points at).
            "ClosestHitCollector", "AllHitsCollector", "AnyHitCollector", "LOSCollector" };

        // A collector's users: the function and what it calls (the cast).
        bool IsCollector(const std::string& name) { return name.find("Collector") != std::string::npos; }

        Kind KindOf(std::string_view raw)
        {
            static const std::vector<std::string> starts = [] {
                std::vector<std::string> out;
                for (const auto* name : kTypes) {
                    out.push_back(std::string{ ".?AV" } + name + "@");
                    out.push_back(std::string{ ".?AU" } + name + "@");
                }
                return out;
            }();
            if (raw.find("bhkCommandQueue") != std::string_view::npos) {
                return Kind::kCommand;
            }
            for (const auto& start : starts) {
                if (raw.starts_with(start)) {
                    return Kind::kType;
                }
            }
            for (const auto* key : kMinorKeys) {
                if (raw.find(key) != std::string_view::npos) {
                    return Kind::kMinor;
                }
            }
            return Kind::kNone;
        }

        // A command's own name inside its handler's mangled name (the last "bhk..." part that is not the queue).
        std::string CommandName(std::string_view raw)
        {
            std::string best;
            for (size_t at = raw.find("bhk"); at != std::string_view::npos; at = raw.find("bhk", at + 3)) {
                const size_t          end  = raw.find_first_of("@<>", at);
                const std::string_view part = raw.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
                if (part != "bhkCommandQueue") {
                    best = std::string{ part };
                }
            }
            return best.empty() ? ShortName(raw) : best;
        }

        // Functions the first physics test build pointed at, for what comes next (hand bodies, drawing held objects): saved
        // deeper, and with who calls them where asked.
        struct Target
        {
            uintptr_t   rva;
            const char* what;
            int         depth;
            int         cap;
            bool        callers;
        };
        constexpr Target kTargets[] = {
            { 0x2be93f0, "BSModelNode::UpdateTransforms (the model update the shown body hooks)", 1, 24, true },
            { 0x2378b10, "bhkNPCollisionObject[41]", 2, 40, false },
            { 0x2378d10, "bhkNPCollisionObject[43]", 2, 40, false },
            { 0x2378df0, "bhkNPCollisionObject[44]", 2, 40, false },
            { 0x2bfc590, "bhkNPCollisionObject[46]", 2, 40, false },
            { 0x2bfc490, "bhkNPCollisionObject[55]", 2, 40, false },
            { 0x23781c0, "bhkNPCollisionObject[56]", 2, 60, false },
            { 0x2379c20, "bhkNPCollisionObject[60]", 2, 40, false },
            { 0x2378fd0, "bhkNPCollisionObject[61]", 2, 40, false },
            { 0x2379140, "bhkNPCollisionObject[62]", 2, 40, false },
            { 0x23e3b80, "the code that queues bhkAddBodiesCommand", 1, 24, true },
            { 0x22371e0, "a caller of the add-bodies queue (22371e0)", 2, 60, false },
            { 0x23640f0, "a caller of the add-bodies queue (23640f0)", 2, 60, false },
            { 0x23a54c0, "a caller of the add-bodies queue (23a54c0)", 2, 60, false },
            { 0x23c9e70, "a caller of the add-bodies queue (23c9e70)", 2, 60, false },
            { 0x2361130, "the world's set-motion-type helper (2361130)", 2, 40, true },
            { 0x23605e0, "the world's set-velocity helper (23605e0)", 2, 40, true },
            { 0x2360d80, "the world's apply-hard-keyframe helper (2360d80)", 2, 40, true },
            { 0x23602d0, "the world's set-transform helper (23602d0)", 2, 40, true },
            { 0x2360410, "the world's set-position helper (2360410)", 1, 20, false },
            { 0x3086e40, "hknpWorld's set-collision-filter-info (3086e40)", 1, 20, false },
            { 0x239b500, "the collision filter's pair test (239b500)", 0, 1, true },
            // Drawing held objects in the hand (the hand grab hooks the scene writer): the writer, who calls it, how the
            // collision object's world update gets the body's pose (the world's +0x88 and +0x90, and the check before), and its
            // other way, a node moving its body (237a890).
            { 0x237cb20, "the scene writer (237cb20, hooked by the hand grab: its first bytes are the hook's jump, the game's are 4C 8B DC 55 48 8D 6C 24 80 48 81 EC 80 01 00 00)", 1, 40, true },
            { 0x1027b70, "the check before the predicted pose (1027b70)", 1, 20, false },
            { 0x3062ac0, "hknpBSWorld type table +0x90 (the predicted pose, 3062ac0)", 1, 30, false },
            { 0x236aff0, "hknpBSWorld type table +0x70/+0x88 (a body, 236aff0)", 0, 1, false },
            { 0x237a890, "a node moving its body (237a890)", 1, 40, false },
            { 0x2bd9330, "NiAVObject::UpdateWorldData (2bd9330, hooked)", 1, 20, true },
        };
        // Functions whose unwind entry covers only their start: saved whole as windows.
        struct CodeWindow
        {
            uintptr_t   rva;
            size_t      size;
            const char* what;
        };
        constexpr CodeWindow kWindows[] = {
            { 0x237cb20, 0x600, "the scene writer (237cb20, hooked: see above)" },
            { 0x2bd9330, 0x200, "NiAVObject::UpdateWorldData (2bd9330)" },
        };

        // (fix190) A place of a list of places (the tables above, ConsumeScan's roots) by its 1.16.236 rva, as At. The port tool does not
        // read such lists: each of their places is also given to a GameCode::Known where the list is used, which puts it in the port table.
        uintptr_t TableAt(uintptr_t rva) { return GameCode::At(rva); }

        void TargetScan(Dump& dump)
        {
            // (fix190) The tables' places, for the port tool (see TableAt): one not known in this game version is left out of the dump.
            if (!GameCode::Known({ 0x2be93f0, 0x2378b10, 0x2378d10, 0x2378df0, 0x2bfc590, 0x2bfc490, 0x23781c0, 0x2379c20, 0x2378fd0, 0x2379140, 0x23e3b80,
                    0x22371e0, 0x23640f0, 0x23a54c0, 0x23c9e70, 0x2361130, 0x23605e0, 0x2360d80, 0x23602d0, 0x2360410, 0x3086e40, 0x239b500, 0x237cb20,
                    0x1027b70, 0x3062ac0, 0x236aff0, 0x237a890, 0x2bd9330 })) {
                spdlog::info("[Physics] (fix190) some of the functions to save are not known in this game version (GameCode): left out");
            }
            std::unordered_set<uintptr_t> wanted;
            for (const auto& t : kTargets) {
                const uintptr_t at = TableAt(t.rva);
                Expand(dump, t.what, at, t.depth, t.cap);
                if (t.callers) {
                    wanted.insert(at);
                }
            }
            std::unordered_map<uintptr_t, std::vector<uintptr_t>> loads, calls;
            ScanText({}, wanted, loads, calls);
            for (const auto& t : kTargets) {
                if (!t.callers) {
                    continue;
                }
                const uintptr_t at      = TableAt(t.rva);
                const auto      it      = calls.find(at);
                const auto      callers = it == calls.end() ? std::vector<uintptr_t>{} : Containing(it->second, 24);
                for (const uintptr_t c : callers) {
                    Expand(dump, fmt::format("calls {}", t.what), c, 1, 6);
                }
                spdlog::info("[Physics] {} is called by:{}", t.what, List(callers));
                dump.Note(fmt::format("{} ({:x}) is called by:{}", t.what, t.rva, List(callers)));
            }
            for (const auto& w : kWindows) {
                dump.Window(w.what, TableAt(w.rva), w.size);
            }
        }

        struct Table
        {
            std::string            name;
            Kind                   kind{ Kind::kNone };
            uintptr_t              vtable{ 0 };
            uint32_t               offset{ 0 };
            std::vector<uintptr_t> functions{};
        };

        void StaticScan(Dump& dump)
        {
            // Type names (RTTI type descriptors keep their name 0x10 bytes in).
            std::unordered_map<int32_t, std::pair<std::string, Kind>> types;
            ForChunks(g_image.data.begin, g_image.data.end, 512, [&](const uint8_t* b, size_t n, uintptr_t p) {
                for (size_t i = 0; i + 16 <= n; ++i) {
                    if (b[i] != '.' || b[i + 1] != '?' || b[i + 2] != 'A') {
                        continue;
                    }
                    const auto*  text = reinterpret_cast<const char*>(b + i);
                    const size_t len  = strnlen(text, n - i);
                    if (len >= n - i || len > 480) {
                        continue;
                    }
                    const std::string_view raw{ text, len };
                    if (const Kind kind = KindOf(raw); kind != Kind::kNone) {
                        types.emplace(static_cast<int32_t>(p + i - 0x10 - g_image.base), std::make_pair(std::string{ raw }, kind));
                    }
                }
            });

            // Their object locators, then the type tables (vtables) that point at those.
            const auto& rdata = g_image.rdata_copy;
            std::unordered_map<uintptr_t, std::pair<int32_t, uint32_t>> locators; // locator -> (type, object offset)
            for (size_t off = 0; off + sizeof(ObjectLocator) <= rdata.size(); off += 4) {
                ObjectLocator locator{};
                std::memcpy(&locator, rdata.data() + off, sizeof(locator));
                if (locator.signature != 1) {
                    continue;
                }
                const uintptr_t at = g_image.rdata.begin + off;
                if (locator.self != static_cast<int32_t>(at - g_image.base) || !types.contains(locator.type_descriptor)) {
                    continue;
                }
                locators.emplace(at, std::make_pair(locator.type_descriptor, locator.offset));
            }
            std::vector<Table> tables;
            for (size_t off = 0; off + 16 <= rdata.size(); off += 8) {
                const auto it = locators.find(static_cast<uintptr_t>(U64(rdata.data(), off)));
                if (it == locators.end()) {
                    continue;
                }
                const auto& [raw, kind] = types[it->second.first];
                Table table{ kind == Kind::kCommand ? CommandName(raw) : ShortName(raw), kind, g_image.rdata.begin + off + 8, it->second.second, {} };
                for (size_t e = 0; e < 400; ++e) {
                    const size_t entry = off + 8 + e * 8;
                    if (entry + 8 > rdata.size()) {
                        break;
                    }
                    const auto fn = static_cast<uintptr_t>(U64(rdata.data(), entry));
                    if (!g_image.text.contains(fn)) {
                        break;
                    }
                    table.functions.push_back(fn);
                }
                tables.push_back(std::move(table));
            }
            int counts[4]{};
            for (const auto& t : tables) {
                ++counts[static_cast<int>(t.kind)];
            }
            spdlog::info("[Physics] {} physics type names; type tables: {} commands of the physics command queue, {} main physics types, {} others", types.size(),
                counts[1], counts[2], counts[3]);

            // Every table into the dump; the main types and the commands into the log too.
            for (const auto& t : tables) {
                std::string list;
                for (size_t e = 0; e < t.functions.size(); ++e) {
                    list += fmt::format(" [{}]{:x}", e, t.functions[e] - g_image.base);
                }
                const char* what = t.kind == Kind::kCommand ? "command" : (t.kind == Kind::kType ? "type" : "other");
                dump.Note(fmt::format("{} {} vtable rva {:x} (object offset {:x}):{}", what, t.name, t.vtable - g_image.base, t.offset, list));
                if (t.kind == Kind::kType) {
                    spdlog::info("[Physics] {} type table at {:x}{}: {} functions:{}", t.name, t.vtable - g_image.base,
                        t.offset ? fmt::format(" (object offset {:x})", t.offset) : "", t.functions.size(), list);
                } else if (t.kind == Kind::kCommand) {
                    spdlog::info("[Physics] command {} type table at {:x}: {}", t.name, t.vtable - g_image.base, list);
                }
            }

            // Code: the commands first (two levels of what they call), then the main types, then the others.
            for (const Kind kind : { Kind::kCommand, Kind::kType, Kind::kMinor }) {
                for (const auto& t : tables) {
                    if (t.kind != kind) {
                        continue;
                    }
                    for (size_t e = 0; e < t.functions.size(); ++e) {
                        const std::string label = fmt::format("{}[{}]", t.name, e);
                        if (kind == Kind::kCommand) {
                            Expand(dump, label, t.functions[e], 2, 30);
                        } else if (kind == Kind::kType) {
                            Expand(dump, label, t.functions[e], 1, 8);
                        } else {
                            Expand(dump, label, t.functions[e], 0, 1);
                        }
                    }
                }
                dump.Flush();
            }

            // Who uses each table (constructors; for a command, the code that queues it), and who calls those.
            std::unordered_set<uintptr_t> vtables;
            for (const auto& t : tables) {
                vtables.insert(t.vtable);
            }
            std::unordered_map<uintptr_t, std::vector<uintptr_t>> loads, unused;
            ScanText(vtables, {}, loads, unused);
            std::unordered_set<uintptr_t>                        queue_functions;
            std::unordered_map<uintptr_t, std::vector<uintptr_t>> users;
            for (const auto& t : tables) {
                const auto it = loads.find(t.vtable);
                if (it == loads.end()) {
                    continue;
                }
                const auto list = Containing(it->second, t.kind == Kind::kCommand ? 12 : 6);
                users[t.vtable] = list;
                for (const uintptr_t f : list) {
                    const bool collector = t.kind == Kind::kMinor && IsCollector(t.name);
                    Expand(dump, fmt::format("uses {} type table", t.name), f, t.kind == Kind::kMinor && !collector ? 0 : 1,
                        collector ? 12 : (t.kind == Kind::kMinor ? 1 : 8));
                    if (t.kind == Kind::kCommand) {
                        queue_functions.insert(f);
                    }
                }
                dump.Note(fmt::format("{} type table {:x} used by:{}", t.name, t.vtable - g_image.base, List(list)));
            }
            dump.Flush();
            std::unordered_map<uintptr_t, std::vector<uintptr_t>> calls;
            ScanText({}, queue_functions, unused, calls);
            for (const auto& t : tables) {
                if (t.kind != Kind::kCommand || !users.contains(t.vtable)) {
                    continue;
                }
                std::string text;
                for (const uintptr_t f : users[t.vtable]) {
                    const auto it      = calls.find(f);
                    const auto callers = it == calls.end() ? std::vector<uintptr_t>{} : Containing(it->second, 6);
                    for (const uintptr_t c : callers) {
                        dump.Code(fmt::format("calls the code that queues {}", t.name), c);
                    }
                    text += fmt::format(" {} (called by{})", Rva(f), List(callers));
                }
                spdlog::info("[Physics] command {} is queued by:{}", t.name, text);
                dump.Note(fmt::format("{} queued by:{}", t.name, text));
            }
            dump.Flush();
            TargetScan(dump);
            dump.Flush();
            spdlog::info("[Physics] saved the code of {} game functions ({:.1f} MB{})", dump.functions(), static_cast<double>(dump.code_bytes()) / (1 << 20),
                dump.skipped() > 0 ? fmt::format(", {} more over the limit", dump.skipped()) : "");
        }

        // ---- Script functions (v3): what the next builds call to equip, unequip, drop, put away and place things ----

        // Papyrus functions by name, with the script type they belong to. The game registers each one with its name, its type's
        // name and its code side by side; that code calls the game's own function (equipping, dropping...), saved two levels down.
        struct NativeName
        {
            const char* name;
            const char* type;
            const char* why;
        };
        constexpr NativeName kNatives[] = {
            { "EquipItem", "Actor", "equip a weapon you grabbed when you fire or swing it" },
            { "UnequipItem", "Actor", "put it back and take your own weapon again" },
            { "GetEquippedWeapon", "Actor", "which weapon is in your hands" },
            { "DropObject", "ObjectReference", "drop a weapon from your inventory to throw it" },
            { "RemoveItem", "ObjectReference", "take an item out of an inventory" },
            { "AddItem", "ObjectReference", "a thrown weapon coming back to you" },
            { "Activate", "ObjectReference", "put what you hold in your bag (as HIGGS does)" },
            { "PlaceAtMe", "ObjectReference", "make an object in the world" },
            { "ApplyHavokImpulse", "ObjectReference", "give a thrown object its speed" },
            { "SetMotionType", "ObjectReference", "make a thrown weapon a free physics object" },
        };

        // Where a string (whole, with its ending zero, and not the end of a longer one) is in the game's constants.
        std::vector<uintptr_t> FindStrings(const char* text, size_t cap)
        {
            std::vector<uintptr_t> out;
            const auto&            rdata = g_image.rdata_copy;
            const size_t           n     = std::strlen(text) + 1; // with the zero
            for (size_t at = 1; at + n <= rdata.size() && out.size() < cap; ++at) {
                if (rdata[at] == static_cast<uint8_t>(text[0]) && rdata[at - 1] == 0 && std::memcmp(rdata.data() + at, text, n) == 0) {
                    out.push_back(g_image.rdata.begin + at);
                }
            }
            return out;
        }

        void NativeScan(Dump& dump)
        {
            const int64_t start = NowUs();
            std::unordered_map<std::string, std::vector<uintptr_t>> names, types;
            std::unordered_set<uintptr_t>                           wanted;
            for (const auto& n : kNatives) {
                names[n.name] = FindStrings(n.name, 8);
                if (!types.contains(n.type)) {
                    types[n.type] = FindStrings(n.type, 32);
                }
            }
            for (const auto& [_, list] : names) {
                wanted.insert(list.begin(), list.end());
            }
            for (const auto& [_, list] : types) {
                wanted.insert(list.begin(), list.end());
            }
            std::unordered_map<uintptr_t, std::vector<uintptr_t>> loads, unused;
            ScanText(wanted, {}, loads, unused);

            // A rip-relative lea at `i` of `b` (code at `p`): where it points.
            const auto lea_target = [](const uint8_t* b, size_t i, size_t n, uintptr_t p, uintptr_t& target) {
                if (i + 7 > n || (b[i] != 0x48 && b[i] != 0x4C) || b[i + 1] != 0x8D || (b[i + 2] & 0xC7) != 0x05) {
                    return false;
                }
                int32_t rel = 0;
                std::memcpy(&rel, b + i + 3, sizeof(rel));
                target = p + i + 7 + static_cast<intptr_t>(rel);
                return true;
            };
            constexpr size_t kAround = 0x80;
            for (const auto& n : kNatives) {
                const auto& type_strings = types[n.type];
                std::string            report;
                int                    registrations = 0;
                std::vector<uintptr_t> untyped; // the name loaded with no type name next to it (kept in case none has one)
                for (const uintptr_t name : names[n.name]) {
                    const auto it = loads.find(name);
                    if (it == loads.end() || registrations >= 2) {
                        continue;
                    }
                    for (const uintptr_t site : it->second) {
                        const uintptr_t from = std::max(g_image.text.begin, site - kAround);
                        const uintptr_t to   = std::min(g_image.text.end, site + kAround);
                        std::vector<uint8_t> code(to - from);
                        if (!SafeRead(code.data(), from, code.size())) {
                            continue;
                        }
                        // This one registration: from the call before the name to the call after it (the code side by side in
                        // registration after registration belongs to its neighbours), at most 0x60 bytes either way.
                        const size_t site_at = static_cast<size_t>(site - from);
                        size_t       first   = site_at > 0x60 ? site_at - 0x60 : 0;
                        size_t       last    = std::min(code.size(), site_at + 0x60);
                        for (size_t i = site_at; i-- > first;) {
                            if (code[i] == 0xE8 && i + 5 <= site_at) {
                                first = i + 5;
                                break;
                            }
                        }
                        for (size_t i = site_at + 7; i + 5 <= last; ++i) {
                            if (code[i] == 0xE8) {
                                last = i + 5;
                                break;
                            }
                        }
                        // Its script type's name may be loaded for a call of its own just before: looked for 0x60 bytes either way.
                        // (If nothing is found between the calls, a byte that only looked like a call cut it short: the whole
                        // 0x60 bytes either way.)
                        bool                   typed = false;
                        std::vector<uintptr_t> functions, nearby;
                        for (size_t i = site_at > 0x60 ? site_at - 0x60 : 0; i < std::min(code.size(), site_at + 0x60); ++i) {
                            uintptr_t target = 0;
                            if (!lea_target(code.data(), i, code.size(), from, target)) {
                                continue;
                            }
                            if (std::find(type_strings.begin(), type_strings.end(), target) != type_strings.end()) {
                                typed = true;
                            } else if (g_image.text.contains(target)) {
                                auto& list = i >= first && i < last ? functions : nearby;
                                if (std::find(list.begin(), list.end(), target) == list.end()) {
                                    list.push_back(target);
                                }
                            }
                        }
                        if (functions.empty()) {
                            functions = std::move(nearby);
                        }
                        if (!typed) {
                            if (untyped.size() < 6) {
                                untyped.push_back(site); // the name used for something else (an event, an animation), or registered another way
                            }
                            continue;
                        }
                        ++registrations;
                        const std::string label = fmt::format("{}.{} ({}): registered at {}", n.type, n.name, n.why, Rva(site));
                        dump.Window(label, from, code.size());
                        for (const uintptr_t f : functions) {
                            Expand(dump, fmt::format("{}.{}: its code {}", n.type, n.name, Rva(f)), f, 2, 25);
                        }
                        report += fmt::format(" registered at {} with code{}", Rva(site), List(functions));
                        if (registrations >= 2) {
                            break;
                        }
                    }
                }
                if (registrations == 0) {
                    for (const uintptr_t site : untyped) {
                        const uintptr_t from = std::max(g_image.text.begin, site - kAround);
                        dump.Window(fmt::format("{}.{}: the name loaded at {} (no type name next to it)", n.type, n.name, Rva(site)), from,
                            std::min<uintptr_t>(g_image.text.end, site + kAround) - from);
                        dump.Code(fmt::format("{}.{}: the function loading the name at {}", n.type, n.name, Rva(site)), site);
                    }
                    if (!untyped.empty()) {
                        report = fmt::format(" no registration found; the name is loaded at{}", List(untyped));
                    }
                }
                spdlog::info("[Probe] script function {}.{} ({}):{}", n.type, n.name, n.why, report.empty() ? std::string{ " not found" } : report);
                dump.Note(fmt::format("script function {}.{}:{}", n.type, n.name, report.empty() ? std::string{ " not found" } : report));
            }
            dump.Flush();
            spdlog::info("[Probe] script functions looked up in {:.1f} s", static_cast<double>(NowUs() - start) * 1e-6);
        }

        // ---- Eating and drinking (v4): what the next build calls to eat what you let go of at your mouth ----

        // A type table (vtable) by its RTTI name (".?AVname@@"; its object offset 0), else 0. Needs the .rdata copy.
        uintptr_t FindTypeTable(std::string_view raw)
        {
            int32_t descriptor = -1;
            ForChunks(g_image.data.begin, g_image.data.end, 512, [&](const uint8_t* b, size_t n, uintptr_t p) {
                for (size_t i = 0; descriptor < 0 && i + raw.size() + 1 <= n; ++i) {
                    if (b[i] == '.' && std::memcmp(b + i, raw.data(), raw.size()) == 0 && b[i + raw.size()] == 0) {
                        descriptor = static_cast<int32_t>(p + i - 0x10 - g_image.base); // (the name is 0x10 into its type descriptor)
                    }
                }
            });
            const auto& rdata = g_image.rdata_copy;
            if (descriptor < 0 || rdata.empty()) {
                return 0;
            }
            uintptr_t locator = 0;
            for (size_t off = 0; off + sizeof(ObjectLocator) <= rdata.size() && locator == 0; off += 4) {
                ObjectLocator l{};
                std::memcpy(&l, rdata.data() + off, sizeof(l));
                const uintptr_t at = g_image.rdata.begin + off;
                if (l.signature == 1 && l.offset == 0 && l.type_descriptor == descriptor && l.self == static_cast<int32_t>(at - g_image.base)) {
                    locator = at;
                }
            }
            for (size_t off = 0; locator != 0 && off + 16 <= rdata.size(); off += 8) {
                if (static_cast<uintptr_t>(U64(rdata.data(), off)) == locator) {
                    return g_image.rdata.begin + off + 8;
                }
            }
            return 0;
        }

        // The type tables (with RTTI) a function and the functions it calls load (lea r64,[rip+x]): a task it queues, a functor it
        // passes; each table's functions are saved (with what they call).
        void FollowTables(Dump& dump, const std::string& label, uintptr_t function)
        {
            uintptr_t            begin = 0;
            std::vector<uint8_t> code;
            if (!dump.Code(label, function, &begin, &code)) {
                return;
            }
            std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> parts{ { begin, code } };
            for (const uintptr_t callee : Callees(code, begin)) {
                uintptr_t            b = 0;
                std::vector<uint8_t> c;
                if (parts.size() < 16 && dump.Code(fmt::format("{} > {:x}", label, callee - g_image.base), callee, &b, &c)) {
                    parts.emplace_back(b, std::move(c));
                }
            }
            std::unordered_set<uintptr_t> done;
            for (const auto& [at, bytes] : parts) {
                for (size_t i = 0; i + 7 <= bytes.size() && done.size() < 8; ++i) {
                    if ((bytes[i] != 0x48 && bytes[i] != 0x4C) || bytes[i + 1] != 0x8D || (bytes[i + 2] & 0xC7) != 0x05) {
                        continue;
                    }
                    int32_t rel = 0;
                    std::memcpy(&rel, bytes.data() + i + 3, sizeof(rel));
                    const uintptr_t table = at + i + 7 + static_cast<intptr_t>(rel);
                    if (!g_image.rdata.contains(table, 8) || done.contains(table)) {
                        continue;
                    }
                    const std::string name = TableName(table);
                    if (name.empty()) {
                        continue;
                    }
                    done.insert(table);
                    std::string list;
                    for (size_t e = 0; e < 12; ++e) {
                        uintptr_t fn = 0;
                        if (!Read(table + e * 8, fn) || !g_image.text.contains(fn)) {
                            break;
                        }
                        list += fmt::format(" [{}]{:x}", e, fn - g_image.base);
                        Expand(dump, fmt::format("{}: {}[{}]", label, name, e), fn, 1, 6);
                    }
                    dump.Note(fmt::format("{} ({:x}) loads the type table of {} at {:x}:{}", label, at - g_image.base, name, table - g_image.base, list));
                    spdlog::info("[Probe] {} ({:x}) loads the type table of {} at {:x}:{}", label, at - g_image.base, name, table - g_image.base, list);
                }
            }
        }

        // A type table by the start of its RTTI name (".?AVname@" ...; the rest, such as an anonymous namespace, read from the
        // game), else 0. `full` gets the whole name.
        uintptr_t FindTypeTableByPrefix(std::string_view prefix, std::string& full)
        {
            uintptr_t found = 0;
            ForChunks(g_image.data.begin, g_image.data.end, 512, [&](const uint8_t* b, size_t n, uintptr_t p) {
                for (size_t i = 0; found == 0 && i + prefix.size() <= n; ++i) {
                    if (b[i] == '.' && std::memcmp(b + i, prefix.data(), prefix.size()) == 0) {
                        found = p + i;
                    }
                }
            });
            if (found == 0) {
                return 0;
            }
            char raw[200]{};
            if (ReadSome(raw, found, sizeof(raw) - 1) < prefix.size()) {
                return 0;
            }
            full = raw;
            return FindTypeTable(full);
        }

        // The grenade shown in your hand (next build): how the game's script commands make, drop, disable and delete objects. Their
        // work is done later by a "delay functor" each queues; each functor's run (its type table slot 1) two levels down, and the
        // game's make-an-object code (PlaceAtMe's, already saved) one level down.
        void ObjectScan(Dump& dump)
        {
            const int64_t start = NowUs();
            constexpr const char* kFunctors[] = { "DropObjectFunctor", "DisableFunctor", "EnableFunctor", "DeleteFunctor", "NonLatentDeleteFunctor",
                "RemoveItemFunctor", "DropRefFunctor", "SetPositionFunctor", "MoveToFunctor" };
            for (const char* name : kFunctors) {
                std::string full;
                const std::string prefix = fmt::format(".?AV{}@", name);
                const uintptr_t table   = FindTypeTableByPrefix(prefix, full);
                if (table == 0) {
                    spdlog::info("[Probe] script functor {}: its type table was not found", name);
                    dump.Note(fmt::format("script functor {}: type table not found", name));
                    continue;
                }
                std::string list;
                for (size_t e = 0; e < 8; ++e) {
                    uintptr_t fn = 0;
                    if (!Read(table + e * 8, fn) || !g_image.text.contains(fn)) {
                        break;
                    }
                    list += fmt::format(" [{}]{:x}", e, fn - g_image.base);
                    Expand(dump, fmt::format("script functor {}[{}] (the grenade in your hand)", name, e), fn, e == 1 ? 2 : 0, e == 1 ? 40 : 1);
                }
                spdlog::info("[Probe] script functor {} ({}): type table at {:x}:{}", name, full, table - g_image.base, list);
                dump.Note(fmt::format("script functor {} ({}): type table at {:x}:{}", name, full, table - g_image.base, list));
                dump.Flush();
            }
            // Making an object (PlaceAtMe's helper 207bcc0 calls it with the data handler and the new object's data), the "delete when
            // able" mark, and the world-object removal seen in the game (a reference's type table slots 0xa7 and 0x33).
            constexpr uintptr_t kMakeObjectRva     = 0x5a10e0; // (fix190) the game's make-an-object
            constexpr uintptr_t kDeleteWhenAbleRva = 0xb3fd60; // (fix190) delete-when-able
            Expand(dump, "the game's make-an-object (5a10e0; the grenade in your hand)", At(kMakeObjectRva), 1, 30);
            Expand(dump, "delete-when-able (b3fd60; the grenade in your hand)", At(kDeleteWhenAbleRva), 1, 10);
            // What the grenade in your hand (GrenadeHand) calls without its code in a dump yet: the move script SetPosition's job makes
            // (20660f0, every frame) and the motion type script SetMotionType's job sets (235a310, once).
            constexpr uintptr_t kSetPositionMoveRva = 0x20660f0; // (fix190) the move script SetPosition's job makes
            constexpr uintptr_t kSetMotionTypeRva   = 0x235a310; // (fix190) the motion type script SetMotionType's job sets
            Expand(dump, "the move script SetPosition's job makes (20660f0; the grenade in your hand moves with it)", At(kSetPositionMoveRva), 2, 40);
            Expand(dump, "the motion type script SetMotionType's job sets (235a310; the grenade in your hand is made keyframed with it)",
                At(kSetMotionTypeRva), 1, 20);
            if (const uintptr_t table = FindTypeTable(".?AVTESObjectREFR@@"); table != 0) {
                for (const size_t slot : { size_t{ 0x33 }, size_t{ 0xa7 } }) {
                    uintptr_t fn = 0;
                    if (Read(table + slot * 8, fn) && g_image.text.contains(fn)) {
                        Expand(dump, fmt::format("TESObjectREFR[{:x}] (the grenade in your hand)", slot), fn, 1, 20);
                        spdlog::info("[Probe] TESObjectREFR type table slot {:x}: {:x}", slot, fn - g_image.base);
                    }
                }
            }
            dump.Flush();
            spdlog::info("[Probe] making and removing objects looked up in {:.1f} s", static_cast<double>(NowUs() - start) * 1e-6);
        }

        // ---- The menu pointer (fix101): how the game reads the mouse for its menu cursor ----
        // Where the game calls Windows' mouse and cursor functions (each instruction using its import slot for one: call, jump or
        // load), the functions those are in (with what they call) and the functions calling those; the type tables with "Mouse" or
        // "Cursor" in their type's name (the game's mouse device, its menu cursor...) with their functions; and Scaleform's movie
        // type table (its GetMouseState, slot 0x24, which UiProbe reads after checking its code).
        void InputScan(Dump& dump)
        {
            const int64_t start  = NowUs();
            const HMODULE user32 = GetModuleHandleW(L"user32.dll");
            constexpr const char* kUser32[] = { "GetCursorPos", "SetCursorPos", "ClipCursor", "ShowCursor", "GetCursorInfo", "GetPhysicalCursorPos",
                "ScreenToClient", "ClientToScreen", "GetRawInputData", "GetRawInputBuffer", "RegisterRawInputDevices", "SetCapture", "ReleaseCapture",
                "TrackMouseEvent", "GetMessagePos", "SetCursor", "WindowFromPoint", "GetAsyncKeyState", "GetKeyState", "GetForegroundWindow" };
            std::unordered_map<uintptr_t, std::string> wanted; // the function's address -> its name
            for (const char* name : kUser32) {
                if (const auto f = user32 != nullptr ? GetProcAddress(user32, name) : nullptr; f != nullptr) {
                    wanted[reinterpret_cast<uintptr_t>(f)] = name;
                }
            }
            if (const HMODULE dinput = GetModuleHandleW(L"dinput8.dll"); dinput != nullptr) {
                if (const auto f = GetProcAddress(dinput, "DirectInput8Create"); f != nullptr) {
                    wanted[reinterpret_cast<uintptr_t>(f)] = "DirectInput8Create";
                }
            }
            // The import slots holding them (by the address each holds, so any form of the import table will do).
            std::unordered_map<uintptr_t, std::string> slots;
            IMAGE_DOS_HEADER   dos{};
            IMAGE_NT_HEADERS64 nt{};
            if (Read(g_image.base, dos) && Read(g_image.base + dos.e_lfanew, nt)) {
                const auto dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
                for (uint32_t k = 0; dir.VirtualAddress != 0 && k < 1024; ++k) {
                    IMAGE_IMPORT_DESCRIPTOR d{};
                    if (!Read(g_image.base + dir.VirtualAddress + k * sizeof(d), d) || d.FirstThunk == 0) {
                        break;
                    }
                    for (size_t i = 0; i < 8192; ++i) {
                        uintptr_t       value = 0;
                        const uintptr_t slot  = g_image.base + d.FirstThunk + i * 8;
                        if (!Read(slot, value) || value == 0) {
                            break;
                        }
                        if (const auto it = wanted.find(value); it != wanted.end()) {
                            slots[slot] = it->second;
                        }
                    }
                }
            }
            std::string found;
            for (const auto& [slot, name] : slots) {
                found += fmt::format(" {} (slot {:x})", name, slot - g_image.base);
            }
            spdlog::info("[Probe] menu pointer: the game imports{}", found.empty() ? " none of the mouse and cursor functions looked for" : found);
            dump.Note("menu pointer: the game imports" + found);

            // Every instruction using a slot: call/jmp [rip+x] (ff 15/25, with or without 48), mov r64,[rip+x] (48/4c 8b).
            std::unordered_map<uintptr_t, std::vector<uintptr_t>> sites;
            ForChunks(g_image.text.begin, g_image.text.end, 16, [&](const uint8_t* b, size_t n, uintptr_t p) {
                for (size_t i = 0; i + 7 <= n; ++i) {
                    uintptr_t target = 0;
                    if (b[i] == 0xFF && (b[i + 1] == 0x15 || b[i + 1] == 0x25)) {
                        int32_t rel = 0;
                        std::memcpy(&rel, b + i + 2, sizeof(rel));
                        target = p + i + 6 + static_cast<intptr_t>(rel);
                    } else if ((b[i] == 0x48 || b[i] == 0x4C) && b[i + 1] == 0x8B && (b[i + 2] & 0xC7) == 0x05) {
                        int32_t rel = 0;
                        std::memcpy(&rel, b + i + 3, sizeof(rel));
                        target = p + i + 7 + static_cast<intptr_t>(rel);
                    } else {
                        continue;
                    }
                    if (slots.contains(target)) {
                        auto& list = sites[target];
                        if (list.size() < 64 && std::find(list.begin(), list.end(), p + i) == list.end()) {
                            list.push_back(p + i);
                        }
                    }
                }
            });
            std::unordered_set<uintptr_t>              users;
            std::unordered_map<uintptr_t, std::string> user_of;
            for (const auto& [slot, list] : sites) {
                const std::string& name      = slots[slot];
                const auto         functions = Containing(list, 16);
                spdlog::info("[Probe] menu pointer: {} is used at {} places, in{}", name, list.size(), List(functions));
                dump.Note(fmt::format("menu pointer: user32 {} is used at {} places, in{}", name, list.size(), List(functions)));
                for (const uintptr_t f : functions) {
                    Expand(dump, fmt::format("menu pointer: uses {} (with what it calls)", name), f, 1, 8);
                    users.insert(f);
                    user_of[f] += user_of[f].empty() ? name : "," + name;
                }
            }
            // The functions calling those (one level up).
            {
                std::unordered_map<uintptr_t, std::vector<uintptr_t>> loads, calls;
                ScanText({}, users, loads, calls);
                for (const auto& [f, list] : calls) {
                    const auto callers = Containing(list, 8);
                    dump.Note(fmt::format("menu pointer: {:x} (uses {}) is called from{}", f - g_image.base, user_of[f], List(callers)));
                    for (const uintptr_t c : callers) {
                        Expand(dump, fmt::format("menu pointer: calls {:x} (which uses {})", f - g_image.base, user_of[f]), c, 0, 1);
                    }
                }
            }
            dump.Flush();

            // The types with "Mouse" or "Cursor" in their name (RTTI type descriptors, in .data), with their type tables.
            std::vector<std::string> names;
            ForChunks(g_image.data.begin, g_image.data.end, 256, [&](const uint8_t* b, size_t n, uintptr_t) {
                for (size_t i = 0; i + 8 < n && names.size() < 24; ++i) {
                    if (b[i] != '.' || b[i + 1] != '?' || b[i + 2] != 'A' || (b[i + 3] != 'V' && b[i + 3] != 'U')) {
                        continue;
                    }
                    size_t len = 0;
                    while (i + len < n && len < 160 && b[i + len] != 0) {
                        ++len;
                    }
                    if (i + len >= n || len >= 160) {
                        continue;
                    }
                    const std::string_view text{ reinterpret_cast<const char*>(b + i), len };
                    if ((text.find("Mouse") != std::string_view::npos || text.find("Cursor") != std::string_view::npos) &&
                        std::find(names.begin(), names.end(), text) == names.end()) {
                        names.emplace_back(text);
                    }
                }
            });
            for (const auto& raw : names) {
                const uintptr_t table = FindTypeTable(raw);
                std::string     list;
                for (size_t e = 0; table != 0 && e < 24; ++e) {
                    uintptr_t fn = 0;
                    if (!Read(table + e * 8, fn) || !g_image.text.contains(fn)) {
                        break;
                    }
                    list += fmt::format(" [{}]{:x}", e, fn - g_image.base);
                    Expand(dump, fmt::format("menu pointer: {}[{}]", ShortName(raw), e), fn, 0, 1);
                }
                spdlog::info("[Probe] menu pointer: type {}: {}{}", ShortName(raw), table != 0 ? fmt::format("type table at {:x}:", table - g_image.base) : "no type table found",
                    list);
                dump.Note(fmt::format("menu pointer: type {}: {}{}", ShortName(raw),
                    table != 0 ? fmt::format("type table at {:x}:", table - g_image.base) : "no type table found", list));
            }

            // (fix104) The game's input mode: its input device manager's poll (where a device's input switches the mode; called
            // with the manager, `mov rcx,[manager]; call poll; mov r8b,1; xor edx,edx; call`), the devices' and the manager's type
            // tables (their polls), and the pass that lays the interface over the picture (see-through menus).
            if (const auto at = utility::scan(MemoryScan::mod, "48 8B 0D ? ? ? ? E8 ? ? ? ? 41 B0 01 33 D2 E8")) {
                int32_t rel = 0;
                std::memcpy(&rel, reinterpret_cast<const void*>(*at + 8), sizeof(rel));
                const uintptr_t poll = *at + 12 + static_cast<intptr_t>(rel);
                if (g_image.text.contains(poll)) {
                    spdlog::info("[Probe] input mode: the input device manager's poll at {:x} (called at {:x})", poll - g_image.base, *at + 7 - g_image.base);
                    dump.Note(fmt::format("input mode: the input device manager's poll at {:x}", poll - g_image.base));
                    Expand(dump, "input mode: the input device manager's poll", poll, 1, 24);
                    // (fix105: its body follows its first part in chained parts: a stretch of it whole.)
                    dump.Window("input mode: the input device manager's poll", poll, 0x900);
                }
            }
            for (const char* raw : { ".?AVBSInputDeviceManager@@", ".?AVBSPCGamepadDevice@@", ".?AVBSPCMouseDevice@@", ".?AVBSPCKeyboardDevice@@",
                     ".?AVBSWin32KeyboardDevice@@", ".?AVBSWin32MouseDevice@@", ".?AVBSPCGamepadDeviceHandler@@",
                     ".?AVScaleformCompositeRenderPass@CreationRendererPrivate@@" }) {
                const uintptr_t table = FindTypeTable(raw);
                std::string     list;
                for (size_t e = 0; table != 0 && e < 16; ++e) {
                    uintptr_t fn = 0;
                    if (!Read(table + e * 8, fn) || !g_image.text.contains(fn)) {
                        break;
                    }
                    list += fmt::format(" [{}]{:x}", e, fn - g_image.base);
                    Expand(dump, fmt::format("input mode: {}[{}]", ShortName(raw), e), fn, e <= 8 ? 1 : 0, 8);
                    // (fix105: the devices' polls whole: their first parts only were saved, the rest follows in chained parts.)
                    if (e == 2 && std::string_view{ raw }.find("Device@@") != std::string_view::npos) {
                        dump.Window(fmt::format("input mode: {}[2] (its poll)", ShortName(raw)), fn, 0x600);
                    }
                }
                spdlog::info("[Probe] input mode: type {}: {}{}", ShortName(raw), table != 0 ? fmt::format("type table at {:x}:", table - g_image.base) : "no type table found",
                    list);
                dump.Note(fmt::format("input mode: type {}: {}{}", ShortName(raw),
                    table != 0 ? fmt::format("type table at {:x}:", table - g_image.base) : "no type table found", list));
            }
            dump.Flush();

            // Scaleform's movie (its functions from SetViewport, 0xc, to past GetMouseState, 0x24).
            if (const uintptr_t table = FindTypeTable(".?AVMovieImpl@GFx@Scaleform@@"); table != 0) {
                std::string list;
                for (size_t e = 0x0c; e <= 0x2a; ++e) {
                    uintptr_t fn = 0;
                    if (!Read(table + e * 8, fn) || !g_image.text.contains(fn)) {
                        break;
                    }
                    list += fmt::format(" [{:x}]{:x}", e, fn - g_image.base);
                    Expand(dump, fmt::format("menu pointer: Scaleform MovieImpl[{:#x}]", e), fn, e == 0x23 || e == 0x24 || e == 0x25 ? 1 : 0, 8);
                }
                spdlog::info("[Probe] menu pointer: Scaleform MovieImpl type table at {:x}:{}", table - g_image.base, list);
                dump.Note(fmt::format("menu pointer: Scaleform MovieImpl type table at {:x}:{}", table - g_image.base, list));
            }
            dump.Flush();
            spdlog::info("[Probe] menu pointer: the game's mouse code looked up in {:.1f} s", static_cast<double>(NowUs() - start) * 1e-6);
        }

        // ---- The menus' 3D views (fix102): what fills their background ----
        // The menu files hand the engine each full-screen 3D view (your character in the Data menu, the item in the inventory) as
        // an object with these members (Shared.AS3.BSAnimating3DSceneRect, sent with the "UpdateSceneRectEvent" data event; its
        // fill colour is "uColor"). The engine code that reads them (each function loading one of these names, with what it
        // calls) and its callers; and the code using the render graph name "FullscreenMenu".
        void SceneRectScan(Dump& dump)
        {
            const int64_t         start    = NowUs();
            constexpr const char* kNames[] = { "uColor", "UpdateSceneRectEvent", "sSceneRectName", "fTopLeftX", "fBottomRightY", "bHasMask",
                "fMaskTopLeftX", "FullscreenMenu" };
            std::unordered_map<std::string, std::vector<uintptr_t>> strings;
            std::unordered_set<uintptr_t>                           wanted;
            for (const char* name : kNames) {
                strings[name] = FindStrings(name, 8);
                wanted.insert(strings[name].begin(), strings[name].end());
            }
            // Every string with "SceneRect" in it (the engine's own names for these views).
            {
                const auto&              rdata = g_image.rdata_copy;
                constexpr std::string_view key{ "SceneRect" };
                std::vector<std::string>   found;
                for (size_t at = 0; at + key.size() < rdata.size() && found.size() < 40; ++at) {
                    if (std::memcmp(rdata.data() + at, key.data(), key.size()) != 0) {
                        continue;
                    }
                    size_t begin = at;
                    while (begin > 0 && at - begin < 120 && rdata[begin - 1] >= 0x20 && rdata[begin - 1] < 0x7F) {
                        --begin;
                    }
                    size_t end = at;
                    while (end < rdata.size() && end - at < 120 && rdata[end] >= 0x20 && rdata[end] < 0x7F) {
                        ++end;
                    }
                    if (begin > 0 && rdata[begin - 1] != 0) {
                        continue; // (not the start of a string)
                    }
                    const std::string text{ reinterpret_cast<const char*>(rdata.data() + begin), end - begin };
                    if (std::find(found.begin(), found.end(), text) == found.end()) {
                        found.push_back(text);
                        const uintptr_t address = g_image.rdata.begin + begin;
                        strings[text].push_back(address);
                        wanted.insert(address);
                    }
                    at = end;
                }
                std::string list;
                for (const auto& text : found) {
                    list += " \"" + text + "\"";
                }
                spdlog::info("[Probe] 3D views: strings with \"SceneRect\":{}", list.empty() ? " none" : list);
                dump.Note("3D views: strings with \"SceneRect\":" + list);
            }
            std::unordered_map<uintptr_t, std::vector<uintptr_t>> loads, unused;
            ScanText(wanted, {}, loads, unused);
            std::unordered_set<uintptr_t> color_readers;
            for (const auto& [name, addresses] : strings) {
                std::vector<uintptr_t> sites;
                for (const uintptr_t a : addresses) {
                    if (const auto it = loads.find(a); it != loads.end()) {
                        sites.insert(sites.end(), it->second.begin(), it->second.end());
                    }
                }
                const auto functions = Containing(sites, 12);
                spdlog::info("[Probe] 3D views: \"{}\" ({} copies) is loaded at {} places, in{}", name, addresses.size(), sites.size(), List(functions));
                dump.Note(fmt::format("3D views: \"{}\" is loaded at {} places, in{}", name, sites.size(), List(functions)));
                const bool key = name == "uColor" || name == "UpdateSceneRectEvent" || name == "FullscreenMenu";
                for (const uintptr_t f : functions) {
                    Expand(dump, fmt::format("3D views: loads \"{}\" (with what it calls)", name), f, key ? 2 : 1, key ? 40 : 12);
                    if (name == "uColor") {
                        color_readers.insert(f);
                    }
                }
            }
            // Who calls the code reading "uColor" (one level up).
            {
                std::unordered_map<uintptr_t, std::vector<uintptr_t>> no_loads, calls;
                ScanText({}, color_readers, no_loads, calls);
                for (const auto& [f, list] : calls) {
                    const auto callers = Containing(list, 8);
                    dump.Note(fmt::format("3D views: {:x} (reads \"uColor\") is called from{}", f - g_image.base, List(callers)));
                    spdlog::info("[Probe] 3D views: {:x} (reads \"uColor\") is called from{}", f - g_image.base, List(callers));
                    for (const uintptr_t c : callers) {
                        Expand(dump, fmt::format("3D views: calls {:x} (which reads \"uColor\")", f - g_image.base), c, 1, 12);
                    }
                }
            }
            dump.Flush();
            spdlog::info("[Probe] 3D views: the code for the menus' 3D views looked up in {:.1f} s", static_cast<double>(NowUs() - start) * 1e-6);
        }

        void ConsumeScan(Dump& dump)
        {
            const int64_t start = NowUs();
            // The game's activate (TESObjectREFR::ActivateRef; ObjectReference.Activate's code calls it at 206a97d), what it calls,
            // and every place that calls it (your activate button's code, the game's consume for food in the world, ...).
            constexpr uintptr_t kActivateRefRva = 0xb48040; // (fix190) TESObjectREFR::ActivateRef
            const uintptr_t     activate        = At(kActivateRefRva);
            Expand(dump, "ActivateRef (b48040; the eat logging hooks its start: its first bytes are the hook's jump, the game's are 48 89 5C 24 08 55 "
                         "56 57 41 54 41 55 41 56 41 57 48 8D AC 24 80 FE FF FF 48 81 EC 80 02 00 00)",
                activate, 1, 90);
            {
                std::unordered_map<uintptr_t, std::vector<uintptr_t>> unused, calls;
                ScanText({}, { activate }, unused, calls);
                const auto it      = calls.find(activate);
                const auto callers = it == calls.end() ? std::vector<uintptr_t>{} : Containing(it->second, 64);
                for (const uintptr_t c : callers) {
                    dump.Code("calls ActivateRef (b48040)", c);
                }
                spdlog::info("[Probe] ActivateRef (b48040) is called from {} functions:{}", callers.size(), List(callers));
                dump.Note(fmt::format("ActivateRef (b48040) is called by:{}", List(callers)));
            }
            dump.Flush();

            // Equipping (drinking a potion is equipping it, as HIGGS does): Actor.EquipItem's code (registered at 1fd0598: lea r9 to
            // it, then the name) and the EquipItem console command's (c3bb30), two levels down, with the type tables they use.
            constexpr uintptr_t kRegistration = 0x1fd0598;
            dump.Window("Actor.EquipItem registered here (lea r9,[its code] at 1fd0598)", At(kRegistration - 0x60), 0xc0);
            const struct
            {
                uintptr_t   rva;
                const char* what;
            } roots[] = { { 0x1fc8df0, "Actor.EquipItem's code (1fc8df0)" }, { 0xc3bb30, "the EquipItem console command (c3bb30)" },
                { 0x1fce980, "Actor.UnequipItem's code (1fce980, registered at 1fd3d98: putting a picked-up weapon away when there was none before)" } };
            // (fix190) The roots' places, for the port tool (see TableAt): one not known in this game version is left out of the dump.
            if (!GameCode::Known({ 0x1fc8df0, 0xc3bb30, 0x1fce980 })) {
                spdlog::info("[Probe] (fix190) some of the equip functions to save are not known in this game version (GameCode): left out");
            }
            for (const auto& r : roots) {
                Expand(dump, r.what, TableAt(r.rva), 2, 50);
                FollowTables(dump, r.what, TableAt(r.rva));
            }
            // What eating calls (Consume.cpp): the game's equip that script EquipItem's task calls (19667d0), and the count of a
            // reference your Take reads (50b8d0).
            constexpr uintptr_t kEquipRva    = 0x19667d0; // (fix190) the game's equip
            constexpr uintptr_t kRefCountRva = 0x50b8d0;  // (fix190) a reference's count
            Expand(dump, "the game's equip (19667d0, called by script EquipItem's task at 18cb355; eating uses it)", At(kEquipRva), 1, 40);
            Expand(dump, "a reference's count (50b8d0, read by your Take at 1a39a2e; eating uses it)", At(kRefCountRva), 1, 10);
            // A thrown melee weapon's hit (MeleeThrow): the game's function applying a melee hit (19813d0: attacker, target, number, 0),
            // in case a hit from afar is turned away there.
            constexpr uintptr_t kMeleeHitRva = 0x19813d0; // (fix190) applying a melee hit
            Expand(dump, "applying a melee hit (19813d0; instant hits and thrown weapons use it)", At(kMeleeHitRva), 2, 40);
            {
                uint8_t lea[7]{};
                if (SafeRead(lea, At(kRegistration), sizeof(lea)) && lea[0] == 0x4C && lea[1] == 0x8D && lea[2] == 0x0D) {
                    int32_t rel = 0;
                    std::memcpy(&rel, lea + 3, sizeof(rel));
                    // (fix190: from the lea's rva in this game version, as its distance is this version's.)
                    const uintptr_t target = GameCode::Rva(kRegistration) + 7 + static_cast<intptr_t>(rel);
                    spdlog::info("[Probe] Actor.EquipItem's registration points at {:x}", target);
                    dump.Note(fmt::format("Actor.EquipItem's registration (1fd0598) points at {:x}", target));
                }
            }
            dump.Flush();

            // Food and drinks: AlchemyItem's type table, and its activate functions (slots 0x53 LoopingActivate, 0x54 Activate: what
            // taking or consuming it from the world runs) two levels down.
            if (const uintptr_t table = FindTypeTable(".?AVAlchemyItem@@"); table != 0) {
                std::string list;
                for (size_t e = 0; e < 0x90; ++e) {
                    uintptr_t fn = 0;
                    if (!Read(table + e * 8, fn) || !g_image.text.contains(fn)) {
                        break;
                    }
                    list += fmt::format(" [{:x}]{:x}", e, fn - g_image.base);
                    if (e == 0x53 || e == 0x54) {
                        Expand(dump, fmt::format("AlchemyItem[{:x}]", e), fn, 2, 40);
                    }
                }
                spdlog::info("[Probe] AlchemyItem type table at {:x}:{}", table - g_image.base, list);
                dump.Note(fmt::format("AlchemyItem type table at {:x}:{}", table - g_image.base, list));
            } else {
                spdlog::info("[Probe] AlchemyItem's type table not found");
            }
            dump.Flush();
            spdlog::info("[Probe] eating and drinking looked up in {:.1f} s", static_cast<double>(NowUs() - start) * 1e-6);
        }

        // ---- What the game has at runtime ----

        struct Node
        {
            std::string path;
            uintptr_t   pointer{ 0 };
            std::string type;
            int         depth{ 0 };
        };

        // The objects (with a type) an object points to, and theirs, `depth` levels down; logged (at most `lines` lines).
        void Describe(const std::string& path, uintptr_t object, size_t size, int depth, int level, int& lines, std::unordered_set<uintptr_t>& seen,
            std::vector<Node>& out)
        {
            if (depth <= 0 || !seen.insert(object).second) {
                return;
            }
            std::vector<uint8_t> raw(size);
            const size_t         got = ReadSome(raw.data(), object, size);
            for (size_t off = 0; off + 8 <= got; off += 8) {
                const auto  p    = static_cast<uintptr_t>(U64(raw.data(), off));
                std::string type = p != object ? TypeOf(p) : std::string{};
                if (type.empty()) {
                    continue;
                }
                const std::string here = fmt::format("{}+{:x}", path, off);
                if (lines > 0) {
                    --lines;
                    spdlog::info("[Physics]   {} -> {} ({})", here, type, InImage(p) ? fmt::format("in the game at {:x}", p - g_image.base) : fmt::format("{:x}", p));
                }
                out.push_back({ here, p, type, level });
                Describe(here, p, 0x200, depth - 1, level + 1, lines, seen, out);
            }
        }

        std::vector<Node> DescribeTree(const std::string& path, uintptr_t object, size_t size, int depth, int lines)
        {
            std::vector<Node>             out;
            std::unordered_set<uintptr_t> seen;
            Describe(path, object, size, depth, 1, lines, seen, out);
            return out;
        }

        bool Contains(const std::string& text, std::string_view key) { return text.find(key) != std::string::npos; }

        std::string Vec(const glm::vec3& v) { return fmt::format("({:.2f}, {:.2f}, {:.2f})", v.x, v.y, v.z); }

        bool PlayerFacts(Dump& dump)
        {
            const auto player = reinterpret_cast<uintptr_t>(CreationEngineSingletonManager::GetPlayerRef());
            if (!Pointer(player)) {
                return false;
            }
            // The AI process (CommonLibSF: Actor+0x228; it has no type table), its middle-high data (+0x8) and that one's
            // character controller (+0x458): tried at +0x228 first, then the places around it.
            uintptr_t   process = 0, middle = 0, controller = 0;
            size_t      process_at = 0, controller_at = 0;
            std::string type;
            std::vector<size_t> places{ 0x228 };
            for (size_t off = 0x1a0; off <= 0x2e0; off += 8) {
                if (off != 0x228) {
                    places.push_back(off);
                }
            }
            for (const size_t off : places) {
                uintptr_t p = 0, m = 0;
                if (!Read(player + off, p) || !Pointer(p) || !Read(p + 8, m) || !Pointer(m)) {
                    continue;
                }
                std::vector<uint8_t> raw(0x1000);
                const size_t         got = ReadSome(raw.data(), m, raw.size());
                for (size_t at = 0; at + 8 <= got; at += 8) {
                    const auto  c = static_cast<uintptr_t>(U64(raw.data(), at));
                    std::string t = TypeOf(c);
                    if (Contains(t, "bhkChar") && Contains(t, "Controller")) {
                        process       = p;
                        process_at    = off;
                        middle        = m;
                        controller    = c;
                        controller_at = at;
                        type          = t;
                        break;
                    }
                }
                if (controller != 0) {
                    break;
                }
            }
            if (controller == 0) {
                return false;
            }
            spdlog::info("[Physics] your character controller: {} at {:x} (player {:x} +{:x} AI process {:x}, +8 {:x}, +{:x}); what it points to:", type,
                controller, player, process_at, process, middle, controller_at);
            dump.Note(fmt::format("player {:x} process +{:x} {:x} middle {:x} controller +{:x} {:x} {}", player, process_at, process, middle, controller_at,
                controller, type));
            uintptr_t proxy = 0;
            uint32_t  info  = 0;
            if (Read(controller + 0x4d0, proxy) && Read(proxy + 0x3c, info)) {
                spdlog::info("[Physics] your character proxy {:x} ({}): collision filter info {:08x} (layer {}, group {})", proxy, TypeOf(proxy), info, info & 0x7fu,
                    info >> 16);
            }
            dump.Data(fmt::format("player character controller {}", type), controller, 0x800);
            dump.Data("player middle-high process data", middle, 0x600);
            for (const auto& node : DescribeTree("controller", controller, 0x800, 2, 80)) {
                if (node.depth == 1 || Contains(node.type, "Character") || Contains(node.type, "Shape")) {
                    dump.Data(fmt::format("{} {}", node.path, node.type), node.pointer, 0x300);
                }
            }
            return true;
        }

        // Starfield 1.16.236's bodies (from the collision object's filter functions, rva 2379700..2379c07, and the world's body
        // lookups, rva 236aff0 and 2363f30): hknpBSWorld+0x38 its bodies, 0xb0 bytes each, +0x40 how many; a body's id at +0x50
        // (its index in bits 0-23), in use when +0x54 & 3, its collision filter info at +0x8c (bits 0-6 layer, 16-31 group).
        constexpr uintptr_t kWorldBodies     = 0x38;
        constexpr uintptr_t kWorldBodyCount  = 0x40;
        constexpr uint32_t  kBodySize        = 0xb0;
        constexpr uint32_t  kBodyId          = 0x50;
        constexpr uint32_t  kBodyFlags       = 0x54;
        constexpr uint32_t  kBodyFilter      = 0x8c;
        constexpr uint32_t  kBodyPlace       = 0x30; // a body's transform: rotation columns at +0, +0x10, +0x20, its place at +0x30 (game units)
        constexpr uint32_t  kBodyOwner       = 0x98; // its bhkNPCollisionObject
        constexpr uintptr_t kFilterGlobalRva = 0x5fe4bc8; // the world's bhkCollisionFilter (as rva 239b900 reads it)
        constexpr uintptr_t kFilterLayers    = 0x1b8;     // its 64 layer masks (u64 each)

        // Where motions are kept: the held body's field `field` indexes an array whose record starts with the object's place.
        struct MotionArray
        {
            size_t    slot{ 0 };     // where the array's pointer is kept in the world
            uintptr_t array{ 0 };
            uint32_t  stride{ 0 };
            uint32_t  field{ 0 };
            uint32_t  index{ 0 };
            float     distance{ 0.0f };
        };

        struct Follow
        {
            bool      active{ false };
            uint32_t  body{ 0 };
            uintptr_t motion{ 0 };
            int       samples{ 0 };
            int64_t   next_us{ 0 };
        };

        std::string FilterText(uint32_t info)
        {
            return fmt::format("{:08x} (layer {}, group {}, bits 7-15 {:x})", info, info & 0x7fu, info >> 16, (info >> 7) & 0x1ffu);
        }

        std::string Layers(uint64_t mask)
        {
            std::string text;
            for (int bit = 0; bit < 64; ++bit) {
                if ((mask >> bit) & 1u) {
                    text += fmt::format(" {}", bit);
                }
            }
            return text.empty() ? std::string{ " none" } : text;
        }

        Follow HeldFacts(Dump& dump, const HandGrab::HeldDebug& held, int run)
        {
            Follow follow{};
            // The held 3D's collision object, its physics system and world, by the same reads as the game's own functions.
            uintptr_t collision = 0, system = 0, instance = 0, ids = 0, world18 = 0;
            int32_t   index = -1;
            uint32_t  count = 0;
            Read(held.root + 0x110, collision);
            Read(collision + 0x20, system);
            Read(collision + 0x28, index);
            Read(system + 0x20, instance);
            Read(instance + 0x38, count);
            Read(instance + 0x30, ids);
            Read(instance + 0x28, world18);
            const uintptr_t world = held.world != 0 ? held.world : (world18 >= 0x18 ? world18 - 0x18 : 0);
            spdlog::info("[Physics] holding body {:x} (run {}): its 3D {:x} is {}, collision object {:x} {}, physics system {:x} {} (its body {} of {}), "
                         "Havok system {:x} {}, world {:x} {}; the spring {:x} is {}; the object at {}, your head at {}",
                held.body, run, held.root, TypeOf(held.root), collision, TypeOf(collision), system, TypeOf(system), index, count, instance, TypeOf(instance),
                world, TypeOf(world), held.spring, TypeOf(held.spring), held.have_object ? Vec(held.object) : "(not known)",
                held.have_head ? Vec(held.head) : "(not known)");
            dump.Note(fmt::format("run {} held body {:x} root {:x} collision {:x} system {:x} instance {:x} world {:x} spring {:x} object {} head {}", run,
                held.body, held.root, collision, system, instance, world, held.spring, Vec(held.object), Vec(held.head)));
            dump.Data(fmt::format("spring {:x}", held.spring), held.spring, 0x100);
            dump.Data(fmt::format("held collision object {}", TypeOf(collision)), collision, 0x100);
            dump.Data(fmt::format("held physics system {}", TypeOf(system)), system, 0x200);
            dump.Data(fmt::format("held Havok physics system {}", TypeOf(instance)), instance, 0x100);
            if (world == 0) {
                spdlog::info("[Physics] the held body's world was not found");
                return follow;
            }
            if (run == 1) {
                spdlog::info("[Physics] what the world points to:");
                dump.Data(fmt::format("world {}", TypeOf(world)), world, 0x1000);
                for (const auto& node : DescribeTree("world", world, 0x1000, 2, 120)) {
                    if (node.depth == 1) {
                        dump.Data(fmt::format("{} {}", node.path, node.type), node.pointer, 0x400);
                    }
                }
            }

            // The held body's record.
            uintptr_t bodies     = 0;
            uint32_t  body_count = 0;
            Read(world + kWorldBodies, bodies);
            Read(world + kWorldBodyCount, body_count);
            const uint32_t held_index = held.body & 0xffffffu;
            std::vector<uint8_t> record(kBodySize);
            if (bodies == 0 || held_index >= body_count || !SafeRead(record.data(), bodies + static_cast<uintptr_t>(held_index) * kBodySize, kBodySize) ||
                U32(record.data(), kBodyId) != held.body) {
                spdlog::info("[Physics] the held body is not where it was expected (bodies {:x}, {} of them)", bodies, body_count);
                return follow;
            }
            dump.Data(fmt::format("held body {:x} record", held.body), bodies + static_cast<uintptr_t>(held_index) * kBodySize, kBodySize);
            dump.Data("bodies 0-7", bodies, 8 * kBodySize);
            // Its own place (its transform's, +0x30) against where the hand grab has the held 3D (they should be close).
            glm::vec3 body_place{};
            std::memcpy(&body_place, record.data() + kBodyPlace, sizeof(body_place));
            const bool have_place = Finite(body_place);
            spdlog::info("[Physics] held body {:x}: its place (+30) {}, {} from the held 3D as the hand grab has it; flags (+54) {:08x}; collision object (+98) {}",
                held.body, have_place ? Vec(body_place) : "(not finite)",
                have_place && held.have_object ? fmt::format("{:.3f} m", glm::length(body_place - held.object)) : std::string{ "not compared" },
                U32(record.data(), kBodyFlags), TypeOf(static_cast<uintptr_t>(U64(record.data(), kBodyOwner))));
            int place_at = have_place ? static_cast<int>(kBodyPlace) : -1;
            {
                std::string words, typed, places;
                for (uint32_t at = 0; at + 4 <= kBodySize; at += 4) {
                    words += fmt::format(" {:x}:{:08x}", at, U32(record.data(), at));
                }
                for (uint32_t at = 0; at + 8 <= kBodySize; at += 8) {
                    if (const std::string t = TypeOf(static_cast<uintptr_t>(U64(record.data(), at))); !t.empty()) {
                        typed += fmt::format(" +{:x} {}", at, t);
                    }
                }
                if (held.have_object) {
                    for (uint32_t at = 0; at + 12 <= kBodySize; at += 4) {
                        glm::vec3 v{};
                        std::memcpy(&v, record.data() + at, sizeof(v));
                        if (Finite(v) && glm::length(v) > 0.01f && glm::length(v - held.object) < 2.0f) {
                            places += fmt::format(" +{:x} {}", at, Vec(v));
                        }
                    }
                }
                spdlog::info("[Physics] held body {:x}: {} bodies in the world (array {:x}); filter {}; points to{}; near the object's place:{}; words:{}",
                    held.body, body_count, bodies, FilterText(U32(record.data(), kBodyFilter)), typed.empty() ? " nothing typed" : typed,
                    places.empty() ? " none" : places, words);
            }

            // The motion array: a field of the held body indexes records starting with the object's place (centre of mass).
            std::vector<MotionArray> motions;
            if (have_place) {
                static constexpr uint32_t kMotionStrides[] = { 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0x100 };
                std::vector<uint8_t>      raw(0x1000);
                const size_t              got = ReadSome(raw.data(), world, raw.size());
                std::vector<uint32_t>     indexes;
                std::vector<uint32_t>     fields;
                for (uint32_t field = 0; field + 4 <= kBodySize; field += 4) {
                    const uint32_t value = U32(record.data(), field);
                    if (field != kBodyId && value != 0 && value < 0x100000 && std::find(indexes.begin(), indexes.end(), value) == indexes.end()) {
                        indexes.push_back(value);
                        fields.push_back(field);
                    }
                }
                for (size_t slot = 0; slot + 8 <= got; slot += 8) {
                    const auto array = static_cast<uintptr_t>(U64(raw.data(), slot));
                    if (!Pointer(array) || (array & 0xF) != 0 || InImage(array)) {
                        continue;
                    }
                    for (size_t i = 0; i < indexes.size(); ++i) {
                        for (const uint32_t stride : kMotionStrides) {
                            glm::vec3 at{};
                            if (!Read(array + static_cast<uintptr_t>(indexes[i]) * stride, at) || !Finite(at) || glm::length(at) < 0.01f) {
                                continue;
                            }
                            const float distance = glm::length(at - body_place);
                            if (distance < 0.5f && motions.size() < 40) {
                                motions.push_back({ slot, array, stride, fields[i], indexes[i], distance });
                            }
                        }
                    }
                }
            }
            std::stable_sort(motions.begin(), motions.end(), [](const MotionArray& a, const MotionArray& b) { return a.distance < b.distance; });
            for (const auto& m : motions) {
                spdlog::info("[Physics] motions maybe at world+{:x} -> {:x}: records of {:x} bytes, the held body's +{:x} ({}) indexes one {:.3f} m from the "
                             "body's place",
                    m.slot, m.array, m.stride, m.field, m.index, m.distance);
            }
            if (!motions.empty()) {
                const auto& m = motions.front();
                follow        = Follow{ true, held.body, m.array + static_cast<uintptr_t>(m.index) * m.stride, 0, NowUs() + 200'000 };
                dump.Data(fmt::format("held body {:x} motion {} (stride {:x}) sample 0, body at {}, object at {}", held.body, m.index, m.stride, Vec(body_place),
                              Vec(held.object)),
                    follow.motion, 0x100);
            }

            // The bodies near your head (yours among them).
            if (place_at >= 0 && held.have_head) {
                const uint32_t       limit = std::min<uint32_t>(body_count, 0x40000);
                constexpr uint32_t   kBlock = 64;
                std::vector<uint8_t> block(static_cast<size_t>(kBlock) * kBodySize);
                int                  nearby = 0, looked = 0;
                for (uint32_t first = 0; first < limit && nearby < 48; first += kBlock) {
                    const size_t got = ReadSome(block.data(), bodies + static_cast<uintptr_t>(first) * kBodySize, block.size());
                    for (uint32_t k = 0; k < kBlock && (k + 1) * kBodySize <= got && nearby < 48; ++k) {
                        const uint8_t* r  = block.data() + static_cast<size_t>(k) * kBodySize;
                        const uint32_t id = U32(r, kBodyId);
                        if ((U32(r, kBodyFlags) & 3u) == 0 || (id & 0xffffffu) != first + k) {
                            continue;
                        }
                        ++looked;
                        if (id == held.body) {
                            continue;
                        }
                        glm::vec3 v{};
                        std::memcpy(&v, r + place_at, sizeof(v));
                        const float distance = glm::length(v - held.head);
                        if (!Finite(v) || distance > 3.0f) {
                            continue;
                        }
                        ++nearby;
                        std::string typed;
                        for (uint32_t at = 0; at + 8 <= kBodySize; at += 8) {
                            if (const std::string t = TypeOf(static_cast<uintptr_t>(U64(r, at))); !t.empty()) {
                                typed += fmt::format(" +{:x} {}", at, t);
                            }
                        }
                        spdlog::info("[Physics] body {:x} {:.2f} m from you at {}: filter {}; flags {:08x};{}", id, distance, Vec(v), FilterText(U32(r, kBodyFilter)),
                            U32(r, kBodyFlags), typed.empty() ? std::string{ " nothing typed" } : typed);
                        dump.Data(fmt::format("body {:x} near your head ({:.2f} m)", id, distance), bodies + static_cast<uintptr_t>(first + k) * kBodySize,
                            kBodySize);
                    }
                }
                spdlog::info("[Physics] {} bodies within 3 m of you (of {} in use; a body's place at +{:x})", nearby, looked, place_at);
            } else {
                spdlog::info("[Physics] bodies near you not listed: {}", place_at < 0 ? "the held body's place was not readable" : "no position of yours");
            }

            // The world's collision filter: which layers each layer collides with.
            if (run == 1) {
                uintptr_t filter = 0;
                Read(At(kFilterGlobalRva), filter);
                const std::string type = TypeOf(filter);
                spdlog::info("[Physics] the collision filter {:x} is {}", filter, type.empty() ? "of no known type" : type);
                if (Contains(type, "CollisionFilter")) {
                    dump.Data(fmt::format("collision filter {}", type), filter, 0x500);
                    for (const int layer : { 1, 2, 4, 5, 8, 9, 10, 13, 15, 19, 20, 25, 29, 30, 31, 32, 33, 35, 40, 43, 44, 55, 56 }) {
                        uint64_t mask = 0;
                        if (Read(filter + kFilterLayers + static_cast<uintptr_t>(layer) * 8, mask)) {
                            spdlog::info("[Physics]   layer {} collides with layers{}", layer, Layers(mask));
                        }
                    }
                    uint64_t special = 0;
                    if (Read(filter + 0x3b8, special)) {
                        spdlog::info("[Physics]   layers a character controller with flag 15 still meets in other groups:{}", Layers(special));
                    }
                }
            }
            return follow;
        }

        void Run()
        {
            if (!ReadImage(g_image)) {
                spdlog::error("[Physics] could not read the game's image");
                return;
            }
            const auto path = Framework::get_persistent_dir("physics_dump.bin");
            Dump       dump;
            if (!dump.Open(path)) {
                spdlog::error("[Physics] could not write {}", path.string());
                return;
            }
            dump.Note("sections" + g_image.sections);
            const int64_t start = NowUs();
            SceneRectScan(dump); // first: the next build needs these most (the menus' backgrounds)
            InputScan(dump);     // (the menu pointer)
            NativeScan(dump);
            ConsumeScan(dump);
            ObjectScan(dump);
            StaticScan(dump);
            g_image.rdata_copy.clear();
            g_image.rdata_copy.shrink_to_fit();
            // The in-game part (your character controller; up to three held objects: the physics world around them, a search for
            // where motions are kept, the bodies near you) is off: its data is in the dumps of five sessions, and its search
            // (hundreds of thousands of checked reads over 40-60 s each time) was running during every freeze of those sessions,
            // and only then (the game stopped 5-15 s at a time).
            constexpr bool kInGameScans = false;
            if (!kInGameScans) {
                dump.Flush();
                spdlog::info("[Physics] done in {:.1f} s (the code only; the in-game scans are off): everything is in {}",
                    static_cast<double>(NowUs() - start) * 1e-6, path.string());
                return;
            }
            spdlog::info("[Physics] static part done in {:.1f} s; now waiting for you to be in the game and to hold something (hand grab)",
                static_cast<double>(NowUs() - start) * 1e-6);

            bool      player_done = false;
            int       player_tries = 0;
            int64_t   player_next = 0;
            int       runs = 0;
            int64_t   last_run_us = 0;
            uintptr_t hold_spring = 0;
            int64_t   hold_start = 0;
            Follow    follow{};
            for (;;) {
                const int64_t now = NowUs();
                if (!player_done && player_tries < 600 && now >= player_next) {
                    player_next = now + 2'000'000;
                    ++player_tries;
                    player_done = PlayerFacts(dump);
                    dump.Flush();
                }
                HandGrab::HeldDebug held{};
                if (HandGrab::DebugHeld(held)) {
                    if (held.spring != hold_spring) {
                        hold_spring = held.spring;
                        hold_start  = now;
                    }
                    if (runs < 3 && !follow.active && now - hold_start > 1'000'000 && (runs == 0 || now - last_run_us > 15'000'000)) {
                        ++runs;
                        last_run_us = now;
                        follow      = HeldFacts(dump, held, runs);
                        dump.Flush();
                    } else if (follow.active && now >= follow.next_us) {
                        if (held.body != follow.body) {
                            follow.active = false;
                        } else {
                            ++follow.samples;
                            dump.Data(fmt::format("held body {} motion sample {}, object at {}, head at {}", held.body, follow.samples, Vec(held.object),
                                          Vec(held.head)),
                                follow.motion, 0x100);
                            follow.next_us = now + 200'000;
                            follow.active  = follow.samples < 5;
                            dump.Flush();
                        }
                    }
                } else {
                    hold_spring   = 0;
                    follow.active = false;
                }
                if ((player_done || player_tries >= 600) && runs >= 3 && !follow.active) {
                    spdlog::info("[Physics] done: everything is in {}", path.string());
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    void Install()
    {
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            Run();
        }).detach();
    }
}

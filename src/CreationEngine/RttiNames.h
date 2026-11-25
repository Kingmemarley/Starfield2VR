#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

// An object's type and its bases by their coded names (".?AVRenderPass@CreationRendererPrivate@@"), read straight from MSVC's
// type information in the game (x64): the type table's entry before it points at the complete object locator (its signature 1;
// offsets in it from the image's start, which is the locator's address less its own offset, +0x14); the locator's class
// descriptor (+0x10) has the number of bases (+8) and the list of them (+0xc), each base's descriptor its type descriptor (+0),
// whose name is at +0x10. Every read guarded: a pointer that is not an object of a known type is only "no". (MenuClean and
// RenderSteps; kananlib's derives_from compares plain names instead, "class X".)
namespace RttiNames
{
    namespace detail
    {
        inline bool Locator(const void* object, uintptr_t& image, uintptr_t& locator)
        {
            __try {
                const uintptr_t table = *reinterpret_cast<const uintptr_t*>(object);
                if (table == 0) {
                    return false;
                }
                locator = *reinterpret_cast<const uintptr_t*>(table - sizeof(uintptr_t));
                if (locator == 0 || *reinterpret_cast<const uint32_t*>(locator) != 1) {
                    return false;
                }
                const uint32_t self = *reinterpret_cast<const uint32_t*>(locator + 0x14);
                image               = locator - self;
                return image != 0 && image < locator;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        inline bool SameName(uintptr_t type_descriptor, const char* coded)
        {
            __try {
                const char* name = reinterpret_cast<const char*>(type_descriptor + 0x10);
                for (size_t i = 0; i < 512; ++i) {
                    if (name[i] != coded[i]) {
                        return false;
                    }
                    if (name[i] == 0) {
                        return true;
                    }
                }
                return false;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        inline bool CopyName(uintptr_t type_descriptor, char* out, size_t size)
        {
            __try {
                const char* name = reinterpret_cast<const char*>(type_descriptor + 0x10);
                size_t      i    = 0;
                for (; i + 1 < size && name[i] != 0; ++i) {
                    out[i] = (name[i] >= 32 && name[i] < 127) ? name[i] : '?';
                }
                out[i] = 0;
                return i > 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                out[0] = 0;
                return false;
            }
        }

        // The i-th base's type descriptor (0: the type itself), or 0.
        inline uintptr_t Base(uintptr_t image, uintptr_t locator, uint32_t i, uint32_t& count)
        {
            __try {
                const uintptr_t hierarchy = image + *reinterpret_cast<const uint32_t*>(locator + 0x10);
                count                     = *reinterpret_cast<const uint32_t*>(hierarchy + 8);
                if (count == 0 || count > 256 || i >= count) {
                    return 0;
                }
                const uintptr_t list = image + *reinterpret_cast<const uint32_t*>(hierarchy + 0xc);
                const uintptr_t base = image + *reinterpret_cast<const uint32_t*>(list + static_cast<uintptr_t>(i) * 4);
                return image + *reinterpret_cast<const uint32_t*>(base);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                count = 0;
                return 0;
            }
        }
    }

    // The object's type, or one of its bases, has this coded name.
    inline bool HasBase(const void* object, const char* coded)
    {
        uintptr_t image = 0, locator = 0;
        if (object == nullptr || coded == nullptr || !detail::Locator(object, image, locator)) {
            return false;
        }
        uint32_t count = 1;
        for (uint32_t i = 0; i < count && i < 256; ++i) {
            const uintptr_t descriptor = detail::Base(image, locator, i, count);
            if (descriptor != 0 && detail::SameName(descriptor, coded)) {
                return true;
            }
        }
        return false;
    }

    // Its type's coded name and its bases' (for the log): "Type < Base < Base...".
    inline std::string Describe(const void* object)
    {
        uintptr_t image = 0, locator = 0;
        if (object == nullptr || !detail::Locator(object, image, locator)) {
            return "no object of a known type";
        }
        std::string text;
        uint32_t    count = 1;
        for (uint32_t i = 0; i < count && i < 12; ++i) {
            const uintptr_t descriptor = detail::Base(image, locator, i, count);
            char            name[128]{};
            if (descriptor != 0 && detail::CopyName(descriptor, name, sizeof(name))) {
                text += (text.empty() ? "" : " < ") + std::string{ name };
            }
        }
        return text.empty() ? std::string{ "a type without a name" } : text;
    }
}

// vrcore - the few NVIDIA Streamline 2.x types the per-eye DLSS hooks read, laid out as in the SDK headers
// (sl_struct.h, sl.h / sl_core_types.h, sl_consts.h, sl_dlss.h; checked against Streamline 2.4.11 and 2.9.0, whose layouts
// and ids are the same). Only what is read or copied is declared; everything else is passed through untouched.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace slabi {
using Result  = int32_t;  // sl::Result (an enum class with an int underneath; 0 = eOk)
using Feature = uint32_t; // sl::Feature

constexpr Feature kFeatureDLSS = 0;

struct StructType {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];

    bool operator==(const StructType& other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
};

// sl::BaseStructure: every Streamline structure starts with this.
struct BaseStructure {
    BaseStructure* next;
    StructType     structType;
    size_t         structVersion;
};

// sl::ViewportHandle {171B6435-9B3C-4FC8-9994-FBE52569AAA4}: the id the game picks for a view; each id gets its own DLSS
// instance (resources, options, constants, history).
constexpr StructType kViewportHandleType{ 0x171b6435, 0x9b3c, 0x4fc8, { 0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4 } };
struct ViewportHandle : BaseStructure {
    uint32_t value;
};

// sl::FrameToken {830A0F35-DB84-4171-A804-59B206499B18}: abstract; its virtual operator gives the frame's index (the one the
// game passed to slGetNewFrameToken).
struct FrameToken : BaseStructure {
    virtual operator uint32_t() const = 0;

protected:
    FrameToken()                             = delete;
    FrameToken(const FrameToken&)            = delete;
    FrameToken& operator=(const FrameToken&) = delete;
};

// sl::Constants {DCD35AD7-4E4A-4BAD-A90C-E0C49EB23AFE} (version 1 members; version 2 adds a float after them).
constexpr StructType kConstantsType{ 0xdcd35ad7, 0x4e4a, 0x4bad, { 0xa9, 0x0c, 0xe0, 0xc4, 0x9e, 0xb2, 0x3a, 0xfe } };
struct Float2 {
    float x, y;
};
struct Float3 {
    float x, y, z;
};
struct Float4x4 {
    float m[16];
};
enum class Boolean : char { eFalse, eTrue, eInvalid };
struct Constants : BaseStructure {
    Float4x4 cameraViewToClip;
    Float4x4 clipToCameraView;
    Float4x4 clipToLensClip;
    Float4x4 clipToPrevClip;
    Float4x4 prevClipToClip;
    Float2   jitterOffset;
    Float2   mvecScale;
    Float2   cameraPinholeOffset;
    Float3   cameraPos;
    Float3   cameraUp;
    Float3   cameraRight;
    Float3   cameraFwd;
    float    cameraNear;
    float    cameraFar;
    float    cameraFOV;
    float    cameraAspectRatio;
    float    motionVectorsInvalidValue;
    Boolean  depthInverted;
    Boolean  cameraMotionIncluded;
    Boolean  motionVectors3D;
    Boolean  reset;
    Boolean  orthographicProjection;
    Boolean  motionVectorsDilated;
    Boolean  motionVectorsJittered;
};

// sl::Resource {3A9D70CF-2418-4B72-8391-13F8721C7261} and sl::ResourceTag {4C6A5AAD-B445-496C-87FF-1AF3845BE653} (Streamline
// 2.2.0, the game's: offsets checked against its headers): a tagged picture of the game's (the laser sight reads its depth).
constexpr StructType kResourceType{ 0x3a9d70cf, 0x2418, 0x4b72, { 0x83, 0x91, 0x13, 0xf8, 0x72, 0x1c, 0x72, 0x61 } };
constexpr StructType kResourceTagType{ 0x4c6a5aad, 0xb445, 0x496c, { 0x87, 0xff, 0x1a, 0xf3, 0x84, 0x5b, 0xe6, 0x53 } };
struct Resource : BaseStructure {
    char     type;   // sl::ResourceType (0: a 2D texture)
    void*    native; // the ID3D12Resource
    void*    memory;
    void*    view;
    uint32_t state;  // its D3D12_RESOURCE_STATES when DLSS uses it
};
struct Extent {
    uint32_t top, left, width, height; // (all 0: the whole picture)
};
constexpr uint32_t kBufferTypeDepth = 0;
struct ResourceTag : BaseStructure {
    Resource* resource;
    uint32_t  type;      // sl::BufferType
    int32_t   lifecycle; // sl::ResourceLifecycle: 0 only valid now (copied as tagged), 1 until present, 2 until evaluate
    Extent    extent;
};

// sl::DLSSOptions {6AC826E4-4C61-4101-A92D-638D421057B8}: only its start is read (for the log).
constexpr StructType kDLSSOptionsType{ 0x6ac826e4, 0x4c61, 0x4101, { 0xa9, 0x2d, 0x63, 0x8d, 0x42, 0x10, 0x57, 0xb8 } };
struct DLSSOptionsStart : BaseStructure {
    uint32_t mode; // sl::DLSSMode: 0 off, 1 max performance, 2 balanced, 3 max quality, 4 ultra performance, 5 ultra quality, 6 DLAA
    uint32_t outputWidth;
    uint32_t outputHeight;
};

// (The field offsets were checked against the SDK headers: jitterOffset at 352, reset at 447, value at 32.)
static_assert(sizeof(BaseStructure) == 32);
static_assert(sizeof(ViewportHandle) == 40);
static_assert(sizeof(Constants) == 456);
static_assert(sizeof(DLSSOptionsStart) == 48);
static_assert(offsetof(Resource, native) == 40 && offsetof(Resource, state) == 64);
static_assert(offsetof(ResourceTag, resource) == 32 && offsetof(ResourceTag, type) == 40 && offsetof(ResourceTag, lifecycle) == 44);
static_assert(offsetof(ResourceTag, extent) == 48 && sizeof(ResourceTag) == 64);
static_assert(offsetof(Constants, cameraViewToClip) == 32 && offsetof(Constants, clipToCameraView) == 96);
static_assert(offsetof(Constants, cameraPos) == 376 && offsetof(Constants, cameraUp) == 388 && offsetof(Constants, cameraRight) == 400);
static_assert(offsetof(Constants, cameraFwd) == 412 && offsetof(Constants, cameraNear) == 424 && offsetof(Constants, depthInverted) == 444);
} // namespace slabi

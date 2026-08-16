// Internal Endstone layout access is deliberately isolated from the public
// SDK probes.  These headers are read-only inputs from the fetched Endstone
// source tree; no internal source is modified or compiled in-place here.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

#include "endstone/core/block/block.h"
#include "endstone/core/map/map_canvas.h"
#include "endstone/event/player/player_event.h"
#include "endstone/level/location.h"
#include "endstone/player.h"
#include "bedrock/world/level/block_source.h"

#include "internal_probe.h"

namespace abi_probe {
namespace {

template <typename Object, typename Member>
std::int64_t member_offset(Object &object, Member &member)
{
    return static_cast<std::int64_t>(reinterpret_cast<const std::byte *>(std::addressof(member)) -
                                     reinterpret_cast<const std::byte *>(std::addressof(object)));
}

template <typename Member>
std::optional<std::int64_t> decode_data_member(Member member)
{
    if constexpr (sizeof(Member) <= sizeof(std::ptrdiff_t)) {
        std::ptrdiff_t offset = 0;
        std::memcpy(&offset, &member, sizeof(Member));
        if (offset >= 0) {
            return static_cast<std::int64_t>(offset);
        }
    }
    return std::nullopt;
}

template <typename Object, typename Member>
std::optional<std::int64_t> member_pointer_offset(Member Object::*member)
{
    return decode_data_member(member);
}

template <typename Method>
std::optional<std::size_t> decode_virtual_slot(Method method)
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(__x86_64__))
    std::uintptr_t thunk = 0;
    if constexpr (sizeof(Method) >= sizeof(thunk)) {
        std::memcpy(&thunk, &method, sizeof(thunk));
    }
    if (thunk == 0) {
        return std::nullopt;
    }
    const auto *code = reinterpret_cast<const unsigned char *>(thunk);
    std::optional<std::size_t> byte_offset;
    if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0xFF && code[4] == 0x20) {
        byte_offset = 0;
    }
    else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0xFF && code[4] == 0x60) {
        byte_offset = code[5];
    }
    else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0xFF && code[4] == 0xA0) {
        std::uint32_t displacement = 0;
        std::memcpy(&displacement, code + 5, sizeof(displacement));
        byte_offset = displacement;
    }
    else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0x48 && code[4] == 0x8B &&
             code[5] == 0x40 && code[7] == 0x48 && code[8] == 0xFF && code[9] == 0xE0) {
        byte_offset = code[6];
    }
    else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0x48 && code[4] == 0x8B &&
             code[5] == 0x00 && code[6] == 0x48 && code[7] == 0xFF && code[8] == 0xE0) {
        byte_offset = 0;
    }
    else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0x48 && code[4] == 0x8B &&
             code[5] == 0x80 && code[10] == 0x48 && code[11] == 0xFF && code[12] == 0xE0) {
        std::uint32_t displacement = 0;
        std::memcpy(&displacement, code + 6, sizeof(displacement));
        byte_offset = displacement;
    }
    if (!byte_offset || *byte_offset % sizeof(void *) != 0) {
        return std::nullopt;
    }
    return *byte_offset / sizeof(void *);
#elif defined(__GXX_ABI_VERSION) || defined(__itanium__)
    struct ItaniumMemberPointer {
        std::ptrdiff_t ptr;
        std::ptrdiff_t adjustment;
    } representation{};
    if constexpr (sizeof(Method) == sizeof(representation)) {
        std::memcpy(&representation, &method, sizeof(representation));
        if ((representation.ptr & 1) != 0) {
            const auto byte_offset = representation.ptr - 1;
            if (byte_offset >= 0 && byte_offset % static_cast<std::ptrdiff_t>(sizeof(void *)) == 0) {
                return static_cast<std::size_t>(byte_offset / static_cast<std::ptrdiff_t>(sizeof(void *)));
            }
        }
    }
    return std::nullopt;
#else
    (void)method;
    return std::nullopt;
#endif
}

Fact measured(std::int64_t value, std::string_view method, std::string_view evidence)
{
    return {value, method, evidence, Provenance::RuntimeProbe};
}

Fact unresolved(std::string_view evidence)
{
    return {std::nullopt, "internal Endstone record/member-pointer probe", evidence, Provenance::Unresolved};
}

std::optional<std::int64_t> vector_suboffset(bool end)
{
    std::vector<std::uint32_t> sample;
    sample.reserve(8);
    sample.push_back(0x12345678U);
    sample.push_back(0x9abcdef0U);
    sample.push_back(0x13579bdfU);
    const auto begin = reinterpret_cast<std::uintptr_t>(sample.data());
    const auto finish = reinterpret_cast<std::uintptr_t>(sample.data() + sample.size());
    const auto target = end ? finish : begin;
    const auto *bytes = reinterpret_cast<const std::byte *>(std::addressof(sample));
    std::optional<std::int64_t> result;
    for (std::size_t offset = 0; offset + sizeof(target) <= sizeof(sample); ++offset) {
        std::uintptr_t candidate = 0;
        std::memcpy(&candidate, bytes + offset, sizeof(candidate));
        if (candidate == target) {
            if (result) {
                return std::nullopt;
            }
            result = static_cast<std::int64_t>(offset);
        }
    }
    return result;
}

}  // namespace

Fact measure_internal(std::string_view name)
{
    if (name == "ES_BLOCK_SOURCE_SLOT_GET_BLOCK_ENTITY") {
        using Method = BlockActor const *(IConstBlockSource::*)(BlockPos const &) const;
        const auto slot = decode_virtual_slot(static_cast<Method>(&IConstBlockSource::getBlockEntity));
        return slot ? measured(static_cast<std::int64_t>(*slot),
                               "member-pointer ABI decode of IConstBlockSource::getBlockEntity",
                               "Endstone bedrock/world/level/block_source.h named virtual; destructor and preceding getBlock overloads form the ABI prefix")
                    : unresolved("IConstBlockSource::getBlockEntity member-pointer encoding was not recognized");
    }
    if (name == "ES_ENDSTONE_BLOCK_OFF_BLOCK_SOURCE") {
        const auto offset = member_pointer_offset(&endstone::core::EndstoneBlock::block_source_);
        return offset ? measured(*offset, "MSVC/Itanium data-member pointer decode",
                                 "Endstone v0.11.8 EndstoneBlock::block_source_ record member")
                      : unresolved("EndstoneBlock::block_source_ member-pointer representation was not decodable");
    }
    if (name == "ES_ENDSTONE_PLAYER_OFF_OFFLINE_PLAYER") {
        // The conversion performs only the compiler's ABI base adjustment; no
        // object is dereferenced and no synthetic reference is constructed.
        constexpr std::uintptr_t marker = 0x1000;
        auto *player = reinterpret_cast<endstone::Player *>(marker);
        auto *offline = static_cast<endstone::OfflinePlayer *>(player);
        const auto offset = reinterpret_cast<std::uintptr_t>(offline) - marker;
        return measured(static_cast<std::int64_t>(offset), "compiler secondary-base pointer adjustment",
                        "EndstonePlayer : EndstoneMobBase<Player, ::Player>; EndstoneActorBase inherits Interface first; Player -> OfflinePlayer compiler adjustment; no object/ref access");
    }
    if (name == "ES_MAPCANVAS_SIZE") {
        return measured(sizeof(endstone::core::EndstoneMapCanvas), "sizeof(endstone::core::EndstoneMapCanvas)",
                        "Endstone v0.11.8 internal map-canvas record");
    }
    if (name == "ES_MAPCANVAS_OFF_BUFFER_BEGIN" || name == "ES_MAPCANVAS_OFF_BUFFER_END") {
        const auto member = member_pointer_offset(&endstone::core::EndstoneMapCanvas::buffer_);
        const auto suboffset = vector_suboffset(name.ends_with("END"));
        if (!member || !suboffset) {
            return unresolved("EndstoneMapCanvas buffer member or matching std::vector pointer suboffset failed");
        }
        return measured(*member + *suboffset, "EndstoneMapCanvas::buffer_ member plus std::vector pointer suboffset",
                        "matching compiler std::vector begin/end pointer representation; unique live vector anchors");
    }
    if (name == "ES_PLAYER_EVENT_OFF_PLAYER") {
        const auto offset = member_pointer_offset(&endstone::PlayerEvent::player_);
        return offset ? measured(*offset, "data-member pointer decode of PlayerEvent::player_",
                                 "public Endstone PlayerEvent record member; no synthetic Player reference")
                      : unresolved("PlayerEvent::player_ member-pointer representation was not decodable");
    }
    if (name == "ES_LOCATION_SIZE") {
        return measured(sizeof(endstone::Location), "sizeof(endstone::Location)", "public Endstone Location record");
    }
    if (name == "ES_LOCATION_ALIGN") {
        return measured(alignof(endstone::Location), "alignof(endstone::Location)", "public Endstone Location record");
    }
    if (name.rfind("ES_LOCATION_OFF_", 0) == 0) {
        std::optional<std::int64_t> offset;
        std::string_view member;
        if (name == "ES_LOCATION_OFF_DIMENSION") {
            offset = member_pointer_offset(&endstone::Location::dimension_);
            member = "dimension_";
        }
        else if (name == "ES_LOCATION_OFF_X") {
            offset = member_pointer_offset(&endstone::Location::x_);
            member = "x_";
        }
        else if (name == "ES_LOCATION_OFF_Y") {
            offset = member_pointer_offset(&endstone::Location::y_);
            member = "y_";
        }
        else if (name == "ES_LOCATION_OFF_Z") {
            offset = member_pointer_offset(&endstone::Location::z_);
            member = "z_";
        }
        else if (name == "ES_LOCATION_OFF_PITCH") {
            offset = member_pointer_offset(&endstone::Location::pitch_);
            member = "pitch_";
        }
        else if (name == "ES_LOCATION_OFF_YAW") {
            offset = member_pointer_offset(&endstone::Location::yaw_);
            member = "yaw_";
        }
        if (offset) {
            return measured(*offset, "data-member pointer decode of endstone::Location member",
                            member);
        }
        return unresolved("Location member-pointer representation was not decodable");
    }
    return unresolved("no internal Endstone measurement for this name");
}

}  // namespace abi_probe

// Matching-standard-library probes are isolated here so private STL access
// cannot leak into the public Endstone headers used by other components.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER) || defined(_LIBCPP_VERSION)

#define private public
#define protected public
#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>
#undef protected
#undef private
#endif

#if defined(_MSC_VER)
template <typename Method>
std::optional<std::size_t> decode_member_slot(Method method)
{
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
             code[5] == 0x00 && code[6] == 0x48 && code[7] == 0xFF && code[8] == 0xE0) {
        byte_offset = 0;
    }
    else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0x48 && code[4] == 0x8B &&
             code[5] == 0x40 && code[7] == 0x48 && code[8] == 0xFF && code[9] == 0xE0) {
        byte_offset = code[6];
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
}
#endif

#define private public
#include <endstone/block/block_data.h>
#include <endstone/inventory/item_stack.h>
#undef private

// The optional differential must exercise the real ItemStack object type, but
// it must not ask the live server for an item merely to discover the optional
// discriminator.  This matching-layout implementation satisfies ItemStack's
// private constructor without invoking any Endstone or Bedrock behavior.
#if defined(_MSC_VER) || defined(_LIBCPP_VERSION)
struct OptionalItemStackImpl final : endstone::ItemStack::Impl {
    std::unique_ptr<endstone::ItemStack::Impl> clone() const override { return nullptr; }
    const endstone::ItemType &getType() const override { std::abort(); }
    void setType(endstone::ItemTypeId) override { std::abort(); }
    int getAmount() const override { return 0; }
    void setAmount(int) override {}
    int getData() const override { return 0; }
    void setData(int) override {}
    std::string getTranslationKey() const override { return {}; }
    int getMaxStackSize() const override { return 0; }
    bool isSimilar(const endstone::ItemStack::Impl &) const override { return false; }
    std::unique_ptr<endstone::ItemMeta> getItemMeta() const override { return nullptr; }
    bool hasItemMeta() const override { return false; }
    bool setItemMeta(const endstone::ItemMeta *) override { return false; }
    endstone::CompoundTag getNbt() const override { std::abort(); }
    void setNbt(const endstone::CompoundTag &) override {}
};
#endif

#include "stl_probe.h"

namespace abi_probe {
namespace {

template <typename Object, typename Member>
std::int64_t member_offset(Object &object, Member &member)
{
    return static_cast<std::int64_t>(reinterpret_cast<const std::byte *>(std::addressof(member)) -
                                     reinterpret_cast<const std::byte *>(std::addressof(object)));
}

Fact measured(std::int64_t value, std::string_view method, std::string_view evidence)
{
    return {value, method, evidence, Provenance::RuntimeProbe};
}

Fact unresolved(std::string_view evidence)
{
    return {std::nullopt, "matching standard-library representation probe", evidence, Provenance::Unresolved};
}

#if defined(_MSC_VER)

using States = endstone::BlockStates;
using Variant = States::mapped_type;
using VariantBase = std::_Variant_base<bool, std::string, int>;

struct RefCountProbe final : std::_Ref_count_base {
    void _Destroy() noexcept override {}
    void _Delete_this() noexcept override {}
};

std::optional<std::int64_t> optional_engaged_offset(std::optional<std::string> &sample)
{
    using Base = std::_Optional_destruct_base<std::string, false>;
    auto &base = static_cast<Base &>(sample);
    return member_offset(sample, base._Has_value);
}

std::optional<std::int64_t> optional_item_engaged_offset(std::optional<endstone::ItemStack> &sample)
{
    using Base = std::_Optional_destruct_base<endstone::ItemStack, false>;
    auto &base = static_cast<Base &>(sample);
    return member_offset(sample, base._Has_value);
}

bool verify_optional(std::optional<std::string> &sample)
{
    using Base = std::_Optional_destruct_base<std::string, false>;
    auto &base = static_cast<Base &>(sample);
    if (base._Has_value) {
        return false;
    }
    sample.emplace("matching-stl");
    if (!base._Has_value) {
        return false;
    }
    sample.reset();
    return !base._Has_value;
}

bool verify_item_optional(std::optional<endstone::ItemStack> &sample)
{
    using Base = std::_Optional_destruct_base<endstone::ItemStack, false>;
    auto &base = static_cast<Base &>(sample);
    if (base._Has_value) {
        return false;
    }
    sample.emplace(std::make_unique<OptionalItemStackImpl>());
    if (!base._Has_value) {
        return false;
    }
    sample.reset();
    return !base._Has_value;
}

template <typename Object, typename Member>
std::optional<std::int64_t> checked_offset(Object &object, Member &member)
{
    const auto offset = member_offset(object, member);
    return offset >= 0 && offset < static_cast<std::int64_t>(sizeof(Object)) ? std::optional(offset) : std::nullopt;
}

Fact measure_block_states(std::string_view name)
{
#if !defined(_MSVC_STL_VERSION) || !defined(_MSVC_STL_UPDATE)
    return unresolved("MSVC STL version/update macros are unavailable");
#else
    States states;
    states.emplace("probe", true);
    auto &list = states._List;
    auto &list_value = list._Mypair._Myval2;
    auto *head = list_value._Myhead;
    if (head == nullptr || head->_Next == nullptr || head->_Prev == nullptr) {
        return unresolved("MSVC list sentinel links are not initialized");
    }
    auto *node = head->_Next;
    auto &pair = node->_Myval;
    auto &variant = pair.second;
    auto &variant_base = static_cast<VariantBase &>(variant);
    auto &vec = states._Vec;
    auto &vec_value = vec._Mypair._Myval2;
    if (vec_value._Myfirst == nullptr || vec_value._Mylast <= vec_value._Myfirst ||
        vec_value._Myend < vec_value._Mylast || states._Mask + 1 != states._Maxidx ||
        states._Maxidx == 0 || (states._Maxidx & (states._Maxidx - 1)) != 0 ||
        static_cast<std::size_t>(vec_value._Mylast - vec_value._Myfirst) != states._Maxidx * 2 ||
        pair.first != "probe" || pair.second.index() != 0 || list_value._Mysize != 1 ||
        head->_Next != node || head->_Prev != node || node->_Prev != head || node->_Next != head ||
        states.max_load_factor() <= 0.0F || states.bucket_count() == 0) {
        return unresolved("MSVC unordered mirror invariant failed (prev/trailing/max-load/variant/padding)");
    }

    if (name == "ES_BLOCK_STATES_SIZE") {
        return measured(sizeof(States), "sizeof(endstone::BlockStates)",
                        "MSVC STL version/update macros; _List/_Vec/_Mask/_Maxidx and mirror invariants");
    }
    if (name == "ES_BLOCK_STATES_OFF_HEAD") {
        return measured(member_offset(states, list_value._Myhead), "MSVC _List._Mypair._Myval2._Myhead offset",
                        "MSVC STL _List head; sentinel prev/next validated");
    }
    if (name == "ES_BLOCK_STATES_OFF_SIZE") {
        return measured(member_offset(states, list_value._Mysize), "MSVC _List._Mypair._Myval2._Mysize offset",
                        "MSVC STL list size; bucket-vector/list-size mirror validated");
    }
    if (name == "ES_BLOCK_STATES_OFF_VECTOR") {
        return measured(member_offset(states, states._Vec), "MSVC _Vec member offset",
                        "MSVC STL bucket vector first/last/trailing pointers validated");
    }
    if (name == "ES_BLOCK_STATES_OFF_MASK") {
        return measured(member_offset(states, states._Mask), "MSVC _Mask member offset",
                        "MSVC STL mask plus power-of-two _Maxidx invariant");
    }
    if (name == "ES_BLOCK_STATES_OFF_MAX_INDEX") {
        return measured(member_offset(states, states._Maxidx), "MSVC _Maxidx member offset",
                        "MSVC STL max-index plus mask/vector invariant");
    }
    if (name == "ES_BLOCK_STATE_NODE_SIZE") {
        using Node = std::remove_pointer_t<decltype(node)>;
        return measured(sizeof(Node), "sizeof(MSVC unordered node)",
                        "MSVC _List node _Next/_Prev/_Myval and variant index validated");
    }
    if (name == "ES_BLOCK_STATE_NODE_OFF_KEY") {
        return measured(member_offset(*node, pair.first), "MSVC node _Myval.first offset",
                        "MSVC _Myval pair key with node links and variant index validated");
    }
    if (name == "ES_BLOCK_STATE_NODE_OFF_VARIANT") {
        return measured(member_offset(*node, pair.second), "MSVC node _Myval.second offset",
                        "MSVC _Myval pair variant with node links and variant index validated");
    }
    if (name == "ES_BLOCK_STATE_NODE_OFF_VARIANT_INDEX") {
        return measured(member_offset(*node, variant_base._Which), "MSVC std::variant::_Which offset",
                        "MSVC variant index member and active alternative validated");
    }
    return unresolved("unsupported MSVC BlockStates measurement");
#endif
}

Fact measure_refcount(std::string_view name)
{
#if !defined(_MSVC_STL_VERSION) || !defined(_MSVC_STL_UPDATE)
    return unresolved("MSVC STL version/update macros are unavailable");
#else
    std::shared_ptr<int> owner = std::make_shared<int>(7);
    std::weak_ptr<int> weak = owner;
    const bool live = owner.use_count() == 1 && weak.use_count() == 1;
    owner.reset();
    if (!live || !weak.expired()) {
        return unresolved("MSVC shared/weak lifecycle differential failed");
    }
    RefCountProbe sample;
    if (name == "ES_REFCOUNT_SIZE") {
        return measured(sizeof(std::_Ref_count_base), "sizeof(std::_Ref_count_base)",
                        "MSVC _MSVC_STL_VERSION/_MSVC_STL_UPDATE guarded _Ref_count_base with private access and virtual callbacks");
    }
    if (name == "ES_REFCOUNT_OFF_USES") {
        return measured(member_offset(sample, sample._Uses), "std::_Ref_count_base::_Uses offset",
                        "MSVC _MSVC_STL_VERSION/_MSVC_STL_UPDATE guarded _Uses; shared/weak lifecycle differential");
    }
    if (name == "ES_REFCOUNT_OFF_WEAKS") {
        return measured(member_offset(sample, sample._Weaks), "std::_Ref_count_base::_Weaks offset",
                        "MSVC _MSVC_STL_VERSION/_MSVC_STL_UPDATE guarded _Weaks; shared/weak lifecycle differential");
    }
    if (name == "ES_REFCOUNT_SLOT_DESTROY_RESOURCE") {
        using Method = void (std::_Ref_count_base::*)() noexcept;
        const auto slot = decode_member_slot(static_cast<Method>(&std::_Ref_count_base::_Destroy));
        return slot ? measured(static_cast<std::int64_t>(*slot), "MSVC member-pointer ABI decode of _Destroy",
                               "MSVC _MSVC_STL_VERSION/_MSVC_STL_UPDATE guarded _Ref_count_base::_Destroy callback")
                    : unresolved("MSVC _Destroy member-pointer encoding was not recognized");
    }
    if (name == "ES_REFCOUNT_SLOT_DELETE_THIS") {
        using Method = void (std::_Ref_count_base::*)() noexcept;
        const auto slot = decode_member_slot(static_cast<Method>(&std::_Ref_count_base::_Delete_this));
        return slot ? measured(static_cast<std::int64_t>(*slot), "MSVC member-pointer ABI decode of _Delete_this",
                               "MSVC _MSVC_STL_VERSION/_MSVC_STL_UPDATE guarded _Ref_count_base::_Delete_this callback")
                    : unresolved("MSVC _Delete_this member-pointer encoding was not recognized");
    }
    return unresolved("unsupported MSVC refcount measurement");
#endif
}

#endif  // _MSC_VER

#if defined(_LIBCPP_VERSION)

template <typename T>
concept has_table_member = requires(T &value) { value.__table_; };

template <typename T>
concept has_bucket_list_member = requires(T &value) { value.__bucket_list_; };

template <typename T>
concept has_size_member = requires(T &value) { value.__size_; };

template <typename T>
concept has_next_member = requires(T &value) { value.__next_; };

template <typename T>
concept has_hash_member = requires(T &value) { value.__hash_; };

template <typename T>
concept has_value_member = requires(T &value) { value.__value_; };

template <typename T>
concept has_engaged_member = requires(T &value) { value.__engaged_; };

template <typename Method>
std::optional<std::size_t> decode_itanium_slot(Method method)
{
    struct Representation {
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
}

std::optional<std::int64_t> libcxx_variant_index_offset()
{
    using Variant = endstone::BlockStates::mapped_type;
    Variant boolean{true};
    Variant text{std::string{"libcxx"}};
    Variant integer{7};
    const auto *first = reinterpret_cast<const std::byte *>(std::addressof(boolean));
    const auto *second = reinterpret_cast<const std::byte *>(std::addressof(text));
    const auto *third = reinterpret_cast<const std::byte *>(std::addressof(integer));
    const auto first_index = boolean.index();
    const auto second_index = text.index();
    const auto third_index = integer.index();
    std::optional<std::int64_t> result;
    for (std::size_t offset = 0; offset < sizeof(Variant); ++offset) {
        if (std::to_integer<unsigned char>(first[offset]) == first_index &&
            std::to_integer<unsigned char>(second[offset]) == second_index &&
            std::to_integer<unsigned char>(third[offset]) == third_index) {
            if (result) {
                return std::nullopt;
            }
            result = static_cast<std::int64_t>(offset);
        }
    }
    return result;
}

Fact measure_libcxx_block_states(std::string_view name)
{
#if _LIBCPP_VERSION < 14000
    return unresolved("libc++ version guard requires a supported __table_ layout");
#else
    using States = endstone::BlockStates;
    States states;
    states.emplace("probe", true);
    if constexpr (!has_table_member<States>) {
        return unresolved("libc++ std::unordered_map has no exposed __table_ member under this version guard");
    }
    else {
        auto &table = states.__table_;
        if constexpr (!requires { table.__first_node_; table.__size_; table.__max_load_factor_; }) {
            return unresolved("libc++ __table_ first-node/size/load representation is unavailable");
        }
        auto &first_record = table.__first_node_;
        if constexpr (!has_next_member<decltype(first_record)>) {
            return unresolved("libc++ __table_ first-node link is not available under the guarded version");
        }
        using Table = std::remove_reference_t<decltype(table)>;
        using NodePointer = typename Table::__node_pointer;
        auto node = static_cast<NodePointer>(first_record.__next_);
        if (node == nullptr) {
            return unresolved("libc++ unordered mirror has no probe node");
        }
        if constexpr (!has_value_member<std::remove_pointer_t<decltype(node)>>) {
            return unresolved("libc++ unordered node value member is not available under the guarded version");
        }
        using Node = typename std::pointer_traits<NodePointer>::element_type;
        auto &stored_value = node->__get_value();
        auto &value = stored_value.__get_value();
        if (states.size() != 1 || value.second.index() != 0 || table.__size_ != states.size() ||
            table.__max_load_factor_ <= 0.0F) {
            return unresolved("libc++ unordered mirror invariant failed (node/size/load/variant)");
        }

        if (name == "ES_BLOCK_STATES_SIZE") {
            return measured(sizeof(States), "sizeof(endstone::BlockStates)",
                            "libc++ __table_ with node/size/load/variant mirror invariants");
        }
        if (name == "ES_BLOCK_STATES_OFF_SIZE") {
            return measured(member_offset(states, table.__size_),
                            "libc++ __table_.__size_ offset",
                            "libc++ table size matched to one live node");
        }
        if (name == "ES_BLOCK_STATES_OFF_MAX_LOAD_FACTOR") {
            return measured(member_offset(states, table.__max_load_factor_),
                            "libc++ __table_.__max_load_factor_ offset",
                            "libc++ table max-load factor validated positive");
        }
        if (name == "ES_BLOCK_STATES_OFF_FIRST_NODE") {
            return measured(member_offset(states, first_record), "libc++ __table_.__first_node_ offset",
                            "libc++ first-node link points to the live probe node");
        }
        if (name == "ES_BLOCK_STATES_OFF_BUCKETS") {
            if constexpr (has_bucket_list_member<decltype(table)>) {
                return measured(member_offset(states, table.__bucket_list_), "libc++ __table_.__bucket_list_ offset",
                                "libc++ bucket-list anchor with size/node mirror");
            }
            return unresolved("libc++ bucket-list member is not available under the guarded version");
        }
        if (name == "ES_BLOCK_STATES_OFF_BUCKET_COUNT") {
            if constexpr (has_bucket_list_member<decltype(table)>) {
                auto &bucket_list = table.__bucket_list_;
                if constexpr (requires { bucket_list.get_deleter().size(); }) {
                    return measured(member_offset(states, bucket_list.get_deleter().size()),
                                    "libc++ bucket-list deleter size offset",
                                    "libc++ bucket count is the matching bucket-list allocation size");
                }
            }
            return unresolved("libc++ bucket count member is not available under the guarded version");
        }

        if (name == "ES_BLOCK_STATE_NODE_SIZE") {
            return measured(sizeof(Node), "sizeof(libc++ unordered node)",
                            "libc++ node next/hash/value members and variant mirror validated");
        }
        if (name == "ES_BLOCK_STATE_NODE_OFF_NEXT") {
            if constexpr (has_next_member<Node>) {
                return measured(member_offset(*node, node->__next_), "libc++ node __next_ offset",
                                "libc++ node next link points to the matching node chain");
            }
        }
        if (name == "ES_BLOCK_STATE_NODE_OFF_HASH") {
            if constexpr (has_hash_member<Node>) {
                return measured(member_offset(*node, node->__hash_), "libc++ node __hash_ offset",
                                "libc++ node hash field under the guarded version");
            }
        }
        if (name == "ES_BLOCK_STATE_NODE_OFF_KEY") {
            return measured(member_offset(*node, value.first), "libc++ node value key offset",
                            "libc++ node pair key with live probe value");
        }
        if (name == "ES_BLOCK_STATE_NODE_OFF_VARIANT") {
            return measured(member_offset(*node, value.second), "libc++ node value variant offset",
                            "libc++ node pair variant with active index validated");
        }
        if (name == "ES_BLOCK_STATE_NODE_OFF_VARIANT_INDEX") {
            const auto offset = libcxx_variant_index_offset();
            return offset ? measured(member_offset(*node, value.second) + *offset,
                                     "differential std::variant index-byte probe",
                                     "libc++ std::variant index() values across three alternatives")
                          : unresolved("libc++ variant index byte was not uniquely identified");
        }
    }
    return unresolved("unsupported libc++ BlockStates measurement");
#endif
}

struct LibcxxRefCountProbe final : std::__shared_weak_count {
    LibcxxRefCountProbe() : std::__shared_weak_count(0) {}
    void __on_zero_shared() noexcept override {}
    void __on_zero_shared_weak() noexcept override {}
};

Fact measure_libcxx_refcount(std::string_view name)
{
#if _LIBCPP_VERSION < 14000
    return unresolved("libc++ version guard requires shared-count owner fields");
#else
    LibcxxRefCountProbe sample;
    if constexpr (!requires(LibcxxRefCountProbe &value) { value.__shared_owners_; value.__shared_weak_owners_; }) {
        return unresolved("libc++ shared-count owner fields are not exposed under the guarded version");
    }
    else {
        std::shared_ptr<int> owner = std::make_shared<int>(7);
        std::weak_ptr<int> weak = owner;
        const bool live = owner.use_count() == 1 && weak.use_count() == 1;
        owner.reset();
        const bool expired = weak.expired();
        if (!live || !expired) {
            return unresolved("libc++ shared/weak lifecycle differential failed");
        }
        if (name == "ES_REFCOUNT_SIZE") {
            return measured(sizeof(std::__shared_weak_count), "sizeof(std::__shared_weak_count)",
                            "libc++ shared-count control block with lifecycle differential");
        }
        if (name == "ES_REFCOUNT_OFF_USES") {
            return measured(member_offset(sample, sample.__shared_owners_),
                            "libc++ __shared_count::__shared_owners_ offset",
                            "libc++ shared owner count with safe shared/weak lifecycle");
        }
        if (name == "ES_REFCOUNT_OFF_WEAKS") {
            return measured(member_offset(sample, sample.__shared_weak_owners_),
                            "libc++ __shared_weak_count::__shared_weak_owners_ offset",
                            "libc++ weak owner count with safe shared/weak lifecycle");
        }
        if (name == "ES_REFCOUNT_SLOT_DESTROY_RESOURCE") {
            using Method = void (std::__shared_weak_count::*)() noexcept;
            const auto slot = decode_itanium_slot(static_cast<Method>(&std::__shared_weak_count::__on_zero_shared));
            return slot ? measured(static_cast<std::int64_t>(*slot), "Itanium member-pointer ABI decode of __on_zero_shared",
                                   "libc++ zero-owner callback")
                        : unresolved("libc++ __on_zero_shared member-pointer encoding was not recognized");
        }
        if (name == "ES_REFCOUNT_SLOT_DELETE_THIS") {
            using Method = void (std::__shared_weak_count::*)() noexcept;
            const auto slot = decode_itanium_slot(static_cast<Method>(&std::__shared_weak_count::__on_zero_shared_weak));
            return slot ? measured(static_cast<std::int64_t>(*slot), "Itanium member-pointer ABI decode of __on_zero_shared_weak",
                                   "libc++ zero-weak callback")
                        : unresolved("libc++ __on_zero_shared_weak member-pointer encoding was not recognized");
        }
    }
    return unresolved("unsupported libc++ refcount measurement");
#endif
}

Fact measure_libcxx_optional(std::string_view name)
{
#if _LIBCPP_VERSION < 14000
    return unresolved("libc++ version guard requires __engaged_ optional representation");
#else
    if (name == "ES_OPTIONAL_STRING_SIZE") {
        return measured(sizeof(std::optional<std::string>), "sizeof(std::optional<std::string>)",
                        "libc++ version-guarded optional representation");
    }
    if (name == "ES_OPTIONAL_ITEM_STACK_SIZE") {
        return measured(sizeof(std::optional<endstone::ItemStack>), "sizeof(std::optional<ItemStack>)",
                        "libc++ version-guarded optional representation");
    }
    if constexpr (!has_engaged_member<std::optional<std::string>> ||
                  !has_engaged_member<std::optional<endstone::ItemStack>>) {
        return unresolved("libc++ optional __engaged_ member is not exposed under the guarded version");
    }
    else if (name == "ES_OPTIONAL_STRING_OFF_HAS_VALUE") {
        std::optional<std::string> sample;
        auto &flag = sample.__engaged_;
        if (flag) {
            return unresolved("libc++ optional string starts engaged unexpectedly");
        }
        sample.emplace("matching-stl");
        if (!flag) {
            return unresolved("libc++ optional string emplace did not engage __engaged_");
        }
        const auto offset = member_offset(sample, flag);
        sample.reset();
        return !flag ? measured(offset, "libc++ optional::__engaged_ offset",
                                "libc++ __engaged_ false/emplace true/reset false differential")
                     : unresolved("libc++ optional string reset did not clear __engaged_");
    }
    else if (name == "ES_OPTIONAL_ITEM_STACK_OFF_HAS_VALUE") {
        std::optional<endstone::ItemStack> sample;
        auto &flag = sample.__engaged_;
        if (flag) {
            return unresolved("libc++ optional ItemStack starts engaged unexpectedly");
        }
        sample.emplace(std::make_unique<OptionalItemStackImpl>());
        if (!flag) {
            return unresolved("libc++ optional ItemStack emplace did not engage __engaged_");
        }
        const auto offset = member_offset(sample, flag);
        sample.reset();
        return !flag ? measured(offset, "libc++ optional::__engaged_ offset",
                                "libc++ optional<ItemStack> __engaged_ false/emplace true/reset false differential")
                     : unresolved("libc++ optional ItemStack reset did not clear __engaged_");
    }
    return unresolved("unsupported libc++ optional measurement");
#endif
}

#endif  // _LIBCPP_VERSION

}  // namespace

Fact measure_stl(std::string_view name)
{
#if defined(_MSC_VER)
    if (name.rfind("ES_BLOCK_STATES_", 0) == 0 || name.rfind("ES_BLOCK_STATE_NODE_", 0) == 0) {
        return measure_block_states(name);
    }
    if (name.rfind("ES_REFCOUNT_", 0) == 0) {
        return measure_refcount(name);
    }
    if (name == "ES_OPTIONAL_STRING_OFF_HAS_VALUE") {
        std::optional<std::string> sample;
        const auto offset = optional_engaged_offset(sample);
        if (!offset || !verify_optional(sample)) {
            return unresolved("MSVC optional _Has_value emplace/reset differential failed");
        }
        return measured(*offset, "MSVC std::_Optional_destruct_base::_Has_value offset",
                        "MSVC _Has_value false/emplace true/reset false differential");
    }
    if (name == "ES_OPTIONAL_ITEM_STACK_OFF_HAS_VALUE") {
        std::optional<endstone::ItemStack> sample;
        const auto offset = optional_item_engaged_offset(sample);
        if (!offset || !verify_item_optional(sample)) {
            return unresolved("MSVC optional<ItemStack> _Has_value differential failed");
        }
        return measured(*offset, "MSVC std::_Optional_destruct_base::_Has_value offset",
                        "MSVC optional<ItemStack> _Has_value false/emplace/reset representation; no live registry value used");
    }
    if (name == "ES_OPTIONAL_STRING_SIZE") {
        return measured(sizeof(std::optional<std::string>), "sizeof(std::optional<std::string>)",
                        "MSVC STL version/update guarded optional representation");
    }
    if (name == "ES_OPTIONAL_ITEM_STACK_SIZE") {
        return measured(sizeof(std::optional<endstone::ItemStack>), "sizeof(std::optional<ItemStack>)",
                        "MSVC STL version/update guarded optional representation");
    }
#elif defined(_LIBCPP_VERSION)
    if (name.rfind("ES_BLOCK_STATES_", 0) == 0 || name.rfind("ES_BLOCK_STATE_NODE_", 0) == 0) {
        return measure_libcxx_block_states(name);
    }
    if (name.rfind("ES_REFCOUNT_", 0) == 0) {
        return measure_libcxx_refcount(name);
    }
    if (name.rfind("ES_OPTIONAL_", 0) == 0) {
        return measure_libcxx_optional(name);
    }
    return unresolved("no matching libc++ representation measurement");
#else
    (void)name;
    return unresolved("unsupported standard library");
#endif
    return unresolved("no matching standard-library measurement");
}

}  // namespace abi_probe

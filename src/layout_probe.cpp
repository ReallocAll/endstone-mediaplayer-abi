// Controlled layout access is kept in this translation unit. Standard
// library headers are included before the access macros so STL declarations
// are never rewritten.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <filesystem>
#include <format>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#define private public
#define protected public
#include <endstone/command/command.h>
#include <endstone/event/player/player_event.h>
#include <endstone/level/location.h>
#include <endstone/map/map_renderer.h>
#include <endstone/message.h>
#include <endstone/plugin/plugin_description.h>
#undef protected
#undef private

#include <endstone/block/block_data.h>
#include <endstone/inventory/item_stack.h>
#include <endstone/inventory/item_type.h>
#include <endstone/permissions/permission.h>
#include <endstone/plugin/plugin.h>
#include <endstone/registry.h>
#include <endstone/util/uuid.h>

#include "internal_probe.h"
#include "layout_probe.h"
#include "stl_probe.h"

namespace abi_probe {
namespace {

template <typename Object, typename Member>
std::int64_t member_offset(Object &object, Member &member)
{
    return static_cast<std::int64_t>(reinterpret_cast<const std::byte *>(std::addressof(member)) -
                                     reinterpret_cast<const std::byte *>(std::addressof(object)));
}

Fact measured(std::int64_t value, std::string_view evidence)
{
    return {value, "sizeof/alignof/controlled member address difference", evidence};
}

Fact unresolved(std::string_view evidence)
{
    return {std::nullopt, "runtime representation probe", evidence};
}

class MinimalPlugin final : public endstone::Plugin {
public:
    [[nodiscard]] const endstone::PluginDescription &getDescription() const override { return description_; }

private:
    endstone::PluginDescription description_{"abi_layout_probe", "runtime"};
};

class LayoutMapRenderer final : public endstone::MapRenderer {
public:
    using endstone::MapRenderer::MapRenderer;

    void render(endstone::MapView &, endstone::MapCanvas &, endstone::Player &) override {}
};

std::optional<std::int64_t> unique_pointer_offset(const endstone::Plugin &plugin, const void *target)
{
    if (target == nullptr) {
        return std::nullopt;
    }
    const auto *bytes = reinterpret_cast<const std::byte *>(std::addressof(plugin));
    std::optional<std::int64_t> result;
    for (std::size_t offset = 0; offset + sizeof(target) <= sizeof(endstone::Plugin); ++offset) {
        const void *candidate = nullptr;
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

std::optional<std::int64_t> message_index_offset()
{
    using Message = endstone::Message;
    alignas(Message) std::array<std::byte, sizeof(Message)> string_storage{};
    alignas(Message) std::array<std::byte, sizeof(Message)> translated_storage{};
    auto *string_message = std::construct_at(reinterpret_cast<Message *>(string_storage.data()), std::string("probe"));
    auto *translated_message =
        std::construct_at(reinterpret_cast<Message *>(translated_storage.data()), endstone::Translatable("probe"));

    const auto string_index = string_message->index();
    const auto translated_index = translated_message->index();
    std::vector<std::size_t> candidates;
    for (std::size_t offset = 0; offset < sizeof(Message); ++offset) {
        const auto string_byte = std::to_integer<unsigned char>(string_storage[offset]);
        const auto translated_byte = std::to_integer<unsigned char>(translated_storage[offset]);
        if (string_byte == string_index && translated_byte == translated_index && string_index != translated_index) {
            candidates.push_back(offset);
        }
    }
    std::destroy_at(string_message);
    std::destroy_at(translated_message);
    return candidates.size() == 1 ? std::optional<std::int64_t>(static_cast<std::int64_t>(candidates.front()))
                                  : std::nullopt;
}

}  // namespace

Fact measure_layout(std::string_view name, const endstone::Plugin *live_plugin, bool live_context)
{
    if (name == "ES_BLOCK_SOURCE_SLOT_GET_BLOCK_ENTITY" || name == "ES_ENDSTONE_BLOCK_OFF_BLOCK_SOURCE" ||
        name == "ES_ENDSTONE_PLAYER_OFF_OFFLINE_PLAYER" || name == "ES_MAPCANVAS_SIZE" ||
        name == "ES_MAPCANVAS_OFF_BUFFER_BEGIN" || name == "ES_MAPCANVAS_OFF_BUFFER_END" ||
        name == "ES_PLAYER_EVENT_OFF_PLAYER" || name == "ES_LOCATION_SIZE" || name == "ES_LOCATION_ALIGN" ||
        name.rfind("ES_LOCATION_OFF_", 0) == 0) {
        return measure_internal(name);
    }
    if (name.rfind("ES_BLOCK_STATES_", 0) == 0 || name.rfind("ES_BLOCK_STATE_NODE_", 0) == 0 ||
        name.rfind("ES_REFCOUNT_", 0) == 0 || name == "ES_OPTIONAL_STRING_SIZE" ||
        name == "ES_OPTIONAL_STRING_OFF_HAS_VALUE" || name == "ES_OPTIONAL_ITEM_STACK_SIZE" ||
        name == "ES_OPTIONAL_ITEM_STACK_OFF_HAS_VALUE") {
        return measure_stl(name);
    }
    if (name == "ES_STRING_SIZE") {
        return measured(sizeof(std::string), "sizeof(std::string)");
    }
    if (name == "ES_VECTOR_SIZE") {
        return measured(sizeof(std::vector<void *>), "sizeof(std::vector<void*>)");
    }
    if (name == "ES_STD_FUNCTION_SIZE") {
        return measured(sizeof(std::function<void()>), "sizeof(std::function<void()>)");
    }
    if (name == "ES_PLUGIN_SIZE") {
        return measured(sizeof(endstone::Plugin), "sizeof(endstone::Plugin)");
    }
    if (name == "ES_PLUGIN_OFF_SERVER" && live_context && live_plugin != nullptr) {
        const auto offset = unique_pointer_offset(*live_plugin, std::addressof(live_plugin->getServer()));
        return offset ? measured(*offset, "unique live Server pointer in Plugin object bytes")
                      : unresolved("live Server pointer was absent or not unique");
    }
    if (name == "ES_PLUGIN_OFF_LOGGER" && live_context && live_plugin != nullptr) {
        const auto offset = unique_pointer_offset(*live_plugin, std::addressof(live_plugin->getLogger()));
        return offset ? measured(*offset, "unique live Logger pointer in Plugin object bytes")
                      : unresolved("live Logger pointer was absent or not unique");
    }
    if (name == "ES_PLUGIN_OFF_DESCRIPTION") {
        if (!live_context || live_plugin == nullptr) {
            return unresolved("live Plugin-derived description is unavailable outside onEnable");
        }
        const auto offset = reinterpret_cast<const std::byte *>(std::addressof(live_plugin->getDescription())) -
                            reinterpret_cast<const std::byte *>(live_plugin);
        return measured(static_cast<std::int64_t>(offset), "live Plugin-derived description address difference");
    }
    if (name == "ES_DESCRIPTION_SIZE") {
        return measured(sizeof(endstone::PluginDescription), "sizeof(endstone::PluginDescription)");
    }
    if (name == "ES_PLUGIN_IMPL_SIZE") {
        return measured(sizeof(MinimalPlugin), "sizeof minimal Plugin-derived object with PluginDescription");
    }
    if (name == "ES_DESC_OFF_NAME" || name == "ES_DESC_OFF_VERSION" || name == "ES_DESC_OFF_FULL_NAME" ||
        name == "ES_DESC_OFF_API_VERSION" || name == "ES_DESC_OFF_DESCRIPTION" || name == "ES_DESC_OFF_LOAD" ||
        name == "ES_DESC_OFF_AUTHORS" || name == "ES_DESC_OFF_CONTRIBUTORS" || name == "ES_DESC_OFF_WEBSITE" ||
        name == "ES_DESC_OFF_PREFIX" || name == "ES_DESC_OFF_PROVIDES" || name == "ES_DESC_OFF_DEPEND" ||
        name == "ES_DESC_OFF_SOFT_DEPEND" || name == "ES_DESC_OFF_LOAD_BEFORE" || name == "ES_DESC_OFF_DEFAULT_PERM" ||
        name == "ES_DESC_OFF_COMMANDS" || name == "ES_DESC_OFF_PERMISSIONS") {
        endstone::PluginDescription description("probe", "runtime");
        if (name == "ES_DESC_OFF_NAME") return measured(member_offset(description, description.name_), "PluginDescription::name_");
        if (name == "ES_DESC_OFF_VERSION") return measured(member_offset(description, description.version_), "PluginDescription::version_");
        if (name == "ES_DESC_OFF_FULL_NAME") return measured(member_offset(description, description.full_name_), "PluginDescription::full_name_");
        if (name == "ES_DESC_OFF_API_VERSION") return measured(member_offset(description, description.api_version_), "PluginDescription::api_version_");
        if (name == "ES_DESC_OFF_DESCRIPTION") return measured(member_offset(description, description.description_), "PluginDescription::description_");
        if (name == "ES_DESC_OFF_LOAD") return measured(member_offset(description, description.load_), "PluginDescription::load_");
        if (name == "ES_DESC_OFF_AUTHORS") return measured(member_offset(description, description.authors_), "PluginDescription::authors_");
        if (name == "ES_DESC_OFF_CONTRIBUTORS") return measured(member_offset(description, description.contributors_), "PluginDescription::contributors_");
        if (name == "ES_DESC_OFF_WEBSITE") return measured(member_offset(description, description.website_), "PluginDescription::website_");
        if (name == "ES_DESC_OFF_PREFIX") return measured(member_offset(description, description.prefix_), "PluginDescription::prefix_");
        if (name == "ES_DESC_OFF_PROVIDES") return measured(member_offset(description, description.provides_), "PluginDescription::provides_");
        if (name == "ES_DESC_OFF_DEPEND") return measured(member_offset(description, description.depend_), "PluginDescription::depend_");
        if (name == "ES_DESC_OFF_SOFT_DEPEND") return measured(member_offset(description, description.soft_depend_), "PluginDescription::soft_depend_");
        if (name == "ES_DESC_OFF_LOAD_BEFORE") return measured(member_offset(description, description.load_before_), "PluginDescription::load_before_");
        if (name == "ES_DESC_OFF_DEFAULT_PERM") return measured(member_offset(description, description.default_permission_), "PluginDescription::default_permission_");
        if (name == "ES_DESC_OFF_COMMANDS") return measured(member_offset(description, description.commands_), "PluginDescription::commands_");
        return measured(member_offset(description, description.permissions_), "PluginDescription::permissions_");
    }
    if (name == "ES_COMMAND_SIZE" || name.rfind("ES_COMMAND_OFF_", 0) == 0) {
        endstone::Command command("probe");
        if (name == "ES_COMMAND_SIZE") return measured(sizeof(endstone::Command), "sizeof(endstone::Command)");
        if (name == "ES_COMMAND_OFF_NAME") return measured(member_offset(command, command.name_), "Command::name_");
        if (name == "ES_COMMAND_OFF_DESC") return measured(member_offset(command, command.description_), "Command::description_");
        if (name == "ES_COMMAND_OFF_ALIASES") return measured(member_offset(command, command.aliases_), "Command::aliases_");
        if (name == "ES_COMMAND_OFF_USAGES") return measured(member_offset(command, command.usages_), "Command::usages_");
        return measured(member_offset(command, command.permissions_), "Command::permissions_");
    }
    if (name == "ES_MESSAGE_SIZE") return measured(sizeof(endstone::Message), "sizeof(endstone::Message)");
    if (name == "ES_MESSAGE_OFF_STRING") {
        endstone::Message message = std::string("probe");
        return measured(member_offset(message, std::get<std::string>(message)), "std::get<string>(Message) address difference");
    }
    if (name == "ES_MESSAGE_OFF_INDEX") {
        const auto offset = message_index_offset();
        return offset ? measured(*offset, "differential scan of runtime Message::index() values")
                      : unresolved("Message index byte was not uniquely identified by two alternatives");
    }
    if (name == "ES_MESSAGE_STRING_INDEX") {
        endstone::Message message = std::string("probe");
        return measured(static_cast<std::int64_t>(message.index()), "runtime Message(string).index()");
    }
    if (name == "ES_PERMISSION_SIZE") return measured(sizeof(endstone::Permission), "sizeof(endstone::Permission)");
    if (name == "ES_SHARED_PTR_SIZE") return measured(sizeof(std::shared_ptr<void>), "sizeof(std::shared_ptr<void>)");
    if (name == "ES_UUID_SIZE") return measured(sizeof(endstone::UUID), "sizeof(endstone::UUID)");
    if (name == "ES_MAPRENDERER_SIZE" || name == "ES_MAPRENDERER_OFF_IS_CONTEXTUAL") {
        LayoutMapRenderer renderer(false);
        if (name == "ES_MAPRENDERER_SIZE") return measured(sizeof(endstone::MapRenderer), "sizeof(endstone::MapRenderer)");
        return measured(member_offset(renderer, renderer.is_contextual_), "MapRenderer::is_contextual_");
    }
    return unresolved("no controlled public Endstone layout expression for this name");
}

}  // namespace abi_probe

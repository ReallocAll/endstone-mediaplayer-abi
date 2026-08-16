#include "registry.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace abi_probe {
namespace {

// This table is deliberately a strategy registry, not a copy of either
// generated consumer header.  The platform mask is the result of the source
// reference contract; no expected measurement value is stored here.
constexpr RegistryEntry kRegistry[] = {
    {"ES_API_VERSION", Platform::Both, "runtime Endstone API version", ""},
    {"ES_BAR_COLOR_GREEN", Platform::Both, "enum endstone::BarColor::Green", ""},
    {"ES_BAR_STYLE_SOLID", Platform::Both, "enum endstone::BarStyle::Solid", ""},
    {"ES_BLOCK_DATA_SLOT_DELETE", Platform::Both, "ABI-prefix destructor of endstone::BlockData", ""},
    {"ES_BLOCK_SLOT_DELETE", Platform::Both, "ABI-prefix deleting destructor of endstone::Block", "ES_BLOCK_SLOT_GET_TYPE"},
    {"ES_BLOCK_SLOT_GET_TYPE", Platform::Both, "vslot(endstone::Block::getType)", ""},
    {"ES_BLOCK_SLOT_SET_DATA", Platform::Both, "vslot(endstone::Block::setData)", ""},
    {"ES_BLOCK_SOURCE_SLOT_GET_BLOCK_ENTITY", Platform::Windows, "vslot(IConstBlockSource::getBlockEntity)", ""},
    {"ES_BLOCK_STATE_NODE_OFF_HASH", Platform::Linux, "libc++ unordered-node hash member offset", ""},
    {"ES_BLOCK_STATE_NODE_OFF_KEY", Platform::Both, "unordered-node pair key member offset", ""},
    {"ES_BLOCK_STATE_NODE_OFF_NEXT", Platform::Linux, "libc++ unordered-node next member offset", ""},
    {"ES_BLOCK_STATE_NODE_OFF_VARIANT", Platform::Both, "unordered-node pair variant member offset", ""},
    {"ES_BLOCK_STATE_NODE_OFF_VARIANT_INDEX", Platform::Both, "unordered-node variant index member offset", ""},
    {"ES_BLOCK_STATE_NODE_SIZE", Platform::Both, "matching-stdlib unordered-node sizeof", ""},
    {"ES_BLOCK_STATES_OFF_BUCKET_COUNT", Platform::Linux, "libc++ unordered-table bucket-count member offset", ""},
    {"ES_BLOCK_STATES_OFF_BUCKETS", Platform::Linux, "libc++ unordered-table bucket member offset", ""},
    {"ES_BLOCK_STATES_OFF_FIRST_NODE", Platform::Linux, "libc++ unordered-table first-node member offset", ""},
    {"ES_BLOCK_STATES_OFF_HEAD", Platform::Windows, "MSVC unordered-table list-head member offset", ""},
    {"ES_BLOCK_STATES_OFF_MASK", Platform::Windows, "MSVC unordered-table mask member offset", ""},
    {"ES_BLOCK_STATES_OFF_MAX_INDEX", Platform::Windows, "MSVC unordered-table maximum-index member offset", ""},
    {"ES_BLOCK_STATES_OFF_MAX_LOAD_FACTOR", Platform::Linux, "libc++ unordered-table max-load-factor member offset", ""},
    {"ES_BLOCK_STATES_OFF_SIZE", Platform::Both, "matching-stdlib unordered-table size member offset", ""},
    {"ES_BLOCK_STATES_OFF_VECTOR", Platform::Windows, "MSVC unordered-table bucket-vector member offset", ""},
    {"ES_BLOCK_STATES_SIZE", Platform::Both, "sizeof(endstone::BlockStates)", ""},
    {"ES_BOSSBAR_SLOT_ADD_PLAYER", Platform::Both, "vslot(endstone::BossBar::addPlayer)", ""},
    {"ES_BOSSBAR_SLOT_DTOR", Platform::Both, "ABI-prefix destructor of endstone::BossBar", "ES_BOSSBAR_SLOT_SET_TITLE"},
    {"ES_BOSSBAR_SLOT_SET_PROGRESS", Platform::Both, "vslot(endstone::BossBar::setProgress)", ""},
    {"ES_BOSSBAR_SLOT_SET_TITLE", Platform::Both, "vslot(endstone::BossBar::setTitle)", ""},
    {"ES_BOSSBAR_SLOT_SET_VISIBLE", Platform::Both, "vslot(endstone::BossBar::setVisible)", ""},
    {"ES_CMD_VTABLE_SLOT_COUNT", Platform::Both, "ABI-prefix contiguous endstone::Command virtual sequence", ""},
    {"ES_COMMAND_OFF_DESC", Platform::Both, "address difference endstone::Command::description_", ""},
    {"ES_COMMAND_OFF_NAME", Platform::Both, "address difference endstone::Command::name_", ""},
    {"ES_COMMAND_OFF_USAGES", Platform::Both, "address difference endstone::Command::usages_", ""},
    {"ES_COMMAND_SIZE", Platform::Both, "sizeof(endstone::Command)", ""},
    {"ES_DESC_OFF_API_VERSION", Platform::Both, "address difference endstone::PluginDescription::api_version_", ""},
    {"ES_DESC_OFF_AUTHORS", Platform::Both, "address difference endstone::PluginDescription::authors_", ""},
    {"ES_DESC_OFF_COMMANDS", Platform::Both, "address difference endstone::PluginDescription::commands_", ""},
    {"ES_DESC_OFF_CONTRIBUTORS", Platform::Both, "address difference endstone::PluginDescription::contributors_", ""},
    {"ES_DESC_OFF_DEFAULT_PERM", Platform::Both, "address difference endstone::PluginDescription::default_permission_", ""},
    {"ES_DESC_OFF_DEPEND", Platform::Both, "address difference endstone::PluginDescription::depend_", ""},
    {"ES_DESC_OFF_DESCRIPTION", Platform::Both, "address difference endstone::PluginDescription::description_", ""},
    {"ES_DESC_OFF_FULL_NAME", Platform::Both, "address difference endstone::PluginDescription::full_name_", ""},
    {"ES_DESC_OFF_LOAD", Platform::Both, "address difference endstone::PluginDescription::load_", ""},
    {"ES_DESC_OFF_LOAD_BEFORE", Platform::Both, "address difference endstone::PluginDescription::load_before_", ""},
    {"ES_DESC_OFF_NAME", Platform::Both, "address difference endstone::PluginDescription::name_", ""},
    {"ES_DESC_OFF_PERMISSIONS", Platform::Both, "address difference endstone::PluginDescription::permissions_", ""},
    {"ES_DESC_OFF_PREFIX", Platform::Both, "address difference endstone::PluginDescription::prefix_", ""},
    {"ES_DESC_OFF_PROVIDES", Platform::Both, "address difference endstone::PluginDescription::provides_", ""},
    {"ES_DESC_OFF_SOFT_DEPEND", Platform::Both, "address difference endstone::PluginDescription::soft_depend_", ""},
    {"ES_DESC_OFF_VERSION", Platform::Both, "address difference endstone::PluginDescription::version_", ""},
    {"ES_DESC_OFF_WEBSITE", Platform::Both, "address difference endstone::PluginDescription::website_", ""},
    {"ES_DESCRIPTION_SIZE", Platform::Both, "sizeof(endstone::PluginDescription)", ""},
    {"ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ", Platform::Both, "vslot(endstone::Dimension::getBlockAt(int,int,int))", ""},
    {"ES_DIMENSION_SLOT_GET_NAME", Platform::Both, "vslot(endstone::Dimension::getName)", ""},
    {"ES_ENDSTONE_BLOCK_OFF_BLOCK_SOURCE", Platform::Both, "member-pointer offset endstone::core::EndstoneBlock::block_source_", ""},
    {"ES_ENDSTONE_PLAYER_OFF_OFFLINE_PLAYER", Platform::Both, "ABI secondary-base adjustment EndstonePlayer to OfflinePlayer", ""},
    {"ES_INVENTORY_SLOT_CLEAR_ALL", Platform::Both, "vslot(endstone::Inventory::clear())", ""},
    {"ES_INVENTORY_SLOT_CLEAR_SLOT", Platform::Both, "vslot(endstone::Inventory::clear(int))", ""},
    {"ES_INVENTORY_SLOT_GET_ITEM", Platform::Both, "vslot(endstone::Inventory::getItem)", ""},
    {"ES_INVENTORY_SLOT_GET_SIZE", Platform::Both, "vslot(endstone::Inventory::getSize)", ""},
    {"ES_INVENTORY_SLOT_SET_ITEM", Platform::Both, "vslot(endstone::Inventory::setItem)", ""},
    {"ES_ITEM_META_SLOT_DELETE", Platform::Both, "ABI-prefix deleting destructor of endstone::ItemMeta", "ES_ITEM_META_SLOT_GET_TYPE"},
    {"ES_ITEM_META_SLOT_GET_TYPE", Platform::Both, "vslot(endstone::ItemMeta::getType)", ""},
    {"ES_ITEM_META_SLOT_SET_DISPLAY_NAME", Platform::Both, "vslot(endstone::ItemMeta::setDisplayName)", ""},
    {"ES_ITEM_META_SLOT_SET_LORE", Platform::Both, "vslot(endstone::ItemMeta::setLore)", ""},
    {"ES_ITEM_REGISTRY_SLOT_GET", Platform::Both, "vslot(nonconst Registry<ItemType>::get)", ""},
    {"ES_ITEM_STACK_SLOT_DELETE", Platform::Both, "ABI-prefix deleting destructor of endstone::ItemStack::Impl", ""},
    {"ES_ITEM_STACK_SLOT_GET_ITEM_META", Platform::Both, "vslot(endstone::ItemStack::Impl::getItemMeta)", ""},
    {"ES_ITEM_STACK_SLOT_SET_ITEM_META", Platform::Both, "vslot(endstone::ItemStack::Impl::setItemMeta)", ""},
    {"ES_ITEM_TYPE_SLOT_CREATE_ITEM_STACK", Platform::Both, "vslot(endstone::ItemType::createItemStack)", ""},
    {"ES_LOAD_POST_WORLD", Platform::Both, "enum endstone::PluginLoadOrder::PostWorld", ""},
    {"ES_LOCATION_ALIGN", Platform::Both, "alignof(endstone::Location)", ""},
    {"ES_LOCATION_OFF_DIMENSION", Platform::Both, "member-pointer offset endstone::Location::dimension_", ""},
    {"ES_LOCATION_OFF_PITCH", Platform::Both, "member-pointer offset endstone::Location::pitch_", ""},
    {"ES_LOCATION_OFF_X", Platform::Both, "member-pointer offset endstone::Location::x_", ""},
    {"ES_LOCATION_OFF_Y", Platform::Both, "member-pointer offset endstone::Location::y_", ""},
    {"ES_LOCATION_OFF_YAW", Platform::Both, "member-pointer offset endstone::Location::yaw_", ""},
    {"ES_LOCATION_OFF_Z", Platform::Both, "member-pointer offset endstone::Location::z_", ""},
    {"ES_LOCATION_SIZE", Platform::Both, "sizeof(endstone::Location)", ""},
    {"ES_LOG_INFO", Platform::Both, "enum endstone::Logger::Info", ""},
    {"ES_LOGGER_SLOT_LOG", Platform::Both, "vslot(endstone::Logger::log)", ""},
    {"ES_MAP_META_SLOT_GET_MAP_ID", Platform::Both, "vslot(endstone::MapMeta::getMapId)", ""},
    {"ES_MAP_META_SLOT_HAS_MAP_ID", Platform::Both, "vslot(endstone::MapMeta::hasMapId)", ""},
    {"ES_MAP_META_SLOT_SET_MAP_VIEW", Platform::Both, "vslot(endstone::MapMeta::setMapView)", ""},
    {"ES_MAPCANVAS_OFF_BUFFER_BEGIN", Platform::Both, "member-pointer plus matching std::vector begin suboffset", ""},
    {"ES_MAPCANVAS_OFF_BUFFER_END", Platform::Both, "member-pointer plus matching std::vector end suboffset", ""},
    {"ES_MAPCANVAS_SIZE", Platform::Both, "sizeof(endstone::core::EndstoneMapCanvas)", ""},
    {"ES_MAPRENDERER_OFF_IS_CONTEXTUAL", Platform::Both, "address difference endstone::MapRenderer::is_contextual_", ""},
    {"ES_MAPRENDERER_SIZE", Platform::Both, "sizeof(endstone::MapRenderer)", ""},
    {"ES_MAPRENDERER_SLOT_INIT", Platform::Both, "vslot(endstone::MapRenderer::initialize)", ""},
    {"ES_MAPRENDERER_SLOT_RENDER", Platform::Both, "vslot(endstone::MapRenderer::render)", ""},
    {"ES_MAPVIEW_SLOT_ADD_RENDERER", Platform::Both, "vslot(endstone::MapView::addRenderer)", ""},
    {"ES_MAPVIEW_SLOT_GET_ID", Platform::Both, "vslot(endstone::MapView::getId)", ""},
    {"ES_MAPVIEW_SLOT_REMOVE_RENDERER", Platform::Both, "vslot(endstone::MapView::removeRenderer)", ""},
    {"ES_MAPVIEW_SLOT_SET_LOCKED", Platform::Both, "vslot(endstone::MapView::setLocked)", ""},
    {"ES_MESSAGE_OFF_INDEX", Platform::Both, "differential std::variant index-byte probe", ""},
    {"ES_MESSAGE_OFF_STRING", Platform::Both, "address difference std::get<std::string>(endstone::Message)", ""},
    {"ES_MESSAGE_SIZE", Platform::Both, "sizeof(endstone::Message)", ""},
    {"ES_MESSAGE_STRING_INDEX", Platform::Both, "endstone::Message(std::string).index()", ""},
    {"ES_OFFLINE_PLAYER_SLOT_GET_UNIQUE_ID", Platform::Both, "vslot(endstone::OfflinePlayer::getUniqueId)", ""},
    {"ES_OPTIONAL_ITEM_STACK_OFF_HAS_VALUE", Platform::Both, "private optional engaged-member offset with emplace/reset differential", ""},
    {"ES_OPTIONAL_ITEM_STACK_SIZE", Platform::Both, "sizeof(std::optional<endstone::ItemStack>)", ""},
    {"ES_OPTIONAL_STRING_OFF_HAS_VALUE", Platform::Both, "private optional engaged-member offset with emplace/reset differential", ""},
    {"ES_OPTIONAL_STRING_SIZE", Platform::Both, "sizeof(std::optional<std::string>)", ""},
    {"ES_PERM_OPERATOR", Platform::Both, "enum endstone::PermissionDefault::Operator", ""},
    {"ES_PERMISSION_SIZE", Platform::Both, "sizeof(endstone::Permission)", ""},
    {"ES_PLAYER_EVENT_OFF_PLAYER", Platform::Both, "member-pointer offset endstone::PlayerEvent::player_", ""},
    {"ES_PLAYER_SLOT_GET_DIMENSION", Platform::Both, "vslot(endstone::Actor::getDimension)", ""},
    {"ES_PLAYER_SLOT_GET_INVENTORY", Platform::Both, "vslot(endstone::Player::getInventory)", ""},
    {"ES_PLAYER_SLOT_GET_LOCATION", Platform::Both, "vslot(endstone::Actor::getLocation)", ""},
    {"ES_PLAYER_SLOT_IS_OP", Platform::Both, "vslot(endstone::Player::isOp)", ""},
    {"ES_PLAYER_SLOT_PLAY_SOUND", Platform::Both, "vslot(endstone::Player::playSound)", ""},
    {"ES_PLAYER_SLOT_SEND_MAP", Platform::Both, "vslot(endstone::Player::sendMap)", ""},
    {"ES_PLAYER_SLOT_SEND_POPUP", Platform::Both, "vslot(endstone::Player::sendPopup)", ""},
    {"ES_PLAYER_SLOT_SEND_TIP", Platform::Both, "vslot(endstone::Player::sendTip)", ""},
    {"ES_PLUGIN_IMPL_SIZE", Platform::Both, "sizeof(runtime minimal Plugin-derived object)", ""},
    {"ES_PLUGIN_OFF_DESCRIPTION", Platform::Both, "live derived Plugin description address difference", ""},
    {"ES_PLUGIN_OFF_LOGGER", Platform::Both, "unique Logger pointer in live Plugin object bytes", ""},
    {"ES_PLUGIN_OFF_SERVER", Platform::Both, "unique Server pointer in live Plugin object bytes", ""},
    {"ES_PM_SLOT_REGISTER_EVENT", Platform::Both, "vslot(endstone::PluginManager::registerEvent)", ""},
    {"ES_PRIORITY_NORMAL", Platform::Both, "enum endstone::EventPriority::Normal", ""},
    {"ES_REFCOUNT_OFF_USES", Platform::Both, "matching stdlib shared-control-block owners member offset", ""},
    {"ES_REFCOUNT_OFF_WEAKS", Platform::Both, "matching stdlib shared-control-block weak-owners member offset", ""},
    {"ES_REFCOUNT_SIZE", Platform::Both, "matching stdlib shared-control-block sizeof", ""},
    {"ES_REFCOUNT_SLOT_DELETE_THIS", Platform::Both, "shared-control-block zero-weak callback vslot", ""},
    {"ES_REFCOUNT_SLOT_DESTROY_RESOURCE", Platform::Both, "shared-control-block zero-owner callback vslot", ""},
    {"ES_SCHEDULER_SLOT_RUN_TIMER", Platform::Both, "vslot(endstone::Scheduler::runTaskTimer)", ""},
    {"ES_SENDER_SLOT_AS_PLAYER", Platform::Both, "vslot(endstone::CommandSender::asPlayer)", ""},
    {"ES_SENDER_SLOT_SEND_MESSAGE", Platform::Both, "vslot(endstone::CommandSender::sendMessage)", ""},
    {"ES_SERVER_SLOT_CREATE_BLOCK_DATA", Platform::Both, "vslot(endstone::Server::createBlockData(std::string))", ""},
    {"ES_SERVER_SLOT_CREATE_BLOCK_DATA_STATES", Platform::Both, "vslot(endstone::Server::createBlockData(std::string,BlockStates))", ""},
    {"ES_SERVER_SLOT_CREATE_BOSS_BAR", Platform::Both, "vslot(endstone::Server::createBossBar(std::string,BarColor,BarStyle))", ""},
    {"ES_SERVER_SLOT_CREATE_MAP", Platform::Both, "vslot(endstone::Server::createMap)", ""},
    {"ES_SERVER_SLOT_GET_MAP", Platform::Both, "vslot(endstone::Server::getMap)", ""},
    {"ES_SERVER_SLOT_GET_ONLINE_PLAYERS", Platform::Both, "vslot(endstone::Server::getOnlinePlayers)", ""},
    {"ES_SERVER_SLOT_GET_PLUGIN_MANAGER", Platform::Both, "vslot(endstone::Server::getPluginManager)", ""},
    {"ES_SERVER_SLOT_GET_REGISTRY", Platform::Both, "vslot(endstone::Server::_getRegistry)", ""},
    {"ES_SERVER_SLOT_GET_SCHEDULER", Platform::Both, "vslot(endstone::Server::getScheduler)", ""},
    {"ES_SHARED_PTR_SIZE", Platform::Both, "sizeof(std::shared_ptr<void>)", ""},
    {"ES_STD_FUNCTION_SIZE", Platform::Both, "sizeof(std::function<void()>)", ""},
    {"ES_STRING_SIZE", Platform::Both, "sizeof(std::string)", ""},
    {"ES_UUID_SIZE", Platform::Both, "sizeof(endstone::UUID)", ""},
    {"ES_VECTOR_SIZE", Platform::Both, "sizeof(std::vector<void*>)", ""},
    {"ES_VTABLE_SLOT_COUNT", Platform::Both, "ABI-prefix contiguous endstone::Plugin virtual sequence", ""},
};

std::vector<std::string> split_dependencies(std::string_view text)
{
    std::vector<std::string> result;
    while (!text.empty()) {
        const auto separator = text.find(',');
        const auto token = text.substr(0, separator);
        if (!token.empty()) {
            result.emplace_back(token);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        text.remove_prefix(separator + 1);
    }
    return result;
}

std::size_t find_entry(std::string_view name)
{
    for (std::size_t index = 0; index < std::size(kRegistry); ++index) {
        if (kRegistry[index].name == name) {
            return index;
        }
    }
    return std::size(kRegistry);
}

bool visit(std::size_t index, std::array<unsigned char, std::size(kRegistry)> &marks, std::string &error)
{
    if (marks[index] == 1) {
        error = "dependency cycle at " + std::string(kRegistry[index].name);
        return false;
    }
    if (marks[index] == 2) {
        return true;
    }
    marks[index] = 1;
    for (const auto &dependency : split_dependencies(kRegistry[index].dependencies)) {
        const auto dependency_index = find_entry(dependency);
        if (dependency_index == std::size(kRegistry)) {
            error = std::string(kRegistry[index].name) + " depends on missing " + dependency;
            return false;
        }
        if (dependency_index == index) {
            error = std::string(kRegistry[index].name) + " depends on itself";
            return false;
        }
        if (!visit(dependency_index, marks, error)) {
            return false;
        }
    }
    marks[index] = 2;
    return true;
}

}  // namespace

std::span<const RegistryEntry> registry()
{
    return kRegistry;
}

bool applies_to_current_platform(Platform platform)
{
#if defined(_WIN32)
    return platform == Platform::Windows || platform == Platform::Both;
#else
    return platform == Platform::Linux || platform == Platform::Both;
#endif
}

RegistryValidation validate_registry()
{
    for (std::size_t index = 0; index < std::size(kRegistry); ++index) {
        for (std::size_t other = index + 1; other < std::size(kRegistry); ++other) {
            if (kRegistry[index].name == kRegistry[other].name) {
                return {false, "duplicate registry name " + std::string(kRegistry[index].name)};
            }
        }
    }

    std::array<unsigned char, std::size(kRegistry)> marks{};
    std::string error;
    for (std::size_t index = 0; index < std::size(kRegistry); ++index) {
        if (!visit(index, marks, error)) {
            return {false, std::move(error)};
        }
    }
    return {true, {}};
}

}  // namespace abi_probe

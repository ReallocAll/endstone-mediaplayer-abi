#include "vtable_probe.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

#define private public
#include <endstone/actor/actor.h>
#include <endstone/block/block.h>
#include <endstone/block/block_data.h>
#include <endstone/boss/boss_bar.h>
#include <endstone/command/command.h>
#include <endstone/command/command_executor.h>
#include <endstone/command/command_sender.h>
#include <endstone/event/event.h>
#include <endstone/inventory/inventory.h>
#include <endstone/inventory/item_type.h>
#include <endstone/inventory/item_stack.h>
#include <endstone/inventory/meta/item_meta.h>
#include <endstone/inventory/meta/map_meta.h>
#include <endstone/level/dimension.h>
#include <endstone/logger.h>
#include <endstone/map/map_canvas.h>
#include <endstone/map/map_view.h>
#include <endstone/map/map_renderer.h>
#undef private
#include <endstone/offline_player.h>
#include <endstone/player.h>
#include <endstone/plugin/plugin.h>
#include <endstone/plugin/plugin_manager.h>
#include <endstone/registry.h>
#include <endstone/scheduler/scheduler.h>
#include <endstone/server.h>

namespace abi_probe {
namespace {

template <typename Method>
std::optional<std::size_t> decode_slot(Method method)
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
    // mov rax,[rcx]; jmp qword ptr [rax]
    if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0xFF && code[4] == 0x20) {
        byte_offset = 0;
    }
    // mov rax,[rcx]; jmp qword ptr [rax+disp8]
    else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0xFF && code[4] == 0x60) {
        byte_offset = code[5];
    }
    // mov rax,[rcx]; jmp qword ptr [rax+disp32]
    else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0xFF && code[4] == 0xA0) {
        std::uint32_t displacement = 0;
        std::memcpy(&displacement, code + 5, sizeof(displacement));
        byte_offset = displacement;
    }
    // mov rax,[rcx]; mov rax,[rax+disp8]; jmp rax
    else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0x48 && code[4] == 0x8B &&
             code[5] == 0x40 && code[7] == 0x48 && code[8] == 0xFF && code[9] == 0xE0) {
        byte_offset = code[6];
    }
    // mov rax,[rcx]; mov rax,[rax]; jmp rax (MSVC vcall thunk, slot zero)
    else if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x01 && code[3] == 0x48 && code[4] == 0x8B &&
             code[5] == 0x00 && code[6] == 0x48 && code[7] == 0xFF && code[8] == 0xE0) {
        byte_offset = 0;
    }
    // mov rax,[rcx]; mov rax,[rax+disp32]; jmp rax
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

Fact slot_fact(std::optional<std::size_t> slot, std::string_view method)
{
    if (!slot) {
        return {std::nullopt, "member-pointer ABI decode", "unsupported or unrecognized member-pointer encoding",
                Provenance::Unresolved};
    }
    return {static_cast<std::int64_t>(*slot), "member-pointer ABI decode", method, Provenance::RuntimeProbe};
}

std::optional<std::size_t> contiguous_count(std::initializer_list<std::optional<std::size_t>> slots)
{
    std::vector<std::size_t> values;
    values.reserve(slots.size());
    for (const auto slot : slots) {
        if (!slot) {
            return std::nullopt;
        }
        values.push_back(*slot);
    }
    std::ranges::sort(values);
    if (std::adjacent_find(values.begin(), values.end(), [](std::size_t left, std::size_t right) {
            return right != left + 1;
        }) != values.end()) {
        return std::nullopt;
    }
    return values.empty() ? std::nullopt : std::optional<std::size_t>(values.back() + 1);
}

std::optional<std::size_t> deleting_destructor(std::optional<std::size_t> first_named)
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(__x86_64__))
    if (first_named && *first_named > 0) {
        return *first_named - 1;
    }
#elif defined(__GXX_ABI_VERSION) || defined(__itanium__)
    if (first_named && *first_named >= 2) {
        // The Itanium deleting destructor is immediately before the first
        // declared virtual; the complete destructor is one slot earlier.
        return *first_named - 1;
    }
#endif
    return std::nullopt;
}

std::optional<std::size_t> prefix_destructor(std::optional<std::size_t> named_slot, std::size_t named_prefix)
{
    if (named_slot && *named_slot >= named_prefix) {
        return *named_slot - named_prefix;
    }
    return std::nullopt;
}

Fact unresolved(std::string_view evidence)
{
    return {std::nullopt, "member-pointer ABI decode", evidence, Provenance::Unresolved};
}

}  // namespace

Fact measure_vtable(std::string_view name)
{
    if (name == "ES_LOGGER_SLOT_LOG") {
        using Method = void (endstone::Logger::*)(endstone::Logger::Level, std::string_view) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Logger::log)), "endstone::Logger::log(Level,string_view) const");
    }
    if (name == "ES_SERVER_SLOT_GET_PLUGIN_MANAGER") {
        using Method = endstone::PluginManager &(endstone::Server::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Server::getPluginManager)), "endstone::Server::getPluginManager() const");
    }
    if (name == "ES_SERVER_SLOT_GET_SCHEDULER") {
        using Method = endstone::Scheduler &(endstone::Server::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Server::getScheduler)), "endstone::Server::getScheduler() const");
    }
    if (name == "ES_SERVER_SLOT_GET_ONLINE_PLAYERS") {
        using Method = std::vector<endstone::Player *> (endstone::Server::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Server::getOnlinePlayers)), "endstone::Server::getOnlinePlayers() const");
    }
    if (name == "ES_SERVER_SLOT_CREATE_BOSS_BAR") {
        using Method = std::unique_ptr<endstone::BossBar> (endstone::Server::*)(std::string, endstone::BarColor, endstone::BarStyle) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Server::createBossBar)), "endstone::Server::createBossBar(string,BarColor,BarStyle) const");
    }
    if (name == "ES_SERVER_SLOT_CREATE_BLOCK_DATA") {
        using Method = std::unique_ptr<endstone::BlockData> (endstone::Server::*)(std::string) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Server::createBlockData)), "endstone::Server::createBlockData(string) const");
    }
    if (name == "ES_SERVER_SLOT_CREATE_BLOCK_DATA_STATES") {
        using Method = std::unique_ptr<endstone::BlockData> (endstone::Server::*)(std::string, endstone::BlockStates) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Server::createBlockData)), "endstone::Server::createBlockData(string,BlockStates) const");
    }
    if (name == "ES_SERVER_SLOT_GET_REGISTRY") {
        using Method = endstone::IRegistry *(endstone::Server::*)(const std::string &) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Server::_getRegistry)), "endstone::Server::_getRegistry(string) const");
    }
    if (name == "ES_SERVER_SLOT_GET_MAP") {
        using Method = endstone::MapView *(endstone::Server::*)(std::int64_t) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Server::getMap)), "endstone::Server::getMap(int64) const");
    }
    if (name == "ES_SERVER_SLOT_CREATE_MAP") {
        using Method = endstone::MapView &(endstone::Server::*)(const endstone::Dimension &) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Server::createMap)), "endstone::Server::createMap(Dimension) const");
    }
    if (name == "ES_PM_SLOT_REGISTER_EVENT") {
        using Method = void (endstone::PluginManager::*)(std::string, std::function<void(endstone::Event &)>, endstone::EventPriority,
                                                          endstone::Plugin &, bool);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::PluginManager::registerEvent)), "endstone::PluginManager::registerEvent(string,function,priority,Plugin,bool)");
    }
    if (name == "ES_SENDER_SLOT_SEND_MESSAGE") {
        using Method = void (endstone::CommandSender::*)(const endstone::Message &) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::CommandSender::sendMessage)), "endstone::CommandSender::sendMessage(Message) const");
    }
    if (name == "ES_SENDER_SLOT_AS_PLAYER") {
        return slot_fact(decode_slot(&endstone::CommandSender::asPlayer), "endstone::CommandSender::asPlayer() const");
    }
    if (name == "ES_PLAYER_SLOT_GET_LOCATION") {
        using Method = endstone::Location (endstone::Actor::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Actor::getLocation)), "endstone::Actor::getLocation() const");
    }
    if (name == "ES_PLAYER_SLOT_GET_DIMENSION") {
        using Method = endstone::Dimension &(endstone::Actor::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Actor::getDimension)), "endstone::Actor::getDimension() const");
    }
    if (name == "ES_PLAYER_SLOT_PLAY_SOUND") {
        using Method = void (endstone::Player::*)(endstone::Location, std::string, float, float);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Player::playSound)), "endstone::Player::playSound(Location,string,float,float)");
    }
    if (name == "ES_PLAYER_SLOT_SEND_POPUP") {
        using Method = void (endstone::Player::*)(std::string) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Player::sendPopup)), "endstone::Player::sendPopup(string) const");
    }
    if (name == "ES_PLAYER_SLOT_SEND_TIP") {
        using Method = void (endstone::Player::*)(std::string) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Player::sendTip)), "endstone::Player::sendTip(string) const");
    }
    if (name == "ES_PLAYER_SLOT_IS_OP") {
        using Method = bool (endstone::Player::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Player::isOp)), "endstone::Player::isOp() const");
    }
    if (name == "ES_PLAYER_SLOT_GET_INVENTORY") {
        using Method = endstone::PlayerInventory &(endstone::Player::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Player::getInventory)), "endstone::Player::getInventory() const");
    }
    if (name == "ES_PLAYER_SLOT_SEND_PACKET") {
        using Method = void (endstone::Player::*)(int, std::string_view) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Player::sendPacket)), "endstone::Player::sendPacket(int,string_view) const");
    }
    if (name == "ES_PLAYER_SLOT_SEND_MAP") {
        using Method = void (endstone::Player::*)(endstone::MapView &);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Player::sendMap)), "endstone::Player::sendMap(MapView&)");
    }
    if (name == "ES_SCHEDULER_SLOT_RUN_TIMER") {
        using Method = std::shared_ptr<endstone::Task> (endstone::Scheduler::*)(endstone::Plugin &, std::function<void()>, std::uint64_t,
                                                                                  std::uint64_t);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Scheduler::runTaskTimer)), "endstone::Scheduler::runTaskTimer(Plugin,function,uint64,uint64)");
    }
    if (name == "ES_BOSSBAR_SLOT_SET_TITLE") {
        using Method = void (endstone::BossBar::*)(std::string);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::BossBar::setTitle)), "endstone::BossBar::setTitle(string)");
    }
    if (name == "ES_BOSSBAR_SLOT_SET_PROGRESS") {
        using Method = void (endstone::BossBar::*)(float);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::BossBar::setProgress)), "endstone::BossBar::setProgress(float)");
    }
    if (name == "ES_BOSSBAR_SLOT_SET_VISIBLE") {
        using Method = void (endstone::BossBar::*)(bool);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::BossBar::setVisible)), "endstone::BossBar::setVisible(bool)");
    }
    if (name == "ES_BOSSBAR_SLOT_ADD_PLAYER") {
        using Method = void (endstone::BossBar::*)(endstone::Player &);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::BossBar::addPlayer)), "endstone::BossBar::addPlayer(Player&)");
    }
    if (name == "ES_COMMAND_SLOT_EXECUTE") {
        using Method = bool (endstone::Command::*)(endstone::CommandSender &, const std::vector<std::string> &) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Command::execute)), "endstone::Command::execute(CommandSender,vector<string>) const");
    }
    if (name == "ES_COMMAND_SLOT_AS_PLUGIN_COMMAND") {
        using Method = endstone::PluginCommand *(endstone::Command::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Command::asPluginCommand)), "endstone::Command::asPluginCommand() const");
    }
    if (name == "ES_DIMENSION_SLOT_GET_NAME") {
        using Method = std::string (endstone::Dimension::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Dimension::getName)), "endstone::Dimension::getName() const");
    }
    if (name == "ES_DIMENSION_SLOT_GET_BLOCK_AT_XYZ") {
        using Method = std::unique_ptr<endstone::Block> (endstone::Dimension::*)(int, int, int) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Dimension::getBlockAt)), "endstone::Dimension::getBlockAt(int,int,int) const");
    }
    if (name == "ES_BLOCK_SLOT_GET_TYPE") {
        using Method = std::string (endstone::Block::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Block::getType)), "endstone::Block::getType() const");
    }
    if (name == "ES_BLOCK_SLOT_SET_DATA") {
        using Method = void (endstone::Block::*)(const endstone::BlockData &, bool);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Block::setData)),
                         "void (endstone::Block::*)(const endstone::BlockData&, bool)");
    }
    if (name == "ES_BLOCK_DATA_SLOT_GET_TYPE") {
        using Method = std::string (endstone::BlockData::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::BlockData::getType)), "endstone::BlockData::getType() const");
    }
    if (name == "ES_ITEM_TYPE_SLOT_CREATE_ITEM_STACK") {
        using Method = endstone::ItemStack (endstone::ItemType::*)(int) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::ItemType::createItemStack)),
                         "endstone::ItemStack (endstone::ItemType::*)(int) const");
    }
    if (name == "ES_ITEM_STACK_SLOT_GET_ITEM_META") {
        using Method = std::unique_ptr<endstone::ItemMeta> (endstone::ItemStack::Impl::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::ItemStack::Impl::getItemMeta)),
                         "endstone::ItemStack::Impl::getItemMeta() const");
    }
    if (name == "ES_ITEM_STACK_SLOT_SET_ITEM_META") {
        using Method = bool (endstone::ItemStack::Impl::*)(const endstone::ItemMeta *);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::ItemStack::Impl::setItemMeta)),
                         "endstone::ItemStack::Impl::setItemMeta(const ItemMeta *)");
    }
    if (name == "ES_ITEM_REGISTRY_SLOT_GET") {
        using Registry = endstone::Registry<endstone::ItemType>;
        using Method = endstone::ItemType *(Registry::*)(endstone::Identifier<endstone::ItemType>) noexcept;
        return slot_fact(decode_slot(static_cast<Method>(&Registry::get)), "endstone::Registry<ItemType>::get(ItemTypeId) noexcept");
    }
    if (name == "ES_ITEM_META_SLOT_GET_TYPE") {
        using Method = endstone::ItemMeta::Type (endstone::ItemMeta::*)() const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::ItemMeta::getType)), "endstone::ItemMeta::getType() const");
    }
    if (name == "ES_ITEM_META_SLOT_SET_DISPLAY_NAME") {
        using Method = void (endstone::ItemMeta::*)(std::optional<std::string>);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::ItemMeta::setDisplayName)), "endstone::ItemMeta::setDisplayName(optional<string>)");
    }
    if (name == "ES_ITEM_META_SLOT_SET_LORE") {
        using Method = void (endstone::ItemMeta::*)(std::optional<std::vector<std::string>>);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::ItemMeta::setLore)), "endstone::ItemMeta::setLore(optional<vector<string>>)");
    }
    if (name == "ES_MAP_META_SLOT_HAS_MAP_ID") {
        return slot_fact(decode_slot(&endstone::MapMeta::hasMapId), "endstone::MapMeta::hasMapId() const");
    }
    if (name == "ES_MAP_META_SLOT_GET_MAP_ID") {
        return slot_fact(decode_slot(&endstone::MapMeta::getMapId), "endstone::MapMeta::getMapId() const");
    }
    if (name == "ES_MAP_META_SLOT_SET_MAP_ID") {
        using Method = void (endstone::MapMeta::*)(endstone::MapMeta::MapId);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::MapMeta::setMapId)), "endstone::MapMeta::setMapId(MapId)");
    }
    if (name == "ES_MAP_META_SLOT_SET_MAP_VIEW") {
        using Method = void (endstone::MapMeta::*)(const endstone::MapView *);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::MapMeta::setMapView)), "endstone::MapMeta::setMapView(MapView*)");
    }
    if (name == "ES_INVENTORY_SLOT_GET_SIZE") {
        return slot_fact(decode_slot(&endstone::Inventory::getSize), "endstone::Inventory::getSize() const");
    }
    if (name == "ES_INVENTORY_SLOT_GET_ITEM") {
        using Method = std::optional<endstone::ItemStack> (endstone::Inventory::*)(int) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Inventory::getItem)), "endstone::Inventory::getItem(int) const");
    }
    if (name == "ES_INVENTORY_SLOT_SET_ITEM") {
        using Method = void (endstone::Inventory::*)(int, std::optional<endstone::ItemStack>);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Inventory::setItem)), "endstone::Inventory::setItem(int,optional<ItemStack>)");
    }
    if (name == "ES_INVENTORY_SLOT_FIRST_EMPTY") {
        return slot_fact(decode_slot(&endstone::Inventory::firstEmpty), "endstone::Inventory::firstEmpty() const");
    }
    if (name == "ES_INVENTORY_SLOT_CLEAR_SLOT") {
        using Method = void (endstone::Inventory::*)(int);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Inventory::clear)), "endstone::Inventory::clear(int)");
    }
    if (name == "ES_INVENTORY_SLOT_CLEAR_ALL") {
        using Method = void (endstone::Inventory::*)();
        return slot_fact(decode_slot(static_cast<Method>(&endstone::Inventory::clear)), "endstone::Inventory::clear()");
    }
    if (name == "ES_MAPVIEW_SLOT_GET_ID") {
        return slot_fact(decode_slot(&endstone::MapView::getId), "endstone::MapView::getId() const");
    }
    if (name == "ES_MAPVIEW_SLOT_IS_VIRTUAL") {
        return slot_fact(decode_slot(&endstone::MapView::isVirtual), "endstone::MapView::isVirtual() const");
    }
    if (name == "ES_MAPVIEW_SLOT_GET_SCALE") {
        return slot_fact(decode_slot(&endstone::MapView::getScale), "endstone::MapView::getScale() const");
    }
    if (name == "ES_MAPVIEW_SLOT_SET_SCALE") {
        using Method = void (endstone::MapView::*)(endstone::MapView::Scale);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::MapView::setScale)), "endstone::MapView::setScale(Scale)");
    }
    if (name == "ES_MAPVIEW_SLOT_ADD_RENDERER") {
        using Method = void (endstone::MapView::*)(std::shared_ptr<endstone::MapRenderer>);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::MapView::addRenderer)), "endstone::MapView::addRenderer(shared_ptr<MapRenderer>)");
    }
    if (name == "ES_MAPVIEW_SLOT_REMOVE_RENDERER") {
        using Method = bool (endstone::MapView::*)(const std::shared_ptr<endstone::MapRenderer> &);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::MapView::removeRenderer)), "endstone::MapView::removeRenderer(shared_ptr<MapRenderer>)");
    }
    if (name == "ES_MAPVIEW_SLOT_SET_LOCKED") {
        using Method = void (endstone::MapView::*)(bool);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::MapView::setLocked)), "endstone::MapView::setLocked(bool)");
    }
    if (name == "ES_MAPRENDERER_SLOT_IS_ENDSTONE") {
        return slot_fact(decode_slot(&endstone::MapRenderer::isEndstoneMapRenderer), "endstone::MapRenderer::isEndstoneMapRenderer() const");
    }
    if (name == "ES_MAPRENDERER_SLOT_INIT") {
        using Method = void (endstone::MapRenderer::*)(endstone::MapView &);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::MapRenderer::initialize)), "endstone::MapRenderer::initialize(MapView&)");
    }
    if (name == "ES_MAPRENDERER_SLOT_RENDER") {
        using Method = void (endstone::MapRenderer::*)(endstone::MapView &, endstone::MapCanvas &, endstone::Player &);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::MapRenderer::render)), "endstone::MapRenderer::render(MapView&,MapCanvas&,Player&)");
    }
    if (name == "ES_MAPCANVAS_SLOT_SET_PIXEL") {
        using Method = void (endstone::MapCanvas::*)(int, int, std::uint32_t);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::MapCanvas::setPixel)), "endstone::MapCanvas::setPixel(int,int,uint32)");
    }
    if (name == "ES_MAPCANVAS_SLOT_GET_PIXEL") {
        using Method = std::uint32_t (endstone::MapCanvas::*)(int, int) const;
        return slot_fact(decode_slot(static_cast<Method>(&endstone::MapCanvas::getPixel)), "endstone::MapCanvas::getPixel(int,int) const");
    }
    if (name == "ES_OFFLINE_PLAYER_SLOT_GET_UNIQUE_ID") {
        return slot_fact(decode_slot(&endstone::OfflinePlayer::getUniqueId), "endstone::OfflinePlayer::getUniqueId() const");
    }

    if (name == "ES_PLUGIN_SLOT_ON_COMMAND") {
        using Method = bool (endstone::CommandExecutor::*)(endstone::CommandSender &, const endstone::Command &, const std::vector<std::string> &);
        return slot_fact(decode_slot(static_cast<Method>(&endstone::CommandExecutor::onCommand)), "endstone::CommandExecutor::onCommand(Sender,Command,vector<string>)");
    }
    if (name == "ES_PLUGIN_SLOT_GET_DESCRIPTION") {
        return slot_fact(decode_slot(&endstone::Plugin::getDescription), "endstone::Plugin::getDescription() const");
    }
    if (name == "ES_PLUGIN_SLOT_ON_LOAD") {
        return slot_fact(decode_slot(&endstone::Plugin::onLoad), "endstone::Plugin::onLoad()");
    }
    if (name == "ES_PLUGIN_SLOT_ON_ENABLE") {
        return slot_fact(decode_slot(&endstone::Plugin::onEnable), "endstone::Plugin::onEnable()");
    }
    if (name == "ES_PLUGIN_SLOT_ON_DISABLE") {
        return slot_fact(decode_slot(&endstone::Plugin::onDisable), "endstone::Plugin::onDisable()");
    }

    if (name == "ES_VTABLE_SLOT_COUNT") {
        const auto command = decode_slot(static_cast<bool (endstone::CommandExecutor::*)(endstone::CommandSender &, const endstone::Command &, const std::vector<std::string> &)>(
            &endstone::CommandExecutor::onCommand));
        const auto description = decode_slot(&endstone::Plugin::getDescription);
        const auto load = decode_slot(&endstone::Plugin::onLoad);
        const auto enable = decode_slot(&endstone::Plugin::onEnable);
        const auto disable = decode_slot(&endstone::Plugin::onDisable);
        const auto count = contiguous_count({command, description, load, enable, disable});
        if (!count) {
            return unresolved("declared Plugin virtual sequence was not contiguous or a member-pointer decode failed");
        }
        return {static_cast<std::int64_t>(*count), "contiguous virtual-sequence derivation",
                "Plugin onCommand/getDescription/onLoad/onEnable/onDisable; named slots are contiguous and ABI prefix validated",
                Provenance::RuntimeDerived};
    }
    if (name == "ES_CMD_VTABLE_SLOT_COUNT") {
        const auto execute = decode_slot(static_cast<bool (endstone::Command::*)(endstone::CommandSender &, const std::vector<std::string> &) const>(
            &endstone::Command::execute));
        const auto plugin_command = decode_slot(static_cast<endstone::PluginCommand *(endstone::Command::*)() const>(&endstone::Command::asPluginCommand));
        const auto count = contiguous_count({execute, plugin_command});
        if (!count) {
            return unresolved("declared Command virtual sequence was not contiguous or a member-pointer decode failed");
        }
        return {static_cast<std::int64_t>(*count), "contiguous virtual-sequence derivation",
                "Command execute/asPluginCommand; named slots are contiguous and ABI prefix validated",
                Provenance::RuntimeDerived};
    }

    if (name == "ES_BOSSBAR_SLOT_DTOR") {
        using Method = void (endstone::BossBar::*)(std::string);
        const auto set_title = decode_slot(static_cast<Method>(&endstone::BossBar::setTitle));
        const auto dtor = prefix_destructor(set_title, 2);
        if (!dtor) {
            return unresolved("BossBar setTitle anchor did not expose the declared destructor/getTitle/setTitle ABI prefix");
        }
        return {static_cast<std::int64_t>(*dtor), "platform ABI destructor derivation",
                "BossBar getTitle/setTitle named prefix; setTitle slot minus two accounts for destructor and getTitle",
                Provenance::RuntimeDerived};
    }
    if (name == "ES_COMMAND_SLOT_DTOR_COMPLETE" || name == "ES_COMMAND_SLOT_DTOR_DELETING") {
        const auto execute = decode_slot(static_cast<bool (endstone::Command::*)(endstone::CommandSender &, const std::vector<std::string> &) const>(
            &endstone::Command::execute));
        const auto dtor = deleting_destructor(execute);
        if (!dtor) {
            return unresolved("Command destructor derivation failed");
        }
#if defined(__GXX_ABI_VERSION) || defined(__itanium__)
        const auto value = name == "ES_COMMAND_SLOT_DTOR_COMPLETE" ? *dtor - 1 : *dtor;
#else
        const auto value = *dtor;
#endif
        return {static_cast<std::int64_t>(value), "platform ABI destructor derivation",
                "Command destructor immediately precedes execute; platform ABI prefix validated", Provenance::RuntimeDerived};
    }
    if (name == "ES_BLOCK_SLOT_DELETE" || name == "ES_BLOCK_DATA_SLOT_DELETE" || name == "ES_ITEM_META_SLOT_DELETE" ||
        name == "ES_ITEM_STACK_SLOT_DELETE" || name == "ES_MAPVIEW_SLOT_DTOR" || name == "ES_MAPVIEW_SLOT_DTOR_COMPLETE" ||
        name == "ES_MAPVIEW_SLOT_DTOR_DELETING" || name == "ES_MAPRENDERER_SLOT_DTOR" || name == "ES_MAPRENDERER_SLOT_DTOR_COMPLETE" ||
        name == "ES_MAPRENDERER_SLOT_DTOR_DELETING" || name == "ES_PLUGIN_SLOT_DTOR_COMPLETE" || name == "ES_PLUGIN_SLOT_DTOR_DELETING") {
        std::optional<std::size_t> first;
        if (name.find("BLOCK_DATA") != std::string_view::npos) {
            first = decode_slot(static_cast<std::string (endstone::BlockData::*)() const>(&endstone::BlockData::getType));
        }
        else if (name.find("BLOCK_SLOT") != std::string_view::npos) {
            first = decode_slot(static_cast<std::string (endstone::Block::*)() const>(&endstone::Block::getType));
        }
        else if (name.find("ITEM_META") != std::string_view::npos) {
            first = decode_slot(&endstone::ItemMeta::getType);
        }
        else if (name.find("MAPVIEW") != std::string_view::npos) {
            first = decode_slot(&endstone::MapView::getId);
        }
        else if (name.find("MAPRENDERER") != std::string_view::npos) {
            first = decode_slot(&endstone::MapRenderer::isEndstoneMapRenderer);
        }
        else if (name.find("PLUGIN") != std::string_view::npos) {
            first = decode_slot(&endstone::Plugin::getDescription);
        }
        else if (name.find("ITEM_STACK") != std::string_view::npos) {
            using Clone = std::unique_ptr<endstone::ItemStack::Impl> (endstone::ItemStack::Impl::*)() const;
            first = decode_slot(static_cast<Clone>(&endstone::ItemStack::Impl::clone));
        }
        else {
            return unresolved("destructor anchor is not exposed by the SDK");
        }
        const auto dtor = deleting_destructor(first);
        if (!dtor) {
            return unresolved("destructor derivation failed");
        }
#if defined(__GXX_ABI_VERSION) || defined(__itanium__)
        const auto complete = name.find("_COMPLETE") != std::string_view::npos;
        const auto value = complete ? *dtor - 1 : *dtor;
#else
        const auto value = *dtor;
#endif
        const auto evidence = name.find("ITEM_STACK") != std::string_view::npos
                                  ? "ItemStack::Impl::clone anchor; destructor prefix is validated by platform ABI"
                                  : "declared contiguous virtual sequence; destructor anchor and ABI prefix validated";
        return {static_cast<std::int64_t>(value), "platform ABI destructor derivation", evidence,
                Provenance::RuntimeDerived};
    }

    return unresolved("no safe public SDK virtual declaration for this measurement");
}

}  // namespace abi_probe

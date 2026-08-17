#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <endstone/endstone.hpp>

#include "endstone_mediaplayer_api.h"

namespace {

constexpr auto kScreenName = "abi-consumer-smoke";

void require_result(const mp_api_v1 &api, mp_result result, const char *operation)
{
    if (result != MP_OK) {
        throw std::runtime_error(std::string(operation) + ": " + api.result_string(result));
    }
}

class AbiConsumerSmoke : public endstone::Plugin {
public:
    void onLoad() override { getLogger().info("ABI_CONSUMER_SMOKE_ON_LOAD"); }

    void onEnable() override
    {
        try {
            getLogger().info("ABI_CONSUMER_SMOKE_SERVER_ACCESS server={}", static_cast<const void *>(&getServer()));

            mp_api_v1 api{};
            if (!mp_try_get_api_v1(&api)) {
                throw std::runtime_error("MediaPlayer API v1 is unavailable");
            }

            mp_screen_handle stale = MP_INVALID_SCREEN_HANDLE;
            if (api.screen_find(kScreenName, &stale) == MP_OK) {
                require_result(api, api.screen_delete(stale), "delete stale logical screen");
            }

            mp_screen_create_info create{};
            create.struct_size = sizeof(create);
            create.name = kScreenName;
            create.width_tiles = 1;
            create.height_tiles = 1;
            create.backend = MP_BACKEND_LOGICAL;

            mp_screen_handle screen = MP_INVALID_SCREEN_HANDLE;
            require_result(api, api.screen_create(&create, &screen), "create logical screen");

            mp_screen_handle found = MP_INVALID_SCREEN_HANDLE;
            require_result(api, api.screen_find(kScreenName, &found), "find logical screen");
            if (found != screen) {
                throw std::runtime_error("screen_find returned a different handle");
            }

            std::array<std::uint8_t, 4 * 4 * 4> pixels{};
            for (std::size_t i = 0; i < pixels.size(); i += 4) {
                pixels[i] = 0x20;
                pixels[i + 1] = 0x80;
                pixels[i + 2] = 0xe0;
                pixels[i + 3] = 0xff;
            }
            require_result(api,
                           api.screen_update_region(screen, 0, 0, 4, 4, pixels.data(), pixels.size(), 16,
                                                    MP_PIXEL_FORMAT_RGBA),
                           "update logical screen");

            mp_frame_handle frame = MP_INVALID_FRAME_HANDLE;
            require_result(api, api.frame_begin(screen, &frame), "begin abort frame");
            require_result(api,
                           api.frame_update_region(frame, 0, 0, 4, 4, pixels.data(), pixels.size(), 16,
                                                   MP_PIXEL_FORMAT_RGBA),
                           "write abort frame");
            require_result(api, api.frame_abort(frame), "abort frame");

            frame = MP_INVALID_FRAME_HANDLE;
            require_result(api, api.frame_begin(screen, &frame), "begin commit frame");
            require_result(api,
                           api.frame_update_region(frame, 0, 0, 4, 4, pixels.data(), pixels.size(), 16,
                                                   MP_PIXEL_FORMAT_RGBA),
                           "write commit frame");
            require_result(api, api.frame_commit(frame), "commit frame");

            mp_screen_info info{};
            info.struct_size = sizeof(info);
            require_result(api, api.screen_get_info(screen, &info), "get logical screen info");
            mp_stats stats{};
            stats.struct_size = sizeof(stats);
            require_result(api, api.screen_get_stats(screen, &stats), "get logical screen stats");
            require_result(api, api.screen_delete(screen), "delete logical screen");

            passed_ = true;
            getLogger().info("ABI_CONSUMER_SMOKE_PASS logical_screen={} generation={}", info.name,
                             stats.generation);
        }
        catch (const std::exception &error) {
            getLogger().error("ABI_CONSUMER_SMOKE_FAIL error={}", error.what());
        }
    }

    void onDisable() override
    {
        getLogger().info("ABI_CONSUMER_SMOKE_ON_DISABLE passed={}", passed_);
    }

    bool onCommand(endstone::CommandSender &, const endstone::Command &command,
                   const std::vector<std::string> &) override
    {
        if (command.getName() != "abi-player-smoke") {
            return false;
        }

        const auto players = getServer().getOnlinePlayers();
        if (players.size() != 1 || players.front() == nullptr) {
            getLogger().error("ABI_CONSUMER_PLAYER_DISPATCH_FAIL online_players={}", players.size());
            return true;
        }

        const auto &player = *players.front();
        const bool accepted = player.performCommand("mpv create abi_generated_smoke");
        if (accepted) {
            getLogger().info("ABI_CONSUMER_PLAYER_DISPATCH_PASS player={}", player.getName());
        }
        else {
            getLogger().error("ABI_CONSUMER_PLAYER_DISPATCH_FAIL player={} accepted=false", player.getName());
        }
        return true;
    }

private:
    bool passed_{false};
};

}  // namespace

ENDSTONE_PLUGIN("abi_consumer_smoke", "1.0.0", AbiConsumerSmoke)
{
    prefix = "ABIConsumerSmoke";
    description = "Generated-header MediaPlayer runtime smoke";
    authors = {"Endstone MediaPlayer ABI"};
    depend = {"mediaplayer"};

    command("abi-player-smoke")
        .description("Dispatch the MediaPlayer physical ABI smoke through one real player")
        .usages("/abi-player-smoke")
        .permissions("abi_consumer_smoke.command.player");

    permission("abi_consumer_smoke.command.player")
        .description("Allow console dispatch of the real-player MediaPlayer ABI smoke")
        .default_(endstone::PermissionDefault::Console);
}

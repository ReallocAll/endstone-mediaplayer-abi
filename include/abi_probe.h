#pragma once

#include <endstone/endstone.hpp>

#include "report.h"

class AbiProbePlugin : public endstone::Plugin {
public:
    void onLoad() override;
    void onEnable() override;
    void onDisable() override;

private:
    abi_probe::ProbeReport report_;
    bool load_attempted_{false};
    bool completed_{false};
};

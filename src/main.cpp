#include "mcls/DroneLogService.hpp"
#include "mcls/Config.hpp"

#include <iostream>

namespace {

constexpr const char* kDefaultConfigPath = "/etc/mcls/config.toml";

} // namespace

int main(int argc, char* argv[]) {
    std::string config_path = kDefaultConfigPath;
    if (argc > 1) {
        config_path = argv[1];
    }

    try {
        std::cerr << "mcls: loading config from " << config_path << std::endl;
        const mcls::Config config = mcls::Config::loadFromFile(config_path);
        std::cerr << "mcls: [companion] enabled=" << (config.companion.enabled ? "true" : "false")
                  << " table_present=" << (config.companion_table_present ? "true" : "false")
                  << std::endl;
        mcls::DroneLogService service(config, config_path);
        service.run();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}

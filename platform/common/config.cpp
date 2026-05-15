#include "toml++/toml.hpp"
#include "logging.h"
#include <filesystem>
#include <unistd.h>

extern toml::table config;

bool init_config(const char* config_path)
{
  config = toml::parse_file(config_path);
  auto game_path = config["paths"]["game_files"].value_or<std::string>("");
  if (chdir(game_path.c_str()) != 0) {
    fatal_error("Could not change directory to %s\n", game_path.c_str());
    return false;
  }
  BOOT_LOG("Changed working directory to %s\n", game_path.c_str());
  return true;
}
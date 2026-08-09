#pragma once

#include <filesystem>
#include <stdexcept>

#include "market/event.hpp"

namespace market_engine::app {

class ModelStoreError : public std::runtime_error { public: using std::runtime_error::runtime_error; };

// Version-one JSON persistence for the complete model consumed by RTL.
[[nodiscard]] market::ModelParameters load_model_file(const std::filesystem::path& path);
void save_model_file_atomically(const std::filesystem::path& path, const market::ModelParameters& model);

}  // namespace market_engine::app

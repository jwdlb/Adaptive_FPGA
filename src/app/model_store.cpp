#include "app/model_store.hpp"

#include <fstream>
#include <limits>

#include <nlohmann/json.hpp>

namespace market_engine::app {
namespace {
constexpr std::uint32_t kSchemaVersion{1};
void validate(const market::ModelParameters& model) {
    if (model.model_version == 0U) throw ModelStoreError("modelVersion must be positive");
    if (model.buy_threshold <= model.sell_threshold) throw ModelStoreError("model BUY threshold must exceed SELL threshold");
}
}  // namespace

market::ModelParameters load_model_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw ModelStoreError("cannot open model file: " + path.string());
    try {
        nlohmann::json json; input >> json;
        if (!json.is_object() || json.at("schemaVersion").get<std::uint32_t>() != kSchemaVersion)
            throw ModelStoreError("unsupported model-file schema");
        const auto& weights = json.at("weightsQ16");
        if (!weights.is_array() || weights.size() != market::FeatureVector::kFeatureCount)
            throw ModelStoreError("model weightsQ16 must contain exactly eight integers");
        market::ModelParameters model{};
        model.model_version = json.at("modelVersion").get<std::uint64_t>();
        model.update_count = json.at("updateCount").get<std::uint64_t>();
        model.buy_threshold = json.at("buyThresholdQ16").get<std::int32_t>();
        model.sell_threshold = json.at("sellThresholdQ16").get<std::int32_t>();
        for (std::size_t i = 0; i < model.weights.size(); ++i) model.weights[i] = weights.at(i).get<std::int32_t>();
        validate(model); return model;
    } catch (const ModelStoreError&) { throw; }
    catch (const nlohmann::json::exception& error) { throw ModelStoreError("invalid model file: " + std::string(error.what())); }
}

void save_model_file_atomically(const std::filesystem::path& path, const market::ModelParameters& model) {
    validate(model);
    nlohmann::json json{{"schemaVersion", kSchemaVersion}, {"modelVersion", model.model_version},
                        {"updateCount", model.update_count}, {"weightsQ16", model.weights},
                        {"buyThresholdQ16", model.buy_threshold}, {"sellThresholdQ16", model.sell_threshold}};
    json["trainingMetadata"] = {{"updateCount", model.update_count}};
    json["metrics"] = nlohmann::json::object();
    json["weightsDecimal"] = nlohmann::json::array();
    for (const auto weight : model.weights) json["weightsDecimal"].push_back(static_cast<double>(weight) / 65536.0);
    json["buyThresholdDecimal"] = static_cast<double>(model.buy_threshold) / 65536.0;
    json["sellThresholdDecimal"] = static_cast<double>(model.sell_threshold) / 65536.0;
    const auto temporary = path.string() + ".tmp";
    { std::ofstream output(temporary, std::ios::trunc); if (!output) throw ModelStoreError("cannot write model temporary file: " + temporary); output << json.dump(2) << '\n'; if (!output) throw ModelStoreError("failed writing model temporary file: " + temporary); }
    std::error_code error; std::filesystem::rename(temporary, path, error);
    if (error) throw ModelStoreError("cannot atomically replace model file: " + error.message());
}
}  // namespace market_engine::app

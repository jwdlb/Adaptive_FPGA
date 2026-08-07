#include "io/event_reader.hpp"

#include <fstream>
#include <stdexcept>

namespace market_engine::io {

// The program supports both CSV and binary event files. This function chooses the correct reader based on the file extension
std::vector<market::MarketEvent> read_events(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);    // Opens a file for reading in binary mode, which still works for csv on linux
    if (!input) throw std::runtime_error("cannot open event input: " + path.string());
    // Return right read handler based on file extension. .mkt and .bin are binary, everything else is csv.
    if (path.extension() == ".mkt" || path.extension() == ".bin") return market::read_binary(input);
    return market::read_csv(input);
}

}  // namespace market_engine::io

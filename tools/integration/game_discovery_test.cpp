// SPDX-License-Identifier: GPL-3.0-or-later

#include "qt_common/game_list/discovery.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <source_location>

namespace fs = std::filesystem;

void Check(bool condition,
           std::source_location location = std::source_location::current()) {
  if (!condition) {
    std::cerr << "Game discovery regression failed at line " << location.line()
              << '\n';
    std::exit(1);
  }
}

int main() {
  const auto temp = fs::temp_directory_path();
  const auto root =
      temp / ("eglantine-discovery-" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()));
  Check(fs::equivalent(root.parent_path(), temp) && fs::create_directory(root));
  struct Cleanup {
    fs::path path;
    ~Cleanup() { fs::remove_all(path); }
  } cleanup{root};
  const auto touch = [](const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream{path}.put('x');
  };
  const auto dump = [&](const fs::path &path) {
    touch(path / "exefs/main");
    touch(path / "exefs/main.npdm");
  };
  const auto supported = [](const fs::path &path) {
    return path.filename() == "main" || path.extension() == ".nsp";
  };
  const auto discover = [&](const fs::path &path, bool deep) {
    return GameListDiscovery::Discover(path, deep, supported,
                                       [] { return false; });
  };

  const auto game = root / fs::path{u8"MKW-églantine"};
  dump(game / "base");
  dump(game / "updates/0100000000000800");
  for (unsigned i = 0; i < 400; ++i) {
    touch(game / "base/romfs/assets" / std::to_string(i) / "false-game.nsp");
  }
  touch(root / "standalone.nsp");
  touch(root / "unrelated/romfs/actual-game.nsp");

  const auto deep = discover(root, true);
  Check(deep.files.size() == 4 && deep.skipped_romfs == 1 && deep.errors == 0);
  Check(deep.directories.size() < 20);
  Check(discover(root, false).files.size() == 3);
  Check(discover(game / "base", false).files.size() == 1);
  Check(discover(game / "base/exefs", false).files.size() == 1);
  Check(discover(root / "missing", true).files.empty());

  touch(root / "incomplete/exefs/main");
  touch(root / "incomplete/romfs/actual-game.nsp");
  Check(discover(root, true).files.size() == 6);

  unsigned calls{};
  const auto cancelled = GameListDiscovery::Discover(
      root, true, supported, [&] { return ++calls > 3; });
  Check(calls < 10 && cancelled.directories.size() <= 1);
  std::cout << "Discovery: loose layouts, payload pruning, Unicode, shallow "
               "scans and cancellation passed\n";
}

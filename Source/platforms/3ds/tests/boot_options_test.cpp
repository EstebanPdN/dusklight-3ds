#include "../runtime/boot_options.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

int main() {
    char directory[] = "/tmp/dusklight-boot-options-XXXXXX";
    assert(mkdtemp(directory));
    const std::filesystem::path root(directory);
    using dusk::native::BootOptions;
    std::ofstream(root / "empty.txt");
    std::ofstream(root / "option.txt") << '7';
    BootOptions options(directory);
    assert(options.present("empty.txt"));
    assert(!options.present("missing.txt") && errno == ENOENT);
    auto* file = options.open("option.txt");
    assert(file && std::fgetc(file) == '7');
    std::fclose(file);
    std::filesystem::remove(root / "option.txt");
    assert(!options.present("option.txt"));

    // Incomplete scans must still try the real file, including names added
    // after construction. Exercise both a failed scan and the capacity limit.
    auto absent = root / "not-mounted-yet";
    BootOptions failed(absent.c_str());
    std::filesystem::create_directory(absent);
    std::ofstream(absent / "late.txt");
    assert(failed.present("late.txt"));
    for (int i = 0; i < 260; ++i) std::ofstream(root / std::to_string(i));
    BootOptions crowded(directory);
    std::ofstream(root / "after-scan.txt");
    assert(crowded.present("after-scan.txt"));
    std::filesystem::remove_all(root);
}

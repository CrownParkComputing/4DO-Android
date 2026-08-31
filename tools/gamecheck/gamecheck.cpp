// gamecheck - boot every disc in a library, headless, and say what happened.
//
// The unit tests cover the parts of the machine in isolation. They cannot tell
// you that a title boots, because booting is the whole machine agreeing with
// itself for two minutes. This does that instead: it runs each disc with no
// window, no audio device and no timing, and reports what the machine actually
// did with it.
//
// What it measures, and why each one is here:
//
//   commands   drive commands issued. Zero means the machine never found the
//              drive; a handful means it found it and gave up.
//   sectors    expansion transfers completed. This is data actually reaching
//              memory rather than a driver talking to itself.
//   cels       cels drawn. The single best proxy for "is a game running":
//              a title that only reaches its publisher logo draws tens, one
//              that reaches gameplay draws tens of thousands.
//   screens    distinct framebuffer contents. Separates a moving picture from
//              a still one, and a still one from a black one.
//   pixels     how much of the busiest frame is actually content: pixels
//              differing from that frame's most common colour. Counting
//              non-black pixels instead stops measuring anything the moment a
//              title programs a non-black background - every pixel is then
//              "lit" and a blank screen scores full marks.
//
// Expectations live beside the library in a plain text file so a regression is
// a test failure rather than something you have to notice by eye. Without one
// the tool still runs and prints a table, which is what you want the first
// time you point it at a new set of discs.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/clio.h"
#include "core/console.h"
#include "core/madam.h"

namespace fs = std::filesystem;
using namespace retro3do;

namespace {

// How often to press start, and for how long. Slow enough not to skip past
// anything, long enough to be noticed.
constexpr int kPressPeriod = 180;   // three seconds
constexpr int kPressHold = 8;

struct Result {
    std::string name;
    u64 commands = 0;
    u64 sectors = 0;
    u64 cels = 0;
    size_t screens = 0;
    int pixels = 0;
    double seconds = 0.0;
    bool loaded = false;
};

// What a title has to reach to count as working. Only a floor: a title that
// does better than its expectation is not a failure, a title that does worse
// is a regression.
struct Expectation {
    u64 cels = 0;
    size_t screens = 0;
    int pixels = 0;
};

u64 hash_frame(const u32* pixels, size_t count) {
    u64 hash = 1469598103934665603ull;
    for (size_t i = 0; i < count; ++i) {
        hash ^= pixels[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

Result run(const fs::path& bios, const fs::path& disc, int frames,
           const fs::path& shot_dir) {
    Result result;
    result.name = disc.stem().string();

    Console console;
    if (!console.load_bios(bios.string())) {
        return result;
    }
    if (!console.load_disc(disc.string())) {
        return result;
    }
    result.loaded = true;
    console.reset();
    console.clio().cdrom().set_disc_present(true);

    std::unordered_set<u64> screens;
    std::vector<u32> best_frame;
    int best_width = 0;
    int best_height = 0;
    std::vector<u32> last_frame;
    int last_width = 0;
    int last_height = 0;

    const auto started = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frames; ++frame) {
        // Press start, periodically.
        //
        // A title sitting on its own menu waiting to be told to begin is not
        // stuck, but it looks exactly like it from the outside: no cels, no
        // sectors, nothing changing. Need for Speed sat at thirty-three cels
        // for sixty thousand frames here and ran perfectly well on a device
        // where somebody pressed a button.
        //
        // Held for a few frames because a title that samples the pad once a
        // frame can still miss a press that lasts one.
        Joypad pad;
        const int phase = frame % kPressPeriod;
        pad.play = phase < kPressHold;
        console.set_joypad(pad);

        console.run_frame();

        const Frame image = console.framebuffer();
        const size_t count = static_cast<size_t>(image.width) * image.height;
        screens.insert(hash_frame(image.pixels, count));

        // Content, not brightness: how many pixels differ from the commonest
        // colour in the frame. A flat screen of any colour scores zero.
        std::map<u32, int> histogram;
        for (size_t i = 0; i < count; ++i) {
            ++histogram[image.pixels[i] & 0x00ffffffu];
        }
        int commonest = 0;
        for (const auto& entry : histogram) {
            if (entry.second > commonest) {
                commonest = entry.second;
            }
        }
        const int lit = static_cast<int>(count) - commonest;
        // Keep the busiest frame rather than the last one. The last frame of a
        // run lands wherever the title happened to be, which for a
        // double-buffered game is as likely to be the blank half as the drawn
        // one.
        //
        // The final frame is kept as well, because "busiest" is not the same
        // as "representative": a title fading in has more lit pixels early,
        // when it is still dark, than it does once the fade finishes. Comparing
        // the busiest frame against another machine's last one is a good way to
        // convince yourself of a bug that is not there.
        if (frame == frames - 1) {
            last_width = image.width;
            last_height = image.height;
            last_frame.assign(image.pixels, image.pixels + count);
        }
        if (lit > result.pixels) {
            result.pixels = lit;
            best_width = image.width;
            best_height = image.height;
            best_frame.assign(image.pixels, image.pixels + count);
        }
    }
    const auto finished = std::chrono::steady_clock::now();
    result.seconds = std::chrono::duration<double>(finished - started).count();

    result.commands = console.clio().cdrom().commands_received();
    result.sectors = console.expansion_dma_count();
    result.cels = console.madam().total_cels_drawn();
    result.screens = screens.size();

    if (!shot_dir.empty()) {
        std::error_code ignored;
        fs::create_directories(shot_dir, ignored);
        const auto write = [&](const fs::path& out, const std::vector<u32>& pixels,
                               int w, int h) {
            if (pixels.empty()) {
                return;
            }
            if (std::FILE* file = std::fopen(out.string().c_str(), "wb")) {
                std::fprintf(file, "P6\n%d %d\n255\n", w, h);
                for (u32 pixel : pixels) {
                    std::fputc((pixel >> 16) & 0xff, file);
                    std::fputc((pixel >> 8) & 0xff, file);
                    std::fputc(pixel & 0xff, file);
                }
                std::fclose(file);
            }
        };
        write(shot_dir / (result.name + ".ppm"), best_frame, best_width, best_height);
        write(shot_dir / (result.name + " (final).ppm"), last_frame, last_width,
              last_height);
    }
    return result;
}

// One title per line: name, then the three floors. The name is matched by
// prefix so a manifest does not have to carry a full regional filename.
std::map<std::string, Expectation> load_expectations(const fs::path& path) {
    std::map<std::string, Expectation> expectations;
    std::FILE* file = std::fopen(path.string().c_str(), "r");
    if (file == nullptr) {
        return expectations;
    }
    char line[512];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        // Split on the separator rather than scanning a format. A format with
        // literal pipes in it does not tolerate spaces around them, and a line
        // that fails to parse is skipped in silence - which reads exactly like
        // a title having no expectation at all, and quietly turns the whole
        // file off.
        std::vector<std::string> fields;
        const std::string text(line);
        size_t start = 0;
        while (start <= text.size()) {
            const size_t bar = text.find('|', start);
            std::string field = text.substr(
                start, bar == std::string::npos ? std::string::npos : bar - start);
            const size_t first = field.find_first_not_of(" \t\r\n");
            const size_t last = field.find_last_not_of(" \t\r\n");
            fields.push_back(first == std::string::npos
                                 ? std::string()
                                 : field.substr(first, last - first + 1));
            if (bar == std::string::npos) {
                break;
            }
            start = bar + 1;
        }
        if (fields.size() != 4 || fields[0].empty()) {
            std::fprintf(stderr, "gamecheck: ignoring malformed line: %s", line);
            continue;
        }
        expectations[fields[0]] =
            Expectation{std::strtoull(fields[1].c_str(), nullptr, 10),
                        std::strtoul(fields[2].c_str(), nullptr, 10),
                        static_cast<int>(std::strtol(fields[3].c_str(), nullptr, 10))};
    }
    std::fclose(file);
    return expectations;
}

// Longest prefix wins. "Alone in the Dark" is a prefix of "Alone in the Dark 2"
// as well as of itself, and taking the first match in name order would give
// the sequel the original's floor.
const Expectation* find(const std::map<std::string, Expectation>& expectations,
                        const std::string& name) {
    const Expectation* best = nullptr;
    size_t best_length = 0;
    for (const auto& entry : expectations) {
        if (name.rfind(entry.first, 0) == 0 && entry.first.size() >= best_length) {
            best = &entry.second;
            best_length = entry.first.size();
        }
    }
    return best;
}

int usage() {
    std::fprintf(stderr,
                 "usage: gamecheck <bios> <library-dir> [options]\n"
                 "  --frames N      frames to run per title (default 20000)\n"
                 "  --expect FILE   floors to check against\n"
                 "  --shots DIR     write the busiest frame of each title here\n"
                 "  --only SUBSTR   run only titles whose name contains this\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    const fs::path bios = argv[1];
    const fs::path library = argv[2];

    int frames = 20000;
    fs::path expect_path;
    fs::path shot_dir;
    std::string only;
    for (int i = 3; i < argc; ++i) {
        const std::string flag = argv[i];
        const bool has_value = (i + 1) < argc;
        if (flag == "--frames" && has_value) {
            frames = std::atoi(argv[++i]);
        } else if (flag == "--expect" && has_value) {
            expect_path = argv[++i];
        } else if (flag == "--shots" && has_value) {
            shot_dir = argv[++i];
        } else if (flag == "--only" && has_value) {
            only = argv[++i];
        } else {
            return usage();
        }
    }

    std::vector<fs::path> discs;
    std::error_code ignored;
    for (const auto& entry : fs::directory_iterator(library, ignored)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (extension != ".chd" && extension != ".iso" && extension != ".bin" &&
            extension != ".cue" && extension != ".img") {
            continue;
        }
        if (!only.empty() &&
            entry.path().filename().string().find(only) == std::string::npos) {
            continue;
        }
        discs.push_back(entry.path());
    }
    std::sort(discs.begin(), discs.end());

    if (discs.empty()) {
        std::fprintf(stderr, "no discs found in %s\n", library.string().c_str());
        return 1;
    }

    const auto expectations = load_expectations(expect_path);

    std::printf("%-52s %8s %8s %10s %8s %8s %7s\n", "title", "cmds", "sectors",
                "cels", "screens", "pixels", "sec");
    std::printf("%s\n", std::string(110, '-').c_str());

    int failures = 0;
    int booted = 0;
    for (const fs::path& disc : discs) {
        const Result result = run(bios, disc, frames, shot_dir);
        if (!result.loaded) {
            std::printf("%-52s  could not be opened\n", result.name.c_str());
            ++failures;
            continue;
        }

        std::string verdict;
        if (const Expectation* expected = find(expectations, result.name)) {
            const bool ok = result.cels >= expected->cels &&
                            result.screens >= expected->screens &&
                            result.pixels >= expected->pixels;
            if (!ok) {
                verdict = "  FAIL (floor " + std::to_string(expected->cels) +
                          " cels / " + std::to_string(expected->screens) +
                          " screens / " + std::to_string(expected->pixels) +
                          " pixels)";
                ++failures;
            }
        }
        if (result.cels > 0) {
            ++booted;
        }

        std::printf("%-52s %8llu %8llu %10llu %8zu %8d %7.1f%s\n",
                    result.name.c_str(), (unsigned long long)result.commands,
                    (unsigned long long)result.sectors,
                    (unsigned long long)result.cels, result.screens,
                    result.pixels, result.seconds, verdict.c_str());
        std::fflush(stdout);
    }

    std::printf("\n%d of %zu titles drew something; %d failed their floor\n",
                booted, discs.size(), failures);
    return failures == 0 ? 0 : 1;
}

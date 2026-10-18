// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-in for libFuzzer's driver where its runtime is missing (Apple's
// clang ships none): runs LLVMFuzzerTestOneInput on each file given, then on
// random mutations of them. Build with -fsanitize=address,undefined so a
// bad input stops the run.
//   fuzz-target [-runs=N] [-seed=S] FILE...
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int main(int argc, char** argv)
{
    unsigned long runs = 20000;
    unsigned long seed = 1;
    std::vector<std::vector<uint8_t>> corpus;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "-runs=", 6) == 0) {
            runs = std::strtoul(argv[i] + 6, nullptr, 10);
        } else if (std::strncmp(argv[i], "-seed=", 6) == 0) {
            seed = std::strtoul(argv[i] + 6, nullptr, 10);
        } else {
            std::ifstream file(argv[i], std::ios::binary);
            if (!file) {
                std::fprintf(stderr, "cannot read %s\n", argv[i]);
                return 2;
            }
            corpus.emplace_back(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
        }
    }
    if (corpus.empty()) corpus.emplace_back();
    for (const auto& input : corpus) LLVMFuzzerTestOneInput(input.data(), input.size());

    std::mt19937_64 random(seed);
    std::vector<uint8_t> input;
    for (unsigned long run = 0; run < runs; ++run) {
        input = corpus[random() % corpus.size()];
        const int mutations = 1 + static_cast<int>(random() % 8);
        for (int m = 0; m < mutations; ++m) {
            const size_t at = input.empty() ? 0 : random() % input.size();
            switch (random() % 6) {
            case 0:  // flip a bit
                if (!input.empty()) input[at] ^= static_cast<uint8_t>(1u << (random() % 8));
                break;
            case 1:  // random byte
                if (!input.empty()) input[at] = static_cast<uint8_t>(random());
                break;
            case 2:  // insert bytes
                input.insert(input.begin() + static_cast<long>(at), 1 + random() % 16,
                             static_cast<uint8_t>(random()));
                break;
            case 3:  // erase a run
                if (!input.empty()) {
                    const size_t count = std::min<size_t>(input.size() - at, 1 + random() % 64);
                    input.erase(input.begin() + static_cast<long>(at),
                                input.begin() + static_cast<long>(at + count));
                }
                break;
            case 4:  // truncate
                input.resize(at);
                break;
            case 5:  // duplicate a run
                if (!input.empty()) {
                    const size_t count = std::min<size_t>(input.size() - at, 1 + random() % 256);
                    std::vector<uint8_t> run_copy(input.begin() + static_cast<long>(at),
                                                  input.begin() + static_cast<long>(at + count));
                    input.insert(input.begin() + static_cast<long>(random() % (input.size() + 1)),
                                 run_copy.begin(), run_copy.end());
                }
                break;
            }
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());
    }
    std::printf("%lu inputs from %zu seed(s): no failures\n", runs, corpus.size());
    return 0;
}

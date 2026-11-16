#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "fuzz_server_cfg.h"
#include "fuzz_support.h"

namespace {

hailstone::fuzz::Bytes cfg_bytes(std::string_view text) {
    hailstone::fuzz::Bytes bytes;
    bytes.reserve(text.size());
    for (const unsigned char c : text) bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

} // namespace

TEST_CASE("fuzz: server cfg survives truncated mutated and binary text with opt-in policy invariants") {
    using namespace hailstone;
    const std::vector<std::string_view> text_seeds = {
        "",
        "[scripts]\nplayer = on\nplayer_caps = read,ui\n[plugins]\nplayer = off\n",
        "[addons]\nenforce = on\ndefault = deny\nrequired = maker/required\nallow = maker/optional\nblock = maker/excluded\n",
        "[addons]\nenforce = on\nrequired = bare-name\nallow = optional\nblock = excluded\n",
        "[addons]\nenforce = off\ndefault = broken\nrequired = *\nunknown = ignored\n",
        "[addons\nenforce = on\ndefault = deny\n",
        "[addons]\nenforce = on\ndefault deny\n",
        "[addons]\nenforce = maybe\nallow = optional,\n",
        "[addons]\nenforce = on\nrequired = Maker/Required\nblock = https://github.com/maker/required/\n",
        "[ADDONS]\r\nENFORCE = YES\r\nALLOW = https://GitHub.com/Maker/Optional.git/\r\nDEFAULT = DENY\r\n",
        "[addons]\nenforce = on\nallow = a,,b\n[addons] trailing text\nblock = helper\n",
        "[addons]\nenforce = on\nallow = \"maker/repo\" ; comment\n# whole line comment\n",
    };
    std::vector<fuzz::Bytes> seeds;
    for (const auto text : text_seeds) seeds.push_back(cfg_bytes(text));
    constexpr char binary_seed[] = "[addons]\nenforce = on\nallow = maker/helper\0,maker/other\n";
    seeds.push_back(cfg_bytes(std::string_view(binary_seed, sizeof(binary_seed) - 1)));
    seeds.push_back(cfg_bytes("\xef\xbb\xbf[addons]\nenforce = on\ndefault = deny\n"));

    std::size_t failures = 0;
    std::size_t cases = 0;
    fuzz::reset_largest_allocation();
    const auto run = [&](fuzz::Bytes input) {
        input.resize(std::min(input.size(), fuzz_server_cfg::kMaxInput));
        ++cases;
        if (!fuzz::run_case("server cfg", input, fuzz::Throws::Never, fuzz_server_cfg::feed)) ++failures;
    };
    fuzz::Rng rng(0xc0f16);
    for (const auto& seed : seeds) {
        for (std::size_t size = 0; size <= seed.size(); ++size)
            run(fuzz::Bytes(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(size)));
        for (unsigned mutation = 0; mutation < 128; ++mutation) run(fuzz::mutate(seed, rng));
    }
    for (unsigned sample = 0; sample < 128; ++sample) run(fuzz::random_bytes(rng, rng.below(2049)));
    for (const std::size_t size : {std::size_t{1024}, std::size_t{4096}, fuzz_server_cfg::kMaxInput}) {
        std::string long_value = "[addons]\nenforce = on\nallow = maker/";
        long_value.append(size, 'x');
        run(cfg_bytes(long_value));
    }
    std::string sections;
    for (unsigned index = 0; index < 256; ++index)
        sections += "[section" + std::to_string(index) + "]\nsetting=value\n";
    run(cfg_bytes(sections));

    CHECK(failures == 0);
    CHECK(cases > 2000);
    CHECK(fuzz::largest_allocation() < (2u << 20));
}

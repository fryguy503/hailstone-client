#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <vector>

#include "core/distribution.h"
#include "core/managed_ui_manifest.h"
#include "managed_ui_fixtures.h"

int main(int argc, char** argv) {
    std::vector<hailstone::distribution::UiSource> sources(hailstone::distribution::kManagedUiSources.begin(),
                                                         hailstone::distribution::kManagedUiSources.end());
    const auto fixtures = hailstone::test::managed_ui::approved_sources();
    sources.insert(sources.end(), fixtures.begin(), fixtures.end());
    hailstone::distribution::enable_managed(sources);
    doctest::Context context(argc, argv);
    return context.run();
}

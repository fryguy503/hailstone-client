#include <doctest/doctest.h>

#include <memory>

#include <imgui.h>
#include <imgui_internal.h>

#include "app/character_import_ui.h"

namespace {

using hailstone::app::CharacterImportUi;

struct ImportDialog {
    std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> context{ImGui::CreateContext(), ImGui::DestroyContext};
    CharacterImportUi dialog;
    int previews = 0;
    int applications = 0;
    int restores = 0;
    CharacterImportUi::Outcome outcome = CharacterImportUi::Outcome::Add;

    CharacterImportUi::Callbacks callbacks{
        [this](const CharacterImportUi::Choice& choice) {
            ++previews;
            CHECK(choice.source == 0);
            CHECK(choice.socials);
            CHECK_FALSE(choice.hotbars);
            CharacterImportUi::Row row{"Social page 1, slot 1", "Ready", "Empty slot", {"/say Ready."}, outcome};
            return CharacterImportUi::Preview{{row}, {}};
        },
        [this] {
            ++applications;
            return CharacterImportUi::Result{true, "Added one setting.", true};
        },
        [this] {
            ++restores;
            return CharacterImportUi::Result{true, "Restored the previous settings.", false};
        },
    };

    ImportDialog() {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        dialog.open("Current character", {{"Source character", {{"character_world.ini", "Character settings"}}, true}});
        frame();
        frame();
    }

    void frame() {
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        dialog.draw(callbacks);
        ImGui::Render();
    }

    void activate(const char* label, bool in_body = false) {
        auto* window = ImGui::FindWindowByName(CharacterImportUi::kWindowName);
        REQUIRE(window);
        if (in_body) {
            REQUIRE(window->DC.ChildWindows.Size == 1);
            window = window->DC.ChildWindows[0];
        }
        ImGui::ActivateItemByID(window->GetID(label));
        frame();
        frame();
    }
};

} // namespace

TEST_CASE("Character import dialog requires category selection and explicit apply and restore") {
    ImportDialog ui;
    CHECK(ui.dialog.visible());
    ui.activate("Apply import");
    ui.activate("Preview import");
    CHECK(ui.previews == 0);
    CHECK(ui.applications == 0);

    ui.activate("Socials", true);
    ui.activate("Preview import");
    REQUIRE(ui.previews == 1);
    CHECK(ui.applications == 0);
    ui.activate("Apply import");
    REQUIRE(ui.applications == 1);
    ui.activate("Apply import");
    CHECK(ui.applications == 1);

    ui.activate("Restore backup");
    CHECK(ui.restores == 0);
    ui.activate("Confirm restore");
    CHECK(ui.restores == 1);
    ui.activate("Close");
    CHECK_FALSE(ui.dialog.visible());
}

TEST_CASE("Character import dialog refuses apply when the preview has no additions") {
    ImportDialog ui;
    ui.outcome = CharacterImportUi::Outcome::Keep;
    ui.activate("Socials", true);
    ui.activate("Preview import");
    REQUIRE(ui.previews == 1);
    ui.activate("Apply import");
    CHECK(ui.applications == 0);
}

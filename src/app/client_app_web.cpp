// ClientApp: the in-game browser (docs/browser.md). The helper (browser/hailstone-web beside the binary) starts with
// the first tab; its data is data_dir()/web/, one profile folder per saved server.

#include "app/client_app_internal.h"
#include "app/lua_ui.h"
#include "platform/open_folder.h"
#include "web/page_rules.h"

namespace hailstone::app::client_internal {

web::BrowserHost* ClientApp::browser() {
    if (browser_ || browser_tried_) return browser_.get();
    browser_tried_ = true;
#if defined(_WIN32)
    constexpr const char* kHelper = "hailstone-web.exe";
#else
    constexpr const char* kHelper = "hailstone-web";
#endif
    const std::filesystem::path helper = platform::install_dir() / "browser" / kHelper;
    std::error_code ec;
    if (!std::filesystem::exists(helper, ec)) {
        log::info("web: no {}: web pages open in the system's browser", helper.string());
        return nullptr;
    }
    const std::filesystem::path data = platform::data_dir() / "web";
    std::filesystem::create_directories(data, ec);
    // hailstone.ini browser.sandbox: auto (the helper decides: docs/browser.md), on, off.
    std::string sandbox = config_.get_string("browser.sandbox", "auto");
    if (sandbox != "on" && sandbox != "off") sandbox = "auto";
    browser_ = std::make_unique<web::BrowserHost>([helper, data, sandbox] {
        return web::process_transport({helper.string(), "--data=" + data.string(), "--sandbox=" + sandbox});
    });
    web_textures_ = std::make_unique<ui::WebTextures>(vk_);
    return browser_.get();
}

void ClientApp::close_orphan_tabs() {
    if (!browser_ || !web_service_ || !automation_) return;
    std::vector<std::string> gone;
    for (std::uint32_t id : browser_->tabs())
        if (const web::Tab* t = browser_->tab(id); t && !automation_->web_owner_alive(t->options.owner))
            gone.push_back(t->options.owner);
    for (const std::string& owner : gone) web_service_->close_owner(owner);
    web_service_->recheck([this](const std::string& owner) { return automation_->plugin_caps(owner); });
}

bool ClientApp::open_web_page(const std::string& url, std::uint32_t* tab) {
    if (tab) *tab = 0;
    if (web_service_ && web_service_->available() && ui_api_) {
        std::string why;
        if (const std::uint32_t id = web_service_->open("", plugin::kCapAll, url, 800, 600, &why)) {
            if (tab) *tab = id;
            ui_api_->show("BrowserWnd", true);
            return true;
        }
        log::warn("web: {}", why);
    }
    platform::open_url(url);
    return false;
}

void ClientApp::open_server_link(const std::string& url) {
    // OP_Weblink: the original opens it at once in an in-game HTML window (WebManager_CreateHtmlWnd); here the
    // browser window (docs/browser.md). Without the helper, a deliberate difference: the system's browser, with the
    // original's /url line (4095 "Opening %1") to say why one opened. Only http(s) pages (what the original lets
    // through is not traced: docs/unknowns.md "OP_Weblink"), and no more than app/server_links.h lets through.
    // The tab is the player's (the original's CHtmlWnd is a browser the player uses), so it is not held to a
    // script's rules, but a server's link never opens this computer or the player's own network (a router's page),
    // and its tab has a cookie folder of its own (WebService::open_server_link).
    if (!web::url_allowed(url, {})) {
        log::warn("OP_Weblink: not an http(s) page, not opened: {}", url);
        return;
    }
    if (!web::script_url_allowed(url, {})) {
        log::warn("OP_Weblink: a page on this computer or its network, not opened: {}", url);
        return;
    }
    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const ServerLinks::Decision d = server_links_.admit(url, now_ms, [this](std::uint32_t id) -> std::optional<std::string> {
        const web::Tab* t = browser_ ? browser_->tab(id) : nullptr;
        if (!t || t->closed || !t->options.owner.empty()) return std::nullopt;
        return t->url;
    });
    if (d.action == ServerLinks::Action::Drop) {
        if (d.first_drop) log::warn("OP_Weblink: the server sends pages faster than they are opened: {} not opened", url);
        return;
    }
    if (d.action == ServerLinks::Action::Reuse && web_service_ && ui_api_) {
        std::string why;
        if (web_service_->navigate("", plugin::kCapAll, d.tab, url, &why)) {
            server_links_.opened(d.tab, url);
            ui_api_->show("BrowserWnd", true);
            return;
        }
        log::warn("web: {}", why);
    }
    std::uint32_t tab = 0;
    if (web_service_ && web_service_->available() && ui_api_) {
        std::string why;
        tab = web_service_->open_server_link(url, &why);
        if (tab) ui_api_->show("BrowserWnd", true);
        else log::warn("web: {}", why);
    }
    if (!tab) {
        platform::open_url(url);
        conn_->show_with_line_breaks(conn_->strings().format(4095, std::vector<std::string>{url}));
    }
    server_links_.opened(tab, url);
}

} // namespace hailstone::app::client_internal

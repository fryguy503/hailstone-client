#include "app/automation.h"

#include <chrono>
#include <set>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <imgui.h>

#include "app/addon_install.h"
#include "core/log.h"
#include "core/distribution.h"
#include "mq/commands.h"
#include "mq/lua_mq.h"
#include "mq/tlo.h"
#include "net/game_connection.h"
#include "plugin/native_plugins.h"
#include "plugin/policy.h"
#include "plugin/ui_bridge.h"
#include "ui/lua/lua_host.h"

namespace hailstone::app {

namespace fs = std::filesystem;

namespace {
// The data folder's own folder first, then the install's when that is another folder and exists (a non-portable
// install: scripts or plugins shipped beside the binary).
std::vector<fs::path> search_dirs(const fs::path& data, const fs::path& install) {
    std::vector<fs::path> dirs{data};
    std::error_code ec;
    if (!install.empty() && fs::is_directory(install, ec) && !fs::equivalent(data, install, ec)) dirs.push_back(install);
    return dirs;
}
} // namespace

struct Automation::Impl {
    AutomationEnv env;
    plugin::Hooks hooks;
    std::shared_ptr<game::ExtensionChain> extensions = std::make_shared<game::ExtensionChain>();
    plugin::UiBridge plugin_ui;
    plugin::Policy policy;
    mq::Registry registry;
    mq::Context ctx;
    std::unique_ptr<mq::Commands> commands;
    std::unique_ptr<mq::ScriptHost> scripts;
    std::unique_ptr<plugin::NativePlugins> plugins;
    std::set<std::string> autoskill_holds; // the scripts holding autoskill off
    std::uint32_t policy_serial = 0;
    bool had_policy = false;
    char run_name[128] = {};
    std::chrono::steady_clock::time_point consent_checked{};
    // The dev window's compiled plugins: NativePlugins::list() reads and hashes every plugin file and reads the
    // approvals file, so it is asked once a second (and at once after a button there), not every frame.
    std::vector<plugin::NativePlugins::Info> dev_plugins;
    std::optional<std::chrono::steady_clock::time_point> dev_plugins_at;

    net::GameConnection* conn() const { return env.connection ? env.connection() : nullptr; }

    const mq::Context& context() {
        ctx.registry = &registry;
        ctx.conn = conn();
        ctx.nav = env.nav ? env.nav() : nullptr;
        ctx.server_name = env.server_name ? env.server_name() : std::string();
        return ctx;
    }

    void print(const std::string& text, std::uint32_t rgb = 0) {
        if (net::GameConnection* c = conn()) c->ui_message(text, rgb);
        else log::info("{}", text);
    }

    // A command line as the player's would go (the hooks first: GameConnection::send_command).
    void run(const std::string& line) {
        if (net::GameConnection* c = conn()) {
            c->send_command(line);
        } else {
            std::string l = line;
            hooks.command(l);
        }
    }

    std::vector<std::string> autoload() const {
        std::vector<std::string> out;
        for (auto& s : mq::split_index(env.autoload)) out.push_back(s);
        return out;
    }

    void sync_policy() {
        net::GameConnection* c = conn();
        std::uint32_t serial = c ? c->server_rules_serial() : 0;
        bool has = c && c->server_rules();
        if (serial == policy_serial && has == had_policy) return;
        policy_serial = serial;
        had_policy = has;
        fs::path dir;
        if (has) {
            policy.set(*c->server_rules());
            dir = c->server_rules()->dir;
        } else {
            policy.clear();
        }
        // The server's scripts and plugins are looked for first (servers/<profile>/scripts and plugins).
        // Out of the world (character select, a login, disconnected): the character's autorun list goes until a
        // character's zone-in sets it again (ClientApp's zone-in).
        if (!has || c->server_rules()->provisional) scripts->set_autorun_file({});
        scripts->set_server_dir(dir.empty() ? fs::path() : dir / "scripts");
        plugins->set_server_dir(dir.empty() ? fs::path() : dir / "plugins");
        recheck();
    }

    // What runs, checked again against the policy (the server's rules, or the add-ons' ceilings, changed).
    void recheck() {
        std::vector<std::string> script_autoload;
        for (auto& s : mq::split_index(env.script_autoload)) script_autoload.push_back(s);
        scripts->policy_changed(script_autoload);
        plugins->policy_changed(autoload());
    }

    // The server as the player knows it: the saved server's name, else the world's.
    std::string server_label() const {
        std::string s = env.profile_name ? env.profile_name() : std::string();
        if (s.empty() && env.server_name) s = env.server_name();
        return s.empty() ? std::string("this server") : s;
    }
    void dev_window();
    void consent_window();
};

Automation::Automation(AutomationEnv env) : impl_(std::make_unique<Impl>()) {
    Impl& m = *impl_;
    m.env = std::move(env);
    mq::register_game_tlos(m.registry);
    m.ctx.window_open = [&m](std::string_view w) { return m.env.window_open && m.env.window_open(w); };
    m.ctx.plugin_loaded = [&m](std::string_view p) { return m.plugins && m.plugins->loaded(p); };
    m.ctx.ability = m.env.ability;
    m.ctx.can_see = m.env.can_see;
    m.ctx.line_of_sight = m.env.line_of_sight;

    mq::CommandEnv ce;
    ce.hooks = &m.hooks;
    ce.registry = &m.registry;
    ce.context = [&m]() -> const mq::Context& { return m.context(); };
    ce.echo = [&m](const std::string& t) { m.print(t); };
    ce.run = [&m](const std::string& l) { m.run(l); };
    ce.set_target = [&m](std::uint32_t id) {
        if (net::GameConnection* c = m.conn()) c->set_target(id);
    };
    ce.face = m.env.face;
    ce.keypress = m.env.keypress;
    m.commands = std::make_unique<mq::Commands>(ce);

    std::error_code ec;
    mq::ScriptEnv se;
    se.hooks = &m.hooks;
    se.policy = &m.policy;
    se.registry = &m.registry;
    se.context = ce.context;
    se.print = ce.echo;
    se.run = ce.run;
    se.dirs = search_dirs(m.env.data_dir / "scripts", m.env.install_dir / "scripts");
    se.config_dir = m.env.data_dir / "scripts" / "config";
    // Scripts as plugins (mq/lua_hailstone.cpp): the same chain, UI bridge and packets as the compiled plugins.
    se.extensions = m.extensions.get();
    se.ui = &m.plugin_ui;
    se.web = m.env.web;
    se.connection = [&m] { return m.conn(); };
    se.hold_autoskill = [&m](const std::string& script, bool hold) {
        if (hold) m.autoskill_holds.insert(script);
        else m.autoskill_holds.erase(script);
    };
    se.autoskill_list = m.env.autoskill_list;
    se.use_ability = m.env.use_ability;
    se.max_camera_distance = m.env.max_camera_distance;
    se.set_max_camera_distance = m.env.set_max_camera_distance;
    se.send = [&m](plugin::Stream s, const net::AppPacket& p) {
        if (net::GameConnection* c = m.conn()) {
            if (s == plugin::Stream::World) c->send_world_packet(p);
            else c->send_zone_packet(p);
        }
    };
    // A compiled plugin's name, also one the add-on manager installed (built later than this env: asked at call time).
    se.plugin_named = [&m](std::string_view name) {
        if (m.plugins && m.plugins->has_plugin(name)) return true;
        const auto addons = read_installed_addons(m.env.data_dir);
        const InstalledAddon* a = find_addon(addons, name);
        return a && a->kind == AddonKind::Plugin;
    };
    fs::create_directories(se.dirs.front(), ec);
    m.scripts = std::make_unique<mq::ScriptHost>(se);

    plugin::NativeEnv ne;
    ne.hooks = &m.hooks;
    ne.policy = &m.policy;
    ne.registry = &m.registry;
    ne.context = ce.context;
    ne.print = [&m](const std::string& t, std::uint32_t rgb) { m.print(t, rgb); };
    ne.run = ce.run;
    ne.send = [&m](plugin::Stream s, const net::AppPacket& p) {
        if (net::GameConnection* c = m.conn()) {
            if (s == plugin::Stream::World) c->send_world_packet(p);
            else c->send_zone_packet(p);
        }
    };
    ne.imgui_context = [] { return static_cast<void*>(ImGui::GetCurrentContext()); };
    ne.extensions = m.extensions.get();
    ne.ui = &m.plugin_ui;
    ne.web = m.env.web;
    ne.connection = [&m] { return m.conn(); };
    ne.max_camera_distance = m.env.max_camera_distance;
    ne.set_max_camera_distance = m.env.set_max_camera_distance;
    ne.hot_button = m.env.hot_button;
    ne.set_hot_button = m.env.set_hot_button;
    ne.hot_bar_page = m.env.hot_bar_page;
    ne.set_hot_bar_page = m.env.set_hot_bar_page;
    ne.dirs = search_dirs(m.env.data_dir / "plugins", m.env.install_dir / "plugins");
    ne.cache_dir = m.env.data_dir / "cache" / "plugins";
    ne.config_dir = m.env.data_dir / "plugins";
    fs::create_directories(ne.dirs.front(), ec);
    m.plugins = std::make_unique<plugin::NativePlugins>(ne);
}

Automation::~Automation() { shutdown(); }

void Automation::set_addon_ceilings(std::vector<plugin::Ceiling> ceilings) {
    impl_->policy.set_ceilings(std::move(ceilings));
    impl_->recheck();
}

plugin::Hooks& Automation::hooks() { return impl_->hooks; }
bool Automation::load_builtin(const HailstoneBuiltinAddon& addon, const std::filesystem::path& trusted_ui_dir,
                              std::string* why) {
    return impl_->plugins->load_builtin(addon, trusted_ui_dir, why);
}
std::shared_ptr<game::ExtensionChain> Automation::extensions() { return impl_->extensions; }
plugin::UiBridge& Automation::plugin_ui() { return impl_->plugin_ui; }
mq::ScriptHost& Automation::scripts() { return *impl_->scripts; }

std::uint32_t Automation::plugin_caps(std::string_view tag) const {
    // By the owner's tag alone (plugin::UiBridge::owner_tag): a script named after a compiled plugin never gets the
    // plugin's capabilities, nor the other way round.
    auto owner = plugin::UiBridge::parse_tag(tag);
    if (!owner) return 0;
    const std::string& name = owner->second;
    // Asked at every switch to a plugin's code in the Lua UI, many times a frame: the loaded plugin's own entry, not
    // NativePlugins::list(), which reads and hashes every plugin file; a script's, not ScriptHost::list().
    if (owner->first == plugin::UiBridge::Kind::Plugin) return impl_->plugins ? impl_->plugins->loaded_caps(name) : 0;
    return impl_->scripts ? impl_->scripts->caps_of(name) : 0;
}

bool Automation::web_owner_alive(const std::string& owner) const {
    if (owner.empty()) return true;
    if (owner.rfind("script:", 0) == 0) return impl_->scripts && impl_->scripts->running(owner.substr(7));
    if (owner.rfind("plugin:", 0) == 0) return plugin_caps(owner) != 0;
    return false;
}
bool Automation::autoskill_held() {
    auto& holds = impl_->autoskill_holds;
    std::erase_if(holds, [this](const std::string& name) { return !impl_->scripts || !impl_->scripts->running(name); });
    return !holds.empty();
}
const mq::Registry& Automation::registry() const { return impl_->registry; }
const mq::Context& Automation::context() { return impl_->context(); }

std::optional<plugin::ConsentPrompt> Automation::plugin_consent() const {
    const Impl& m = *impl_;
    auto p = m.plugins ? m.plugins->consent() : std::nullopt;
    if (p) p->server = m.server_label();
    return p;
}

void Automation::answer_plugin_consent(std::optional<bool> allow) {
    if (impl_->plugins) impl_->plugins->answer_consent(allow);
}

void Automation::frame() {
    Impl& m = *impl_;
    m.sync_policy();
    ui::lua::set_consent_guard(m.plugins->consent().has_value()); // (the question's windows: lua_host.h)
    // Another Hailstone of a box set may answer the same question: its answer is taken from the file.
    const auto now = std::chrono::steady_clock::now();
    if (m.plugins->consent() && now - m.consent_checked > std::chrono::seconds(1)) {
        m.consent_checked = now;
        m.plugins->recheck_consent();
    }
    m.hooks.pulse();
    m.scripts->pulse();
}

void Automation::imgui(bool dev) {
    Impl& m = *impl_;
    m.scripts->imgui();
    m.plugins->imgui();
    m.hooks.imgui();
    if (!ui::lua::replaces("PluginConsent")) m.consent_window();
    if (dev && !distribution::managed()) m.dev_window();
    ui::lua::raise_guarded_windows(); // the question above every window drawn this frame
}

void Automation::shutdown() {
    Impl& m = *impl_;
    if (m.scripts) m.scripts->stop_all();
    if (m.plugins) m.plugins->unload_all();
}

// The server plugins' question when no UI pack draws it: the server, each plugin's file, size and hash, the
// warning, Allow and Don't allow; the close box puts it away until the next zone-in.
void Automation::Impl::consent_window() {
    auto prompt = plugins->consent();
    if (!prompt) return;
    const std::string server = server_label();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.4f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.95f);
    bool open = true;
    ImGui::Begin("Allow this server's plugin?##hailstone_plugin_consent", &open,
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ui::lua::guard_current_window();
    ImGui::Text("%s wants to run %s on your computer:", server.c_str(),
                prompt->files.size() == 1 ? "a compiled plugin" : "compiled plugins");
    for (const auto& f : prompt->files) {
        ImGui::BulletText("%s  (%s, %llu bytes)", f.name.c_str(), f.file.filename().string().c_str(),
                          static_cast<unsigned long long>(f.size));
        ImGui::TextDisabled("    sha256 %s", to_hex(f.sha256).substr(0, 16).c_str());
    }
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", std::string(plugin::kConsentWarning).c_str());
    ImGui::TextDisabled("Your answer is kept for this server. The add-on list on the login screen can change it.");
    ImGui::PopTextWrapPos();
    const float em = ImGui::GetFontSize();
    if (ImGui::Button("Allow", ImVec2(em * 7.0f, 0.0f))) plugins->answer_consent(true);
    ImGui::SameLine();
    if (ImGui::Button("Don't allow", ImVec2(em * 7.0f, 0.0f))) plugins->answer_consent(false);
    ImGui::End();
    if (!open) plugins->answer_consent(std::nullopt);
}

// The developer window: the server's rules, the plugins and scripts found with their hashes, and controls to load,
// run and stop them.
void Automation::Impl::dev_window() {
    ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Plugins & Lua")) {
        ImGui::End();
        return;
    }
    if (!policy.received()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "Not in the world: plugins and scripts are off.");
    } else if (const auto& r = *policy.current(); !r.configured) {
        ImGui::Text("No hailstone.cfg on this server: plugins and scripts run freely.");
    } else {
        ImGui::Text("This server's hailstone.cfg: %zu scripts and plugins of its own", r.server_files.size());
        ImGui::Text("Your scripts: %s", r.player_script_caps ? plugin::caps_text(r.player_script_caps).c_str() : "not allowed");
        ImGui::Text("Your plugins: %s", r.player_plugin_caps ? plugin::caps_text(r.player_plugin_caps).c_str() : "not allowed");
        ImGui::Text("Your own models and zones in the server's assets: %s", r.player_assets ? "allowed" : "not used");
    }
    if (ImGui::CollapsingHeader("Compiled plugins", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto now = std::chrono::steady_clock::now();
        if (!dev_plugins_at || now - *dev_plugins_at >= std::chrono::seconds(1)) {
            dev_plugins = plugins->list();
            dev_plugins_at = now;
        }
        for (const auto& i : dev_plugins) {
            ImGui::PushID(i.name.c_str());
            if (i.loaded) {
                if (!i.kept && ImGui::SmallButton("Unload")) {
                    plugins->unload(i.name);
                    dev_plugins_at.reset();
                }
            } else if (ImGui::SmallButton("Load")) {
                plugins->load(i.name);
                dev_plugins_at.reset();
            }
            ImGui::SameLine();
            ImGui::Text("%s  [%s%s]", i.name.c_str(), i.loaded ? "loaded" : i.caps ? "allowed" : "not allowed",
                        i.server ? ", the server's" : "");
            ImGui::TextDisabled("  sha256 %s", i.sha256.c_str());
            if (i.asks) {
                ImGui::TextDisabled("  from the server's folder: %s",
                                    i.consent == plugin::Consent::Allowed  ? "you allowed it"
                                    : i.consent == plugin::Consent::Denied ? "you did not allow it"
                                                                           : "not answered yet");
                if (i.consent != plugin::Consent::Unasked && policy.received() && !policy.current()->dir.empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Forget my answer")) {
                        std::vector<Sha256Digest> hashes;
                        if (auto h = parse_sha256(i.sha256)) hashes.push_back(*h);
                        plugin::forget_plugin_consent(policy.current()->dir, i.name, hashes);
                        recheck();
                        dev_plugins_at.reset();
                    }
                }
            }
            if (i.caps) ImGui::TextDisabled("  may: %s", plugin::caps_text(i.caps).c_str());
            if (!i.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "  %s", i.error.c_str());
            ImGui::PopID();
        }
    }
    if (ImGui::CollapsingHeader("Lua scripts", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("##run", run_name, sizeof run_name);
        ImGui::SameLine();
        if (ImGui::Button("Run") && run_name[0]) scripts->run(run_name, {});
        for (const auto& i : scripts->list()) {
            ImGui::PushID(i.name.c_str());
            if (ImGui::SmallButton("Stop")) scripts->stop(i.name);
            ImGui::SameLine();
            if (ImGui::SmallButton(i.status == "PAUSED" ? "Resume" : "Pause")) scripts->pause(i.name);
            ImGui::SameLine();
            ImGui::Text("%s  [%s]  %s", i.name.c_str(), i.status.c_str(), plugin::caps_text(i.caps).c_str());
            ImGui::TextDisabled("  sha256 %s", i.sha256.c_str());
            ImGui::PopID();
        }
        std::string folders;
        if (policy.received() && !policy.current()->dir.empty()) folders = (policy.current()->dir / "scripts").string();
        for (const auto& d : search_dirs(env.data_dir / "scripts", env.install_dir / "scripts"))
            folders += (folders.empty() ? "" : ", ") + d.string();
        ImGui::TextDisabled("Folders: %s", folders.c_str());
    }
    ImGui::End();
}

} // namespace hailstone::app

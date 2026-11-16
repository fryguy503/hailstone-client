#include "app/connection_frontend.h"

#include <utility>

namespace hailstone::app {

bool connection_frontend(net::GameConnection& connection, std::optional<std::string>& pending_skin,
                         const std::function<void()>& apply_assets,
                         const std::function<void(const std::string&)>& load_skin,
                         const std::function<void(net::GameConnection::QuitRequest)>& quit,
                         const std::function<void()>& disconnected) {
    if (const auto request = connection.take_quit_request(); request != net::GameConnection::QuitRequest::None) {
        pending_skin.reset();
        quit(request);
        return false;
    }
    if (connection.state() == net::ConnectionState::Disconnected) {
        pending_skin.reset(); // a request from this connection must not override the next server's UI choice
        disconnected();
        return false;
    }
    // A skin may read the server's assets as it loads. Refresh those rules first, while quit/disconnect still
    // take precedence over any add-on wait or asset work.
    apply_assets();
    if (pending_skin) load_skin(*std::exchange(pending_skin, std::nullopt));
    return true;
}

} // namespace hailstone::app

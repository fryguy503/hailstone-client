#pragma once

// The Find window's guide path (a Hailstone addition, docs/navigation.md "Find"): a glowing trail from the player to
// an NPC over the client's own navigation mesh (app/navigator.h), so it goes round walls on every zone, where the
// server's find path is a straight line without the server's own mesh. It is found again as the player strays from
// it or the NPC moves, trimmed behind the player, and ends when the player gets there.
//
// It moves nothing: the player walks it (or the Find window's Walk there does, through the navigator).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace hailstone::net { class GameConnection; }

namespace hailstone::app {

// Recheck the current Find presentation policy before a running guide observes the destination's live position.
std::optional<glm::vec3> guide_spawn_position(const net::GameConnection&, std::uint32_t spawn);
// Bind only a Find walk to this connection and this incarnation, including while its mesh is still building.
// The current-connection callback is checked before dereferencing the captured connection.
std::function<bool()> find_walk_eligibility(std::function<const net::GameConnection*()> current,
                                          const net::GameConnection&, std::uint32_t spawn);

class GuidePath {
public:
    // A path over the mesh from `from` to `to` (the start first). nullopt: the mesh is not ready yet (or there is
    // none); an empty path: there is no way there.
    using PathFn = std::function<std::optional<std::vector<glm::vec3>>(const glm::vec3& from, const glm::vec3& to)>;

    static constexpr float kArrive = 12.0f;      // flat, and within 10 in height: arrived
    static constexpr float kStray = 8.0f;        // flat distance off the line that finds it again
    static constexpr float kGoalMoved = 6.0f;    // the NPC moved this far since the path was found: again
    static constexpr double kReplanGap = 0.5;    // seconds between two searches
    static constexpr double kMeshRetry = 1.0;    // seconds between tries while the mesh is not ready

    void start(std::uint32_t spawn, std::string name, std::function<bool()> eligible = {});
    bool eligible() const;
    std::uint64_t revision() const { return revision_; }
    void stop();
    bool active() const { return spawn_ != 0; }
    std::uint32_t spawn() const { return spawn_; }
    const std::string& name() const { return name_; }

    struct Update {
        bool arrived = false;        // the player got there: the path ended
        bool gone = false;           // the NPC is gone: the path ended
        bool no_way = false;         // the mesh has no way there: the path ended
        bool waiting_mesh = false;   // the mesh is not ready: nothing to draw yet (once a path: say so)
    };
    // Once a frame: `goal` is the NPC's feet (nullopt: gone).
    Update update(const glm::vec3& feet, std::optional<glm::vec3> goal, double now, const PathFn& path);

    // The line to draw, the player first and the NPC last (empty while there is none).
    const std::vector<glm::vec3>& line() const { return line_; }
    // A partial path: the mesh could not reach the NPC, the line stops where it gets nearest.
    bool partial() const { return partial_; }

private:
    void trim(const glm::vec3& feet);

    std::uint32_t spawn_ = 0;
    std::string name_;
    std::vector<glm::vec3> line_;
    glm::vec3 planned_goal_{0.0f};
    double next_plan_ = 0.0;
    bool partial_ = false;
    bool told_waiting_ = false;
    std::function<bool()> eligible_;
    std::uint64_t revision_ = 0;
};

} // namespace hailstone::app

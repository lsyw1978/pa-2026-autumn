#include "host_session.h"

#include <utility>

namespace uno {

// HostSession constructor：initializes the session with the host role and sets up lobby commands
HostSession::HostSession(std::string name, std::istream& in, std::ostream& out)
    : Session(Role::Host, std::move(name), in, out) {

    // Register the host as player 0
    m_players.emplace(0, Player{session_name(), PeerId{}});

    // Register the "start" command, which calls start() to begin the game
    add_lobby_command("start", "start",
                      "start the game and deal the cards (host only)",
                      [this](const Args& args) { return start(args); });

    // Register the "kick" command, which calls kick() to remove a player
    add_lobby_command("kick", "kick <player_id|player_name>",
                      "remove a player from the room (host only)",
                      [this](const Args& args) { return kick(args); }, 1, 1);
}

// HostSession destructor: default implementation
HostSession::~HostSession() = default;


// ---------------------------------------------------------------------------
// 传输初始化。这段是框架的一部分：房间路径推导、残留 socket 清理、权限、
// 非阻塞设置都已经处理好，你不需要改动。
// ---------------------------------------------------------------------------
bool HostSession::listen_room(const std::string& room, std::string& err) {
    const std::string path = room_socket_path(room, err);
    if (path.empty()) return false;

    auto server = std::make_unique<UnixServer>();
    if (!server->listen_path(path, err)) return false;

    m_room_path = path;
    m_server = std::move(server);
    loop().set_server(m_server.get());
    return true;
}

bool HostSession::listen_tcp(int port, std::string& err) {
    auto server = std::make_unique<TcpServer>();
    if (!server->listen(port, err)) return false;

    m_server = std::move(server);
    loop().set_server(m_server.get());
    return true;
}


// ---------------------------------------------------------------------------
// Helper functions
// ---------------------------------------------------------------------------

// Find the first available client seat (seat 0 belongs to the host)
int HostSession::next_available_seat() const {
    int seat = 1;

    while (m_players.contains(seat)) {
        ++seat;
    }

    return seat;
}

 // Send current player list to all connected players
void HostSession::broadcast_players() {
    Message players_message("PLAYERS");

    // Set the total number of players
    players_message.set("count", static_cast<int>(m_players.size()));

    // Add each player's seat number and name
    for (const auto& [seat, player] : m_players) {
        players_message.set(std::to_string(seat), player.name);
    }

    // Send the updated player list to all clients
    broadcast(std::move(players_message));
}



// ---------------------------------------------------------------------------
// 事件钩子：这些是你要实现的部分。
// ---------------------------------------------------------------------------

// Wait for the client's JOIN message before registering the player
void HostSession::on_peer_join(PeerId who) {
    (void)who;
}

// Handle messages received from connected players
void HostSession::on_peer_message(PeerId from, const Message& message) {
    
    // Handle the JOIN message from a client
    if (message.type == "JOIN") {

        // Get the player's name from the message
        std::string player_name = message.get("name");

        // Reject empty names
        if (player_name.empty()) {
            error("JOIN requires a name");
            return;
        }

        // Check if the player name is already taken
        for (const auto& [seat, player] : m_players) {
            if (player.name == player_name) {
                send_to(from, Message("ERROR").set("msg", "name already taken"));
                return;
            }
        }

        // Prevent the same connection from joining twice
        if (m_peer_seats.contains(from)) {
            send_to(from, Message("ERROR").set("msg", "Client has already joined"));
            return;
        }

        // Only accept new players while in the lobby
        if (phase() != Phase::Lobby) {
            send_to(from, Message("ERROR").set("msg", "Cannot join after the game has started"));
            return;
        }

        // Find an available seat for the new player
        int seat = next_available_seat();

        // Add the new player to the player list
        m_players.emplace(seat, Player{player_name, from});

        // Map the player's connection to their seat number
        m_peer_seats.emplace(from, seat);

        // Send the assigned seat number and name to the new player
        Message welcome_message("WELCOME");
        welcome_message.set("id", seat);
        welcome_message.set("name", player_name);
        send_to(from, std::move(welcome_message));

        // Send the updated player list to all clients
        broadcast_players();

        return;
    }
}

void HostSession::on_peer_leave(PeerId who) {
    info("TODO(host): peer " + who.str() +
         " disconnected -- drop it from the player list");
}


// ---------------------------------------------------------------------------
// 本地命令行：host 玩家自己敲的命令。
// ---------------------------------------------------------------------------
   
 // Start the game only when >=2 players are present
Session::Result HostSession::start(const Args&) {

    // Check whether there are enough players to start
    if (m_players.size() < 2) {
        error("At least two players are required to start");
        return Result::Continue;
    }

    // Notify all connected clients that the game has started
    broadcast(Message("START"));

    // Switch the Host from lobby mode to game mode
    enter_game();

    // 骨架行为：即使游戏逻辑还没写，也让大厅 -> 游戏 -> 大厅的流程可走通。
    return Result::StartGame;
}

Session::Result HostSession::kick(const Args& args) {
    info("TODO(host): remove player " + args[1] + " and broadcast PLAYERS");
    return Result::Continue;
}

Session::Result HostSession::rename(const Args& args) {
    info("TODO(host): rename player to " + args[1] + " and broadcast PLAYERS");
    return Result::Continue;
}

Session::Result HostSession::play(const Args& args) {
    info("TODO(host): validate and play " + args[1] +
         ", then send HAND/PLAYED/TURN");
    return Result::Continue;
}

Session::Result HostSession::cards(const Args&) {
    info("TODO(host): print the authoritative hand of this player");
    return Result::Continue;
}

Session::Result HostSession::draw(const Args&) {
    info("TODO(host): draw one card, decide playability, update the turn");
    return Result::Continue;
}

Session::Result HostSession::pass(const Args&) {
    info("TODO(host): finish the draw turn and advance the current player");
    return Result::Continue;
}

Session::Result HostSession::uno(const Args& args) {
    info(args.size() > 1 ? "TODO(host): resolve a forgotten-UNO report"
                         : "TODO(host): record this player's UNO call");
    return Result::Continue;
}

Session::Result HostSession::challenge(const Args& args) {
    if (!valid_challenge_answer(args)) return Result::Continue;
    info("TODO(host): resolve challenge answer=" + args[1]);
    return Result::Continue;
}

Session::Result HostSession::swap(const Args& args) {
    info("TODO(host): swap hands with player " + args[1]);
    return Result::Continue;
}

} // 命名空间 uno

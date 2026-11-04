#include "host_session.h"

#include <utility>
#include <algorithm>
#include <random>
#include <cstdint>

using namespace std;

namespace uno {

// ============================================================================
// 1. CONSTRUCTOR AND SETUP
// ============================================================================

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


// ============================================================================
// 2. NETWORK AND CONNECTION MANAGEMENT
// ============================================================================

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


// ============================================================================
// 3. PLAYER MANAGEMENT
// ============================================================================

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


// ============================================================================
// 4. DECK MANAGEMENT
// ============================================================================

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

    
    // Handle a client's request to change their name
    if (message.type == "RENAME") {

        // Only allow renaming while in the lobby
        if (phase() != Phase::Lobby) {
            send_to(from, Message("ERROR").set("msg", "Cannot rename during a game"));
            return;
        }

        // Find the seat associated with this connection
        auto it = m_peer_seats.find(from);

        // Reject requests from clients who have not joined
        if (it == m_peer_seats.end()) {
            send_to(from, Message("ERROR").set("msg", "Player not found"));
            return;
        }

        // Get the requested new name
        std::string new_name = message.get("name");

        // Reject empty names
        if (new_name.empty()) {
            send_to(from, Message("ERROR").set("msg", "Name cannot be empty"));
            return;
        }

        // Check whether another player already uses this name
        for (const auto& [seat, player] : m_players) {
            if (seat != it->second && player.name == new_name) {
                send_to(from, Message("ERROR").set("msg", "Name already taken"));
                return;
            }
        }

        // Update the player's name
        m_players.at(it->second).name = new_name;

        // Notify all clients of the updated player list
        broadcast_players();

        return;
    }

}

// Remove a disconnected player and update the player list
void HostSession::on_peer_leave(PeerId who) {

    // Find the disconnected player's seat
    auto it = m_peer_seats.find(who);

    // Ignore connections that never joined
    if (it == m_peer_seats.end()) {
        return;
    }

    // Get the player's seat number
    int seat = it->second;

    // Remove the player from both maps
    m_players.erase(seat);
    m_peer_seats.erase(it);

    // Notify the remaining clients
    broadcast_players();
}

// Create a standard 108-card UNO deck
void HostSession::create_deck() {
    m_deck.clear();
    m_deck.reserve(108);

    // Create number & action cards for each color
    for (char color : {'R', 'Y', 'G', 'B'}) {
        string prefix(1, color);

        // Add 1 zero card
        m_deck.push_back(prefix + "0");

        // Add 2 of each number card (1-9)
        for (int num = 1; num <= 9; ++num) {
            string card = prefix + to_string(num);
            m_deck.push_back(card);
            m_deck.push_back(card);
        }

        // Add 2 of each action card (skip, reverse, draw two)
        for (const char* action : {"SK", "RV", "D2"}) {
            string card = prefix + action;
            m_deck.push_back(card);
            m_deck.push_back(card);
        }
    }

    // Add 4 Wild cards
    for (int i = 0; i < 4; ++i) {
        m_deck.push_back("W");
    }

    // Add 4 Wild Draw Four cards
    for (int i = 0; i < 4; ++i) {
        m_deck.push_back("W4");
    }
}

// Shuffle the deck using the configured random seed
void HostSession::shuffle_deck() {
    // Use provided seed or generate random seed if none given
    uint32_t actual_seed;
    if (has_seed()) {
        actual_seed = seed();
    } else {
        actual_seed = random_device{}();
    }

    // Initialize random number generator with actual seed
    mt19937 gen(actual_seed);

    // Shuffle the cards in the deck
    shuffle(m_deck.begin(), m_deck.end(), gen);
}


// ============================================================================
// 5. GAME INITIALIZATION
// ============================================================================
   
 // Start the game only when >=2 players are present
Session::Result HostSession::start(const Args&) {

    // Check whether there are enough players to start
    if (m_players.size() < 2) {
        error("At least two players are required to start");
        return Result::Continue;
    }

    // Initialize deck for new game
    if (has_deck_override()) {
        // Use specified deck without shuffling
        m_deck = deck_override();
    } else {
        // Create and shuffle standard UNO deck
        create_deck();
        shuffle_deck();
    }

    // Check that there are enough cards for all players + 1 starting card
    if (m_deck.size() < m_players.size() * 7 + 1) {
        error("Not enough cards to start the game");
        return Result::Continue;
    }

    // Reset deck position to 1st card for new game
    m_next_card = 0;

    // Notify all connected clients that the game has started
    broadcast(Message("START"));

    // Switch the Host from lobby mode to game mode
    enter_game();

    // 骨架行为：即使游戏逻辑还没写，也让大厅 -> 游戏 -> 大厅的流程可走通。
    return Result::StartGame;
}

// Remove a client from the lobby using their seat number or name
Session::Result HostSession::kick(const Args& args) {

    // Get the player identifier from the command
    std::string target = args[1];
    int target_seat = -1;

    // First, look for a matching seat number
    for (const auto& [seat, player] : m_players) {
        if (seat != 0 && std::to_string(seat) == target) {
            target_seat = seat;
            break;
        }
    }

    // If no seat matches, look for a matching player name
    if (target_seat == -1) {
        for (const auto& [seat, player] : m_players) {
            if (seat != 0 && player.name == target) {
                target_seat = seat;
                break;
            }
        }
    }

    // Reject invalid player identifiers
    if (target_seat == -1) {
        error("Player not found or cannot be kicked");
        return Result::Continue;
    }

    // Find the connection associated with the selected player
    PeerId peer = m_players.at(target_seat).peer;

    // Disconnect the player and trigger on_peer_leave()
    loop().close_peer(peer);

    return Result::Continue;
}

// Rename the Host and notify all connected clients
Session::Result HostSession::rename(const Args& args) {

    // Get the new name from the command
    std::string new_name = args[1];

    // Reject empty names
    if (new_name.empty()) {
        error("Name cannot be empty");
        return Result::Continue;
    }

    // Check whether another player already uses this name
    for (const auto& [seat, player] : m_players) {
        if (seat != 0 && player.name == new_name) {
            error("Name already taken");
            return Result::Continue;
        }
    }

    // Update the Host's name in the player list
    m_players.at(0).name = new_name;

    // Notify all clients of the updated player list
    broadcast_players();

    return Result::Continue;
}


// ============================================================================
// 6. GAMEPLAY AND TURN MANAGEMENT
// ============================================================================

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


// ============================================================================
// 7. ADVANCED RULES (STAGE 3)
// ============================================================================

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

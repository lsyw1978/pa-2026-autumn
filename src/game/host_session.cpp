#include "host_session.h"

#include <utility>
#include <algorithm>
#include <random>
#include <cstdint>
#include <cctype>
#include <sstream>

using namespace std;

namespace uno {

// ============================================================================
// 1. CONSTRUCTOR AND SETUP
// ============================================================================

// HostSession constructor：initializes the session with the host role and sets up lobby commands
HostSession::HostSession(std::string name, std::istream& in, std::ostream& out)
    : Session(Role::Host, std::move(name), in, out) {

    // Register the host as player 0
    m_players.emplace(0, Player{session_name(), PeerId{}, {}});

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

// Handle messages received from connected clients
void HostSession::on_peer_message(PeerId from, const Message& message) {
    
    // Handle JOIN message from a client
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
        m_players.emplace(seat, Player{player_name, from, {}});

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
    
    // Handle RENAME request from a player
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

    // Handle PLAY request from a player
    if (message.type == "PLAY") {
        auto it = m_peer_seats.find(from);

        if (it == m_peer_seats.end()) {
            send_to(from, Message("ERROR").set("msg", "Player not found"));
            return;
        }

        // Get the player's seat number
        int seat = it->second;

        // Reject attempts to play before game starts
        if (phase() != Phase::Game) {
            send_player_error(seat, "Game has not started");
            return;
        }

        // Check that a card was specified
        string card = message.get("cards");
        if (card.empty()) {
            send_player_error(seat, "PLAY requires a card");
            return;
        }

        // Get the optional Wild color
        string color = message.get("color");

        // Validate & process the client's play
        play_card(seat, card, color);

        return;
    }

    // Handle DRAW request from a player
    if (message.type == "DRAW") {
        auto it = m_peer_seats.find(from);

        if (it != m_peer_seats.end()) {
            take_card(it->second);
        }

        return;
    }

    // Handle PASS request from a player
    if (message.type == "PASS") {
        auto it = m_peer_seats.find(from);

        if (it != m_peer_seats.end()) {
            finish_draw_turn(it->second);
        }

        return;
    }

    // Handle UNO declaration from a player
    if (message.type == "UNO") {
        auto it = m_peer_seats.find(from);

        if (it == m_peer_seats.end()) {
            send_to(from, Message("ERROR").set("msg", "Player not found"));
            return;
        }

        int seat = it->second;

        if (phase() != Phase::Game) {
            send_player_error(seat, "Game has not started");
            return;
        }

        if (message.has("player")) {
            report_uno(seat, message.get("player"));
            return;
        }

        // Validate the player's UNO declaration
        handle_uno(seat);
        return;
    }

    if (message.type == "CHALLENGE" || message.type == "SWAP") {
        auto it = m_peer_seats.find(from);
        if (it == m_peer_seats.end()) {
            send_to(from, Message("ERROR").set("msg", "Player not found"));
            return;
        }
        if (phase() != Phase::Game) {
            send_player_error(it->second, "Game has not started");
            return;
        }
        if (message.type == "CHALLENGE") {
            resolve_challenge(it->second, message.get("accept"));
        } else {
            resolve_swap(it->second, message.get("player"));
        }
        return;
    }

    // Unknown messages and malformed protocol requests must not change state.
    send_to(from, Message("ERROR").set("msg", "Unknown or invalid request"));
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

    // Reuse this generator to shuffle the discard pile when the draw pile empties.
    m_rng.seed(actual_seed);
    shuffle(m_deck.begin(), m_deck.end(), m_rng);
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
        // Use specified deck without shuffling; later reshuffles are deterministic.
        m_deck = deck_override();
        m_rng.seed(has_seed() ? seed() : 0);
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

    // Reset deck position to 1st card
    m_next_card = 0;

    // Deal 7 cards to each player
    deal_cards();

    // Reveal the starting card
    m_top_card = m_deck[m_next_card];
    ++m_next_card;

    m_discard.clear();
    m_discard.push_back(m_top_card);

    // Initialize turn order for a new game
    m_current_player = 0;
    m_direction = 1;

    // Reset game state for a new game
    m_active_color.clear();
    m_pending_draw = 0;
    m_chain_has_d2 = false;
    m_waiting = WaitState::None;
    m_waiting_player = -1;
    m_w4_player = -1;
    m_w4_illegal = false;
    m_uno_vulnerable = -1;
    m_has_drawn = false;
    m_drawn_card.clear();

    // Apply starting card's special effect
    if (m_top_card == "W" || m_top_card == "W4") {
        // Handle wild card effects
        if (m_top_card == "W") {
            // Wild card -> Host goes 1st
            m_current_player = 0;

        } else if (m_top_card == "W4") {
            // Wild draw four -> Host draws 4 cards
            draw_many(0, 4);

            // Skip Host & select next occupied seat
            auto next = m_players.upper_bound(0); // Find next player
            if (next == m_players.end()) {        // Handle wraparound
                next = m_players.begin();
            }
            m_current_player = next->first;
        }
    } else if (m_top_card.size() > 1) {
        // Handle colored action cards (i.e. skip, reverse, draw two)
        string action = m_top_card.substr(1);

        if (action == "SK") {
            // Skip Host & select next occupied seat
            auto next = m_players.upper_bound(0); // Find next player
            if (next == m_players.end()) {        // Handle wraparound
                next = m_players.begin();
            }
            m_current_player = next->first;       // Update current player

        } else if (action == "RV") {
            // Reverse initial turn direction
            m_direction = -1;

            // Select last occupied seat as 1st player
            m_current_player = m_players.rbegin()->first;

        } else if (action == "D2") {
            // Make the Host draw 2 cards
            draw_many(0, 2);

            // Skip Host & select next occupied seat
            auto next = m_players.upper_bound(0); // Find next player
            if (next == m_players.end()) {        // Handle wraparound
                next = m_players.begin();
            }
            m_current_player = next->first;
        }
    }

    // Send each player their updated private hand
    for (const auto& entry : m_players) {
        send_hand(entry.first);
    }

    // Notify all connected clients that the game has started
    broadcast(Message("START"));

    // Broadcast the initial turn & starting card
    broadcast_turn();

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

// Deal 7 cards to each player according to seat order
void HostSession::deal_cards() {
    for (auto& [seat, player] : m_players) {
        // Clear any cards remaining from previous game
        player.hand.clear();

        // Consecutively deal 7 cards to current player
        for (int i = 0; i < 7; ++i) {
            player.hand.push_back(m_deck[m_next_card]);
            ++m_next_card;
        }
    }
}

// Privately send a player their current hand
void HostSession::send_hand(int seat) {
    const Player& player = m_players.at(seat);

    // Combine player's cards into comma-separated string
    std::string cards;
    for (size_t i = 0; i < player.hand.size(); ++i) {
        if (i > 0) {
            cards += ", ";
        }
        cards += player.hand[i];
    }

    // Create HAND message
    Message message("HAND");
    message.set("cards", cards);

    // Display Host's hand locally or send a Client's hand privately
    if (seat == 0) {
        out() << message.serialize() << '\n';
    } else {
        send_to(player.peer, message);
    }
}

// Convert a Wild color to its one-letter representation
static string normalize_wild_color(string color) {
    // Convert input to lowercase
    transform(color.begin(), color.end(), color.begin(),
              [](unsigned char c) { return static_cast<char>(tolower(c)); });

    if (color == "red" || color == "r") return "R";
    if (color == "yellow" || color == "y") return "Y";
    if (color == "green" || color == "g") return "G";
    if (color == "blue" || color == "b") return "B";

    // Return empty string for invalid colors
    return "";
}

// Calculate the point value of a card
static int card_points(const string& card) {
    // Wild cards are worth 50 points
    if (card == "W" || card == "W4") {
        return 50;
    }

    string face = card.substr(1);

    // Action cards are worth 20 points
    if (face == "SK" || face == "RV" || face == "D2") {
        return 20;
    }

    // Number cards are worth their face value
    return stoi(face);
}

// Remove whitespace around individual card names while preserving their order.
static vector<string> parse_play_cards(const string& text) {
    vector<string> result;
    string value;
    istringstream in(text);
    while (getline(in, value, ',')) {
        value.erase(remove_if(value.begin(), value.end(),
                   [](unsigned char c) { return isspace(c); }), value.end());
        if (value.empty()) return {};
        result.push_back(value);
    }
    if (text.empty() || text.back() == ',') return {};
    return result;
}

// Validate an entire play before mutating any game state.
bool HostSession::play_card(int seat, const string& card_text,
                            const string& chosen_color) {
    if (m_waiting != WaitState::None) {
        send_player_error(seat, "Resolve the pending challenge or swap first");
        return false;
    }
    if (seat != m_current_player) {
        send_player_error(seat, "Not your turn");
        return false;
    }
    const auto cards = parse_play_cards(card_text);
    if (cards.empty()) {
        send_player_error(seat, "Invalid card list");
        return false;
    }

    auto& hand = m_players.at(seat).hand;
    auto remaining = hand;
    for (const auto& card : cards) {
        auto it = find(remaining.begin(), remaining.end(), card);
        if (it == remaining.end()) {
            send_player_error(seat, "Card is not in your hand");
            return false;
        }
        remaining.erase(it);
    }
    if (cards.size() > 1 && remaining.empty()) {
        send_player_error(seat, "The last card must be played alone");
        return false;
    }

    const bool wild = (cards[0] == "W" || cards[0] == "W4");
    const string selected = normalize_wild_color(chosen_color);
    if (!chosen_color.empty() && selected.empty()) {
        send_player_error(seat, "Invalid color");
        return false;
    }

    bool mixed_colors = false;
    bool multi_skip = false;
    if (cards.size() > 1) {
        // Legal multi-card combinations: equal colored numbers, at least three
        // equal numbers of differing colors, or same-colored Skip cards.
        if (wild) {
            send_player_error(seat, "Wild cards cannot be played together");
            return false;
        }
        const string face = cards[0].substr(1);
        const char color = cards[0][0];
        const bool numeric = face.size() == 1 && isdigit(static_cast<unsigned char>(face[0]));
        multi_skip = (face == "SK");
        if (!numeric && !multi_skip) {
            send_player_error(seat, "Invalid multiple-card combination");
            return false;
        }
        for (const auto& card : cards) {
            if (card.size() < 2 || card.substr(1) != face ||
                card == "W" || card == "W4") {
                send_player_error(seat, "Multiple cards must have the same face");
                return false;
            }
            if (card[0] != color) mixed_colors = true;
        }
        if ((mixed_colors && (cards.size() < 3 || multi_skip)) ||
            (mixed_colors && selected.empty())) {
            send_player_error(seat, "Mixed-color numbers require at least 3 cards and a color");
            return false;
        }
    }
    if ((wild && selected.empty()) ||
        (!wild && !mixed_colors && !chosen_color.empty())) {
        send_player_error(seat, "Invalid color selection");
        return false;
    }

    const string current_color = m_active_color.empty() &&
            m_top_card != "W" && m_top_card != "W4"
            ? m_top_card.substr(0, 1) : m_active_color;

    if (m_pending_draw > 0) {
        // Matching is waived when a player is stacking a penalty card.
        // D2 -> D2 or W4; W4 -> W4 only if a D2 started the chain.
        const bool can_stack = cards.size() == 1 &&
            ((m_top_card.size() >= 2 && m_top_card.substr(1) == "D2" &&
              ((cards[0].size() >= 2 && cards[0].substr(1) == "D2") || cards[0] == "W4")) ||
             (m_top_card == "W4" && cards[0] == "W4" && m_chain_has_d2));
        if (!can_stack) {
            send_player_error(seat, "Must stack a permitted penalty card or draw");
            return false;
        }
    } else if (!wild) {
        bool matches_color = cards[0].substr(0, 1) == current_color;
        bool matches_face = m_top_card != "W" && m_top_card != "W4" &&
                            cards[0].substr(1) == m_top_card.substr(1);
        // An initial Wild has no selected color; the first player may set it
        // by playing an ordinary card of any color.
        if (!matches_color && !matches_face && !current_color.empty()) {
            send_player_error(seat, "Card must match color or face");
            return false;
        }
    }
    if (m_has_drawn && (cards.size() != 1 || cards[0] != m_drawn_card)) {
        send_player_error(seat, "Only the newly drawn card can be played");
        return false;
    }

    // Compute W4 legality using the previous effective color and the cards
    // remaining after playing W4, not its newly chosen color.
    bool illegal_w4 = false;
    if (cards[0] == "W4" && m_pending_draw == 0) {
        for (const auto& other : remaining) {
            if (other != "W" && other != "W4" &&
                other.substr(0, 1) == current_color) {
                illegal_w4 = true;
                break;
            }
        }
    }

    close_uno_window();
    hand = move(remaining);
    for (const auto& card : cards) m_discard.push_back(card);
    m_top_card = cards.back();
    m_active_color = (wild || mixed_colors) ? selected : cards.back().substr(0, 1);
    m_has_drawn = false;
    m_drawn_card.clear();

    send_hand(seat);
    Message played("PLAYED");
    played.set("player", seat).set("cards", card_text);
    if (wild || mixed_colors) played.set("color", chosen_color);
    out() << played.serialize() << '\n';
    out().flush();
    broadcast(move(played));

    // Finishing with a 0 or 7 wins immediately, before any swaps take place.
    if (hand.empty()) {
        finish_game(seat);
        return true;
    }
    update_uno_window_after_play(seat);

    if (cards.size() == 1 && m_top_card == "W4") {
        if (m_pending_draw > 0) {
            m_pending_draw += 4;
            m_current_player = next_seat(seat);
            broadcast_turn();
        } else {
            m_pending_draw = 4;
            m_w4_player = seat;
            m_w4_illegal = illegal_w4;
            m_waiting = WaitState::Challenge;
            m_waiting_player = next_seat(seat);
            m_current_player = m_waiting_player;
            Message request("CHALLENGE");
            request.set("player", m_waiting_player).set("ask", "w4")
                   .set("cards", 4).set("color", chosen_color);
            out() << request.serialize() << '\n';
            out().flush();
            broadcast(move(request));
        }
        return true;
    }

    const string face = cards[0].substr(1);
    if (face == "D2") {
        m_pending_draw += 2;
        m_chain_has_d2 = true;
        m_current_player = next_seat(seat);
    } else if (face == "SK") {
        m_current_player = next_seat(seat, static_cast<int>(cards.size()) + 1);
    } else if (face == "RV") {
        if (m_players.size() == 2)
            m_current_player = seat;
        else {
            m_direction *= -1;
            m_current_player = next_seat(seat);
        }
    } else {
        m_current_player = next_seat(seat);
    }

    // A 0 cycles every hand in the current direction and is not a waiting state.
    if (cards.size() == 1 && face == "0") cycle_hands();
    if (cards.size() == 1 && face == "7") {
        m_waiting = WaitState::Swap;
        m_waiting_player = seat;
        Message ask("SWAP");
        ask.set("player", seat).set("waiting", "yes");
        out() << ask.serialize() << '\n';
        out().flush();
        broadcast(move(ask));
        return true;
    }
    broadcast_turn();
    return true;
}

// ============================================================================
// 6. GAMEPLAY AND TURN MANAGEMENT
// ============================================================================

// Broadcast the current turn & face-up card
void HostSession::broadcast_turn() {
    // Create TURN message
    Message turn("TURN");

    // Specify the current player & face-up card
    turn.set("player", m_current_player);
    turn.set("top", m_top_card);

    // Include current turn direction
    turn.set("direction", m_direction);

    // Include pending penalty cards
    if (m_pending_draw > 0) {
        turn.set("pending", m_pending_draw);
    }

    // Display turn info in Host terminal
    out() << turn.serialize() << '\n';
    out().flush();

    // Notify all connected clients
    broadcast(move(turn));
}

// Find the next occupied set in the current turn direction
int HostSession::next_seat(int seat, int steps) const {
    auto it = m_players.find(seat); // Locate current player

    if (it == m_players.end()) {    // Check whether seat exists
        return seat;
    }

    for (int i = 0; i < steps; ++i) {
        if (m_direction == 1) {
            // Move forward to next player
             ++it;
            if (it == m_players.end()) {    // Handle wraparound
                it = m_players.begin();
            }
        } else {
            // Move backward to previous player
            if (it == m_players.begin()) {  // Handle wraparound
                it = m_players.end();
            }
            --it;
        }
    }

    return it->first;   // Return selected player's seat
}

// Send error message to player that attempts invalid action
void HostSession::send_player_error(int seat, const string& reason) {
    if (seat == 0) {
        // Display error in Host terminal
        error(reason);
    } else {
        // Send error message privately to the player
        Message err("ERROR");
        err.set("msg", reason);
        send_to(m_players.at(seat).peer, move(err));
    }
}

// Handle PLAY command from the Host
Session::Result HostSession::play(const Args& args) {
    // Get the card to play
    string card = args[1];

    // Get chosen color if playing Wild card
    string color;
    if (args.size() > 2) {
        color = args[2];
    } else {
        color = "";
    }

    // Validate & process the Host's play
    play_card(0, card, color);

    return Result::Continue;
}

// Draw from the pile, recycling previous discards but keeping the top card.
bool HostSession::draw_one(int seat) {
    if (m_next_card >= m_deck.size()) {
        if (m_discard.size() <= 1) return false;
        // Recycling does not include the visible top card.
        string current = m_discard.back();
        m_deck.assign(m_discard.begin(), m_discard.end() - 1);
        m_discard.assign(1, current);
        shuffle(m_deck.begin(), m_deck.end(), m_rng);
        m_next_card = 0;
    }
    m_players.at(seat).hand.push_back(m_deck[m_next_card++]);
    return true;
}

int HostSession::draw_many(int seat, int count) {
    int actual = 0;
    while (actual < count && draw_one(seat)) ++actual;
    return actual;
}

void HostSession::announce_draw(int seat, int count) {
    Message message("DRAWN");
    message.set("player", seat).set("count", count);
    out() << message.serialize() << '\n';
    out().flush();
    broadcast(move(message));
    send_hand(seat);
}

bool HostSession::take_card(int seat) {
    if (m_waiting != WaitState::None) {
        send_player_error(seat, "Resolve the challenge or swap first");
        return false;
    }
    if (seat != m_current_player) {
        send_player_error(seat, "Not your turn");
        return false;
    }
    if (m_pending_draw > 0) {
        const int count = m_pending_draw;
        m_pending_draw = 0;
        m_chain_has_d2 = false;
        close_uno_window();
        const int actual = draw_many(seat, count);
        announce_draw(seat, actual);
        m_has_drawn = false;
        m_drawn_card.clear();
        m_current_player = next_seat(seat);
        broadcast_turn();
        return true;
    }
    if (m_has_drawn) {
        send_player_error(seat, "Already drew a card this turn");
        return false;
    }
    if (!draw_one(seat)) {
        send_player_error(seat, "No cards left in draw pile");
        return false;
    }
    close_uno_window();
    m_has_drawn = true;
    m_drawn_card = m_players.at(seat).hand.back();
    announce_draw(seat, 1);
    broadcast_turn();
    return true;
}

// Handle PASS command from a player
bool HostSession::finish_draw_turn(int seat) {
    if (m_waiting != WaitState::None) {
        send_player_error(seat, "Resolve the challenge or swap first");
        return false;
    }
    // Verify that it is the player's turn
    if (seat != m_current_player) {
        send_player_error(seat, "Not your turn");
        return false;
    }

    // Players can only pass after voluntarily drawing a card
    if (!m_has_drawn || m_pending_draw > 0) {
        send_player_error(seat, "Can only pass after drawing");
        return false;
    }

    // Reset drawing state & advance to next player
    m_has_drawn = false;
    m_drawn_card.clear();
    m_current_player = next_seat(seat);

    // Announce the next turn
    broadcast_turn();

    return true;
}

// Display the Host's current hand
Session::Result HostSession::cards(const Args&) {
    // Display the Host's hand locally
    send_hand(0);

    return Result::Continue;
}

// Handle DRAW command from the Host
Session::Result HostSession::draw(const Args&) {
    take_card(0);
    return Result::Continue;
}

// Handle PASS command from the Host
Session::Result HostSession::pass(const Args&) {
    finish_draw_turn(0);
    return Result::Continue;
}

// The report window closes once the next player makes a legal move.
void HostSession::close_uno_window() { m_uno_vulnerable = -1; }

void HostSession::update_uno_window_after_play(int seat) {
    m_uno_vulnerable = m_players.at(seat).hand.size() == 1 ? seat : -1;
}

void HostSession::announce_penalty(int seat) {
    Message penalty("UNOPENALTY");
    penalty.set("player", seat).set("count", 2);
    out() << penalty.serialize() << '\n';
    out().flush();
    broadcast(move(penalty));
    draw_many(seat, 2);
    send_hand(seat);
    if (m_uno_vulnerable == seat) close_uno_window();
}

// 'uno' can be called outside the player's turn, except during a forced window.
void HostSession::handle_uno(int seat) {
    if (m_waiting != WaitState::None) {
        send_player_error(seat, "Resolve the challenge or swap first");
        return;
    }
    if (m_players.at(seat).hand.size() == 1) {
        Message declared("UNO");
        declared.set("player", seat);
        out() << declared.serialize() << '\n';
        out().flush();
        broadcast(move(declared));
        if (m_uno_vulnerable == seat) close_uno_window();
    } else {
        announce_penalty(seat);
    }
}

bool HostSession::report_uno(int seat, const string& target) {
    if (m_waiting != WaitState::None) {
        send_player_error(seat, "Resolve the challenge or swap first");
        return false;
    }
    // Only a missed declaration on the latest play is reportable.
    if (m_uno_vulnerable == -1 || target != to_string(m_uno_vulnerable) ||
        seat == m_uno_vulnerable ||
        m_players.at(m_uno_vulnerable).hand.size() != 1) {
        send_player_error(seat, "No unreported UNO in the reporting window");
        return false;
    }
    announce_penalty(m_uno_vulnerable);
    return true;
}

// All hands rotate one seat in the current direction (0-card effect).
void HostSession::cycle_hands() {
    map<int, vector<string>> original;
    for (const auto& [seat, player] : m_players) original[seat] = player.hand;
    // Each player receives the hand of the previous seat in the current direction.
    for (auto& [seat, player] : m_players)
        player.hand = original.at(next_seat(seat, static_cast<int>(m_players.size()) - 1));
    Message swap_message("SWAP");
    swap_message.set("all", "yes");
    out() << swap_message.serialize() << '\n';
    out().flush();
    broadcast(move(swap_message));
    for (const auto& [seat, player] : m_players) send_hand(seat);
    // A new hand with exactly one card needs its own UNO declaration.
    m_uno_vulnerable = -1;
    for (const auto& [seat, player] : m_players)
        if (player.hand.size() == 1) m_uno_vulnerable = seat;
}

bool HostSession::resolve_swap(int seat, const string& target) {
    if (m_waiting != WaitState::Swap || seat != m_waiting_player) {
        send_player_error(seat, "No swap choice available for this player");
        return false;
    }
    int other = -1;
    for (const auto& [candidate, player] : m_players)
        if (to_string(candidate) == target) other = candidate;
    if (other < 0 || other == seat) {
        send_player_error(seat, "Invalid swap target");
        return false;
    }
    std::swap(m_players.at(seat).hand, m_players.at(other).hand);
    m_waiting = WaitState::None;
    m_waiting_player = -1;
    Message done("SWAP");
    done.set("player", seat).set("with", other);
    out() << done.serialize() << '\n';
    out().flush();
    broadcast(move(done));
    send_hand(seat);
    send_hand(other);
    m_uno_vulnerable = -1;
    if (m_players.at(seat).hand.size() == 1) m_uno_vulnerable = seat;
    if (m_players.at(other).hand.size() == 1) m_uno_vulnerable = other;
    broadcast_turn();
    return true;
}

bool HostSession::resolve_challenge(int seat, const string& accept) {
    if (m_waiting != WaitState::Challenge || seat != m_waiting_player ||
        (accept != "yes" && accept != "no")) {
        send_player_error(seat, "No pending Wild Draw Four challenge for this player");
        return false;
    }
    const bool caught = accept == "yes" && m_w4_illegal;
    const int target = caught ? m_w4_player : seat;
    const int count = caught ? 4 : (accept == "yes" ? 6 : 4);
    m_waiting = WaitState::None;
    m_waiting_player = -1;
    m_pending_draw = 0;
    m_chain_has_d2 = false;
    Message verdict("CHALLENGE");
    verdict.set("player", seat).set("accept", accept)
           .set("result", caught ? "illegal" : (accept == "yes" ? "draw6" : "draw4"));
    out() << verdict.serialize() << '\n';
    out().flush();
    broadcast(move(verdict));
    draw_many(target, count);
    send_hand(target);
    m_current_player = caught ? seat : next_seat(seat);
    m_w4_player = -1;
    m_w4_illegal = false;
    broadcast_turn();
    return true;
}

// Calculate the winner's score and end the game.
void HostSession::finish_game(int winner) {
    int points = 0;
    for (const auto& [seat, player] : m_players) {
        if (seat == winner) continue;
        for (const auto& card : player.hand) points += card_points(card);
    }
    Message gameover("GAMEOVER");
    gameover.set("winner", winner).set("points", points);
    out() << gameover.serialize() << '\n';
    out().flush();
    broadcast(move(gameover));
    m_pending_draw = 0;
    m_chain_has_d2 = false;
    m_waiting = WaitState::None;
    m_waiting_player = -1;
    m_uno_vulnerable = -1;
    m_has_drawn = false;
    m_drawn_card.clear();
    return_to_lobby();
}

Session::Result HostSession::uno(const Args& args) {
    if (args.size() > 1) report_uno(0, args[1]);
    else handle_uno(0);
    return Result::Continue;
}

Session::Result HostSession::challenge(const Args& args) {
    if (!valid_challenge_answer(args)) return Result::Continue;
    resolve_challenge(0, args[1]);
    return Result::Continue;
}

Session::Result HostSession::swap(const Args& args) {
    resolve_swap(0, args[1]);
    return Result::Continue;
}

} // namespace uno
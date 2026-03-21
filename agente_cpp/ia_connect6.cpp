#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <map>
#include <random>

#include <grpcpp/grpcpp.h>
#include "pb/connect6.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReaderWriter;
using connect6::GameServer;
using connect6::PlayerAction;
using connect6::GameState;
using connect6::Point;

using namespace std;

typedef int8_t Board[19][19];

struct Pos { int x, y; };

struct Frame { int x1, y1, x2, y2; };

struct MovePair {
    Pos p1, p2;
    bool single_stone = false;
};

struct ScoredMove {
    MovePair move;
    int score;
};

std::atomic<bool> timeout_flag; 

class Zobrist {
public:
    uint64_t table[19][19][3];

    Zobrist() {
        std::mt19937_64 rng(1337);
        std::uniform_int_distribution<uint64_t> dist;
        for (int r = 0; r < 19; r++) {
            for (int c = 0; c < 19; c++) {
                for (int s = 0; s < 3; s++) table[r][c][s] = dist(rng);
            }
        }
    }

    // AHORA DEVUELVE uint64_t
    uint64_t init_hash(Board board) {
        uint64_t h = 0;
        for (int r = 0; r < 19; r++) {
            for (int c = 0; c < 19; c++) {
                h ^= table[r][c][board[r][c]];
            }
        }
        return h;
    }

    // AHORA RECIBE EL HASH Y LO DEVUELVE MODIFICADO
    uint64_t update(uint64_t h, int r, int c, int old_piece, int new_piece) {
        h ^= table[r][c][old_piece];
        h ^= table[r][c][new_piece];
        return h;
    }
};

Zobrist zobrist_engine;

void sync_board(const connect6::GameState& state, Board local_board);
vector<Pos> get_candidates(Board board);
vector<MovePair> generate_move_combinations(const vector<Pos>& candidates, int stones_required);
int score_by_count(int count, bool is_mine);
int evaluate_line(Board board, int r, int c, int dr, int dc);
int evaluate_fitness(Board board);
int minimax(Board board, uint64_t h, int depth, int alpha, int beta, bool isMaximizing, int stones_req, int current_score);
MovePair solve_at_fixed_depth(Board board, int depth, int stones_req);

void sync_board(const connect6::GameState& state, Board local_board) {
    for (int r = 0; r < 19; ++r) {
        const auto& row = state.board(r);
        for (int c = 0; c < 19; ++c) {
            connect6::PlayerColor color = row.cells(c);
            if (color == state.my_color()) {
                local_board[r][c] = 1; // Mi piedra
            } else if (color == connect6::UNKNOWN) {
                local_board[r][c] = 0; // Vacío
            } else {
                local_board[r][c] = 2; // Oponente
            }
        }
    }
}

vector<Pos> get_candidates(Board board, int min_r, int min_c, int max_r, int max_c) {
    std::vector<Pos> candidates;
    bool board_empty = true;
    bool visited[19][19] = {false};

    for (int r = min_r; r < max_r; ++r) {
        for (int c = min_c; c < max_c; ++c) {
            if (board[r][c] != 0) {
                board_empty = false;
                for (int dr = -2; dr <= 2; ++dr) {
                    for (int dc = -2; dc <= 2; ++dc) {
                        int nr = r + dr, nc = c + dc;
                        if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19 && board[nr][nc] == 0 && !visited[nr][nc]) {
                            visited[nr][nc] = true;
                            candidates.push_back({nr, nc});
                        }
                    }
                }
            }
        }
    }

    // Si el tablero está vacío, proponer un cuadro central de 3x3
    if (board_empty) {
        for (int r = 8; r <= 10; ++r)
            for (int c = 8; c <= 10; ++c)
                candidates.push_back({r, c});
    }

    return candidates;
}

std::vector<MovePair> generate_move_combinations(const std::vector<Pos>& candidates, int stones_required) {
    std::vector<MovePair> combinations;
    
    if (stones_required == 1) {
        for (const auto& p : candidates) {
            combinations.push_back({p, {-1, -1}, true});
        }
    } else {
        // Combinaciones de N en 2: n^(2_) /2!
        for (size_t i = 0; i < candidates.size(); ++i) {
            for (size_t j = i + 1; j < candidates.size(); ++j) {
                combinations.push_back({candidates[i], candidates[j], false});
            }
        }
    }
    return combinations;
}


int score_by_count(int count, bool is_mine) {
    switch (count) {
        case 6: return is_mine ? 1000000 : -1000000;
        case 5: return is_mine ? 10000 : -20000;
        case 4: return is_mine ? 10000 : -20000;
        case 3: return is_mine ? 200 : -200;
        case 2: return is_mine ? 20 : -20;
        default: return 0;
    }
}


int evaluate_line(Board board, int r, int c, int dr, int dc) {
    int my_stones = 0;
    int opponent_stones = 0;

    for (int i = 0; i < 6; ++i) {
        int nr = r + i * dr;
        int nc = c + i * dc;

        if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19) {
            if (board[nr][nc] == 1) my_stones++;
            else if (board[nr][nc] == 2) opponent_stones++;
        } else {
            // Fuera del tablero, esta ventana de 6 no es válida
            return 0;
        }
    }

    // Si hay piedras de ambos colores, nadie puede hacer 6 en esta ventana
    if (my_stones > 0 && opponent_stones > 0) return 0;

    // Puntuación para mí
    if (my_stones > 0) return score_by_count(my_stones, true);
    
    // Puntuación para el oponente (negativa)
    if (opponent_stones > 0) return score_by_count(opponent_stones, false);

    return 0;
}

int evaluate_fitness(Board board, int min_r, int min_c, int max_r, int max_c) {
    int total_score = 0;

    // Direcciones: Horizontal (1,0), Vertical (0,1), Diag1 (1,1), Diag2 (1,-1)
    int dr[] = {1, 0, 1, 1};
    int dc[] = {0, 1, 1, -1};

    for (int d = 0; d < 4; ++d) {
        for (int r = min_r; r <= max_r; ++r) {
            for (int c = min_c; c <= max_c; ++c) {
                total_score += evaluate_line(board, r, c, dr[d], dc[d]);
            }
        }
    }

    return total_score;
}

int evaluate_move_impact(Board board, MovePair m) {
    int score = 0;
    int dr[] = {1, 0, 1, 1};
    int dc[] = {0, 1, 1, -1};

    // Usamos un set o un mapa para no repetir ventanas si p1 y p2 están cerca
    // Pero es más rápido simplemente iterar y validar coordenadas
    for (int d = 0; d < 4; ++d) {
        // Marcamos qué ventanas ya procesamos para no duplicar
        // Una ventana se identifica por su celda de inicio (r, c)
        std::map<pair<int, int>, bool> visited_windows;

        Pos points[2] = {m.p1, m.p2};
        int num_points = m.single_stone ? 1 : 2;

        for (int p = 0; p < num_points; ++p) {
            for (int i = 0; i < 6; ++i) {
                int start_r = points[p].x - i * dr[d];
                int start_c = points[p].y - i * dc[d];
                
                if (visited_windows.find({start_r, start_c}) == visited_windows.end()) {
                    score += evaluate_line(board, start_r, start_c, dr[d], dc[d]);
                    visited_windows[{start_r, start_c}] = true;
                }
            }
        }
    }
    return score;
}
Frame get_frame(Board board){

    int min_r = 18, min_c = 18;
    int max_r = 0, max_c = 0;
    for (int r = 0; r < 19; ++r) {
        for (int c = 0; c < 19; ++c) {
            if(board[r][c] != 0){
                int possible_max_r = r + 2;
                int possible_min_r = r - 2;
                int possible_max_c = c + 2;
                int possible_min_c = c - 2;
                if(possible_max_r > max_r && possible_max_r < 19) max_r = possible_max_r;
                if(possible_min_r < min_r && possible_min_r >= 0) min_r = possible_min_r;
                if(possible_max_c > max_c && possible_max_c < 19) max_c = possible_max_c;
                if(possible_min_c < min_c && possible_min_c >= 0) min_c = possible_min_c;
            }
        }
    }

    if (min_r > max_r) return {0, 0, 18, 18};
    return {min_r, min_c, max_r, max_c};
}

struct Threat{
    bool valid;
    bool mine;
    bool direct_win;
    vector<Pos> threats;
};

struct ThreatPos{
    Pos pos;
    int count;
    bool mine;
};

struct GameThreats{
    bool op_direct_win;
    bool direct_win;
    vector<Pos> op_direct_win_threats;
    vector<Pos> win;
    vector<ThreatPos> threats;
};

Threat evaluate_threat(Board board, int r, int c, int dr, int dc) {
    int my_stones = 0;
    int opponent_stones = 0;
    vector<Pos> empty;
    int empty_count;
    // int possible_opened = 0;

    for (int i = 0; i < 6; ++i) {
        int nr = r + i * dr;
        int nc = c + i * dc;

        if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19) {
            if (board[nr][nc] == 1) my_stones++;
            else if (board[nr][nc] == 2) opponent_stones++;
            else{
                empty.push_back({nr, nc});
            }

            // if(i == 0 && board[nr][nc] != 1 && board[nr][nc] != 2){
            //     possible_opened++;
            // } else if(i == 5 && board[nr][nc] != 1 && board[nr][nc] != 2){
            //     possible_opened++;
            // }

        } else {
            return {false, false, false, {}};
        }
    }

    if (my_stones > 0 && opponent_stones > 0) return {false, false, false, {}};

    if(my_stones > 0){
        if(empty.size() <= 2) return {true, true, true, empty};
        else if(empty.size() > 2 && empty.size() <= 4) return {true, true, false, empty};
    }

    if(opponent_stones > 0){
        if(empty.size() == 2){
            vector<int> range = {-1, 0, 5, 6};
            bool is_opened = true;
            for(int &i : range){
                int nr = r + i * dr;
                int nc = c + i * dc;

                if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19) {
                    if (board[nr][nc] == 1 || board[nr][nc] == 2){
                        is_opened = false;
                        break;
                    }
                } else {
                    is_opened = false;
                    break;
                }
            }

            return {true, false, is_opened, empty};
        }
        else if(empty.size() != 2 && empty.size() <= 4) return {true, false, false, empty};
    }

    return {false, false, false, {}};
}

GameThreats get_threats(Board board, int min_r, int min_c, int max_r, int max_c) {
    int total_score = 0;

    // Direcciones: Horizontal (1,0), Vertical (0,1), Diag1 (1,1), Diag2 (1,-1)
    int dr[] = {1, 0, 1, 1};
    int dc[] = {0, 1, 1, -1};
    map<pair<int,int>, ThreatPos> threat_map;
    map<pair<int,int>, Pos> op_direct_win_threats_map;
    bool op_direct_win = false;
    Threat threat;

    for (int d = 0; d < 4; ++d) {
        for (int r = min_r; r <= max_r; ++r) {
            for (int c = min_c; c <= max_c; ++c) {
                threat = evaluate_threat(board, r, c, dr[d], dc[d]);
                if(!threat.valid){
                    continue;
                }
                
                if(!threat.mine && !threat.direct_win){
                    continue;
                }

                if(!threat.mine && threat.direct_win){
                    op_direct_win = true;

                    for (auto &pos : threat.threats) {
                        auto key = make_pair(pos.x, pos.y);
                    
                        if (op_direct_win_threats_map.find(key) == op_direct_win_threats_map.end()) {
                            op_direct_win_threats_map[key] = {pos.x, pos.y};
                        }
                    }
                    continue;
                }

                if(threat.mine && threat.direct_win){
                    return {false, true, {}, threat.threats, {}};
                }

                for (auto &pos : threat.threats) {
                    auto key = make_pair(pos.x, pos.y);

                    if (threat_map.find(key) == threat_map.end()) {
                        threat_map[key] = {{pos.x, pos.y}, 0, true};
                    }

                    threat_map[key].count++;
                }

            }
        }
    }

    vector<ThreatPos> result;
    for (auto &kv : threat_map) {
        result.push_back(kv.second);
    }

    vector<Pos> op_direct_win_result;
    for (auto &kv : op_direct_win_threats_map) {
        op_direct_win_result.push_back(kv.second);
    }

    return {op_direct_win, false, op_direct_win_result, {}, result};
}


vector<MovePair> get_ordered_moves(Board board, const vector<MovePair>& moves, bool isMaximizing) {
    vector<ScoredMove> scored;
    scored.reserve(moves.size());

    for (const auto& m : moves) {
        int s = evaluate_move_impact(board, m);
        scored.push_back({m, s});
    }

    std::sort(scored.begin(), scored.end(), [isMaximizing](const ScoredMove& a, const ScoredMove& b) {
        return isMaximizing ? (a.score > b.score) : (a.score < b.score);
    });

    vector<MovePair> ordered;
    ordered.reserve(moves.size());
    for (const auto& sm : scored) ordered.push_back(sm.move);
    return ordered;
}


struct TTEntry {
    int depth;
    int score;
};
map<uint64_t, TTEntry> transposition_table;

int quick_score(Board board, Pos p) {
    int score = 0;
    int dr[] = {1, 0, 1, 1};
    int dc[] = {0, 1, 1, -1};

    for (int d = 0; d < 4; ++d) {
        for (int i = 0; i < 6; ++i) {
            int start_r = p.x - i * dr[d];
            int start_c = p.y - i * dc[d];
            score += abs(evaluate_line(board, start_r, start_c, dr[d], dc[d]));
        }
    }
    return score;
}

struct ScoredPos { Pos p; int score; };

int minimax(Board board, uint64_t h, int depth, int alpha, int beta, bool isMaximizing, int stones_req, int current_score) {

    if (transposition_table.count(h) && transposition_table[h].depth >= depth) {
        return transposition_table[h].score;
    }

    // Verificación de seguridad y salida
    if (timeout_flag.load()) return isMaximizing ? -1e8 : 1e8;

    Frame frame = get_frame(board);

    if (depth == 0) {
        return current_score;
    }

    vector<Pos> candidates = get_candidates(board, frame.x1, frame.y1, frame.x2, frame.y2);
    // auto moves = generate_move_combinations(candidates, stones_req);
    vector<ScoredPos> rated;
    for(auto p : candidates) {
        rated.push_back({p, quick_score(board, p)}); 
    }

    sort(rated.begin(), rated.end(), [](const ScoredPos& a, const ScoredPos& b) {
        return a.score > b.score;
    });
    vector<Pos> best_points;
    int limit = min((int)rated.size(), 20); 
    for(int i = 0; i < limit; ++i) {
        best_points.push_back(rated[i].p);
    }
    
    auto moves = generate_move_combinations(best_points, stones_req);
    if (moves.empty()) return current_score;

    

    // --- MOVE ORDERING ---
    // auto moves = get_ordered_moves(board, raw_moves, isMaximizing);
    // ---------------------

    // Si no hay movimientos posibles, evaluamos el estado actual
    // if (moves.empty()) return current_score;

    if (isMaximizing) {
        int maxEval = -1e9;
        for (auto& m : moves) {

            uint64_t next_h = zobrist_engine.update(h, m.p1.x, m.p1.y, 0, 1);
            if (!m.single_stone) next_h = zobrist_engine.update(next_h, m.p2.x, m.p2.y, 0, 1);

            int old_impact = evaluate_move_impact(board, m);

            board[m.p1.x][m.p1.y] = 1; 
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 1;

            int new_impact = evaluate_move_impact(board, m);

            int next_score = current_score - old_impact + new_impact;

            // En Connect6, tras el primer turno, siempre se piden 2 piedras
            int eval = minimax(board, next_h, depth - 1, alpha, beta, false, 2, next_score);
            
            // DESHACER
            board[m.p1.x][m.p1.y] = 0;
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 0;

            maxEval = max(maxEval, eval);
            alpha = max(alpha, eval);
            
            if (beta <= alpha || timeout_flag.load()) break;
        }
        transposition_table[h] = {depth, maxEval};
        return maxEval;
    } else {
        int minEval = 1e9;
        for (auto& m : moves) {

            uint64_t next_h = zobrist_engine.update(h, m.p1.x, m.p1.y, 0, 1);
            if (!m.single_stone) next_h = zobrist_engine.update(next_h, m.p2.x, m.p2.y, 0, 1);

            int old_impact = evaluate_move_impact(board, m);

            // SIMULAR oponente
            board[m.p1.x][m.p1.y] = 2;
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 2;

            int new_impact = evaluate_move_impact(board, m);

            int next_score = current_score - old_impact + new_impact;

            int eval = minimax(board, next_h, depth - 1, alpha, beta, true, 2, next_score);

            board[m.p1.x][m.p1.y] = 0;
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 0;

            minEval = min(minEval, eval);
            beta = min(beta, eval);

            if (beta <= alpha || timeout_flag.load()) break;
        }
        transposition_table[h] = {depth, minEval};
        return minEval;
    }
}

MovePair solve_at_fixed_depth(Board board, int depth, int stones_req) {

    // Si no se pide una sola piedra

    Frame frame = get_frame(board);
    int base_score = evaluate_fitness(board, frame.x1, frame.y1, frame.x2, frame.y2);
    uint64_t current_h = zobrist_engine.init_hash(board);

    GameThreats actual_threats = get_threats(board, frame.x1, frame.y1, frame.x2, frame.y2);

    vector<Pos> candidates;
    vector<MovePair> moves; 
    // Si tenemos victoria directa
    if(actual_threats.direct_win){
        cout << "Tengo victoria segura..." << endl;
        cout << "La victoria es en:" << endl;
        for (auto &p : actual_threats.win) {
            cout << p.x << ", " << p.y << endl;
        }
        MovePair m;
        if(actual_threats.win.size() == 2){
            m.p1 = actual_threats.win[0];
            m.p2 = actual_threats.win[1];
            m.single_stone = false;
            return m;
        } else if(actual_threats.win.size() == 1){
            m.p1 = actual_threats.win[0];
            vector<Pos> candidates = get_candidates(board, frame.x1, frame.y1, frame.x2, frame.y2);
            for (auto& p : candidates) {
                if (p.x != m.p1.x || p.y != m.p1.y) {
                    m.p2 = p;
                    break;
                }
            }
            m.single_stone = false;
            return m;
        }
    } else if (actual_threats.op_direct_win){
        cout << "El rival obliga a jugar forzado..." << endl;
        cout << "Maneras de detenerlo:" << endl;
        for (auto &p : actual_threats.op_direct_win_threats) {
            cout << p.x << ", " << p.y << endl;
        }
        moves = generate_move_combinations(actual_threats.op_direct_win_threats, stones_req);

    } else{

        candidates = get_candidates(board, frame.x1, frame.y1, frame.x2, frame.y2);
        vector<ScoredPos> rated;
        for(auto p : candidates) {
            rated.push_back({p, quick_score(board, p)}); 
        }

        sort(rated.begin(), rated.end(), [](const ScoredPos& a, const ScoredPos& b) {
            return a.score > b.score;
        });
        vector<Pos> best_points;
        int limit = min((int)rated.size(), 25); 
        for(int i = 0; i < limit; ++i) {
            best_points.push_back(rated[i].p);
        }
        
        moves = generate_move_combinations(best_points, stones_req);
        
    }

    // auto moves = generate_move_combinations(candidates, stones_req);
    

    // --- MOVE ORDERING ---
    // auto moves = get_ordered_moves(board, raw_moves, true);
    // ---------------------
    
    MovePair best_m;
    int best_v = -1e9;

    for (auto& m : moves) {
        if (timeout_flag.load()) break;

        int old_impact = evaluate_move_impact(board, m);

        board[m.p1.x][m.p1.y] = 1;
        if (!m.single_stone) board[m.p2.x][m.p2.y] = 1;

        uint64_t next_h = zobrist_engine.update(current_h, m.p1.x, m.p1.y, 0, 1);
        if (!m.single_stone) next_h = zobrist_engine.update(next_h, m.p2.x, m.p2.y, 0, 1);

        int new_impact = evaluate_move_impact(board, m);

        // Llamada al minimax recursivo
        int current_v = base_score - old_impact + new_impact;
        int v = minimax(board, next_h, depth - 1, -1e9, 1e9, false, 2, current_v);

        board[m.p1.x][m.p1.y] = 0;
        if (!m.single_stone) board[m.p2.x][m.p2.y] = 0;

        if (v > best_v) {
            best_v = v;
            best_m = m;
        }
    }
    return best_m;
}


void playGame(shared_ptr<Channel> channel, string teamName) {
    auto stub = GameServer::NewStub(channel);
    ClientContext context;

    // Abrir el stream bidireccional
    shared_ptr<ClientReaderWriter<PlayerAction, GameState>> stream(
        stub->Play(&context));

    cout << "🔄 Registrando equipo: " << teamName << endl;
    PlayerAction register_action;
    register_action.set_register_team(teamName);
    stream->Write(register_action);

    GameState state;
    while (stream->Read(&state)) {
        if (state.status() == connect6::GameState_Status_WAITING) {
            cout << "⏳ Esperando contrincante..." << endl;
        } 
        else if (state.status() == connect6::GameState_Status_PLAYING) {
            if (state.is_my_turn()) {
                cout << "🎲 Es mi turno, papaíto. Piedras requeridas: " << state.stones_required() << endl;

                // Lógica de minimax iterativo 
                MovePair best_action; 
                Board current_board;
                sync_board(state, current_board);

                PlayerAction move_action;
                auto* move = move_action.mutable_move();

                if (state.stones_required() == 1) {
                    vector<Pos> center_cells = {
                        {8,8}, {8,9}, {8,10},
                        {9,8}, {9,9}, {9,10},
                        {10,8}, {10,9}, {10,10}
                    };

                    vector<Pos> available;

                    for (auto &p : center_cells) {
                        if (current_board[p.x][p.y] == 0) {
                            available.push_back(p);
                        }
                    }

                    if (!available.empty()) {
                        int idx = rand() % available.size();

                        MovePair m;
                        m.p1 = available[idx];
                        m.single_stone = true;

                        best_action = m;
                    }
                } else {

                    timeout_flag.store(false);

                    // Iniciamos cronómetro en un hilo aparte
                    thread timer([&]() {
                        this_thread::sleep_for(chrono::milliseconds(9500));
                        timeout_flag.store(true); 
                    });

                    for (int d = 1; d <= 6; ++d) {
                        
                        // Llamamos a la búsqueda para esta profundidad específica
                        MovePair move_at_depth = solve_at_fixed_depth(current_board, d, state.stones_required());
                        
                        // Si durante la búsqueda se acabó el tiempo, ignoramos el resultado incompleto
                        if (timeout_flag.load()) {
                            cout << "Tiempo agotado. Usando mejor jugada de capa " << d-1 << endl;
                            break;
                        }

                        // Si terminamos la capa a tiempo, esta es nuestra nueva mejor jugada
                        best_action = move_at_depth;
                        cout << "Capa " << d << " analizada con éxito." << endl;
                    }

                    if (timer.joinable()) timer.detach();
                }

                connect6::Point* stone1 = move->add_stones();
                stone1->set_x(best_action.p1.x);
                stone1->set_y(best_action.p1.y);

                // Piedra 2 (Solo si el juego requiere 2 y no es un movimiento "single_stone")
                if (state.stones_required() == 2 && !best_action.single_stone) {
                    connect6::Point* stone2 = move->add_stones();
                    stone2->set_x(best_action.p2.x);
                    stone2->set_y(best_action.p2.y);
                }

                stream->Write(move_action);
                if(state.stones_required() == 1){
                    cout << "✅ Movimiento enviado: (" << best_action.p1.x << "," << best_action.p1.y << ")" << endl;
                } else {
                    cout << "✅ Movimiento enviado: (" << best_action.p1.x << "," << best_action.p1.y << ") y (" << best_action.p2.x << "," << best_action.p2.y << ")" << endl;
                }
                
            } else {
                cout << "⌛ Esperando al perdedor..." << endl;
            }
        } 
        else if (state.status() == connect6::GameState_Status_FINISHED) {
            cout << "🏁 PARTIDA FINALIZADA. Ganador: " << state.winner() << endl;
            break; 
        }
    }
}

int main() {
    srand(time(NULL));

    // Lee la dirección del servidor de las variables de entorno, como en Go
    char* addr_env = std::getenv("SERVER_ADDR");
    std::string target_str = (addr_env) ? addr_env : "servidor:50051";

    char* team_env = std::getenv("TEAM_NAME");
    std::string team_name = (team_env) ? team_env : "Bot_CPP_David";

    while (true) {
        std::cout << "🔄 Conectando a " << target_str << " como " << team_name << "..." << std::endl;
        
        auto channel = grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials());
        playGame(channel, team_name);

        std::cout << "⏳ Reconectando en 3 segundos..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    return 0;
}
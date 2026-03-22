#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <map>

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

struct MovePair {
    Pos p1, p2;
    bool single_stone = false;
};

std::atomic<bool> timeout_flag; 

// --- Prototipos de Funciones ---
void sync_board(const connect6::GameState& state, Board local_board);
vector<Pos> get_candidates(Board board);
vector<MovePair> generate_move_combinations(const vector<Pos>& candidates, int stones_required);
int score_by_count(int count, bool is_mine);
int evaluate_line(Board board, int r, int c, int dr, int dc);
int evaluate_fitness(Board board);
int minimax(Board board, int depth, int alpha, int beta, bool isMaximizing, int stones_req);
MovePair solve_at_fixed_depth(Board board, int depth, int stones_req);

// --- Implementación de Funciones ---
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

vector<Pos> get_candidates(Board board) {
    std::vector<Pos> candidates;
    bool board_empty = true;
    bool visited[19][19] = {false};

    for (int r = 0; r < 19; ++r) {
        for (int c = 0; c < 19; ++c) {
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
        case 5: return is_mine ? 10000 : -90000;
        case 4: return is_mine ? 10000 : -90000;
        case 3: return is_mine ? 100 : -500;
        case 2: return is_mine ? 10 : -50;
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

int evaluate_fitness(Board board) {
    int total_score = 0;

    // Direcciones: Horizontal (1,0), Vertical (0,1), Diag1 (1,1), Diag2 (1,-1)
    int dr[] = {1, 0, 1, 1};
    int dc[] = {0, 1, 1, -1};

    for (int r = 0; r < 19; ++r) {
        for (int c = 0; c < 19; ++c) {
            // Solo evaluamos si hay una piedra o si es un inicio de posible línea
            for (int d = 0; d < 4; ++d) {
                total_score += evaluate_line(board, r, c, dr[d], dc[d]);
            }
        }
    }
    return total_score;
}



// ---------------------------- Lógica de Amenazas --------------------------------

struct Win{
    bool win;
    vector<Pos> move;
};

struct Move {
    Pos p1, p2;
    int type;
};

struct ThreatsSearch{
    Win win;
    bool op_forced;
    int score;
    vector<Move> threats;
    vector<Move> forced_defense;
};

struct Threat{
    Win win;
    bool op_forced;
};

Threat evaluate_line_threat(Board board, int r, int c, int dr, int dc, int &acc_score, vector<Move>& threats, vector<Move>& forced_defense, int player) {
    
    Threat threat = {{false, {}}, false};
    int my_stones = 0;
    int opponent_stones = 0;
    int opponent = (player == 1 ? 2 : 1);
    int automaton_state = 0;
    vector<Pos> empty;
    Move possible_threats;
    int possible_threat_side = 0;

    for (int i = 0; i < 6; ++i) {
        int nr = r + i * dr;
        int nc = c + i * dc;

        if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19) {
            int eval = board[nr][nc];
            if (board[nr][nc] == player) my_stones++;
            else if (board[nr][nc] == opponent) opponent_stones++;
            else empty.push_back({nr, nc});

            if(automaton_state == 0 && eval == 0) automaton_state = 1;
            else if(automaton_state == 0 && eval != 0) automaton_state = 2;
            else if(automaton_state == 1 && eval == 0) automaton_state = 1;
            else if(automaton_state == 1 && eval != 0){
                int last_nr = r + (i - 1) * dr;
                int last_nc = c + (i - 1) * dc;
                possible_threats.p1 = {last_nr, last_nc};
                possible_threat_side = 1;
                automaton_state = 3;
            }
            else if(automaton_state == 3 && eval == 0){
                possible_threats.p2 = {nr, nc};
                possible_threat_side = 2;
                automaton_state = 4;
            }
            else if(automaton_state == 3 && eval != 0) automaton_state = 3;
            else if(automaton_state == 4 && eval == 0) automaton_state = 4;
            else if(automaton_state == 4 && eval != 0) automaton_state = 2;

        } else {
            // Fuera del tablero, esta ventana de 6 no es válida
            acc_score += 0;
        }
    }

    // Si hay piedras de ambos colores, nadie puede hacer 6 en esta ventana
    if (my_stones > 0 && opponent_stones > 0){
        return threat;
    }

    bool open = false;

    if(automaton_state == 4){
        open = true;
    }

    // Puntuación para mí
    if (my_stones > 0){

        if(my_stones == 4 || my_stones == 5){
            threat.win.win = true;
            threat.win.move = empty;
            return threat;
        }

        if(open){

            vector<int> range = {-1, 6};
            bool wide_open = true;
            for(int &i : range){
                int nr = r + i * dr;
                int nc = c + i * dc;

                if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19) {
                    if (board[nr][nc] == opponent){
                        wide_open = false;
                        break;
                    }
                } else {
                    wide_open = false;
                    break;
                }
            }
            
            acc_score += score_by_count(my_stones, true);

            if(wide_open && my_stones == 2){

                bool different = true;
                for(auto &t : threats){
                    if( t.p1.x == possible_threats.p1.x && t.p1.y == possible_threats.p1.y && 
                        t.p2.x == possible_threats.p2.x && t.p2.y == possible_threats.p2.y)
                        {
                        different = false;
                        break;
                    }
                }

                if(different){
                    possible_threats.type = 2;
                    threats.push_back(possible_threats);
                }
                

            } else if (wide_open && my_stones == 3){

                bool different = true;

                // Chequear si hay una amenaza de 3 open doble que es imparable
                for(auto &t : threats){
                    if(t.type == 3 && 
                        (t.p1.x != possible_threats.p1.x && t.p1.y != possible_threats.p1.y) && 
                        (t.p2.x != possible_threats.p2.x && t.p2.y != possible_threats.p2.y))
                        {
                        threat.win.win = true;
                        threat.win.move = {t.p1, possible_threats.p1};
                    }

                    if( t.p1.x == possible_threats.p1.x && t.p1.y == possible_threats.p1.y && 
                        t.p2.x == possible_threats.p2.x && t.p2.y == possible_threats.p2.y)
                        {
                        different = false;
                    }

                }
                
                if(different){
                    possible_threats.type = 3;
                    threats.push_back(possible_threats);
                }

            }

            return threat;

        } else {

            // Caso en que no es amenaza open, pero también se puede hacer un 4 wide_open

            vector<int> range = {-1, 0, 5, 6};
            bool wide_open = true;
            for(int &i : range){
                int nr = r + i * dr;
                int nc = c + i * dc;

                if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19) {
                    if (board[nr][nc] != 0){
                        wide_open = false;
                        break;
                    }
                } else {
                    wide_open = false;
                    break;
                }
            }
            
            acc_score += score_by_count(my_stones, true);

            if(wide_open && my_stones == 2){

                Move m;

                if(board[r + 1 * dr][c + 1 * dc] == 0){
                    m = {{r + 1 * dr, c + 1 * dc}, {r + 3 * dr, c + 3 * dc}, 2};
                } else if(board[r + 3 * dr][c + 3 * dc] == player){
                    m = {{r + 2 * dr, c + 2 * dc}, {r + 4 * dr, c + 4 * dc}, 2};
                } else{
                    m = {{r + 2 * dr, c + 2 * dc}, {r + 3 * dr, c + 3 * dc}, 2};
                }

                bool different = true;
                for(auto &t : threats){
                    if( t.p1.x == m.p1.x && t.p1.y == m.p1.y && 
                        t.p2.x == m.p2.x && t.p2.y == m.p2.y)
                        {
                        different = false;
                        break;
                    }
                }

                if(different){
                    threats.push_back(m);
                }
            }

            // } else if (wide_open && my_stones == 3){

            //     bool different = true;

            //     // Chequear si hay una amenaza de 3 open doble que es imparable
            //     for(auto &t : threats){
            //         if(t.type == 3 && 
            //             (t.p1.x != possible_threats.p1.x && t.p1.y != possible_threats.p1.y) && 
            //             (t.p2.x != possible_threats.p2.x && t.p2.y != possible_threats.p2.y))
            //             {
            //             threat.win.win = true;
            //             threat.win.move = {t.p1, possible_threats.p1};
            //         }

            //         if( t.p1.x == possible_threats.p1.x && t.p1.y == possible_threats.p1.y && 
            //             t.p2.x == possible_threats.p2.x && t.p2.y == possible_threats.p2.y)
            //             {
            //             different = false;
            //         }

            //     }
                
            //     if(different){
            //         possible_threats.type = 3;
            //         threats.push_back(possible_threats);
            //     }

            // }

            // return threat;


        }

        acc_score += score_by_count(my_stones, true);
        return threat;
    } 

    // Puntuación para el oponente (negativa)
    if (opponent_stones > 0){

        if(opponent_stones == 4 || opponent_stones == 5){
            threat.op_forced = true;
            // cout << "Hay una jugada forzada de al menos una piedra del rival" << endl;
        }

        if(open){
            vector<int> range = {-1, 6};
            bool wide_open = true;
            vector<Pos> empty_bounds;
            for(int &i : range){
                int nr = r + i * dr;
                int nc = c + i * dc;

                if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19) {
                    if (board[nr][nc] == opponent){
                        wide_open = false;
                        break;
                    } else {
                        empty_bounds.push_back({nr, nc});
                    }
                } else {
                    wide_open = false;
                    break;
                }
            }
            acc_score += score_by_count(opponent_stones, false);

            if(wide_open && opponent_stones == 4){
                // printf("Jugadas que tengo para defender: (%d,%d) y (%d,%d)\n", empty_bounds[0].x, empty_bounds[0].y, possible_threats.p2.x, possible_threats.p2.y);
                // printf("(%d,%d) y (%d,%d)\n", empty_bounds[1].x, empty_bounds[1].y, possible_threats.p1.x, possible_threats.p1.y);
                // printf("(%d,%d) y (%d,%d)\n", possible_threats.p1.x, possible_threats.p1.y, possible_threats.p2.x, possible_threats.p2.y);
                forced_defense.push_back({empty_bounds[0], possible_threats.p2, 4});
                forced_defense.push_back({empty_bounds[1], possible_threats.p1, 4});
                forced_defense.push_back({possible_threats.p1, possible_threats.p2, 4});
            }

            return threat;
        } else if(opponent_stones == 5){

            vector<int> range = {-1, 6};
            bool wide_open = true;
            vector<Pos> empty_bounds;
            for(int &i : range){
                int nr = r + i * dr;
                int nc = c + i * dc;

                if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19) {
                    if (board[nr][nc] == opponent){
                        wide_open = false;
                        break;
                    } else {
                        empty_bounds.push_back({nr, nc});
                    }
                } else {
                    wide_open = false;
                    break;
                }
            }
            acc_score += score_by_count(opponent_stones, false);

            if(wide_open && possible_threat_side == 1){
                // printf("Jugadas que tengo para defender: (%d,%d) y (%d,%d)\n", empty_bounds[1].x, empty_bounds[1].y, possible_threats.p1.x, possible_threats.p1.y);
                forced_defense.push_back({empty_bounds[1], possible_threats.p1, 5});
            } else if(wide_open && possible_threat_side == 2){
                // printf("Jugadas que tengo para defender: (%d,%d) y (%d,%d)\n", empty_bounds[0].x, empty_bounds[0].y, possible_threats.p2.x, possible_threats.p2.y);
                forced_defense.push_back({empty_bounds[0], possible_threats.p2, 5});
            }

            return threat;

        }

        acc_score += score_by_count(opponent_stones, false);
        return threat;
    }

    return threat;

}


// Tiene que devolver múltiples cosas: 
// - Debe ser capaz de detectar victorias directas, en cuyo caso, devolvemos esa jugada de una vez
// - Debe ser capaz de detectar si el rival tiene jugada forzada de al menos una casilla, es decir, el rival tiene un open (no wide_open), en este caso llamamos a minimax normal
// - Como minimax puede llegar a ser llamada a partir de ella, aprovechamos y calculamos el fitness o score de la posición actual que no cuesta nada.
// - En caso de que el rival no tenga ninguna jugada que nos fuerce a taparlo, entonces llamamos a VCF y le pasamos los threats calculados aquí
ThreatsSearch evaluate_threats(Board board, int player) {

    ThreatsSearch threatSearch = {{false, {}}, false, 0, {}};

    int total_score = 0;
    vector<Move> threats = {};
    vector<Move> forced_defense = {};
    // Direcciones: Horizontal (1,0), Vertical (0,1), Diag1 (1,1), Diag2 (1,-1)
    int dr[] = {1, 0, 1, 1};
    int dc[] = {0, 1, 1, -1};

    for (int r = 0; r < 19; ++r) {
        for (int c = 0; c < 19; ++c) {
            // Solo evaluamos si hay una piedra o si es un inicio de posible línea
            for (int d = 0; d < 4; ++d) {
                Threat current_threat = evaluate_line_threat(board, r, c, dr[d], dc[d], total_score, threats, forced_defense, player);
                if(current_threat.win.win){
                    threatSearch.win = current_threat.win;
                    return threatSearch;
                }
                if(current_threat.op_forced){
                    threatSearch.op_forced = true;
                }
            }
        }
    }
    threatSearch.score = total_score;
    threatSearch.threats = threats;
    threatSearch.forced_defense = forced_defense;
    return threatSearch;
}


// < ---------------------------------------------------------------------------------- >



int minimax(Board board, int depth, int alpha, int beta, bool isMaximizing, int stones_req) {
    // 1. Verificación de seguridad y salida
    if (timeout_flag.load()) return isMaximizing ? -1e8 : 1e8; // Valor neutral/malo si cortamos
    
    if (depth == 0) {
        return evaluate_fitness(board);
    }

    // 2. Generar movimientos basados en los candidatos de cercanía
    auto candidates = get_candidates(board);
    auto moves = generate_move_combinations(candidates, stones_req);

    // Si no hay movimientos posibles, evaluamos el estado actual
    if (moves.empty()) return evaluate_fitness(board);

    if (isMaximizing) {
        int maxEval = -1e9;
        for (auto& m : moves) {
            // SIMULAR (Backtracking)
            board[m.p1.x][m.p1.y] = 1; 
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 1;

            // En Connect6, tras el primer turno, siempre se piden 2 piedras
            int eval = minimax(board, depth - 1, alpha, beta, false, 2);

            // DESHACER
            board[m.p1.x][m.p1.y] = 0;
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 0;

            maxEval = std::max(maxEval, eval);
            alpha = std::max(alpha, eval);
            
            if (beta <= alpha || timeout_flag.load()) break;
        }
        return maxEval;
    } else {
        int minEval = 1e9;
        for (auto& m : moves) {
            // SIMULAR oponente
            board[m.p1.x][m.p1.y] = 2;
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 2;

            int eval = minimax(board, depth - 1, alpha, beta, true, 2);

            // DESHACER
            board[m.p1.x][m.p1.y] = 0;
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 0;

            minEval = std::min(minEval, eval);
            beta = std::min(beta, eval);

            if (beta <= alpha || timeout_flag.load()) break;
        }
        return minEval;
    }
}

MovePair solve_at_fixed_depth(Board board, int depth, int stones_req) {
    auto candidates = get_candidates(board);
    auto moves = generate_move_combinations(candidates, stones_req);
    
    MovePair best_m;
    int best_v = -1e9;

    for (auto& m : moves) {
        if (timeout_flag.load()) break;

        board[m.p1.x][m.p1.y] = 1;
        if (!m.single_stone) board[m.p2.x][m.p2.y] = 1;

        // Llamada al minimax recursivo
        int v = minimax(board, depth - 1, -1e9, 1e9, false, 2);

        board[m.p1.x][m.p1.y] = 0;
        if (!m.single_stone) board[m.p2.x][m.p2.y] = 0;

        if (v > best_v) {
            best_v = v;
            best_m = m;
        }
    }
    return best_m;
}



bool vcf_recursive(Board board, bool is_attacking, int depth){

    if(depth == 0) return false;

    if(is_attacking){

        cout << "Ataque..." << endl;
        ThreatsSearch board_threats = evaluate_threats(board, 1);
        if(board_threats.win.win){
            cout << "Victoria evaluada..." << endl;
            return true;
        }
        if(board_threats.op_forced){
            cout << "Tengo jugada forzada que no puedo controlar..." << endl;
            return false;
        }

        for(auto &t : board_threats.threats){

            printf("Ahora podemos atacar con (%d,%d) y (%d,%d)\n", t.p1.x, t.p1.y, t.p2.x, t.p2.y);

            board[t.p1.x][t.p1.y] = 1;
            board[t.p2.x][t.p2.y] = 1;

            if(vcf_recursive(board, false, depth - 1)){
                return true;
            }

            board[t.p1.x][t.p1.y] = 0;
            board[t.p2.x][t.p2.y] = 0;

        }

        return false;

    } else {

        cout << "Evaluamos defensa..." << endl;
        ThreatsSearch board_threats = evaluate_threats(board, 2);
        if(board_threats.win.win){
            cout << "Gana el rival" << endl;
            return false;
        }
        if(board_threats.forced_defense.size() == 0){
            cout << "No hay amenazas forzadas" << endl;
            return false;
        } 

        bool forced_win = true;

        for(auto &t : board_threats.forced_defense){
            board[t.p1.x][t.p1.y] = 2;
            board[t.p2.x][t.p2.y] = 2;

            printf("Nos defiende con (%d,%d) y (%d,%d)\n", t.p1.x, t.p1.y, t.p2.x, t.p2.y);

            if(!vcf_recursive(board, true, depth - 1)){
                forced_win = false;
                board[t.p1.x][t.p1.y] = 0;
                board[t.p2.x][t.p2.y] = 0;
                break;
            }

            board[t.p1.x][t.p1.y] = 0;
            board[t.p2.x][t.p2.y] = 0;

        }

        return forced_win;

    }

}

struct VCFMove{
    bool vcf_win;
    Move vcf_move;
};



VCFMove vcf_search(Board board, vector<Move> threats){

    VCFMove vcf_move;

    cout << "Busquemos una victoria forzada con VCF..." << endl;

    for(auto &t : threats){

        board[t.p1.x][t.p1.y] = 1;
        board[t.p2.x][t.p2.y] = 1;

        printf("Probando rama de (%d,%d) y (%d,%d)\n", t.p1.x, t.p1.y, t.p2.x, t.p2.y);

        if(vcf_recursive(board, false, 8)){
            vcf_move.vcf_win = true;
            vcf_move.vcf_move = {t.p1, t.p2, 0};
            cout << "Conseguimos una victoria" << endl;
            return vcf_move;
        }

        board[t.p1.x][t.p1.y] = 0;
        board[t.p2.x][t.p2.y] = 0;

    }

    vcf_move.vcf_win = false;
    vcf_move.vcf_move = {{}, {}, 0};

    return vcf_move;

}


void playGame(std::shared_ptr<Channel> channel, std::string teamName) {
    auto stub = GameServer::NewStub(channel);
    ClientContext context;

    // Abrir el stream bidireccional
    std::shared_ptr<ClientReaderWriter<PlayerAction, GameState>> stream(
        stub->Play(&context));

    // 1. Registro del equipo
    std::cout << "🔄 Registrando equipo: " << teamName << std::endl;
    PlayerAction register_action;
    register_action.set_register_team(teamName);
    stream->Write(register_action);

    GameState state;
    // 2. Bucle principal de juego (Recibir estados del servidor)
    while (stream->Read(&state)) {
        if (state.status() == connect6::GameState_Status_WAITING) {
            std::cout << "⏳ Esperando contrincante..." << std::endl;
        } 
        else if (state.status() == connect6::GameState_Status_PLAYING) {
            if (state.is_my_turn()) {
                std::cout << "🎲 Es mi turno, papaíto. Piedras requeridas: " << state.stones_required() << std::endl;

                // Lógica de minimax iterativo 
                MovePair best_action; 
                Board current_board;
                sync_board(state, current_board);

                timeout_flag.store(false);

                // --------- Threats ------------
                ThreatsSearch board_threats = evaluate_threats(current_board, 1);
                bool win = false;
                if(board_threats.win.win && board_threats.win.move.size() == 2){
                    best_action.p1 = board_threats.win.move[0];
                    best_action.p2 = board_threats.win.move[1];
                    best_action.single_stone = false;
                    win = true;
                } else if(board_threats.win.win && board_threats.win.move.size() == 1){
                    best_action.p1 = board_threats.win.move[0];
                    vector<Pos> candidates = get_candidates(current_board);
                    for (auto& p : candidates) {
                        if (p.x != best_action.p1.x || p.y != best_action.p1.y) {
                            best_action.p2 = p;
                            break;
                        }
                    }
                    best_action.single_stone = false;
                    win = true;
                }

                // ------------------------------

                PlayerAction move_action;
                auto* move = move_action.mutable_move();
                
                // Iniciamos cronómetro en un hilo aparte
                std::thread timer([&]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(9000)); // Tiempo límite
                    timeout_flag.store(true); 
                });

                if(!win && !board_threats.op_forced){

                    VCFMove vcf_move = vcf_search(current_board, board_threats.threats);
                    
                    // cout << "En este momento llamaría a VCF, SI TAN SOLO TUVIERA UNO" << endl;
                    if(vcf_move.vcf_win){
                        best_action.p1 = vcf_move.vcf_move.p1;
                        best_action.p2 = vcf_move.vcf_move.p2;
                        best_action.single_stone = false;
                        win = true;
                    }

                }

                if (!win){
                    
                    for (int d = 1; d <= 6; ++d) {
                        
                        // Llamamos a la búsqueda para esta profundidad específica
                        MovePair move_at_depth = solve_at_fixed_depth(current_board, d, state.stones_required());
                        
                        // Si durante la búsqueda se acabó el tiempo, ignoramos el resultado incompleto
                        if (timeout_flag.load()) {
                            std::cout << "Tiempo agotado. Usando mejor jugada de capa " << d-1 << std::endl;
                            break;
                        }

                        // Si terminamos la capa a tiempo, esta es nuestra nueva mejor jugada
                        best_action = move_at_depth;
                        std::cout << "Capa " << d << " analizada con éxito." << std::endl;
                    }

                    if (timer.joinable()) timer.detach();

                }

                if (timer.joinable()) timer.detach();

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
                if(state.stones_required() == 2){
                    std::cout << "✅ Movimiento enviado: (" << best_action.p1.x << "," << best_action.p1.y << ")" << " y (" << best_action.p2.x << "," << best_action.p2.y << ")";
                } else {
                    std::cout << "✅ Movimiento enviado: (" << best_action.p1.x << "," << best_action.p1.y << ")";
                }
            } else {
                std::cout << "⌛ Esperando al perdedor..." << std::endl;
            }
        } 
        else if (state.status() == connect6::GameState_Status_FINISHED) {
            std::cout << "🏁 PARTIDA FINALIZADA. Ganador: " << state.winner() << std::endl;
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
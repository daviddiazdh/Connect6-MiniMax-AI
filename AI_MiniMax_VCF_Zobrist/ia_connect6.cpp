#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <map>
#include <random>
#include <unordered_map>


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


// Representación del tablero
typedef int8_t Board[19][19];
// Posición en el tablero
struct Pos { int x, y; };
// Representa una posición en el tablero
struct MovePair {
    Pos p1, p2;
    bool single_stone = false;
};
// Representa una jugada (1 o 2 piedras dependiendo del turno)
atomic<bool> timeout_flag; 

// Prototipos de Funciones
void sync_board(const connect6::GameState& state, Board local_board);
vector<Pos> get_candidates(Board board);
vector<MovePair> generate_move_combinations(const vector<Pos>& candidates, int stones_required);
int score_by_count(int count, bool is_mine);
int evaluate_line(Board board, int r, int c, int dr, int dc);
int evaluate_fitness(Board board);
int minimax(Board board, int depth, int alpha, int beta, bool isMaximizing, int stones_req, uint64_t hash);
MovePair solve_at_fixed_depth(Board board, int depth, int stones_req);

// Tabla de hashing aleatorio por celda y estado (vacío, jugador, oponente)
uint64_t zobrist[19][19][3];
uint64_t zobrist_turn[2];


/**
 * @brief Inicializa la tabla Zobrist con valores aleatorios para hashing rápido.
 *
 * Cada celda del tablero y cada posible valor (vacío, mi piedra, piedra oponente)
 * recibe un número aleatorio de 64 bits para usar en hashing Zobrist.
 *
 * Complejidad: O(19*19*3)
 */
void init_zobrist() {
    mt19937_64 rng(123456);
    zobrist_turn[0] = rng();
    zobrist_turn[1] = rng();
    for (int r = 0; r < 19; ++r)
        for (int c = 0; c < 19; ++c)
            for (int k = 0; k < 3; ++k)
                zobrist[r][c][k] = rng();
}

/**
 * @brief Calcula el hash Zobrist de un tablero.
 *
 * Combina los valores Zobrist de cada celda usando XOR.
 *
 * Complejidad: O(19*19)
 *
 * @param board Tablero actual representado como Board[19][19].
 * @return uint64_t Valor del hash Zobrist del tablero.
 */
uint64_t compute_hash(Board board) {
    uint64_t h = 0;
    for (int r = 0; r < 19; ++r)
        for (int c = 0; c < 19; ++c)
            h ^= zobrist[r][c][board[r][c]];
    return h;
}

// Calcula hash completo del tablero (O(n²))
enum TTFlag { EXACT, LOWERBOUND, UPPERBOUND };

// Entrada almacenada en TT
struct TTEntry {
    int value;
    int depth;
    TTFlag flag;
};

unordered_map<uint64_t, TTEntry> TT;

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

/**
 * @brief Sincroniza el tablero local con el estado recibido del servidor.
 *
 * Convierte los valores de connect6::GameState a un arreglo local Board[19][19]:
 * 0 -> vacío, 1 -> mi piedra, 2 -> piedra del oponente.
 *
 * Complejidad: O(19*19)
 *
 * @param state Estado del juego recibido del servidor.
 * @param local_board Tablero local que será actualizado.
 */
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

/**
 * @brief Sincroniza el tablero local con el estado recibido del servidor.
 *
 * Convierte los valores de connect6::GameState a un arreglo local Board[19][19]:
 * 0 -> vacío, 1 -> mi piedra, 2 -> piedra del oponente.
 *
 * Complejidad: O(19*19)
 *
 * @param state Estado del juego recibido del servidor.
 * @param local_board Tablero local que será actualizado.
 */
vector<Pos> get_candidates(Board board) {
    vector<Pos> candidates;
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

/**
 * @brief Genera todas las combinaciones posibles de movimientos a partir de los candidatos.
 *
 * Si se requiere 1 piedra, genera un movimiento por candidato.  
 * Si se requieren 2 piedras, genera todas las combinaciones únicas de 2 candidatos.
 *
 * Complejidad: O(n^2) si stones_required == 2, O(n) si stones_required == 1
 *
 * @param candidates Lista de posiciones candidatas.
 * @param stones_required Número de piedras a colocar (1 o 2).
 * @return vector<MovePair> Lista de movimientos posibles.
 */
vector<MovePair> generate_move_combinations(const vector<Pos>& candidates, int stones_required) {
    vector<MovePair> combinations;
    
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



/**
 * @brief Asigna un valor heurístico según la cantidad de piedras en una ventana
 * de longitud 6, considerando si es propia, del oponente y si es "wide open".
 *
 * @param count Número de piedras consecutivas en la ventana.
 * @param is_mine Indica si las piedras son propias (true) o del oponente (false).
 * @param wide_open Indica si la ventana tiene ambos extremos abiertos.
 * @return Valor heurístico asignado a la ventana.
 */
int score_by_count(int count, bool is_mine, bool wide_open) {
    switch (count) {
        case 6: return is_mine ? 1000000 : -1000000;
        case 5: return is_mine ? 10000 : -90000;
        case 4: return is_mine ? 10000 : -90000;
        case 3: return is_mine ? 100 : -500;
        case 2: return is_mine ? 10 : -50;
        default: return 0;
    }
}

/**
 * @brief Asigna un valor heurístico según la cantidad de piedras en una ventana
 * de longitud 6, considerando si es propia, del oponente y si es "wide open".
 *
 * @param count Número de piedras consecutivas en la ventana.
 * @param is_mine Indica si las piedras son propias (true) o del oponente (false).
 * @param wide_open Indica si la ventana tiene ambos extremos abiertos.
 * @return Valor heurístico asignado a la ventana.
 */
int evaluate_line(Board board, int r, int c, int dr, int dc) {
    int my_stones = 0;
    int opponent_stones = 0;

    int automaton_state = 0;

    for (int i = 0; i < 6; ++i) {
        int nr = r + i * dr;
        int nc = c + i * dc;

        if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19) {
            int eval = board[nr][nc];
            if (board[nr][nc] == 1) my_stones++;
            else if (board[nr][nc] == 2) opponent_stones++;

            if(automaton_state == 0 && eval == 0) automaton_state = 1;
            else if(automaton_state == 0 && eval != 0) automaton_state = 2;
            else if(automaton_state == 1 && eval == 0) automaton_state = 1;
            else if(automaton_state == 1 && eval != 0) automaton_state = 3;
            else if(automaton_state == 3 && eval == 0) automaton_state = 4;
            else if(automaton_state == 3 && eval != 0) automaton_state = 3;
            else if(automaton_state == 4 && eval == 0) automaton_state = 4;
            else if(automaton_state == 4 && eval != 0) automaton_state = 2;

        } else {
            // Fuera del tablero, esta ventana de 6 no es válida
            return 0;
        }
    }

    // Si hay piedras de ambos colores, nadie puede hacer 6 en esta ventana
    if (my_stones > 0 && opponent_stones > 0) return 0;

    bool open = false;

    if(automaton_state == 4){
        open = true;
    }

    // Puntuación para mí
    if (my_stones > 0){

        if(open){
            vector<int> range = {-1, 6};
            bool wide_open = true;
            for(int &i : range){
                int nr = r + i * dr;
                int nc = c + i * dc;

                if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19) {
                    if (board[nr][nc] == 2){
                        wide_open = false;
                        break;
                    }
                } else {
                    wide_open = false;
                    break;
                }
            }
            return score_by_count(my_stones, true, wide_open);
        }

        return score_by_count(my_stones, true, false);
    } 
    // Puntuación para el oponente (negativa)
    if (opponent_stones > 0){

        if(open){
            vector<int> range = {-1, 6};
            bool wide_open = true;
            for(int &i : range){
                int nr = r + i * dr;
                int nc = c + i * dc;

                if (nr >= 0 && nr < 19 && nc >= 0 && nc < 19) {
                    if (board[nr][nc] == 2){
                        wide_open = false;
                        break;
                    }
                } else {
                    wide_open = false;
                    break;
                }
            }
            return score_by_count(opponent_stones, false, wide_open);
        }

        return score_by_count(opponent_stones, false, false);

    }

    return 0;
}

/**
 * @brief Evalúa todo el tablero calculando el puntaje heurístico total
 * sumando las evaluaciones de todas las líneas posibles.
 *
 * @param board Tablero de juego de 19x19.
 * @return Puntaje heurístico total de la posición.
 */
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


/**
 * @brief Evalúa amenazas en una línea específica y actualiza puntaje y amenazas.
 *
 * @param board Tablero de juego.
 * @param r Fila inicial.
 * @param c Columna inicial.
 * @param dr Incremento de fila.
 * @param dc Incremento de columna.
 * @param acc_score Referencia al score acumulado del tablero.
 * @param threats Vector de amenazas detectadas (se actualiza).
 * @param forced_defense Vector de defensas forzadas del oponente (se actualiza).
 * @param player Jugador a evaluar (1 = propio, 2 = oponente).
 * @return Estructura Threat con información de la línea.
 */
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
            
            acc_score += score_by_count(my_stones, true, wide_open);

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
            
            acc_score += score_by_count(my_stones, true, wide_open);

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

        }

        acc_score += score_by_count(my_stones, true, false);
        return threat;
    } 

    // Puntuación para el oponente
    if (opponent_stones > 0){

        if(opponent_stones == 4 || opponent_stones == 5){
            threat.op_forced = true;
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
            acc_score += score_by_count(opponent_stones, false, wide_open);

            if(wide_open && opponent_stones == 4){
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
            acc_score += score_by_count(opponent_stones, false, wide_open);

            if(wide_open && possible_threat_side == 1){
                forced_defense.push_back({empty_bounds[1], possible_threats.p1, 5});
            } else if(wide_open && possible_threat_side == 2){
                forced_defense.push_back({empty_bounds[0], possible_threats.p2, 5});
            }

            return threat;

        }

        acc_score += score_by_count(opponent_stones, false, false);
        return threat;
    }

    return threat;

}


/**
 * @brief Evalúa todas las amenazas del tablero para un jugador dado.
 *
 * - Detecta victorias directas.
 * - Detecta jugadas forzadas del oponente.
 * - Calcula score heurístico de la posición.
 * - Devuelve amenazas y defensas posibles.
 *
 * @param board Tablero de juego.
 * @param player Jugador a evaluar (1 = propio, 2 = oponente).
 * @return Estructura ThreatsSearch con toda la información de amenazas.
 */
ThreatsSearch evaluate_threats(Board board, int player) {

    ThreatsSearch threatSearch = {{false, {}}, false, 0, {}};

    int total_score = 0;
    vector<Move> threats = {};
    vector<Move> forced_defense = {};
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


/**
 * @brief Actualiza el hash de Zobrist para un cambio de posición en el tablero.
 *
 * @param hash Hash actual del tablero.
 * @param r Fila de la posición a cambiar.
 * @param c Columna de la posición a cambiar.
 * @param old_val Valor antiguo en la posición.
 * @param new_val Valor nuevo en la posición.
 * @return Hash actualizado.
 */
inline uint64_t update_hash(uint64_t hash, int r, int c, int old_val, int new_val) {
    hash ^= zobrist[r][c][old_val]; // quitar valor viejo
    hash ^= zobrist[r][c][new_val]; // agregar valor nuevo
    return hash;
}

/**
 * @brief Algoritmo minimax con poda alpha-beta y transposición usando hash Zobrist.
 *
 * @param board Tablero actual.
 * @param depth Profundidad restante para búsqueda.
 * @param alpha Valor alpha para poda.
 * @param beta Valor beta para poda.
 * @param isMaximizing Indica si el nodo actual es de maximización.
 * @param stones_req Número de piedras a colocar en el turno.
 * @param hash Hash Zobrist del tablero.
 * @return Valor evaluado del nodo.
 */
int minimax(Board board, int depth, int alpha, int beta, bool isMaximizing, int stones_req, uint64_t hash) {
    if (timeout_flag.load()) return isMaximizing ? -1e8 : 1e8;

    auto it = TT.find(hash);

    if (it != TT.end()) {
        TTEntry entry = it->second;

        if (entry.depth >= depth) {
            if (entry.flag == EXACT)
                return entry.value;
            else if (entry.flag == LOWERBOUND)
                alpha = max(alpha, entry.value);
            else if (entry.flag == UPPERBOUND)
                beta = min(beta, entry.value);

            if (alpha >= beta)
                return entry.value;
        }
    }

    int original_alpha = alpha;
    int original_beta = beta;


    if (depth == 0) {
        return evaluate_fitness(board);
    }

    // 2. Generar movimientos basados en los candidatos de cercanía
    auto candidates = get_candidates(board);
    auto moves = generate_move_combinations(candidates, stones_req);

    // Si no hay movimientos posibles, evaluamos el estado actual
    if (moves.empty()) return evaluate_fitness(board);

    int maxEval, minEval;

    if (isMaximizing) {
        maxEval = -1e9;
        for (auto& m : moves) {
            board[m.p1.x][m.p1.y] = 1; 
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 1;

            uint64_t new_hash = hash;
            new_hash = update_hash(new_hash, m.p1.x, m.p1.y, 0, 1);
            if (!m.single_stone)
                new_hash = update_hash(new_hash, m.p2.x, m.p2.y, 0, 1);

            new_hash ^= zobrist_turn[1];
            new_hash ^= zobrist_turn[0];

            // En Connect6, tras el primer turno, siempre se piden 2 piedras
            int eval = minimax(board, depth - 1, alpha, beta, false, 2, new_hash);

            // DESHACER
            board[m.p1.x][m.p1.y] = 0;
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 0;

            maxEval = max(maxEval, eval);
            alpha = max(alpha, eval);
            
            if (beta <= alpha || timeout_flag.load()) break;
        }
    } else {
        minEval = 1e9;
        for (auto& m : moves) {
            board[m.p1.x][m.p1.y] = 2;
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 2;

            uint64_t new_hash = hash;
            new_hash = update_hash(new_hash, m.p1.x, m.p1.y, 0, 2);
            if (!m.single_stone)
                new_hash = update_hash(new_hash, m.p2.x, m.p2.y, 0, 2);

            new_hash ^= zobrist_turn[0];
            new_hash ^= zobrist_turn[1];

            int eval = minimax(board, depth - 1, alpha, beta, true, 2, new_hash);

            board[m.p1.x][m.p1.y] = 0;
            if (!m.single_stone) board[m.p2.x][m.p2.y] = 0;

            minEval = min(minEval, eval);
            beta = min(beta, eval);

            if (beta <= alpha || timeout_flag.load()) break;
        }
    }

    int result = isMaximizing ? maxEval : minEval;

    TTEntry entry;
    entry.value = result;
    entry.depth = depth;

    if (result <= original_alpha)
        entry.flag = UPPERBOUND;
    else if (result >= original_beta)
        entry.flag = LOWERBOUND;
    else
        entry.flag = EXACT;

    TT[hash] = entry;

    return result;

}

/**
 * @brief Busca la mejor jugada considerando todas las combinaciones de movimientos
 * hasta una profundidad fija, usando minimax.
 *
 * @param board Tablero actual.
 * @param depth Profundidad de búsqueda.
 * @param stones_req Número de piedras a colocar.
 * @return Mejor par de movimientos encontrado.
 */
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
        uint64_t hash = compute_hash(board);
        hash ^= zobrist_turn[0];
        int v = minimax(board, depth - 1, -1e9, 1e9, false, 2, hash);

        board[m.p1.x][m.p1.y] = 0;
        if (!m.single_stone) board[m.p2.x][m.p2.y] = 0;

        if (v > best_v) {
            best_v = v;
            best_m = m;
        }
    }
    return best_m;
}


/**
 * @brief Busca recursivamente Victory by Continuous Four (VCF) en el tablero.
 *
 * @param board Tablero actual.
 * @param is_attacking Indica si es turno de ataque (true) o defensa (false).
 * @param depth Profundidad restante para la búsqueda VCF.
 * @return true si se encuentra victoria forzada, false de lo contrario.
 */
bool vcf_recursive(Board board, bool is_attacking, int depth){

    if(depth == 0) return false;

    if(is_attacking){
        ThreatsSearch board_threats = evaluate_threats(board, 1);
        if(board_threats.win.win){
            return true;
        }
        if(board_threats.op_forced){
            return false;
        }

        for(auto &t : board_threats.threats){

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

        ThreatsSearch board_threats = evaluate_threats(board, 2);
        if(board_threats.win.win){
            return false;
        }
        if(board_threats.forced_defense.size() == 0){
            return false;
        } 

        bool forced_win = true;

        for(auto &t : board_threats.forced_defense){
            board[t.p1.x][t.p1.y] = 2;
            board[t.p2.x][t.p2.y] = 2;

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

/**
 * @brief Resultado de búsqueda VCF.
 */
struct VCFMove{
    bool vcf_win;
    Move vcf_move;
};


/**
 * @brief Busca una Victory by Continuous Four (VCF) considerando todas las amenazas.
 *
 * @param board Tablero actual.
 * @param threats Amenazas detectadas previamente.
 * @return Estructura VCFMove indicando si se logró VCF y el movimiento correspondiente.
 */
VCFMove vcf_search(Board board, vector<Move> threats){

    VCFMove vcf_move = {false, {}};

    for(auto &t : threats){

        board[t.p1.x][t.p1.y] = 1;
        board[t.p2.x][t.p2.y] = 1;

        if(vcf_recursive(board, false, 8)){
            vcf_move.vcf_win = true;
            vcf_move.vcf_move = {t.p1, t.p2, 0};
            return vcf_move;
        }

        board[t.p1.x][t.p1.y] = 0;
        board[t.p2.x][t.p2.y] = 0;

    }

    vcf_move.vcf_win = false;
    vcf_move.vcf_move = {{}, {}, 0};

    return vcf_move;

}


/**
 * @brief Lógica principal de juego conectándose al servidor, enviando movimientos y recibiendo estados.
 *
 * @param channel Canal GRPC para la comunicación.
 * @param teamName Nombre del equipo registrado.
 */
void playGame(shared_ptr<Channel> channel, string teamName) {
    auto stub = GameServer::NewStub(channel);
    ClientContext context;

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
                cout << "🎲 Es mi turno. Piedras requeridas: " << state.stones_required() << std::endl;

                // Lógica de minimax iterativo 
                MovePair best_action; 
                Board current_board;
                sync_board(state, current_board);

                timeout_flag.store(false);

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

                PlayerAction move_action;
                auto* move = move_action.mutable_move();
                
                // Iniciamos cronómetro en un hilo aparte
                thread timer([&]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(9000));
                    timeout_flag.store(true); 
                });

                if(!win && !board_threats.op_forced){

                    VCFMove vcf_move = vcf_search(current_board, board_threats.threats);
                    
                    if(vcf_move.vcf_win){
                        best_action.p1 = vcf_move.vcf_move.p1;
                        best_action.p2 = vcf_move.vcf_move.p2;
                        best_action.single_stone = false;
                        win = true;
                    }

                }

                if (!win){
                    TT.clear();
                    for (int d = 1; d <= 6; ++d) {
                        
                        // Llamamos a la búsqueda para esta profundidad específica
                        MovePair move_at_depth = solve_at_fixed_depth(current_board, d, state.stones_required());
                        
                        // Si durante la búsqueda se acabó el tiempo, ignoramos el resultado incompleto
                        if (timeout_flag.load()) {
                            break;
                        }

                        // Si terminamos la capa a tiempo, esta es nuestra nueva mejor jugada
                        best_action = move_at_depth;
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
                    cout << "✅ Movimiento enviado: (" << best_action.p1.x << "," << best_action.p1.y << ")" << " y (" << best_action.p2.x << "," << best_action.p2.y << ")";
                } else {
                    cout << "✅ Movimiento enviado: (" << best_action.p1.x << "," << best_action.p1.y << ")";
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
    init_zobrist();  
    srand(time(NULL));

    // Lee la dirección del servidor de las variables de entorno, como en Go
    char* addr_env = getenv("SERVER_ADDR");
    string target_str = (addr_env) ? addr_env : "servidor:50051";

    char* team_env = getenv("TEAM_NAME");
    string team_name = (team_env) ? team_env : "Bot_CPP_David";

    while (true) {
        cout << "🔄 Conectando a " << target_str << " como " << team_name << "..." << endl;
        
        auto channel = grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials());
        playGame(channel, team_name);

        cout << "⏳ Reconectando en 3 segundos..." << endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    return 0;
}
package main

import (
	"context"
	"fmt"
	"log"
	"math/rand"
	"os"
	"time"

	pb "agente_test/pb"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

func playGame(addr, teamName string) {
	conn, err := grpc.Dial(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		log.Printf("Error al conectar: %v", err)
		return
	}
	defer conn.Close()

	client := pb.NewGameServerClient(conn)
	stream, err := client.Play(context.Background())
	if err != nil {
		log.Printf("Error al abrir stream: %v", err)
		return
	}

	// Registro del equipo
	log.Printf("Registrando equipo: %s", teamName)
	stream.Send(&pb.PlayerAction{
		Action: &pb.PlayerAction_RegisterTeam{RegisterTeam: teamName},
	})

	// Bucle principal de juego
	for {
		state, err := stream.Recv()
		if err != nil {
			log.Println("🔌 Conexión cerrada por el servidor")
			return
		}

		switch state.Status {
		case pb.GameState_WAITING:
			log.Println("⏳ Esperando contrincante...")

		case pb.GameState_PLAYING:
			if state.IsMyTurn {
				log.Printf("🎲 Es mi turno (%v). Piedras requeridas: %d", state.MyColor, state.StonesRequired)

				// Simular tiempo de procesamiento
				time.Sleep(500 * time.Millisecond)

				stones := []*pb.Point{}
				chosenInTurn := make(map[string]bool)

				// Buscar posiciones vacías aleatorias
				for int32(len(stones)) < state.StonesRequired {
					x := int32(rand.Intn(19))
					y := int32(rand.Intn(19))
					posKey := fmt.Sprintf("%d-%d", x, y)

					// Validar que el tablero esté vacío en esa celda Y no hayamos elegido ya esa celda en este turno
					if state.Board[x].Cells[y] == pb.PlayerColor_UNKNOWN && !chosenInTurn[posKey] {
						stones = append(stones, &pb.Point{X: x, Y: y})
						chosenInTurn[posKey] = true
					}
				}

				// Enviar movimiento
				err := stream.Send(&pb.PlayerAction{
					Action: &pb.PlayerAction_Move{
						Move: &pb.Move{Stones: stones},
					},
				})
				if err != nil {
					log.Printf("Error al enviar movimiento: %v", err)
					return
				}
				log.Printf("✅ Movimiento enviado: %v", stones)
			} else {
				log.Println("⌛ Esperando a que el oponente mueva...")
			}

		case pb.GameState_FINISHED:
			log.Printf("🏁 PARTIDA FINALIZADA")
			log.Printf("🏆 Ganador: %v", state.Winner)
			log.Printf("📝 Resultado: %v", state.Result)
			return
		}
	}
}

func main() {
	rand.Seed(time.Now().UnixNano())

	addr := os.Getenv("SERVER_ADDR")
	if addr == "" {
		addr = "servidor:50051"
	}

	teamName := os.Getenv("TEAM_NAME")
	if teamName == "" {
		teamName = "Bot_Aleatorio_Go"
	}

	// Loop de reconexión: después de cada partida, reconectar
	for {
		log.Printf("🔄 Conectando a %s como %s...", addr, teamName)
		playGame(addr, teamName)
		log.Println("⏳ Reconectando en 3 segundos...")
		time.Sleep(3 * time.Second)
	}
}
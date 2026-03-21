# Connect6 Arena - IA I

Este repositorio contiene el **Starter Kit** para el proyecto de la materia **Inteligencia Artificial I**. El objetivo es desarrollar un agente capaz de jugar al Connect6 utilizando técnicas de búsqueda y heurísticas, compitiendo en un entorno distribuido mediante **gRPC**.

## 🎮 El Juego: Connect6

Connect6 es un juego de estrategia pura de la familia k-in-a-row.

1. **Inicio**: El primer jugador coloca una piedra negra.

2. **Turnos**: A partir de ahí, cada jugador coloca dos piedras de su color por turno.

3. **Victoria**: Gana el primero que logre alinear 6 piedras (horizontal, vertical o diagonal).

## ⚠️ Reglas de Competición (Importante)

Para asegurar la fluidez y legalidad de las partidas, el servidor aplica las siguientes reglas automáticas:

- **⏱️ Límite de Tiempo:** Cada agente tiene un máximo de 10 segundos para enviar su movimiento tras recibir el estado del tablero. Si un jugador excede este tiempo, será descalificado inmediatamente y perderá la partida.

- **🚫 Movimientos Inválidos:** Se considera movimiento inválido intentar colocar una piedra en una casilla ocupada o fuera del tablero.

- **❌ Tres Strikes:** Si un agente intenta realizar 3 movimientos inválidos de forma consecutiva, será descalificado automáticamente.

## 🛠️ Requisitos del Sistema

- **Docker y Docker Compose:** (Obligatorio) Para ejecutar el servidor y la arena visual.

- **Compilador de Protocol Buffers (protoc):** Necesario para generar el código cliente en el lenguaje que elijas (Python, Java, C++, etc.).

## 📡 Comunicación (gRPC)

El contrato está en `proto/connect6.proto`. Al compilarlo (ej. en Go), se generarán dos archivos en la carpeta `/pb`:

1. **`connect6.pb.go`**: Contiene los mensajes y estructuras (Tablero, Puntos, Colores).

2. **`connect6_grpc.pb.go`**: Contiene la interfaz del cliente para conectarse al servicio `Play`.

``` bash
# Ejemplo para generar código en Go:
protoc --go_out=. --go-grpc_out=. proto/connect6.proto

# Ejemplo para generar código en Python:
python -m grpc_tools.protoc -I./proto --python_out=. --grpc_python_out=. proto/connect6.proto
```

#### Entendiendo el archivo .proto

El contrato define los siguientes puntos clave:

- `GameServer`: Maneja un método `Play` que es un stream bidireccional.

- `PlayerColor:0` = Vacío (UNKNOWN), `1` = Negras (BLACK), `2` = Blancas (WHITE).

- `GameState`: Contiene el tablero (`board`), si es tu turno (`is_my_turn`) y cuántas piedras debes enviar (`stones_required`).

### 🤖 Guía del Agente de Ejemplo (test.go)

El código proporcionado en `test.go`(disponible en la imagen `lrprll/ia-aleatoria`) es una base funcional que puedes seguir:

**1. Conexión y Registro:** El bot lee la dirección del servidor de una variable de entorno y envía su nombre de equipo mediante RegisterTeam.

**2. Escucha de Estado:** Entra en un bucle donde recibe el GameState.

**3. Lógica de Decisión:**

Espera a que `Status == PLAYING `e `IsMyTurn == true.`

Usa `stones_required `para saber si debe enviar 1 o 2 piedras (crucial para el primer turno).

Verifica que la celda esté vacía (`PlayerColor_UNKNOWN`) antes de elegirla.

**4. Envío de Movimiento:** Crea un objeto `Move` con los puntos elegidos y lo envía por el stream.

**5. Reconexión:** El main incluye un bucle infinito para que el bot se reconecte automáticamente si la partida termina o el servidor se reinicia.

### 🐳 Ejecución con Docker Compose

Para que tu IA funcione, necesitas dos archivos de configuración en tu carpeta raíz:

**1. El Dockerfile (Ejemplo para Go)**
Este archivo le dice a Docker cómo empaquetar tu código:

``` Dockerfile
FROM golang:1.24-alpine
WORKDIR /app
COPY go.mod go.sum* ./
RUN go mod download
COPY . .
CMD ["go", "run", "main.go"]
```

**2. El docker-compose.yml**
Por defecto, este archivo viene configurado con IAs aleatorias de prueba. Para usar la tuya, cambia image por *build: .* como se muestra abajo:

``` YAML
services:
  # Arena visual y servidor gRPC
  servidor:
    image: lrprll/connect6-server:latest
    ports:
      - "8080:8080"

  # Oponente de práctica (IA Aleatoria)
  ia-random:
    image: lrprll/ia-aleatoria:latest
    environment:
      - SERVER_ADDR=servidor:50051
      - TEAM_NAME=IA_Aleatoria

  # Tu agente local
  mi-ia:
    build: . 
    environment:
      - SERVER_ADDR=servidor:50051
      - TEAM_NAME=Nombre_De_Tu_Equipo
```

**Nota técnica:** El nombre del servicio `servidor` en el YAML define el nombre de host. Si lo cambias, debes actualizar `SERVER_ADDR` en tu código.

### 🚀 Comandos Rápidos

Levantar todo: docker compose up --build

Ver el tablero: `http://localhost:8080`

Logs de tu IA: docker logs -f mi-ia-1

### 📩 Contacto y Soporte

Si encuentras errores en el servidor o tienes dudas sobre el protocolo gRPC, puedes contactarnos:

📧 <17-10778@usb.ve> - Laura Parilli

📧 <19-10274@usb.ve> - Miguel Salomón


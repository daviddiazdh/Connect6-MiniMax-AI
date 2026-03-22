# Usamos una imagen de Debian estable que tiene buenos repositorios de gRPC
FROM debian:12

# 1. Instalar compilador y librerías de desarrollo de gRPC y Protobuf
RUN apt-get update && apt-get install -y \
    build-essential \
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# 2. Establecer directorio de trabajo
WORKDIR /app

# 3. Copiar el código y la carpeta de protos
# Esto asume que estás parado en agente_cpp/ y ahí tienes ia_connect6.cpp y pb/
COPY ia_connect6.cpp .
COPY pb/ ./pb/

# 4. Compilar el binario
# Enlazamos con los archivos generados que están dentro de pb/
RUN g++ -std=c++17 ia_connect6.cpp \
    pb/connect6.pb.cc \
    pb/connect6.grpc.pb.cc \
    -o ia_connect6 \
    `pkg-config --cflags --libs protobuf grpc++`

# 5. Ejecutar la IA
CMD ["./ia_connect6"]
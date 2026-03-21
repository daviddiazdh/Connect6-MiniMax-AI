FROM golang:1.24-alpine
WORKDIR /app
RUN apk add --no-cache git
COPY go.mod ./
COPY go.mod go.sum* ./
RUN go mod download
COPY . .

CMD ["go", "run", "bot/test.go"]
// ESP_ChaCha — базовый тестовый клиент

// Тестирует: ping, getKey, relay on/off, неизвестная команда, приём телеметрии.

package main

import (
	"bufio"
	"crypto/rand"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"time"

	"golang.org/x/crypto/chacha20poly1305"
)

// ВВОД КЛЮЧА И АДРЕСА

func promptLine(label string) string {
	fmt.Print(label)
	sc := bufio.NewScanner(os.Stdin)
	sc.Scan()
	return strings.TrimSpace(sc.Text())
}

func parseKey(s string) ([]byte, error) {
	// Принимает формат "0x01,0x02,..." или "0102..."
	s = strings.ReplaceAll(s, " ", "")
	s = strings.ReplaceAll(s, "0x", "")
	s = strings.ReplaceAll(s, "0X", "")
	s = strings.ReplaceAll(s, ",", "")

	if len(s) != 64 {
		return nil, fmt.Errorf("ключ должен быть 32 байта (64 hex символа), получено %d символов", len(s))
	}

	key := make([]byte, 32)
	for i := 0; i < 32; i++ {
		var b byte
		_, err := fmt.Sscanf(s[i*2:i*2+2], "%02x", &b)
		if err != nil {
			return nil, fmt.Errorf("неверный hex на позиции %d: %w", i, err)
		}
		key[i] = b
	}
	return key, nil
}

// ПРОТОКОЛ ESP_ChaCha

func sendPacket(conn net.Conn, aead interface {
	Seal(dst, nonce, plaintext, additionalData []byte) []byte
	NonceSize() int
}, plaintext []byte) error {
	nonce := make([]byte, aead.NonceSize())
	if _, err := io.ReadFull(rand.Reader, nonce); err != nil {
		return fmt.Errorf("nonce: %w", err)
	}

	cipherWithTag := aead.Seal(nil, nonce, plaintext, nil)
	payload := append(nonce, cipherWithTag...)

	header := make([]byte, 2)
	binary.LittleEndian.PutUint16(header, uint16(len(payload)))

	if _, err := conn.Write(header); err != nil {
		return fmt.Errorf("write header: %w", err)
	}
	if _, err := conn.Write(payload); err != nil {
		return fmt.Errorf("write payload: %w", err)
	}
	return nil
}

func recvPacket(conn net.Conn, aead interface {
	Open(dst, nonce, ciphertext, additionalData []byte) ([]byte, error)
	NonceSize() int
	Overhead() int
}) ([]byte, error) {
	header := make([]byte, 2)
	if _, err := io.ReadFull(conn, header); err != nil {
		return nil, fmt.Errorf("read header: %w", err)
	}
	payloadLen := binary.LittleEndian.Uint16(header)

	minLen := uint16(aead.NonceSize() + aead.Overhead() + 1)
	if payloadLen < minLen || payloadLen > 4096 {
		return nil, fmt.Errorf("неверная длина пакета: %d", payloadLen)
	}

	payload := make([]byte, payloadLen)
	if _, err := io.ReadFull(conn, payload); err != nil {
		return nil, fmt.Errorf("read payload: %w", err)
	}

	nonce := payload[:aead.NonceSize()]
	cipherWithTag := payload[aead.NonceSize():]

	plain, err := aead.Open(nil, nonce, cipherWithTag, nil)
	if err != nil {
		return nil, fmt.Errorf("decrypt: %w", err)
	}
	return plain, nil
}

func prettyJSON(data []byte) string {
	var v any
	if err := json.Unmarshal(data, &v); err != nil {
		return string(data)
	}
	b, err := json.MarshalIndent(v, "  ", "  ")
	if err != nil {
		return string(data)
	}
	return string(b)
}

// ─── Main ─────────────────────────────────────────────────────────────────────

func main() {
	fmt.Println("╔══════════════════════════════════════╗")
	fmt.Println("║   ESP_ChaCha — базовый тест          ║")
	fmt.Println("╚══════════════════════════════════════╝")
	fmt.Println()

	address := promptLine("Введите IP:PORT (например 192.168.1.100:8888): ")
	if address == "" {
		fmt.Println("Адрес не введён, выход.")
		waitEnter()
		return
	}

	keyStr := promptLine("Введите ключ (формат 0x73,0x33,... или hex строка): ")
	key, err := parseKey(keyStr)
	if err != nil {
		fmt.Println("Ошибка ключа:", err)
		waitEnter()
		return
	}

	aead, err := chacha20poly1305.New(key)
	if err != nil {
		fmt.Println("Ошибка инициализации шифра:", err)
		waitEnter()
		return
	}

	fmt.Printf("\nПодключение к %s...\n", address)
	conn, err := net.DialTimeout("tcp", address, 5*time.Second)
	if err != nil {
		fmt.Println("Ошибка подключения:", err)
		waitEnter()
		return
	}
	defer conn.Close()
	fmt.Println("Подключено!")

	// ГОРУТИНА ПРИЁМА
	done := make(chan struct{})
	go func() {
		defer close(done)
		for {
			conn.SetReadDeadline(time.Now().Add(15 * time.Second))
			plain, err := recvPacket(conn, aead)
			if err != nil {
				fmt.Println("\nСоединение:", err)
				return
			}
			fmt.Printf("ESP → \n  %s\n\n", prettyJSON(plain))
		}
	}()

	send := func(msg string) {
		fmt.Printf("Отправляем: %s\n", msg)
		if err := sendPacket(conn, aead, []byte(msg)); err != nil {
			fmt.Println("Ошибка отправки:", err)
		}
		time.Sleep(600 * time.Millisecond)
	}

	// ТЕСТЫ
	fmt.Println("─── Тест 1: ping ───────────────────────")
	send(`{"cmd":"ping"}`)

	fmt.Println("─── Тест 2: запрос ключа ───────────────")
	send(`{"cmd":"getKey"}`)

	fmt.Println("─── Тест 3: реле ON ────────────────────")
	send(`{"cmd":"relay","state":true}`)

	fmt.Println("─── Тест 4: реле OFF ───────────────────")
	send(`{"cmd":"relay","state":false}`)

	fmt.Println("─── Тест 5: неизвестная команда ────────")
	send(`{"cmd":"unknown_test"}`)

	fmt.Println("─── Слушаем телеметрию 10 сек ──────────")
	select {
	case <-done:
		fmt.Println("Соединение закрыто ESP.")
	case <-time.After(10 * time.Second):
		fmt.Println("Таймаут ожидания.")
	}

	fmt.Println("\nТест завершён!")
	waitEnter()
}

func waitEnter() {
	fmt.Print("\nНажмите Enter для выхода...")
	bufio.NewReader(os.Stdin).ReadString('\n')
}

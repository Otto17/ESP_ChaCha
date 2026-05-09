// ESP_ChaCha — тест клиент для SensorServer.
//
// Подключается к SensorServer, получает данные от эмитированных датчиков в реальном времени.
// Команды: getSensors, setInterval, ping, getStats, resetStats.

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
	"strconv"
	"strings"
	"time"

	"golang.org/x/crypto/chacha20poly1305"
)

func promptLine(label string) string {
	fmt.Print(label)
	sc := bufio.NewScanner(os.Stdin)
	sc.Scan()
	return strings.TrimSpace(sc.Text())
}

func parseKey(s string) ([]byte, error) {
	s = strings.ReplaceAll(s, " ", "")
	s = strings.ReplaceAll(s, "0x", "")
	s = strings.ReplaceAll(s, "0X", "")
	s = strings.ReplaceAll(s, ",", "")
	if len(s) != 64 {
		return nil, fmt.Errorf("нужно 64 hex символа, получено %d", len(s))
	}
	key := make([]byte, 32)
	for i := 0; i < 32; i++ {
		var b byte
		fmt.Sscanf(s[i*2:i*2+2], "%02x", &b)
		key[i] = b
	}
	return key, nil
}

func sendPacket(conn net.Conn, aead interface {
	Seal(dst, nonce, plaintext, additionalData []byte) []byte
	NonceSize() int
}, plaintext []byte) error {
	nonce := make([]byte, aead.NonceSize())
	io.ReadFull(rand.Reader, nonce)
	payload := append(nonce, aead.Seal(nil, nonce, plaintext, nil)...)
	hdr := make([]byte, 2)
	binary.LittleEndian.PutUint16(hdr, uint16(len(payload)))
	conn.Write(hdr)
	conn.Write(payload)
	return nil
}

func recvPacket(conn net.Conn, aead interface {
	Open(dst, nonce, ciphertext, additionalData []byte) ([]byte, error)
	NonceSize() int
	Overhead() int
}) ([]byte, error) {
	hdr := make([]byte, 2)
	if _, err := io.ReadFull(conn, hdr); err != nil {
		return nil, err
	}
	plen := binary.LittleEndian.Uint16(hdr)
	if plen < uint16(aead.NonceSize()+aead.Overhead()+1) || plen > 4096 {
		return nil, fmt.Errorf("плохая длина %d", plen)
	}
	payload := make([]byte, plen)
	if _, err := io.ReadFull(conn, payload); err != nil {
		return nil, err
	}
	return aead.Open(nil, payload[:aead.NonceSize()], payload[aead.NonceSize():], nil)
}

func main() {
	fmt.Println("╔══════════════════════════════════════╗")
	fmt.Println("║   ESP_ChaCha — клиент датчиков       ║")
	fmt.Println("╚══════════════════════════════════════╝")
	fmt.Println()

	address := promptLine("IP:PORT (например 192.168.1.100:8888): ")
	keyStr := promptLine("Введите ключ (HEX): ")

	key, err := parseKey(keyStr)
	if err != nil {
		fmt.Println("Ошибка ключа: ", err)
		waitEnter()
		return
	}

	aead, _ := chacha20poly1305.New(key)

	fmt.Printf("Подключение к %s...\n", address)
	conn, err := net.DialTimeout("tcp", address, 5*time.Second)
	if err != nil {
		fmt.Println("Ошибка: ", err)
		waitEnter()
		return
	}
	defer conn.Close()
	fmt.Println("Подключено!")
	fmt.Println()
	fmt.Println("Команды: [1] getSensors  [2] setInterval  [3] ping")
	fmt.Println("         [4] getStats    [5] resetStats   [q] выход")
	fmt.Println()

	// ГОРУТИНА ПРИЁМА
	recvCh := make(chan []byte, 16)
	go func() {
		for {
			conn.SetReadDeadline(time.Now().Add(30 * time.Second))
			plain, err := recvPacket(conn, aead)
			if err != nil {
				fmt.Println("\n📡 Разрыв: ", err)
				close(recvCh)
				return
			}
			recvCh <- plain
		}
	}()

	// ГОРУТИНА ВВОДА
	go func() {
		for plain := range recvCh {
			var m map[string]any
			if json.Unmarshal(plain, &m) != nil {
				fmt.Printf("RAW: %s\n", plain)
				continue
			}

			typ, _ := m["type"].(string)
			switch typ {
			case "sensors":
				fmt.Printf("Temp: %.1f°C  Hum: %.1f%%  Pres: %.1fhPa  Vcc: %.2fV  "+
					"Heap: %v  Uptime: %vs\n",
					toFloat(m["temp"]), toFloat(m["hum"]),
					toFloat(m["pres"]), toFloat(m["vcc"]),
					m["heap"], m["uptime"])
			case "stats":
				fmt.Printf("Stats: rx=%v ok=%v badTag=%v tx=%v\n",
					m["rx"], m["ok"], m["badTag"], m["tx"])
			default:
				b, _ := json.MarshalIndent(m, "  ", "  ")
				fmt.Printf("ESP → \n  %s\n\n", string(b))
			}
		}
	}()

	// ИНТЕРАКТИВНЫЙ ВВОД
	scanner := bufio.NewScanner(os.Stdin)
	for {
		fmt.Print("> ")
		if !scanner.Scan() {
			break
		}
		line := strings.TrimSpace(scanner.Text())

		switch line {
		case "1":
			sendPacket(conn, aead, []byte(`{"cmd":"getSensors"}`))
		case "2":
			ms := promptLine("Интервал в мс (100-60000): ")
			n, _ := strconv.Atoi(ms)
			if n < 100 {
				n = 100
			}
			msg := fmt.Sprintf(`{"cmd":"setInterval","ms":%d}`, n)
			sendPacket(conn, aead, []byte(msg))
		case "3":
			sendPacket(conn, aead, []byte(`{"cmd":"ping"}`))
		case "4":
			sendPacket(conn, aead, []byte(`{"cmd":"getStats"}`))
		case "5":
			sendPacket(conn, aead, []byte(`{"cmd":"resetStats"}`))
		case "q", "Q", "exit", "quit":
			fmt.Println("Выход.")
			waitEnter()
			return
		default:
			fmt.Println("Неизвестная команда. Введите 1-5 или q.")
		}
		time.Sleep(100 * time.Millisecond)
	}

	waitEnter()
}

func toFloat(v any) float64 {
	switch x := v.(type) {
	case float64:
		return x
	case float32:
		return float64(x)
	default:
		return 0
	}
}

func waitEnter() {
	fmt.Print("\nНажмите Enter для выхода...")
	bufio.NewReader(os.Stdin).ReadString('\n')
}

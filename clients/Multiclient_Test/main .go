// ESP_ChaCha — тест нескольких клиентов
//
// Запускает N параллельных подключений к MultiClient примеру
// Каждый клиент отправляет команды и получает broadcast от других

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
	"sync"
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
		return nil, fmt.Errorf("нужно 64 hex символа")
	}
	key := make([]byte, 32)
	for i := 0; i < 32; i++ {
		fmt.Sscanf(s[i*2:i*2+2], "%02x", &key[i])
	}
	return key, nil
}

// КЛИЕНТ
type Client struct {
	id   int
	conn net.Conn
	aead interface {
		Seal(dst, nonce, plaintext, additionalData []byte) []byte
		Open(dst, nonce, ciphertext, additionalData []byte) ([]byte, error)
		NonceSize() int
		Overhead() int
	}
	mu      sync.Mutex
	rxCount int
}

func (c *Client) send(msg string) error {
	nonce := make([]byte, c.aead.NonceSize())
	io.ReadFull(rand.Reader, nonce)
	payload := append(nonce, c.aead.Seal(nil, nonce, []byte(msg), nil)...)
	hdr := make([]byte, 2)
	binary.LittleEndian.PutUint16(hdr, uint16(len(payload)))
	c.conn.Write(hdr)
	c.conn.Write(payload)
	return nil
}

func (c *Client) recv() ([]byte, error) {
	hdr := make([]byte, 2)
	if _, err := io.ReadFull(c.conn, hdr); err != nil {
		return nil, err
	}
	plen := binary.LittleEndian.Uint16(hdr)
	if plen < uint16(c.aead.NonceSize()+c.aead.Overhead()+1) || plen > 4096 {
		return nil, fmt.Errorf("плохая длина %d", plen)
	}
	payload := make([]byte, plen)
	if _, err := io.ReadFull(c.conn, payload); err != nil {
		return nil, err
	}
	return c.aead.Open(nil, payload[:c.aead.NonceSize()], payload[c.aead.NonceSize():], nil)
}

func (c *Client) run(address string, wg *sync.WaitGroup, stop <-chan struct{}) {
	defer wg.Done()

	conn, err := net.DialTimeout("tcp", address, 5*time.Second)
	if err != nil {
		fmt.Printf("[Клиент %d] Ошибка подключения: %v\n", c.id, err)
		return
	}
	c.conn = conn
	defer conn.Close()

	fmt.Printf("[Клиент %d] Подключён\n", c.id)

	// Горутина приёма
	recvDone := make(chan struct{})
	go func() {
		defer close(recvDone)
		for {
			conn.SetReadDeadline(time.Now().Add(20 * time.Second))
			plain, err := c.recv()
			if err != nil {
				fmt.Printf("[Клиент %d] Разрыв: %v\n", c.id, err)
				return
			}
			c.mu.Lock()
			c.rxCount++
			c.mu.Unlock()

			var m map[string]any
			if json.Unmarshal(plain, &m) == nil {
				evt, _ := m["event"].(string)
				typ, _ := m["type"].(string)
				switch {
				case evt == "welcome":
					fmt.Printf("[Клиент %d] Добро пожаловать! Слот=%v\n", c.id, m["slot"])
				case evt == "peer_joined":
					fmt.Printf("[Клиент %d] Присоединился слот %v (всего: %v)\n",
						c.id, m["slot"], m["totalClients"])
				case evt == "peer_left":
					fmt.Printf("[Клиент %d] Отключился слот %v\n", c.id, m["slot"])
				case evt == "broadcast":
					fmt.Printf("[Клиент %d] Broadcast от %v: %v\n",
						c.id, m["from"], m["text"])
				case typ == "private":
					fmt.Printf("[Клиент %d] Личное от %v: %v\n",
						c.id, m["from"], m["text"])
				case typ == "clientList":
					fmt.Printf("[Клиент %d] Список клиентов: %v слотов активно\n",
						c.id, m["count"])
				default:
					b, _ := json.Marshal(m)
					fmt.Printf("[Клиент %d] %s\n", c.id, string(b))
				}
			}
		}
	}()

	// Пауза чтобы welcome успел прийти
	time.Sleep(500 * time.Millisecond)

	// Отправляем ping
	c.send(`{"cmd":"ping"}`)
	time.Sleep(300 * time.Millisecond)

	// Запрашиваем список клиентов
	c.send(`{"cmd":"who"}`)
	time.Sleep(300 * time.Millisecond)

	// Рассылаем сообщение от этого клиента
	msg := fmt.Sprintf(`{"cmd":"broadcast","text":"Привет от клиента %d!"}`, c.id)
	c.send(msg)
	time.Sleep(300 * time.Millisecond)

	// Ждём сигнала остановки
	select {
	case <-stop:
	case <-recvDone:
	}

	c.mu.Lock()
	rx := c.rxCount
	c.mu.Unlock()
	fmt.Printf("[Клиент %d] 🏁 Завершён. Получено пакетов: %d\n", c.id, rx)
}

func main() {
	fmt.Println("╔══════════════════════════════════════╗")
	fmt.Println("║   ESP_ChaCha — тест нескольких       ║")
	fmt.Println("║   клиентов одновременно              ║")
	fmt.Println("╚══════════════════════════════════════╝")
	fmt.Println()

	address := promptLine("IP:PORT (например 192.168.1.100:8888): ")
	keyStr := promptLine("Введите ключ (HEX): ")
	countStr := promptLine("Количество клиентов (1-4): ")

	key, err := parseKey(keyStr)
	if err != nil {
		fmt.Println("Ошибка ключа:", err)
		waitEnter()
		return
	}

	count := 2
	fmt.Sscanf(countStr, "%d", &count)
	if count < 1 {
		count = 1
	}
	if count > 4 {
		count = 4
	}

	fmt.Printf("\nЗапускаем %d клиентов...\n\n", count)

	stop := make(chan struct{})
	var wg sync.WaitGroup

	for i := 1; i <= count; i++ {
		aead, _ := chacha20poly1305.New(key)
		c := &Client{id: i, aead: aead}
		wg.Add(1)

		// Небольшая задержка между подключениями
		time.Sleep(300 * time.Millisecond)
		go c.run(address, &wg, stop)
	}

	// Даём клиентам поработать 15 секунд
	fmt.Printf("\nТест идёт 15 секунд...\n")
	time.Sleep(15 * time.Second)

	fmt.Println("\nОстанавливаем клиентов...")
	close(stop)
	wg.Wait()

	fmt.Println("\nВсе клиенты завершились.")
	waitEnter()
}

func waitEnter() {
	fmt.Print("\nНажмите Enter для выхода...")
	bufio.NewReader(os.Stdin).ReadString('\n')
}

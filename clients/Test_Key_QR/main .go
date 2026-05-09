// Test_Key_QR —  тест для проверки корректности декодирования ключа ChaCha20 из QR-кода.

// Принимает 64-символьную HEX-строку (формат ESP_ChaCha), декодирует в 32 байта
// и выводит результат в нескольких форматах для визуальной сверки.

package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"os"
	"strings"
)

func main() {
	reader := bufio.NewReader(os.Stdin)

	fmt.Println("=== ESP ChaCha — проверка ключа из QR-кода ===")
	fmt.Println()
	fmt.Print("Введите HEX-строку из QR-кода: ")

	input, err := reader.ReadString('\n')
	if err != nil {
		fmt.Println("Ошибка чтения:", err)
		return
	}

	// Убираем пробелы и переносы строк
	hexStr := strings.TrimSpace(input)

	if len(hexStr) != 64 {
		fmt.Printf("Ошибка: ожидается 64 символа, получено %d\n", len(hexStr))
		fmt.Print("\nНажмите Enter для завершения...")
		reader.ReadString('\n')
		return
	}

	// Декодируем HEX в байты
	keyBytes, err := hex.DecodeString(hexStr)
	if err != nil {
		fmt.Println("Ошибка декодирования HEX:", err)
		fmt.Print("\nНажмите Enter для завершения...")
		reader.ReadString('\n')
		return
	}

	fmt.Println()
	fmt.Println("=== Результат ===")
	fmt.Println()

	// Выводим в формате как в ESP_ChaCha (0xNN,0xNN,...)
	fmt.Print("Ключ (Arduino): ")
	for i, b := range keyBytes {
		if i > 0 {
			fmt.Print(",")
		}
		fmt.Printf("0x%02X", b)
	}
	fmt.Println()
	fmt.Println()

	// Выводим байты построчно по 8 штук — удобно для сверки
	fmt.Println("Ключ (по байтам):")
	for i, b := range keyBytes {
		fmt.Printf("  [%02d] 0x%02X  (%3d)\n", i, b, b)
	}
	fmt.Println()

	// Проверочный вывод — тот же HEX обратно
	fmt.Print("HEX обратно:    ")
	fmt.Println(strings.ToUpper(hex.EncodeToString(keyBytes)))
	fmt.Println()

	if strings.EqualFold(hexStr, hex.EncodeToString(keyBytes)) {
		fmt.Println("✓ Ключ декодирован корректно")
	} else {
		fmt.Println("✗ Ошибка: HEX не совпадает после декодирования!")
	}

	fmt.Println()
	fmt.Print("Нажмите Enter для завершения...")
	reader.ReadString('\n')
}

# Leadshine EM3E-556 Quick Start Guide

## Быстрый запуск

### 1. Подключение к EtherCAT сети

**Windows:**
```cmd
dummy-ecat-cli.exe -i "\Device\NPF_{E0FF3CC3-015D-401E-9F41-6C525F9D4DB9}"
```

**Linux:**
```bash
sudo ./dummy-ecat-cli -i eth0
```

### 2. Инициализация

```
dummy_says> scan
Found 1 slaves:
  Slave 1: EM3E (0x0000001B:0x00000556)

dummy_says> pdo-start
Starting PDO exchange...
✓ All slaves reached OP state
```

### 3. Управление двигателем

#### Включить драйвер
```
dummy_says> motor-enable 1
Enabling drive (slave 1)...
  State: Operation Enabled (0x0637)
✓ Drive enabled successfully
```

#### Запустить на 10 секунд
```
dummy_says> motor-run 1 100 10
=== Running motor for 10 seconds at 100 RPM ===
Motor running.......... Done!
```

#### Изменить скорость
```
dummy_says> motor-velocity 1 200
Target velocity set to: 200 RPM

dummy_says> motor-velocity 1 -150
Target velocity set to: -150 RPM  # Реверс
```

#### Остановить двигатель
```
dummy_says> motor-stop 1
EMERGENCY STOP activated
```

#### Проверить статус
```
dummy_says> motor-status 1
=== EM3E-556 Status (Slave 1) ===
State:            Operation Enabled
Status Word:      0x1637
Actual Position:  12000 counts
Actual Velocity:  0 RPM
```

#### Выключить драйвер
```
dummy_says> motor-disable 1
Drive disabled
```

## Доступные команды

| Команда | Описание | Пример |
|---------|----------|--------|
| `motor-enable <idx>` | Включить драйвер | `motor-enable 1` |
| `motor-disable <idx>` | Выключить драйвер | `motor-disable 1` |
| `motor-run <idx> <rpm> <sec>` | Запустить на N секунд | `motor-run 1 100 10` |
| `motor-velocity <idx> <rpm>` | Установить скорость | `motor-velocity 1 200` |
| `motor-stop <idx>` | Аварийный останов | `motor-stop 1` |
| `motor-status <idx>` | Показать статус | `motor-status 1` |

## Примеры сценариев

### Сценарий 1: Простой тест
```
scan
pdo-start
motor-enable 1
motor-run 1 100 5
motor-disable 1
exit
```

### Сценарий 2: Изменение скорости на ходу
```
scan
pdo-start
motor-enable 1
motor-velocity 1 50
# Ждем 3 секунды
motor-velocity 1 200
# Ждем 3 секунды
motor-velocity 1 -100  # Реверс
motor-stop 1
motor-disable 1
exit
```

### Сценарий 3: Проверка статуса
```
scan
pdo-start
motor-enable 1
motor-velocity 1 150
motor-status 1
# Мотор продолжает вращаться
motor-stop 1
motor-status 1
motor-disable 1
exit
```

## Параметры

### Скорость
- Единицы: **RPM** (обороты в минуту)
- Положительное значение = вращение вперед
- Отрицательное значение = вращение назад
- Диапазон: обычно ±3000 RPM (зависит от мотора)

### Slave Index
- Индекс устройства в EtherCAT сети
- Начинается с 1
- Узнать индекс: команда `scan`

## Состояния драйвера (CiA 402)

| Состояние | Описание |
|-----------|----------|
| Not Ready | Не готов |
| Switch On Disabled | Выключен |
| Ready to Switch On | Готов к включению |
| Switched On | Включен |
| Operation Enabled | Работа разрешена (рабочее состояние) |
| Fault | Ошибка |

## PDO Mapping

### TX PDO (к драйверу)
```
Offset  Size  Object    Description
0       2     0x6040    Control Word
2       4     0x607A    Target Position
6       4     0x60FF    Target Velocity
Total: 10 bytes
```

### RX PDO (от драйвера)
```
Offset  Size  Object    Description
0       2     0x6041    Status Word
2       4     0x6064    Position Actual Value
6       4     0x606C    Velocity Actual Value
Total: 10 bytes
```

## Устранение неполадок

### Ошибка: "PDO not active"
**Решение:** Запустите PDO обмен
```
pdo-start
```

### Ошибка: Drive in FAULT state
**Решение:** Команда `motor-enable` автоматически сбрасывает ошибку
```
motor-enable 1
```
Если не помогает - проверьте:
- Питание драйвера
- Подключение мотора
- Правильность параметров мотора

### Мотор не вращается
1. Проверьте статус: `motor-status 1`
2. Убедитесь что state = "Operation Enabled"
3. Проверьте что PDO активен: `status`
4. Проверьте физическое подключение мотора
5. Проверьте напряжение питания драйвера

### Invalid slave index
**Решение:** Проверьте индекс устройства
```
scan  # Покажет список всех устройств
```

## Безопасность

⚠️ **ВАЖНО:**
- Всегда вызывайте `motor-disable` перед выходом
- Используйте `motor-stop` для аварийной остановки
- Проверяйте механические ограничения движения
- Начинайте с малых скоростей (50-100 RPM)
- Убедитесь что мотор закреплен надежно

## Технические детали

### Режим работы
По умолчанию используется **Profile Velocity Mode (PV)** - режим управления скоростью.

### Control Word биты
- Bit 0: Switch On
- Bit 1: Enable Voltage  
- Bit 2: Quick Stop
- Bit 3: Enable Operation
- Bit 7: Fault Reset
- Bit 8: Halt

### Status Word биты
- Bit 0: Ready to Switch On
- Bit 1: Switched On
- Bit 2: Operation Enabled
- Bit 3: Fault
- Bit 10: Target Reached

## Дополнительная информация

См. также:
- `README.md` - общее описание проекта
- Leadshine EM3E-556 User Manual
- CiA 402 Drive Profile Specification

Для получения списка всех команд в CLI:
```
dummy_says> help
```

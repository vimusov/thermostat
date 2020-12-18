#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <LiquidCrystal_I2C.h>

// Примерное время дребезга контактов энкодера, мс.
#define ENCODER_JITTER (5)
// Таймаут на ожидание следующего события от энкодера, мс.
#define ENCODER_TIMEOUT (350)

// Параметры длительности сигналов азбуки Морзе, мс. >:3
#define DOT_LEN (500)
#define DASH_LEN (3 * DOT_LEN)
#define SIGN_DELAY (DOT_LEN)
#define LETTER_DELAY (3 * DOT_LEN)
#define REPEAT_DELAY (7 * DOT_LEN)

// Макрос для удобства записи часов.
#define HOURS(value) (value * 3600UL)

// Пин термодатчика.
#define SENSOR_PIN (2)
// Пин "пищалки".
#define BEEPER_PIN (11)
// Пин твердотельного реле управления нагревателем.
#define HEATER_PIN (12)
// Пин сигналов от энкодера.
#define ENCODER_PIN (A0)

// Действие, произведённое энкодером.
enum EncoderAction
{
    NoAction, // Бездействие.
    ActionNext, // Вращение в одну сторону.
    ActionPrev, // Вращение в другую сторону.
    ActionConfirm, // Нажатие кнопки.
};

// Стадия (состояние) сушки.
enum HeatingStage
{
    Idle, // Выключено (бездействие).
    PreHeating, // Прогрев.
    Working, // Стабилизация температуры.
};

// Описание настроек пластика.
typedef struct
{
    const char *const name; // Название.
    const uint8_t temp; // Температура сушки.
    const unsigned long time_sec; // Время сушки, с.
} Filament;

// Таблица с настройками для разных видов пластика.
const Filament filaments[] = {
    {
        .name = "PLA",
        .temp = 45,
        .time_sec = HOURS(6),
    },
    {
        .name = "ABS",
        .temp = 60,
        .time_sec = HOURS(4),
    },
    {
        .name = "PETG",
        .temp = 65,
        .time_sec = HOURS(4),
    },
    {
        .name = "TPU",
        .temp = 50,
        .time_sec = HOURS(8),
    },
    {
        .name = "Nylon",
        .temp = 70,
        .time_sec = HOURS(12),
    },
};

// Минимальный индекс таблицы с настройками пластиков.
#define MIN_IDX (0)
// Максимальный индекс таблицы с настройками пластиков.
#define MAX_IDX ((sizeof(filaments) / sizeof(filaments[0])) - 1)

// Настройка шины 1-wire и термодатчика DS18B20.
OneWire ow_bus(SENSOR_PIN);
DallasTemperature sensor(&ow_bus);
// Настройка LCD-дисплея 1602.
LiquidCrystal_I2C screen(0x27, 16, 2);

// Выбранный пластик.
volatile const Filament *filament = NULL;
// Флаг, показывающий что пора обновить значения на дисплее.
volatile bool refresh_screen = false;
// Счётчик секунд, прошедших с момента запуска текущей стадии.
volatile unsigned long seconds = 0;
// Флаг, показывающий включен сейчас нагрев или выключен.
// На дисплее отображается буквой 'H'.
volatile bool heater_is_on = false;
// Флаг, показывающий что есть событие от энкодера.
volatile bool event_on_encoder = false;
// Текущая стадия сушки.
volatile HeatingStage heating_stage = Idle;

// Обработчик прерывания от таймера. Срабатывает 1 раз в секунду.
ISR(TIMER1_COMPA_vect)
{
    seconds++;
    refresh_screen = true;
}

// Обработчик прерывания с пина энкодера.
// Срабатывает по изменению напряжения
// в любую сторону (уменьшение/увеличение).
ISR(PCINT1_vect)
{
    event_on_encoder = true;
}

// Сброс таймера.
void reset_timer(void)
{
    // На время сброса запрещаем прерывания
    // чтобы значение счётчика не изменилось.
    noInterrupts();
    seconds = 0;
    refresh_screen = false;
    interrupts();
}

// Включение нагрева.
void turn_on(void)
{
    digitalWrite(HEATER_PIN, HIGH);
    heater_is_on = true;
}

// Выключение нагрева.
void turn_off(void)
{
    digitalWrite(HEATER_PIN, LOW);
    heater_is_on = false;
}

// Пищание "пищалкой".
void beep(uint16_t duration)
{
    digitalWrite(BEEPER_PIN, HIGH);
    delay(duration);
    digitalWrite(BEEPER_PIN, LOW);
}

// Полностью очистить дисплей.
void clear_screen(void)
{
    screen.clear();
    screen.home();
}

// Обработчик ошибок.
// Аргументом получает сообщение об ошибке.
// Играет "пищалкой" сигнал 'S.O.S' азбукой Морзе.
void panic(const char *const reason)
{
    turn_off();

    clear_screen();

    screen.setCursor(0, 0);
    screen.print("PANIC! Reason:");
    screen.setCursor(0, 1);
    screen.print(reason);

    for (;;) {
        // 'S': ...
        beep(DOT_LEN);
        delay(SIGN_DELAY);
        beep(DOT_LEN);
        delay(SIGN_DELAY);
        beep(DOT_LEN);
        delay(LETTER_DELAY);
        // 'O': ---
        beep(DASH_LEN);
        delay(SIGN_DELAY);
        beep(DASH_LEN);
        delay(SIGN_DELAY);
        beep(DASH_LEN);
        delay(LETTER_DELAY);
        // 'S': ...
        beep(DOT_LEN);
        delay(SIGN_DELAY);
        beep(DOT_LEN);
        delay(SIGN_DELAY);
        beep(DOT_LEN);
        delay(LETTER_DELAY);

        delay(REPEAT_DELAY);
    }
}

// Получение текущей температуры с термодатчика.
uint8_t query_sensor(void)
{
    sensor.requestTemperatures();
    const float value = sensor.getTempCByIndex(0);

    if (value == DEVICE_DISCONNECTED_C)
        panic("Temp NaN.");

    const uint8_t temp = ((unsigned int) value) & 0xFF;
    if (temp <= 1)
        panic("Frozen.");
    if (temp >= 120)
        panic("Burned.");

    return temp;
}

// Показывает на дисплее температуру и время сушки
// выбранного пластика.
void present_filament(void)
{
    screen.setCursor(0, 0);
    screen.print(filament->name);
    screen.print(" ?   ");

    screen.setCursor(0, 1);
    screen.print(filament->time_sec / 3600);
    screen.print(" hours at ");
    screen.print(filament->temp);
    screen.print("*      ");
}

// Чтение действия энкодера.
// Выполняется до тех пор, пока не определит действие,
// игнорируя случайные срабатывания.
// Значения настраиваются эмпирическим путём.
// Текущие значения указаны для номиналов резисторов согласно схеме,
// при точности резисторов 1%.
EncoderAction read_action(void)
{
    int value = 0;
    for (;;) {
        value = analogRead(ENCODER_PIN);
        if (value == 0)
            return NoAction;
        if (value > 840 && value < 850)
            return ActionPrev;
        if (value > 690 && value < 705)
            return ActionNext;
        if (value > 560 && value < 610)
            return ActionConfirm;
    }
}

// Дожидается любого действия от энкодера и возвращает его.
EncoderAction wait_for_action(void)
{
    // Ждём пока на энкодере произойдёт какое-либо действие.
    while (!event_on_encoder)
        delay(1);

    const EncoderAction action = read_action();

    // Если энкодер бездействует, то сразу же выходим.
    if (action == NoAction)
        return NoAction;

    unsigned long time_diff = 0;
    unsigned long time_begin = millis();

    /*
    Алгоритм обработки вращения энкодера и подавления дребезга контактов.
    Основан на механике работы энкодера. При вращении в любую сторону сначала
    замыкается один контакт, затем пока он замкнут замыкается другой контакт.
    За счёт того, что в схеме реализован делитель напряжения на резисторах,
    два этих замыкания контактов дают разное значение напряжения. И мы здесь
    получаем два события: сначала о том, что замкнулся один контакт, затем
    что замкнулся второй.
    При вращении в одну сторону (action == ActionPrev) ожидаем прихода
    следующего действие (ActionNext). Однако здесь есть гонка! Если из-за
    дребезга контактов или тормозов в АЦП раньше фронта сигнала со второго
    контакта напряжение упало до нуля, то придёт действие NoAction. Поэтому
    ограничиваем ожидание таймаутом.
    При вращении в другую сторону всё точно также, только порядок замыкания
    контактов меняется местами.
  */

    if (action == ActionPrev) {
        for (;;) {
            if (read_action() == ActionNext)
                break;
            delay(1);
            if (millis() - time_begin > ENCODER_TIMEOUT)
                break;
        }
    }

    if (action == ActionNext) {
        for (;;) {
            if (read_action() == ActionPrev)
                break;
            delay(1);
            if (millis() - time_begin > ENCODER_TIMEOUT)
                break;
        }
    }

    /*
    Когда при вращении оба контакта отработали, напряжение возвращается
    в ноль (действие NoAction). Дожидаемся этого. Если нажимали кнопку,
    то ждём пока её отпустят.
  */
    while (read_action() != NoAction)
        delay(1);

    /*
    Если нажимали кнопку, тогда спим в пределах погрешности энкодера
    (приблизительного времени, в течение которого контакты дребезжат).
    Если энкодер крутили, тогда спим в два раза большее время, чем заняла
    длительность импульса, начавшегося с замыкания одного контакта и
    закончившаяся с размыканием любого из контактов. Это с достаточно
    высокой вероятностью гарантирует, что контакты отработали и даёт
    защиту от ложных срабатываний при слишком быстром вращении энкодера.
  */
    if (action == ActionConfirm)
        time_diff = 0;
    else
        time_diff = millis() - time_begin;
    delay(time_diff * 2 + ENCODER_JITTER);

    // Сбрасываем флаг, показывающий что от энкодера приходили события.
    // Это нужно для подавления дребезга контактов, событий могло прийти
    // множество, но одно мы уже обработали. Остальные будут обработаны
    // позже.
    noInterrupts();
    event_on_encoder = false;
    interrupts();

    return action;
}

// Цикл отображения меню выбора пластика.
void choose_filament(void)
{
    clear_screen();

    size_t cur_idx = MIN_IDX;
    filament = &(filaments[cur_idx]);
    present_filament();

    for (;;) {
        EncoderAction action = wait_for_action();
        if (action == ActionConfirm)
            return;
        if (action == ActionNext) {
            // Если добрались до конца таблицы, переходим в её начало.
            if (cur_idx == MAX_IDX)
                cur_idx = 0;
            else
                cur_idx++;
        }
        if (action == ActionPrev) {
            // Если добрались до начала таблицы, переходим в её конец.
            if (cur_idx == MIN_IDX)
                cur_idx = MAX_IDX;
            else
                cur_idx--;
        }
        filament = &(filaments[cur_idx]);
        present_filament();
    }
}

// Включаем/выключаем нагреватель и переключаем стадию сушки.
void set_heater_state(const uint8_t temp)
{
    if (filament == NULL)
        panic("Heater state.");

    if (temp > filament->temp) {
        turn_off();
        // Если сушилка была в состоянии прогрева, значит с первого выключения
        // нагревателя включается основной рабочий режим просушки. Сбрасываем
        // счётчик времени, чтобы начать обратный отсчёт.
        // Если сушилка была в состоянии бездействия, но температура уже выше
        // нужной, значит плаcтик начали сушить не дождавшись пока она остынет.
        // Тоже переключаемся в основной режим.
        if (heating_stage == Idle || heating_stage == PreHeating) {
            heating_stage = Working;
            reset_timer();
        }
    } else {
        turn_on();
        if (heating_stage == Idle) {
            // Если сушилка бездействовала, значит с первого включения нагревателя
            // начинаем прогрев. Сбрасываем счётчик времени, чтобы показать, сколько
            // уже идёт прогрев.
            heating_stage = PreHeating;
            reset_timer();
        }
    }
}

// Обновление данных на дисплее.
void update_screen(const uint8_t temp)
{
    screen.setCursor(0, 0);
    screen.print(filament->name);
    screen.print(" ");
    screen.print(filament->temp);
    screen.print(" / ");
    screen.print(temp);
    screen.print("* ");
    // Если нагреватель включен, рисуем в конце первой строки букву 'H'.
    if (heater_is_on)
        screen.print("H");
    // Добавляем в конец несколько пробелов чтобы гарантированно корректно
    // отрисовать всю строку и в ней не осталось "призраков" от предыдущих
    // символов, если прежняя строка была короче по длине.
    screen.print("      ");

    screen.setCursor(0, 1);

    unsigned long time_val = 0;

    // Если сушилка находится в стадии сушки, отображаем
    // сколько времени осталось до окончания.
    if (heating_stage == Working) {
        screen.print("ETA ");
        time_val = filament->time_sec - seconds;
        const uint8_t hours = (time_val / 3600) & 0xFF;
        if (hours < 10)
            screen.print("0");
        screen.print(hours);
        screen.print(":");
    } else {
        // Если сушилка находится в состоянии прогрева, тогда
        // показываем, сколько времени прошло с момента его
        // начала.
        screen.print("Preheating ");
        time_val = seconds;
        // Если в течение часа так и не удалось прогреть сушилку
        // до заданной температуры, значит что-то точно идёт не так.
        if (time_val >= 3600)
            panic("Preheating.");
    }

    const uint8_t mins = ((time_val % 3600) / 60) & 0xFF;
    if (mins < 10)
        screen.print("0");
    screen.print(mins);
    screen.print(":");

    const uint8_t secs = (time_val % 60) & 0xFF;
    if (secs < 10)
        screen.print("0");
    screen.print(secs);

    screen.print("      ");
}

void setup()
{
    // Настраиваем пины на выход.
    pinMode(BEEPER_PIN, OUTPUT);
    pinMode(HEATER_PIN, OUTPUT);

    // Сразу же выключаем нагреватель.
    turn_off();

    // Настраиваем экран, выводим приветствие и пищим.
    screen.init();
    screen.backlight();
    clear_screen();
    screen.print("Hello world!");
    beep(250);

    // Настраиваем термодатчик.
    sensor.begin();

    // Настраиваем обработчик прерывания от таймера.
    // Подробнее см.: https://habr.com/ru/post/453276/
    noInterrupts();
    TCCR1A = 0;
    TCCR1B = 0;
    OCR1A = 15624;
    TCCR1B |= (1 << WGM12);
    TCCR1B |= (1 << CS10);
    TCCR1B |= (1 << CS12);
    TIMSK1 |= (1 << OCIE1A);
    interrupts();

    // Настраиваем прерывания от пина, куда подключен энкодер.
    // Подробнее см.:
    // https://tsibrov.blogspot.com/2019/06/arduino-interrupts-part2.html
    PCICR |= (1 << PCIE1);
    PCMSK1 |= (1 << PC0);
}

void loop()
{
    uint8_t temp = 0;

    // Если пластик ещё не выбран, показываем меню выбора.
    // Затем запускаем прогрев.
    if (filament == NULL) {
        turn_off();
        choose_filament();
        clear_screen();
        reset_timer();
        heating_stage = Idle;
        refresh_screen = true;
    }

    // Если идёт сушка и время подошло к концу, показываем сообщение,
    // пищим, ожидаем нажатия на кнопку энкодера и снова показываем
    // меню выбора пластика.
    if (heating_stage == Working && seconds > filament->time_sec) {
        turn_off();

        clear_screen();
        screen.setCursor(0, 0);
        screen.print("Finished!");

        beep(2000);
        delay(1000);
        beep(2000);
        delay(1000);
        beep(2000);

        screen.setCursor(0, 1);
        screen.print("Press any key...");

        while (wait_for_action() != ActionConfirm)
            ;

        filament = NULL;
        return;
    }

    temp = query_sensor();
    set_heater_state(temp);

    if (refresh_screen) {
        refresh_screen = false;
        update_screen(temp);
    }
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include "ipc.h"

// Подключаем системные библиотеки для сна и потоков под Windows или Linux
#ifdef _WIN32
    #include <windows.h>
    #define SLEEP_MS(ms) Sleep(ms)
#else
    #include <unistd.h>
    #include <pthread.h>
    #define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#define LOG_FILE "counter.log"

static FILE* log_fp = NULL;
static volatile sig_atomic_t running = 1; // Флаг работы программы (общий для потоков)

// Обработчик нажатия Ctrl+C
void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

// Получение текущего времени с миллисекундами (Пункты 1, 4, 5)
void get_timestamp(char* buf, size_t size) {
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st); // Windows API сразу даёт год, месяц, день, время и миллисекунды
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    struct tm tm_info;
    clock_gettime(CLOCK_REALTIME, &ts); // Получаем секунды и наносекунды
    localtime_r(&ts.tv_sec, &tm_info);  // Переводим секунды в привычную дату и время
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, ts.tv_nsec / 1000000);
#endif
}

// Безопасная запись в лог-файл с блокировкой через семафор
void log_write(const char* fmt, ...) {
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    ipc_lock(); // Блокируем файл, чтобы другие процессы не вклинились в середину строки
    fprintf(log_fp, "[%s] ", timestamp);
    va_list args;
    va_start(args, fmt);
    vfprintf(log_fp, fmt, args);
    va_end(args);
    fflush(log_fp); // Сразу сбрасываем буфер на диск, чтобы текст появился в файле мгновенно
    ipc_unlock();   // Снимаем блокировку
}

// Отдельный поток для чтения команд пользователя с клавиатуры (Пункт 3)
#ifdef _WIN32
DWORD WINAPI input_thread(LPVOID arg) {
#else
void* input_thread(void* arg) {
#endif
    (void)arg;
    char line[128];
    // fgets ждёт ввода строки от пользователя
    while (running && fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0'; // Удаляем символ переноса строки в конце

        if (strncmp(line, "set ", 4) == 0) {
            int64_t val = atoll(line + 4); // Переводим текст после "set " в число
            ipc_lock();
            ipc_get_data()->counter = val; // Меняем общий счётчик
            ipc_unlock();
            printf("Counter set to %lld\n", (long long)val);
        } else if (strcmp(line, "quit") == 0) {
            running = 0; // Останавливаем главный цикл
            break;
        }
    }
    return 0;
}

// Логика работы дочерних копий (Пункт 5a и 5b)
void run_child(const char* type, int pid) {
    shared_data_t* data = ipc_get_data();

    if (strcmp(type, "copy1") == 0) {
        // Пункт 5a: увеличить на 10 и выйти
        ipc_lock();
        data->counter += 10;
        ipc_unlock();
    } else if (strcmp(type, "copy2") == 0) {
        // Пункт 5b: увеличить в 2 раза, подождать 2 сек, уменьшить в 2 раза
        ipc_lock();
        data->counter *= 2;
        ipc_unlock();

        SLEEP_MS(2000);

        ipc_lock();
        data->counter /= 2;
        ipc_unlock();
    }

    // Записываем время выхода и завершаемся
    log_write("PID=%d (%s) exiting\n", pid, type);
}

int main(int argc, char* argv[]) {
    // Если передан аргумент (например, "copy1"), значит это дочерняя копия
    int is_child = (argc > 1);

    if (ipc_init() != 0) return 1;

    // Пункт 1: открываем лог-файл на дозапись ("a" - append)
    log_fp = fopen(LOG_FILE, "a");
    if (!log_fp) {
        ipc_cleanup();
        return 1;
    }

    int pid = get_pid();
    log_write("PID=%d started\n", pid);

    // Если это копия — выполняем её задачу и сразу завершаем программу
    if (is_child) {
        run_child(argv[1], pid);
        fclose(log_fp);
        ipc_cleanup();
        return 0;
    }

    // Настраиваем реакцию на Ctrl+C
    signal(SIGINT, signal_handler);

    // Запускаем фоновый поток для ввода команд (set / quit)
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 0, input_thread, NULL, 0, NULL));
#else
    pthread_t th;
    pthread_create(&th, NULL, input_thread, NULL);
    pthread_detach(th); // Отсоединяем поток, чтобы ОС сама очистила его память при выходе
#endif

    int is_master = ipc_try_become_master();
    printf("PID=%d started (%s). Commands: set <num>, quit\n",
           pid, is_master ? "MASTER" : "WORKER");

    shared_data_t* data = ipc_get_data();
    int elapsed_ms = 0; // Счётчик прошедшего времени в миллисекундах

    // Главный бесконечный цикл с шагом 100 мс
    while (running) {
        SLEEP_MS(100);
        elapsed_ms += 100;

        // Пункт 6: если мы были Worker, проверяем, не закрылся ли старый Master
        if (!is_master && ipc_try_become_master()) {
            is_master = 1;
            printf("PID=%d became MASTER\n", pid);
        }

        // Пункт 2: каждые 300 мс увеличиваем счётчик на 1 (делают ВСЕ процессы)
        if (elapsed_ms % 300 == 0) {
            ipc_lock();
            data->counter++;
            ipc_unlock();
        }

        // Пункт 4: каждые 1000 мс (1 сек) пишем в лог (ТОЛЬКО Master)
        if (is_master && elapsed_ms % 1000 == 0) {
            ipc_lock();
            int64_t val = data->counter;
            ipc_unlock();
            log_write("PID=%d counter=%lld\n", pid, (long long)val);
        }

        // Пункт 5: каждые 3000 мс (3 сек) запускаем 2 копии (ТОЛЬКО Master)
        if (is_master && elapsed_ms % 3000 == 0) {
            // Пункт 5c: проверяем, не работает ли ещё предыдущая копия
            if (is_child_running(0)) {
                log_write("PID=%d copy1 still running, skipping\n", pid);
            } else {
                spawn_child(0, "copy1");
            }

            if (is_child_running(1)) {
                log_write("PID=%d copy2 still running, skipping\n", pid);
            } else {
                spawn_child(1, "copy2");
            }
            elapsed_ms = 0; // Сбрасываем счётчик каждые 3 секунды, чтобы не переполнился
        }
    }

    printf("Exiting PID=%d...\n", pid);
    fclose(log_fp);
    ipc_cleanup();
    return 0;
}
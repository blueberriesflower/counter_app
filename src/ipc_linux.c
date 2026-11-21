#include "ipc.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <signal.h>

#define SHM_NAME "/counter_shm"
#define SEM_NAME "/counter_sem"
#define LOCK_FILE "/tmp/counter_master.lock"

static int shm_fd = -1;
static sem_t* sem = NULL;
static shared_data_t* data = NULL;
static int master_fd = -1;
static int is_master = 0;
static pid_t children[2] = {0, 0};

int ipc_init(void) {
    // Создаём или открываем именованный семафор с начальным значением 1 (свободен)
    sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) return -1;

    // Создаём или открываем объект разделяемой памяти в /dev/shm
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) return -1;

    // Задаём размер общей памяти равным размеру нашей структуры (8 байт для int64_t)
    ftruncate(shm_fd, sizeof(shared_data_t));

    // Отображаем общую память в адресное пространство нашего процесса
    data = mmap(NULL, sizeof(shared_data_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (data == MAP_FAILED) return -1;

    // Открываем файл, через который будем определять главного (Master)
    master_fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0666);
    return 0;
}

int ipc_try_become_master(void) {
    if (is_master) return 1;
    // LOCK_EX - эксклюзивная блокировка, LOCK_NB - не ждать, если уже занято
    if (master_fd >= 0 && flock(master_fd, LOCK_EX | LOCK_NB) == 0) {
        is_master = 1;
        return 1;
    }
    return 0;
}

shared_data_t* ipc_get_data(void) { return data; }
void ipc_lock(void) { sem_wait(sem); }   // Уменьшаем семафор с 1 до 0 (заняли)
void ipc_unlock(void) { sem_post(sem); } // Увеличиваем семафор с 0 до 1 (освободили)

void ipc_cleanup(void) {
    munmap(data, sizeof(shared_data_t));
    close(shm_fd);
    sem_close(sem);
    if (is_master) {
        // Только Master удаляет системные объекты при завершении
        shm_unlink(SHM_NAME);
        sem_unlink(SEM_NAME);
        flock(master_fd, LOCK_UN);
    }
    if (master_fd >= 0) close(master_fd);
}

int spawn_child(int idx, const char* arg) {
    // fork() создаёт точную копию текущего процесса
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        // Код внутри дочернего процесса:
        signal(SIGINT, SIG_IGN); // Игнорируем Ctrl+C
        char path[512];
        // Узнаём путь к нашему исполняемому файлу через системную ссылку /proc/self/exe
        ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (len > 0) {
            path[len] = '\0';
            // Заменяем код процесса на запуск самого себя с аргументом ("copy1" или "copy2")
            execl(path, path, arg, NULL);
        }
        _exit(1);
    }
    // Код внутри родителя (Master): запоминаем PID ребёнка
    children[idx] = pid;
    return 0;
}

int is_child_running(int idx) {
    if (children[idx] <= 0) return 0;
    // waitpid с флагом WNOHANG проверяет статус ребёнка без остановки родителя
    // Если вернуло 0 — ребёнок ещё работает. Если > 0 — он завершился
    if (waitpid(children[idx], NULL, WNOHANG) == 0) return 1;
    children[idx] = 0;
    return 0;
}

int get_pid(void) { return (int)getpid(); }

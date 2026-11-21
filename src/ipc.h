#ifndef IPC_H
#define IPC_H

#include <stdint.h>

// Структура данных, которая лежит в общей (разделяемой) памяти
typedef struct {
    int64_t counter; // Общий для всех процессов счётчик
} shared_data_t;

int ipc_init(void);                      // Подключиться к общей памяти и семафору
void ipc_cleanup(void);                  // Освободить ресурсы при выходе
shared_data_t* ipc_get_data(void);       // Получить указатель на общий счётчик
void ipc_lock(void);                     // Заблокировать доступ (взять семафор)
void ipc_unlock(void);                   // Разблокировать доступ (отдать семафор)
int ipc_try_become_master(void);         // Попытаться стать главным (1 - да, 0 - нет)
int spawn_child(int idx, const char* arg); // Запустить копию (idx: 0 или 1)
int is_child_running(int idx);           // Проверить, работает ли ещё копия
int get_pid(void);                       // Узнать свой номер процесса (PID)

#endif

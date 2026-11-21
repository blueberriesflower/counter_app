#include "ipc.h"
#include <stdio.h>
#include <windows.h>

#define SHM_NAME "Global\\CounterSHM"
#define SEM_NAME "Global\\CounterSem"
#define MASTER_NAME "Global\\CounterMaster"

static HANDLE hMap = NULL;
static HANDLE hSem = NULL;
static HANDLE hMaster = NULL;
static shared_data_t* data = NULL;
static int is_master = 0;
static HANDLE children[2] = {NULL, NULL};

int ipc_init(void) {
    // Создаём семафор с начальным и максимальным значением 1
    hSem = CreateSemaphoreA(NULL, 1, 1, SEM_NAME);
    if (!hSem) return -1;

    // Создаём файл подкачки в оперативной памяти (разделяемую память)
    hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(shared_data_t), SHM_NAME);
    if (!hMap) return -1;

    // Получаем указатель на эту память
    data = (shared_data_t*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shared_data_t));
    if (!data) return -1;

    // Создаём именованный мьютекс для определения роли Master
    hMaster = CreateMutexA(NULL, FALSE, MASTER_NAME);
    if (!hMaster) return -1;

    return 0;
}

int ipc_try_become_master(void) {
    if (is_master) return 1;
    // Пытаемся захватить мьютекс с ожиданием 0 мс
    DWORD res = WaitForSingleObject(hMaster, 0);
    if (res == WAIT_OBJECT_0 || res == WAIT_ABANDONED) {
        is_master = 1;
        return 1;
    }
    return 0;
}

shared_data_t* ipc_get_data(void) { return data; }
void ipc_lock(void) { WaitForSingleObject(hSem, INFINITE); }
void ipc_unlock(void) { ReleaseSemaphore(hSem, 1, NULL); }

void ipc_cleanup(void) {
    for (int i = 0; i < 2; i++) {
        if (children[i]) CloseHandle(children[i]);
    }
    if (is_master && hMaster) ReleaseMutex(hMaster);
    if (data) UnmapViewOfFile(data);
    if (hMap) CloseHandle(hMap);
    if (hSem) CloseHandle(hSem);
    if (hMaster) CloseHandle(hMaster);
}

int spawn_child(int idx, const char* arg) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH); // Получаем путь к текущему .exe файлу

    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof(cmd), "\"%s\" %s", path, arg);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    // CREATE_NO_WINDOW запускает копию в фоне, чтобы она не мешала консоли родителя
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return -1;
    }

    CloseHandle(pi.hThread); // Поток нам не нужен, закрываем его дескриптор
    children[idx] = pi.hProcess; // Дескриптор процесса сохраняем, чтобы проверять статус
    return 0;
}

int is_child_running(int idx) {
    if (!children[idx]) return 0;
    // Проверяем состояние процесса с ожиданием 0 мс
    if (WaitForSingleObject(children[idx], 0) == WAIT_TIMEOUT) return 1;
    CloseHandle(children[idx]); // Если завершился — закрываем дескриптор
    children[idx] = NULL;
    return 0;
}

int get_pid(void) { return (int)GetCurrentProcessId(); }

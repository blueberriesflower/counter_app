cmake_minimum_required(VERSION 3.10)
project(CounterApp LANGUAGES C)

set(CMAKE_C_STANDARD 11)

# Выбираем исходники в зависимости от платформы
if(WIN32)
    set(IPC_SRC src/ipc_win.c)
else()
    set(IPC_SRC src/ipc_linux.c)
endif()

add_executable(counter_app src/main.c ${IPC_SRC})

# Линкуем библиотеки для лин
if(NOT WIN32)
    target_link_libraries(counter_app PRIVATE pthread rt)
endif()
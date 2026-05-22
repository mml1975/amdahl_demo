/*
 * Программа: parallel_search_v2.c
 * Стандарт: C89/C90 (ANSI C)
 * Платформа: Linux (POSIX threads, mmap)
 * Назначение: Подсчет вхождений подстроки в бинарном файле с использованием
 *             параллелизации на уровне алгоритма поиска (интерливинг позиций).
 *             Демонстрация закона Амдала через конкуренцию за ресурсы.
 *
 * Отличия от версии 1:
 *   - Файл не делится на чанки
 *   - Все потоки работают со всеми позициями (чередованием)
 *   - Больше конкуренции за кэш и память
 *   - Лучше демонстрирует ограничения закона Амдала
 *
 * Компиляция:
 *   gcc -std=c89 -pedantic -Wall -O2 parallel_search_v2.c -o parallel_search_v2 -lpthread
 *
 * Использование:
 *   ./parallel_search_v2 <num_threads> <filename> <substring>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

/* Глобальные переменные для синхронизации */
static pthread_mutex_t count_mutex;
static long total_matches = 0;

/* Структура аргументов для потока */
typedef struct {
    const char *data;
    size_t file_size;
    size_t thread_id;
    size_t num_threads;
    const char *pattern;
    size_t pattern_len;
} ThreadArgs;

/*
 * Проверка совпадения паттерна с позиции pos
 * Возвращает 1 если совпадение, 0 если нет
 */
static int check_match(const char *data, size_t pos, size_t file_size,
                       const char *pattern, size_t pattern_len)
{
    size_t i;
    
    /* Проверка выхода за границы */
    if (pos + pattern_len > file_size) {
        return 0;
    }
    
    /* Посимвольное сравнение */
    for (i = 0; i < pattern_len; i++) {
        if (data[pos + i] != pattern[i]) {
            return 0;
        }
    }
    
    return 1;
}

/*
 * Функция потока - проверяет позиции с интерливингом
 * Поток с ID=i проверяет позиции: i, i+N, i+2N, i+3N, ...
 */
static void *worker_thread(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    long local_count = 0;
    size_t pos;
    
    /* 
     * Интерливинг: каждый поток проверяет свои позиции
     * Поток 0: 0, N, 2N, 3N...
     * Поток 1: 1, N+1, 2N+1, 3N+1...
     * и т.д.
     */
    for (pos = args->thread_id; 
         pos + args->pattern_len <= args->file_size; 
         pos += args->num_threads) {
        
        if (check_match(args->data, pos, args->file_size,
                        args->pattern, args->pattern_len)) {
            local_count++;
        }
    }
    
    /* Критическая секция: обновление глобального счетчика */
    /* Это создает точку сериализации - закон Амдала */
    pthread_mutex_lock(&count_mutex);
    total_matches += local_count;
    pthread_mutex_unlock(&count_mutex);
    
    return NULL;
}

/*
 * Последовательная функция инициализации (демонстрация последовательной части)
 * Выполняет некоторые вычисления перед запуском потоков
 */
static void sequential_init(const char *data, size_t file_size, 
                           const char *pattern, size_t pattern_len)
{
    size_t i;
    volatile size_t dummy = 0;
    
    /* Искусственная последовательная работа для демонстрации закона Амдала */
    /* В реальности это может быть подготовка структур данных, валидация и т.д. */
    for (i = 0; i < pattern_len && i < 1000; i++) {
        dummy += (size_t)pattern[i];
        dummy += (size_t)data[i % file_size];
    }
    
    /* Предотвращаем оптимизацию компилятором */
    if (dummy == 0) {
        printf("Init dummy check\n");
    }
}

/*
 * Последовательная функция финализации (демонстрация последовательной части)
 * Выполняет некоторые вычисления после работы потоков
 */
static void sequential_finalize(long total)
{
    volatile long dummy = 0;
    size_t i;
    
    /* Искусственная последовательная работа */
    for (i = 0; i < 10000; i++) {
        dummy += total;
        dummy ^= (i * 7);
    }
    
    /* Предотвращаем оптимизацию компилятором */
    if (dummy == 0) {
        printf("Finalize dummy check\n");
    }
}

int main(int argc, char *argv[])
{
    int num_threads;
    const char *filename;
    const char *pattern;
    size_t pattern_len;
    int fd;
    struct stat sb;
    char *file_data;
    size_t file_size;
    pthread_t *threads;
    ThreadArgs *thread_args;
    int i;
    int ret;

    /* Проверка аргументов командной строки */
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <num_threads> <filename> <substring>\n", argv[0]);
        return 1;
    }

    num_threads = atoi(argv[1]);
    if (num_threads <= 0) {
        fprintf(stderr, "Error: Number of threads must be positive.\n");
        return 1;
    }

    filename = argv[2];
    pattern = argv[3];
    pattern_len = strlen(pattern);

    if (pattern_len == 0) {
        fprintf(stderr, "Error: Substring cannot be empty.\n");
        return 1;
    }

    /* Открытие файла */
    fd = open(filename, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    /* Получение размера файла */
    if (fstat(fd, &sb) == -1) {
        perror("fstat");
        close(fd);
        return 1;
    }

    file_size = (size_t)sb.st_size;

    /* Отображение файла в память */
    if (file_size > 0) {
        file_data = (char *)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (file_data == MAP_FAILED) {
            perror("mmap");
            close(fd);
            return 1;
        }
    } else {
        file_data = NULL;
    }

    close(fd); /* Дескриптор больше не нужен после mmap */

    /* Инициализация мьютекса */
    if (pthread_mutex_init(&count_mutex, NULL) != 0) {
        fprintf(stderr, "Error: Failed to initialize mutex.\n");
        if (file_data != NULL) munmap(file_data, file_size);
        return 1;
    }

    /* Выделение памяти для потоков и аргументов */
    threads = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)num_threads);
    thread_args = (ThreadArgs *)malloc(sizeof(ThreadArgs) * (size_t)num_threads);

    if (threads == NULL || thread_args == NULL) {
        fprintf(stderr, "Error: Memory allocation failed.\n");
        pthread_mutex_destroy(&count_mutex);
        if (file_data != NULL) munmap(file_data, file_size);
        return 1;
    }

    /* Инициализация счетчика */
    total_matches = 0;

    /* Если файл пустой или меньше паттерна, результат 0 */
    if (file_size == 0 || file_size < pattern_len) {
        total_matches = 0;
    } else {
        /* === ПОСЛЕДОВАТЕЛЬНАЯ ЧАСТЬ: Инициализация === */
        sequential_init(file_data, file_size, pattern, pattern_len);
        
        /* Подготовка аргументов для потоков */
        for (i = 0; i < num_threads; i++) {
            thread_args[i].data = file_data;
            thread_args[i].file_size = file_size;
            thread_args[i].thread_id = (size_t)i;
            thread_args[i].num_threads = (size_t)num_threads;
            thread_args[i].pattern = pattern;
            thread_args[i].pattern_len = pattern_len;
        }

        /* Создание потоков */
        for (i = 0; i < num_threads; i++) {
            ret = pthread_create(&threads[i], NULL, worker_thread, 
                                (void *)&thread_args[i]);
            if (ret != 0) {
                fprintf(stderr, "Error: Failed to create thread %d.\n", i);
                break;
            }
        }

        /* Ожидание завершения потоков */
        for (i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
        }
        
        /* === ПОСЛЕДОВАТЕЛЬНАЯ ЧАСТЬ: Финализация === */
        sequential_finalize(total_matches);
    }

    /* Вывод результата */
    printf("Occurrences: %ld\n", total_matches);

    /* Очистка ресурсов */
    pthread_mutex_destroy(&count_mutex);
    if (file_data != NULL) {
        munmap(file_data, file_size);
    }
    free(threads);
    free(thread_args);

    return 0;
}
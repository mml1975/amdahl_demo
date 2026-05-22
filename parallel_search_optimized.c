/*
 * Программа: parallel_search_optimized.c
 * Стандарт: C89/C90 с POSIX-расширениями
 * Платформа: Linux (POSIX threads, mmap)
 * Назначение: Оптимизированная версия parallel_search.c
 *
 * Компиляция:
 *   gcc -std=c89 -pedantic -Wall -O3 parallel_search_optimized.c -o parallel_search_optimized -lpthread
 *
 * Использование:
 *   ./parallel_search_optimized <num_threads> <filename> <substring>
 */

/* ВАЖНО: Определить макрос ДО всех include для доступа к POSIX-функциям */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

/* Размер кэш-линии для выравнивания (обычно 64 байта) */
#define CACHE_LINE_SIZE 64

/* Структура аргументов для потока */
typedef struct {
    const char *data;
    size_t file_size;
    size_t start_offset;
    size_t end_offset;
    size_t search_start;
    size_t search_end;
    const char *pattern;
    size_t pattern_len;
    unsigned char first_byte;
    /* Локальный счетчик - без необходимости в мьютексе */
    long local_count;
} ThreadArgs;

/*
 * Оптимизированная проверка совпадения
 * Сначала проверяем первый байт (быстрый отсев)
 */
static int fast_check_match(const char *data, size_t pos, size_t max_pos,
                            const char *pattern, size_t pattern_len,
                            unsigned char first_byte)
{
    /* Быстрая проверка первого байта */
    if ((unsigned char)data[pos] != first_byte) {
        return 0;
    }
    
    /* Проверка выхода за границы */
    if (pos + pattern_len > max_pos) {
        return 0;
    }
    
    /* Сравнение остальных байтов */
    if (pattern_len > 1) {
        if (memcmp(data + pos + 1, pattern + 1, pattern_len - 1) != 0) {
            return 0;
        }
    }
    
    return 1;
}

/*
 * Функция потока с локальным счетчиком
 */
static void *worker_thread(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    size_t pos;
    size_t max_pos = args->search_end;
    
    /* Инициализация локального счетчика */
    args->local_count = 0;
    
    /* Поиск с оптимизацией */
    for (pos = args->search_start; 
         pos + args->pattern_len <= max_pos; 
         pos++) {
        
        if (fast_check_match(args->data, pos, args->file_size,
                             args->pattern, args->pattern_len,
                             args->first_byte)) {
            
            /* Проверяем, что начало вхождения в логическом диапазоне */
            if (pos >= args->start_offset) {
                args->local_count++;
            }
        }
    }
    
    return NULL;
}

int main(int argc, char *argv[])
{
    int num_threads;
    const char *filename;
    const char *pattern;
    size_t pattern_len;
    unsigned char first_byte;
    int fd;
    struct stat sb;
    char *file_data;
    size_t file_size;
    size_t chunk_size;
    size_t aligned_chunk_size;
    pthread_t *threads;
    ThreadArgs *thread_args;
    int i;
    int ret;
    long total_matches;

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

    /* Ограничиваем количество потоков разумным пределом */
    if (num_threads > 64) {
        num_threads = 64;
    }

    filename = argv[2];
    pattern = argv[3];
    pattern_len = strlen(pattern);

    if (pattern_len == 0) {
        fprintf(stderr, "Error: Substring cannot be empty.\n");
        return 1;
    }

    /* Сохраняем первый байт паттерна для оптимизации */
    first_byte = (unsigned char)pattern[0];

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
        
        /* Совет ядру о последовательном доступе */
        madvise(file_data, file_size, MADV_SEQUENTIAL);
    } else {
        file_data = NULL;
    }

    close(fd);

    /* Выделение памяти для потоков и аргументов */
    threads = (pthread_t *)malloc(sizeof(pthread_t) * (size_t)num_threads);
    thread_args = (ThreadArgs *)malloc(sizeof(ThreadArgs) * (size_t)num_threads);

    if (threads == NULL || thread_args == NULL) {
        fprintf(stderr, "Error: Memory allocation failed.\n");
        if (file_data != NULL) munmap(file_data, file_size);
        return 1;
    }

    /* Инициализация общего счетчика */
    total_matches = 0;

    /* Если файл пустой или меньше паттерна, результат 0 */
    if (file_size == 0 || file_size < pattern_len) {
        total_matches = 0;
    } else {
        /* Расчет размеров чанков с выравниванием */
        chunk_size = file_size / (size_t)num_threads;
        
        /* Выравниваем размер чанка по границе кэш-линии для лучшей производительности */
        aligned_chunk_size = (chunk_size / CACHE_LINE_SIZE) * CACHE_LINE_SIZE;
        if (aligned_chunk_size < CACHE_LINE_SIZE) {
            aligned_chunk_size = chunk_size;
        }

        /* Подготовка аргументов для потоков */
        for (i = 0; i < num_threads; i++) {
            thread_args[i].data = file_data;
            thread_args[i].file_size = file_size;
            thread_args[i].pattern = pattern;
            thread_args[i].pattern_len = pattern_len;
            thread_args[i].first_byte = first_byte;
            thread_args[i].local_count = 0;

            /* Логические границы */
            thread_args[i].start_offset = (size_t)i * aligned_chunk_size;
            if (i == num_threads - 1) {
                thread_args[i].end_offset = file_size;
            } else {
                thread_args[i].end_offset = (size_t)(i + 1) * aligned_chunk_size;
            }

            /* Реальные границы поиска (с перекрытием для учета вхождений на стыке) */
            if (i == 0) {
                thread_args[i].search_start = 0;
            } else {
                if (thread_args[i].start_offset > pattern_len - 1) {
                    thread_args[i].search_start = thread_args[i].start_offset - (pattern_len - 1);
                } else {
                    thread_args[i].search_start = 0;
                }
            }
            thread_args[i].search_end = thread_args[i].end_offset;
        }

        /* Создание потоков */
        for (i = 0; i < num_threads; i++) {
            ret = pthread_create(&threads[i], NULL, worker_thread, 
                                (void *)&thread_args[i]);
            if (ret != 0) {
                fprintf(stderr, "Error: Failed to create thread %d.\n", i);
                /* Продолжаем с меньшим количеством потоков */
                num_threads = i;
                break;
            }
        }

        /* Ожидание завершения потоков и суммирование локальных счетчиков */
        for (i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
            /* Суммируем без мьютекса - только главный поток читает */
            total_matches += thread_args[i].local_count;
        }
    }

    /* Вывод результата */
    printf("Occurrences: %ld\n", total_matches);

    /* Очистка ресурсов */
    if (file_data != NULL) {
        munmap(file_data, file_size);
    }
    free(threads);
    free(thread_args);

    return 0;
}
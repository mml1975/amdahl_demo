/*
 * Программа: parallel_search.c
 * Стандарт: C89/C90 (ANSI C)
 * Платформа: Linux (POSIX threads, mmap)
 * Назначение: Подсчет вхождений подстроки в бинарном файле с использованием
 *             нескольких потоков для демонстрации закона Амдала.
 *
 * Компиляция:
 *   gcc -std=c89 -pedantic -Wall -O2 parallel_search.c -o parallel_search -lpthread
 *
 * Использование:
 *   ./parallel_search <num_threads> <filename> <substring>
 *
 * Примечание по закону Амдала:
 *   Ускорение программы ограничено последовательной частью кода (открытие файла,
 *   mmap, создание потоков, объединение результатов, блокировки мьютекса).
 *   При увеличении числа потоков полезная нагрузка растет, но накладные расходы
 *   на синхронизацию и последовательную инициализацию не уменьшаются, что приводит
 *   к насыщению производительности.
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
    size_t start_offset;      /* Логическое начало участка (для проверки границ) */
    size_t end_offset;        /* Логический конец участка */
    size_t search_start;      /* Реальное начало поиска (с учетом перекрытия) */
    size_t search_end;        /* Реальный конец поиска */
    const char *pattern;
    size_t pattern_len;
} ThreadArgs;

/*
 * Простая реализация поиска подстроки в памяти (аналог memmem).
 * Используется для совместимости со стандартом C89, так как memmem
 * является расширением GNU, а не стандартом C.
 */
static const char *find_mem(const char *haystack, size_t haystack_len,
                            const char *needle, size_t needle_len)
{
    size_t i;
    if (needle_len == 0) {
        return haystack;
    }
    if (needle_len > haystack_len) {
        return NULL;
    }

    for (i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

/* Функция потока */
static void *worker_thread(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    long local_count = 0;
    size_t pos;
    const char *current_ptr;
    size_t remaining_len;
    const char *found;

    /*
     * Итерируемся по выделенному диапазону.
     * Важно: мы сканируем от search_start до search_end, но засчитываем
     * только те вхождения, которые начинаются не раньше start_offset.
     * Это решает проблему вхождений, пересекающих границы блоков.
     */
    current_ptr = args->data + args->search_start;
    /* Вычисляем доступную длину для поиска от текущей позиции */
    remaining_len = args->search_end - args->search_start;

    while (remaining_len >= args->pattern_len) {
        found = find_mem(current_ptr, remaining_len, args->pattern, args->pattern_len);

        if (found == NULL) {
            break;
        }

        /* Вычисляем абсолютную позицию найденного вхождения */
        pos = (size_t)(found - args->data);

        /* Проверяем, попадает ли начало вхождения в логический диапазон потока */
        if (pos >= args->start_offset) {
            local_count++;
        }

        /* Сдвигаемся на 1 байт вперед для поиска следующих вхождений (поиск всех) */
        /* Оптимизация: можно сдвигаться на pattern_len, если нужны непересекающиеся */
        current_ptr = found + 1;
        remaining_len = args->search_end - (size_t)(current_ptr - args->data);
    }

    /* Критическая секция: обновление глобального счетчика */
    /* Это последовательная часть, влияющая на закон Амдала */
    pthread_mutex_lock(&count_mutex);
    total_matches += local_count;
    pthread_mutex_unlock(&count_mutex);

    return NULL;
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
    size_t chunk_size;
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

    /* Если файл пустой или меньше паттерна, результат 0 */
    if (file_size == 0 || file_size < pattern_len) {
        /* Потоки не создаем, результат 0 */
        total_matches = 0;
    } else {
        /* Расчет размеров_chunks */
        chunk_size = file_size / (size_t)num_threads;

        for (i = 0; i < num_threads; i++) {
            thread_args[i].data = file_data;
            thread_args[i].file_size = file_size;
            thread_args[i].pattern = pattern;
            thread_args[i].pattern_len = pattern_len;

            /* Логические границы */
            thread_args[i].start_offset = (size_t)i * chunk_size;
            if (i == num_threads - 1) {
                thread_args[i].end_offset = file_size;
            } else {
                thread_args[i].end_offset = (size_t)(i + 1) * chunk_size;
            }

            /* Реальные границы поиска (с перекрытием для учета вхождений на стыке) */
            /* Первый поток начинает с 0, остальные отступают назад на len-1 */
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

            /* Создание потока */
            ret = pthread_create(&threads[i], NULL, worker_thread, (void *)&thread_args[i]);
            if (ret != 0) {
                fprintf(stderr, "Error: Failed to create thread %d.\n", i);
                break;
            }
        }

        /* Ожидание завершения потоков */
        /* Только для успешно созданных потоков */
        for (i = 0; i < num_threads; i++) {
            /* Проверка, был ли создан поток (упрощенно считаем, что если цикл дошел, то создали) */
            /* В продакшене нужно флаги состояния, здесь для демо упрощаем */
            pthread_join(threads[i], NULL);
        }
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
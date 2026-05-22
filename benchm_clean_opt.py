#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
benchmark.py

Скрипт для бенчмаркинга программы parallel_search.
Запускает программу с разным количеством потоков (N=1..12),
замеряет время выполнения, вычисляет ускорение и строит график.

Требования:
    pip install matplotlib

Использование:
    python benchmark.py [путь_к_файлу] [подстрока]
    
Пример:
    python benchmark.py testfile.bin "ABC"
"""

import subprocess
import time
import csv
import sys
import os

# Попытка импортировать matplotlib
try:
    import matplotlib
    matplotlib.use('Agg')  # Неинтерактивный бэкенд для сохранения в файл
    import matplotlib.pyplot as plt
    MATPLOTLIB_AVAILABLE = True
except ImportError:
    MATPLOTLIB_AVAILABLE = False
    print("Warning: matplotlib not available. Graph will not be generated.")
    print("Install with: pip install matplotlib")


# Константы
PROGRAM_PATH = "./parallel_search_clean_opt"
MIN_THREADS = 1
MAX_THREADS = 12
NUM_REPEATS = 5
CSV_FILENAME = "benchmark_results_clean_opt.csv"
PLOT_FILENAME = "speedup_graph_clean_opt.png"


def run_program(num_threads, filename, substring):
    """
    Запускает программу parallel_search с заданным количеством потоков.
    
    Args:
        num_threads (int): Количество потоков
        filename (str): Имя файла для поиска
        substring (str): Искомая подстрока
    
    Returns:
        float: Время выполнения в секундах, или None при ошибке
    """
    try:
        start_time = time.time()
        
        # Запуск программы
        result = subprocess.run(
            [PROGRAM_PATH, str(num_threads), filename, substring],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=300  # Таймаут 5 минут
        )
        
        end_time = time.time()
        elapsed_time = end_time - start_time
        
        # Проверка кода возврата
        if result.returncode != 0:
            print("Error: Program returned non-zero exit code for N=%d" % num_threads)
            print("Stderr: %s" % result.stderr.decode('utf-8', errors='ignore'))
            return None
        
        return elapsed_time
        
    except subprocess.TimeoutExpired:
        print("Error: Timeout for N=%d" % num_threads)
        return None
    except Exception as e:
        print("Error: %s for N=%d" % (str(e), num_threads))
        return None


def benchmark(filename, substring):
    """
    Проводит бенчмарк программы для разного количества потоков.
    
    Args:
        filename (str): Имя файла для поиска
        substring (str): Искомая подстрока
    
    Returns:
        list: Список словарей с результатами [{'threads': N, 'avg_time': T, 'speedup': S}, ...]
    """
    results = []
    baseline_time = None  # Время для N=1 (базовое)
    
    print("Starting benchmark...")
    print("File: %s" % filename)
    print("Substring: %s" % substring)
    print("Threads: %d to %d" % (MIN_THREADS, MAX_THREADS))
    print("Repeats per thread count: %d" % NUM_REPEATS)
    print("-" * 60)
    
    for num_threads in range(MIN_THREADS, MAX_THREADS + 1):
        times = []
        
        print("Testing N=%2d..." % num_threads, end=" ")
        
        for i in range(NUM_REPEATS):
            elapsed = run_program(num_threads, filename, substring)
            
            if elapsed is not None:
                times.append(elapsed)
                print(".", end="")
            else:
                print("x", end="")
        
        # Вычисление среднего времени
        if len(times) > 0:
            avg_time = sum(times) / len(times)
            
            # Вычисление ускорения
            if baseline_time is None:
                baseline_time = avg_time
                speedup = 1.0
            else:
                speedup = baseline_time / avg_time
            
            results.append({
                'threads': num_threads,
                'avg_time': avg_time,
                'speedup': speedup
            })
            
            print(" avg=%.4f s, speedup=%.2f" % (avg_time, speedup))
        else:
            print(" FAILED (no successful runs)")
            results.append({
                'threads': num_threads,
                'avg_time': None,
                'speedup': None
            })
    
    print("-" * 60)
    return results


def save_to_csv(results, filename):
    """
    Сохраняет результаты в CSV файл.
    
    Args:
        results (list): Список словарей с результатами
        filename (str): Имя выходного файла
    """
    try:
        with open(filename, 'w', newline='') as csvfile:
            fieldnames = ['количество процессоров', 'среднее время работы', 'ускорение вычислений']
            writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
            
            writer.writeheader()
            for result in results:
                row = {
                    'количество процессоров': result['threads'],
                    'среднее время работы': '%.6f' % result['avg_time'] if result['avg_time'] else 'N/A',
                    'ускорение вычислений': '%.4f' % result['speedup'] if result['speedup'] else 'N/A'
                }
                writer.writerow(row)
        
        print("Results saved to: %s" % filename)
    except Exception as e:
        print("Error saving CSV: %s" % str(e))


def plot_speedup(results, filename):
    """
    Строит график зависимости ускорения от количества процессоров.
    
    Args:
        results (list): Список словарей с результатами
        filename (str): Имя выходного файла графика
    """
    if not MATPLOTLIB_AVAILABLE:
        print("Cannot create plot: matplotlib not available")
        return
    
    try:
        # Фильтрация успешных результатов
        valid_results = [r for r in results if r['avg_time'] is not None]
        
        if len(valid_results) == 0:
            print("No valid results to plot")
            return
        
        threads = [r['threads'] for r in valid_results]
        speedups = [r['speedup'] for r in valid_results]
        
        # Создание фигуры
        fig, ax = plt.subplots(figsize=(10, 6))
        
        # Основная линия - фактическое ускорение
        ax.plot(threads, speedups, 'bo-', label='Фактическое ускорение', linewidth=2, markersize=8)
        
        # Идеальная линия - линейное ускорение
        ideal_speedup = [float(t) for t in threads]
        ax.plot(threads, ideal_speedup, 'r--', label='Идеальное ускорение (линейное)', linewidth=2)
        
        # Настройка графика
        ax.set_xlabel('Количество процессоров/потоков', fontsize=12)
        ax.set_ylabel('Ускорение вычислений', fontsize=12)
        ax.set_title('Зависимость ускорения от количества потоков', fontsize=14)
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=10)
        ax.set_xticks(threads)
        ax.set_xlim(left=MIN_THREADS, right=MAX_THREADS)
        ax.set_ylim(bottom=0)
        
        # Добавление значений на график
        for i, (t, s) in enumerate(zip(threads, speedups)):
            ax.annotate('%.2f' % s, xy=(t, s), xytext=(5, 5), 
                       textcoords='offset points', fontsize=9)
        
        # Сохранение
        plt.tight_layout()
        plt.savefig(filename, dpi=150, bbox_inches='tight')
        plt.close()
        
        print("Graph saved to: %s" % filename)
        
    except Exception as e:
        print("Error creating plot: %s" % str(e))


def print_summary(results):
    """
    Выводит сводную таблицу результатов в консоль.
    
    Args:
        results (list): Список словарей с результатами
    """
    print("\n" + "=" * 60)
    print("СВОДНАЯ ТАБЛИЦА РЕЗУЛЬТАТОВ")
    print("=" * 60)
    print("%-15s %-20s %-15s" % ("Процессоры", "Время (сек)", "Ускорение"))
    print("-" * 60)
    
    for result in results:
        time_str = '%.6f' % result['avg_time'] if result['avg_time'] else 'N/A'
        speedup_str = '%.4f' % result['speedup'] if result['speedup'] else 'N/A'
        print("%-15d %-20s %-15s" % (result['threads'], time_str, speedup_str))
    
    print("=" * 60)
    
    # Поиск лучшего результата
    valid_results = [r for r in results if r['speedup'] is not None]
    if len(valid_results) > 1:
        best = max(valid_results, key=lambda x: x['speedup'])
        print("\nЛучшее ускорение: %.2f при N=%d потоках" % (best['speedup'], best['threads']))


def main():
    """Основная функция."""
    # Проверка аргументов командной строки
    if len(sys.argv) < 3:
        # По умолчанию используем тестовые значения
        filename = "testfile.bin"
        substring = "ABC"
        print("Using default values:")
        print("  File: %s" % filename)
        print("  Substring: %s" % substring)
        print("\nOr specify: python %s <filename> <substring>" % sys.argv[0])
    else:
        filename = sys.argv[1]
        substring = sys.argv[2]
    
    # Проверка существования программы
    if not os.path.isfile(PROGRAM_PATH):
        print("Error: Program '%s' not found." % PROGRAM_PATH)
        print("Please compile it first:")
        print("  gcc -std=c89 -pedantic -Wall -O2 parallel_search.c -o parallel_search -lpthread")
        sys.exit(1)
    
    # Проверка существования файла
    if not os.path.isfile(filename):
        print("Error: File '%s' not found." % filename)
        sys.exit(1)
    
    # Проверка существования файла для поиска
    if os.path.getsize(filename) == 0:
        print("Warning: File '%s' is empty." % filename)
    
    # Запуск бенчмарка
    results = benchmark(filename, substring)
    
    # Сохранение результатов
    save_to_csv(results, CSV_FILENAME)
    
    # Построение графика
    plot_speedup(results, PLOT_FILENAME)
    
    # Вывод сводки
    print_summary(results)
    
    print("\nBenchmark completed!")
    print("CSV file: %s" % CSV_FILENAME)
    print("Graph file: %s" % PLOT_FILENAME)


if __name__ == "__main__":
    main()
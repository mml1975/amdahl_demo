A small program to demonstrate Amdahl's Law

Compiling:<br/>
gcc -std=c89 -pedantic -Wall -O2 parallel_search.c -o parallel_search -lpthread

Usage:<br/>
./parallel_search <num_threads> <filename> <substring>

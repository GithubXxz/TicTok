#include <stdio.h>
FILE *null_file = NULL;

void c() {
  for (int i = 0; i < 100000; i++) {
    fprintf(null_file,"waste c()\n");
  }
}

void b() {
  for (int i = 0; i < 1000000; i++) {
    fprintf(null_file,"waste b()\n");
  }
}

void a() {
  for (int i = 0; i < 10000000; i++) {
    fprintf(null_file,"waste a()\n");
  }
}

int main() {
  null_file = fopen("/dev/null", "w");
  c();
  c();
  b();
  b();
  a();
  a();
  return 0;
}

extern int gm;
int gi = 1;

int add(int i, int j) { return i + j; }

int main(int argc, const char **argv) {
  int i = 2;
  int b = add(gi, gm);
  i = b;
  return 0;
}

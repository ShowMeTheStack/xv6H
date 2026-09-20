// a shared library to load at run time, for the dlopen tests
int lib_counter;

int
lib_add(int a, int b)
{
  lib_counter++;
  return a + b;
}

const char *
lib_name(void)
{
  return "libtest";
}

// an initializer, to prove the loader runs them
static void __attribute__((constructor))
lib_init(void)
{
  lib_counter = 100;
}

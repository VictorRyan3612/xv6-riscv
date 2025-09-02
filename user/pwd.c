#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  char buf[128];

  if (getcwd(buf, sizeof(buf)) < 0) {
    printf("pwd: erro ao obter diretório atual\n");
    exit(1);
  }

  printf("%s\n", buf);
  exit(0);
}

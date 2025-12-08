#include "kernel/types.h"
#include "user/user.h"
#include "path.h"
#include <string.h>

// Lista de diretórios do PATH
static char *PATHS[] = { "/bin", "/", 0 };

// Tenta executar o comando em todos os diretórios do PATH
int exec_with_path(const char *cmd, char **argv) {
  char buf[128];

  if(cmd[0] == '/') {
    // Caminho absoluto
    return exec(cmd, argv);
  }

  for(int i = 0; PATHS[i]; i++) {
    strcpy(buf, PATHS[i]);
    if(PATHS[i][0] != '/') strcat(buf, "/");
    strcat(buf, cmd);

    if(exec(buf, argv) >= 0) {
      return 0;  // sucesso
    }
  }

  return -1; // não achou
}

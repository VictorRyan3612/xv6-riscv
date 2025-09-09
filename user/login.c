#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"
// separa uma string line em nome e valor por ':'  
int parse_line(char *line, char *name, char *value) {
    int i = 0, j = 0;
    // copia até ':'
    while(line[i] && line[i] != ':') {
        name[i] = line[i];
        i++;
    }
    name[i] = 0;
    if(line[i] != ':') return -1; // formato inválido
    i++; // pula ':'
    while(line[i]) {
        value[j++] = line[i++];
    }
    value[j] = 0;
    return 0;
}

// função para ler linha por linha de um arquivo
int readline(int fd, char *buf, int max) {
  int i = 0;
  char c;
  while (i+1 < max && read(fd, &c, 1) == 1) {
    if (c == '\n') break;
    buf[i++] = c;
  }
  buf[i] = 0;
  if (i == 0) return -1; // EOF
  return i;
}

// procura UID no /passwd
int get_uid(char *user) {
  int fd = open("/passwd", O_RDONLY);
  if (fd < 0) return -1;

  char line[64];
  while (readline(fd, line, sizeof(line)) > 0) {
    char uname[32], uidstr[32];
    if(parse_line(line, uname, uidstr) < 0) continue;
    if(strcmp(uname, user) == 0){
      close(fd);
      return atoi(uidstr);
    }
  }
  close(fd);
  return -1;
}

// valida senha em /shadow
int check_pass(char *user, char *pass) {
  int fd = open("/shadow", O_RDONLY);
  if (fd < 0) return 0;

  char line[64];
  while (readline(fd, line, sizeof(line)) > 0) {
    char uname[32], passfile[32];
    if(parse_line(line, uname, passfile) < 0) continue;
    if(strcmp(uname, user) == 0){
      close(fd);
      return strcmp(passfile, pass) == 0;
    }
  }
  close(fd);
  return 0;
}


int
main(void)
{
  char user[32], pass[32];

  while (1) {
    printf("login: ");
    gets(user, sizeof(user));
    user[strlen(user)-1] = 0;

    printf("password: ");
    gets(pass, sizeof(pass));
    pass[strlen(pass)-1] = 0;

    int uid = get_uid(user);
    if (uid >= 0 && check_pass(user, pass)) {
      printf("Login successful! Welcome %s (uid=%d)\n", user, uid);

      // futuramente: setuid(uid);

      char *argv[] = { "sh", 0 };
      exec("sh", argv);
      printf("login: exec sh failed\n");
      exit(1);
    } else {
      printf("Login incorrect\n");
    }
  }
}

// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// Parsed command representation
#define EXEC  1
#define REDIR 2
#define PIPE  3
#define LIST  4
#define BACK  5

#define MAXARGS 10

#define HISTORY_FILE "history"

#define MAX_HISTORY 100
#define MAX_CMD_LEN 128

#define PATH_MAX 256

char history[MAX_HISTORY][MAX_CMD_LEN];
int hist_count = 0;
int hist_index = 0;

// PATH (default)
static char shell_path[PATH_MAX] = "/bin";

struct cmd {
  int type;
};

struct execcmd {
  int type;
  char *argv[MAXARGS];
  char *eargv[MAXARGS];
};

struct redircmd {
  int type;
  struct cmd *cmd;
  char *file;
  char *efile;
  int mode;
  int fd;
};

struct pipecmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct listcmd {
  int type;
  struct cmd *left;
  struct cmd *right;
};

struct backcmd {
  int type;
  struct cmd *cmd;
};

int fork1(void);  // Fork but panics on failure.
void panic(char*);
struct cmd *parsecmd(char*);
void runcmd(struct cmd*) __attribute__((noreturn));

int append_open(char *file) {
  char aux[2048];
  int fd, n = 0;
  fd = open(file, O_RDONLY);
  if(fd >= 0){
      n = read(fd, aux, sizeof(aux));
      close(fd);
  }
  // abre arquivo para truncar
  fd = open(file, O_WRONLY|O_CREATE|O_TRUNC);
  if(fd < 0){
      fprintf(2, "open %s failed\n", file);
      exit(1);
  }
  // escreve conteúdo antigo
  if(n > 0)
      write(fd, aux, n);
  return fd;
}
void history_add(char *buf){
  // ---- Adiciona ao histórico ----
  int fd = open(HISTORY_FILE, O_WRONLY | O_CREATE);
  if(fd >= 0){
    // lê conteúdo antigo
    char aux[1024];
    int n = read(open(HISTORY_FILE, O_RDONLY), aux, sizeof(aux));
    close(open(HISTORY_FILE, O_RDONLY));

    // abre truncando e escreve conteúdo antigo
    int fdt = open(HISTORY_FILE, O_WRONLY | O_CREATE | O_TRUNC);
    if(n > 0)
      write(fdt, aux, n);

    // escreve comando atual + newline
    write(fdt, buf, strlen(buf));
    write(fdt, "", 1);

    close(fdt);
  }
  // ---- fim do histórico ----
}
// load history from file into history[][] (call once at shell startup)
void history_load(void) {
  int fd = open(HISTORY_FILE, 0);
  if(fd < 0) return;
  char buf[4096];
  int n = read(fd, buf, sizeof(buf));
  close(fd);
  if(n <= 0) return;
  int pos = 0;
  while(pos < n && hist_count < MAX_HISTORY) {
    int len = 0;
    while(pos < n && buf[pos] != '\n' && len < MAX_CMD_LEN-1) {
      history[hist_count][len++] = buf[pos++];
    }
    history[hist_count][len] = '\0';
    hist_count++;
    if(pos < n && buf[pos] == '\n') pos++;
  }
}

// append new line to history[] in memory
void history_add_mem(const char *line) {
  if(!line || !*line) return;
  if(hist_count < MAX_HISTORY) {
    // safe copy
    int i=0;
    for(; i < MAX_CMD_LEN-1 && line[i]; i++) history[hist_count][i] = line[i];
    history[hist_count][i] = '\0';
    hist_count++;
  } else {
    // rotate up
    int i;
    for(i = 1; i < MAX_HISTORY; i++) strcpy(history[i-1], history[i]);
    // append at end
    int i2=0;
    for(; i2 < MAX_CMD_LEN-1 && line[i2]; i2++) history[MAX_HISTORY-1][i2] = line[i2];
    history[MAX_HISTORY-1][i2] = '\0';
  }
}

// append new line to history file (simple read->rewrite method)
void history_append_file(const char *line) {
  if(!line) return;
  char aux[4096];
  int n = 0;
  int fd = open(HISTORY_FILE, 0);
  if(fd >= 0) {
    n = read(fd, aux, sizeof(aux));
    close(fd);
  }
  int f = open(HISTORY_FILE, O_WRONLY | O_CREATE | O_TRUNC);
  if(f < 0) return;
  if(n > 0) write(f, aux, n);
  write(f, line, strlen(line));
  write(f, "\n", 1);
  close(f);
}

// redraw helper: prints "\r$ " + s and clears leftover chars
static void redraw_prompt(const char *s, int *prev_len) {
  int newlen = strlen(s);
  // print prompt+text
  write(1, "\r$ ", 3);
  write(1, s, newlen);
  // clear leftover
  if(*prev_len > newlen) {
    int k;
    for(k = 0; k < (*prev_len - newlen); k++) write(1, " ", 1);
    // reposition after printed text
    write(1, "\r$ ", 3);
    write(1, s, newlen);
  }
  *prev_len = newlen;
}

// ---------------- PATH helpers ----------------
void set_path(const char *p) {
  int i;
  for (i = 0; i < PATH_MAX-1 && p[i]; i++) shell_path[i] = p[i];
  shell_path[i] = '\0';
}

void print_path(void) {
  write(1, "PATH=", 5);
  write(1, shell_path, strlen(shell_path));
  write(1, "\n", 1);
}

// try to exec cmd by searching shell_path directories (returns -1 if not found)
// note: exec replaces process on success so function returns only on failure
int
find_in_path(char *cmd, char **argv)
{
  char dirbuf[PATH_MAX];
  char path2[PATH_MAX];
  int len = strlen(shell_path);
  int i = 0;

  while (i < len) {
    int j = 0;

    // copia próximo diretório
    while (i < len && shell_path[i] != ':') {
      if (j < PATH_MAX-1)
        dirbuf[j++] = shell_path[i];
      i++;
    }
    dirbuf[j] = 0;

    // pula ':'
    if (i < len && shell_path[i] == ':')
      i++;

    // diretório vazio = "."
    if (dirbuf[0] == 0) {
      dirbuf[0] = '.';
      dirbuf[1] = 0;
    }

    // monta caminho completo
    int p = 0;
    for (j = 0; dirbuf[j] && p < PATH_MAX-1; j++)
      path2[p++] = dirbuf[j];
    if (p < PATH_MAX-1)
      path2[p++] = '/';
    for (j = 0; cmd[j] && p < PATH_MAX-1; j++)
      path2[p++] = cmd[j];
    path2[p] = 0;

    exec(path2, argv);
    // se voltar, tenta próximo
  }

  return -1;
}

// ==== helpers do mesmo jeito que na Versão A ====
/* Reuse history_load(), history_add_mem(), history_append_file(), redraw_prompt() */

int getcmd_with_kernel_sentinals(char *buf, int nbuf) {
  int i = 0;
  int prev_len = 0;
  int hist_index = hist_count;
  char c;

  write(2, "$ ", 2);
  memset(buf, 0, nbuf);

  while(1) {
    if(read(0, &c, 1) != 1) return -1;

    if(c == '\n') {
      buf[i] = 0;
      write(1, "\n", 1);
      if(i > 0) {
        history_add_mem(buf);
        history_append_file(buf);
      }
      return 0;
    }

    // Kernel-intercepted sentinels (single byte)
    if(c == 1) { // KEY_HISTORY_UP
      if(hist_count == 0) continue;
      if(hist_index > 0) hist_index--;
      if(hist_index < hist_count) {
        int len = strlen(history[hist_index]);
        int j;
        for(j=0; j < nbuf-1 && j < len; j++) buf[j] = history[hist_index][j];
        buf[j] = 0;
        i = strlen(buf);
        redraw_prompt(buf, &prev_len);
      }
      continue;
    } else if(c == 2) { // KEY_HISTORY_DOWN
      if(hist_count == 0) continue;
      if(hist_index < hist_count) hist_index++;
      if(hist_index == hist_count) {
        buf[0] = 0; i = 0;
        redraw_prompt("", &prev_len);
      } else {
        int len = strlen(history[hist_index]);
        int j;
        for(j=0; j < nbuf-1 && j < len; j++) buf[j] = history[hist_index][j];
        buf[j] = 0;
        i = strlen(buf);
        redraw_prompt(buf, &prev_len);
      }
      continue;
    }

    // Backspace
    if(c == 0x7f || c == 8) {
      if(i > 0) {
        i--;
        write(1, "\b \b", 3);
        prev_len = (prev_len > 0) ? prev_len - 1 : 0;
      }
      continue;
    }

    // Normal char
    if(c != 0 && i + 1 < nbuf) {
      buf[i++] = c;
      write(1, &c, 1);
      prev_len++;
    }
  }
}

// Execute cmd.  Never returns.
void
runcmd(struct cmd *cmd)
{
  int p[2];
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    exit(1);

  switch(cmd->type){
  default:
    panic("runcmd");

  case EXEC:
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0){
      exit(1);
    }

    // Builtin: cd
    if(strcmp(ecmd->argv[0], "cd") == 0){
      if(ecmd->argv[1] == 0){
        fprintf(2, "cd: expected argument\n");
      } else {
        if(chdir(ecmd->argv[1]) < 0){
          fprintf(2, "cd: cannot cd %s\n", ecmd->argv[1]);
        }
      }
      exit(0);  // don't fork/exec, just return
    }

    // Builtin: setpath
    if(strcmp(ecmd->argv[0], "setpath") == 0) {
      if(ecmd->argv[1] == 0) {
        fprintf(2, "usage: setpath dir:dir:...\n");
      } else {
        set_path(ecmd->argv[1]);
      }
      exit(0);
    }
    // Builtin: printpath
    if(strcmp(ecmd->argv[0], "printpath") == 0) {
      print_path();
      exit(0);
    }

    // If command contains '/', try to exec it directly
    if(strchr(ecmd->argv[0], '/')) {
      exec(ecmd->argv[0], ecmd->argv);
      // if exec returns -> failed, fall through to error
    } else {
      // Try lookup in PATH
      if(find_in_path(ecmd->argv[0], ecmd->argv) < 0) {
        // not found in PATH; as fallback try /<cmd> (root) like you had before
        char fallback[128];
        fallback[0] = '/';
        strncpy(fallback + 1, ecmd->argv[0], sizeof(fallback)-2);
        fallback[sizeof(fallback)-1] = 0;
        exec(fallback, ecmd->argv);
      }
    }

    fprintf(2, "exec %s failed\n", ecmd->argv[0]);
    break;

case REDIR:
    rcmd = (struct redircmd*)cmd;
    {
    int fd;

    if(rcmd->mode == (O_WRONLY|O_CREATE)) { // >>
        fd = append_open(rcmd -> file);
        if(fd < 0){
          fprintf(2, "open %s failed\n", rcmd->file);
          exit(1);
        }
    } else {
        fd = open(rcmd->file, rcmd->mode);
        if(fd < 0){
            fprintf(2, "open %s failed\n", rcmd->file);
            exit(1);
        }
    }

    // redirect stdout
    close(rcmd->fd);
    dup(fd);
    close(fd);

    runcmd(rcmd->cmd);
    }
    break;


  case LIST:
    lcmd = (struct listcmd*)cmd;
    if(fork1() == 0)
      runcmd(lcmd->left);
    wait(0);
    runcmd(lcmd->right);
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0)
      panic("pipe");
    if(fork1() == 0){
      close(1);
      dup(p[1]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->left);
    }
    if(fork1() == 0){
      close(0);
      dup(p[0]);
      close(p[0]);
      close(p[1]);
      runcmd(pcmd->right);
    }
    close(p[0]);
    close(p[1]);
    wait(0);
    wait(0);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    if(fork1() == 0)
      runcmd(bcmd->cmd);
    break;
  }
  exit(0);
}

int
getcmd(char *buf, int nbuf)
{
  write(2, "$ ", 2);
  memset(buf, 0, nbuf);
  gets(buf, nbuf);
  if(buf[0] == 0) // EOF
    return -1;

  history_add(buf);

  return 0;
}
// ==== getcmd version A ====
int getcmd_with_raw_arrows(char *buf, int nbuf) {
  int i = 0;
  int prev_len = 0;            // how many chars currently displayed after prompt
  int hist_index = hist_count; // position for navigation (hist_count == "after last")
  char c;

  write(2, "$ ", 2);
  memset(buf, 0, nbuf);

  while(1) {
    if(read(0, &c, 1) != 1) return -1;

    if(c == '\n') {
      buf[i] = 0;
      write(1, "\n", 1);
      if(i > 0) {
        history_add_mem(buf);
        history_append_file(buf);
      }
      return 0;
    }

    // ESC sequence handling (ESC '[' X)
    if(c == 27) {
      char s1 = 0, s2 = 0;
      if(read(0, &s1, 1) != 1) continue;
      if(read(0, &s2, 1) != 1) continue;
      if(s1 == '[') {
        if(s2 == 'A') {
          // UP
          if(hist_count == 0) {
            ; // nothing
          } else {
            if(hist_index > 0) hist_index--;
            if(hist_index < hist_count) {
              // replace current buffer with history[hist_index]
              int len = strlen(history[hist_index]);
              // copy into buf
              int j;
              for(j=0; j < nbuf-1 && j < len; j++) buf[j] = history[hist_index][j];
              buf[j] = 0;
              i = strlen(buf);
              redraw_prompt(buf, &prev_len);
            }
          }
          continue;
        } else if(s2 == 'B') {
          // DOWN
          if(hist_count == 0) {
            ; // nothing
          } else {
            if(hist_index < hist_count) hist_index++;
            if(hist_index == hist_count) {
              // clear line
              buf[0] = 0; i = 0;
              redraw_prompt("", &prev_len);
            } else {
              int len = strlen(history[hist_index]);
              int j;
              for(j=0; j < nbuf-1 && j < len; j++) buf[j] = history[hist_index][j];
              buf[j] = 0;
              i = strlen(buf);
              redraw_prompt(buf, &prev_len);
            }
          }
          continue;
        } else {
          // other ESC seq: ignore (or could handle left/right here)
          continue;
        }
      }
      continue;
    }

    // Backspace
    if(c == 0x7f || c == 8) {
      if(i > 0) {
        i--;
        // erase last char visually
        write(1, "\b \b", 3);
        prev_len = (prev_len > 0) ? prev_len - 1 : 0;
      }
      continue;
    }

    // Normal printable characters
    if(c != 0 && i + 1 < nbuf) {
      buf[i++] = c;
      write(1, &c, 1);
      prev_len++;
    }
  }
}
int
main(void)
{
  static char buf[100];
  int fd;
  history_load();
  // Ensure that three file descriptors are open.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    char *cmd = buf;
    while (*cmd == ' ' || *cmd == '\t')
      cmd++;
    if (*cmd == '\n') // is a blank command
      continue;
    if(cmd[0] == 'c' && cmd[1] == 'd' && cmd[2] == ' '){
      // Chdir must be called by the parent, not the child.
      cmd[strlen(cmd)-1] = 0;  // chop \n
      if(chdir(cmd+3) < 0)
        fprintf(2, "cannot cd %s\n", cmd+3);
    } else {
      if(fork1() == 0)
        runcmd(parsecmd(cmd));
      wait(0);
    }
  } // ESC [ A B C D 
  exit(0);
}

void
panic(char *s)
{
  fprintf(2, "%s\n", s);
  exit(1);
}

int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1)
    panic("fork");
  return pid;
}

//PAGEBREAK!
// Constructors

struct cmd*
execcmd(void)
{
  struct execcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
redircmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
  struct redircmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = REDIR;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->efile = efile;
  cmd->mode = mode;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
pipecmd(struct cmd *left, struct cmd *right)
{
  struct pipecmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = PIPE;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
listcmd(struct cmd *left, struct cmd *right)
{
  struct listcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = LIST;
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

struct cmd*
backcmd(struct cmd *subcmd)
{
  struct backcmd *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = BACK;
  cmd->cmd = subcmd;
  return (struct cmd*)cmd;
}
//PAGEBREAK!
// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int
gettoken(char **ps, char *es, char **q, char **eq)
{
  char *s;
  int ret;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  if(q)
    *q = s;
  ret = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '(':
  case ')':
  case ';':
  case '&':
  case '<':
    s++;
    break;
  case '>':
    s++;
    if(*s == '>'){
      ret = '+';
      s++;
    }
    break;
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }
  if(eq)
    *eq = s;

  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return ret;
}

int
peek(char **ps, char *es, char *toks)
{
  char *s;

  s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);
struct cmd *nulterminate(struct cmd*);

struct cmd*
parsecmd(char *s)
{
  char *es;
  struct cmd *cmd;

  es = s + strlen(s);
  cmd = parseline(&s, es);
  peek(&s, es, "");
  if(s != es){
    fprintf(2, "leftovers: %s\n", s);
    panic("syntax");
  }
  nulterminate(cmd);
  return cmd;
}

struct cmd*
parseline(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parsepipe(ps, es);
  while(peek(ps, es, "&")){
    gettoken(ps, es, 0, 0);
    cmd = backcmd(cmd);
  }
  if(peek(ps, es, ";")){
    gettoken(ps, es, 0, 0);
    cmd = listcmd(cmd, parseline(ps, es));
  }
  return cmd;
}

struct cmd*
parsepipe(char **ps, char *es)
{
  struct cmd *cmd;

  cmd = parseexec(ps, es);
  if(peek(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd*
parseredirs(struct cmd *cmd, char **ps, char *es)
{
  int tok;
  char *q, *eq;

  while(peek(ps, es, "<>")){
    tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a')
      panic("missing file for redirection");
    switch(tok){
    case '<':
      cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
      break;
    case '>':
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE|O_TRUNC, 1);
      break;
    case '+':  // >>
      cmd = redircmd(cmd, q, eq, O_WRONLY|O_CREATE, 1);
      break;
    }
  }
  return cmd;
}

struct cmd*
parseblock(char **ps, char *es)
{
  struct cmd *cmd;

  if(!peek(ps, es, "("))
    panic("parseblock");
  gettoken(ps, es, 0, 0);
  cmd = parseline(ps, es);
  if(!peek(ps, es, ")"))
    panic("syntax - missing )");
  gettoken(ps, es, 0, 0);
  cmd = parseredirs(cmd, ps, es);
  return cmd;
}

struct cmd*
parseexec(char **ps, char *es)
{
  char *q, *eq;
  int tok, argc;
  struct execcmd *cmd;
  struct cmd *ret;

  if(peek(ps, es, "("))
    return parseblock(ps, es);

  ret = execcmd();
  cmd = (struct execcmd*)ret;

  argc = 0;
  ret = parseredirs(ret, ps, es);
  while(!peek(ps, es, "|)&;")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    if(tok != 'a')
      panic("syntax");
    cmd->argv[argc] = q;
    cmd->eargv[argc] = eq;
    argc++;
    if(argc >= MAXARGS)
      panic("too many args");
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  cmd->eargv[argc] = 0;
  return ret;
}

// NUL-terminate all the counted strings.
struct cmd*
nulterminate(struct cmd *cmd)
{
  int i;
  struct backcmd *bcmd;
  struct execcmd *ecmd;
  struct listcmd *lcmd;
  struct pipecmd *pcmd;
  struct redircmd *rcmd;

  if(cmd == 0)
    return 0;

  switch(cmd->type){
  case EXEC:
    ecmd = (struct execcmd*)cmd;
    for(i=0; ecmd->argv[i]; i++)
      *ecmd->eargv[i] = 0;
    break;

  case REDIR:
    rcmd = (struct redircmd*)cmd;
    nulterminate(rcmd->cmd);
    *rcmd->efile = 0;
    break;

  case PIPE:
    pcmd = (struct pipecmd*)cmd;
    nulterminate(pcmd->left);
    nulterminate(pcmd->right);
    break;

  case LIST:
    lcmd = (struct listcmd*)cmd;
    nulterminate(lcmd->left);
    nulterminate(lcmd->right);
    break;

  case BACK:
    bcmd = (struct backcmd*)cmd;
    nulterminate(bcmd->cmd);
    break;
  }
  return cmd;
}

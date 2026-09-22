//
// httpd: a web server for static files.
//
//   httpd [-p PORT] [-d DIRECTORY] [-n] [-l LOGFILE]
//
// it answers GET and HEAD for files under its directory, with the
// directory listing of index.html when one is asked for a directory.
// a path with ".." in it is refused outright rather than resolved,
// because the one thing a file server must not do is serve a file
// outside what it was given.
//
// it is the example service of the init system and the target of the
// published-port tests: something that listens, says what it is, and
// can be asked for a file whose contents are known.
//
#include <sys/socket.h>
#include <netinet/in.h>
#include "user/user.h"
#include <signal.h>
#include <syslog.h>

#define BACKLOG 8

static const char *docroot = "/srv/www";
static int port = 80;

static const char *
content_type(const char *path)
{
  const char *dot = strrchr(path, '.');

  if (dot == 0)
    return "application/octet-stream";
  if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
    return "text/html";
  if (strcmp(dot, ".txt") == 0)
    return "text/plain";
  if (strcmp(dot, ".css") == 0)
    return "text/css";
  if (strcmp(dot, ".js") == 0)
    return "application/javascript";
  if (strcmp(dot, ".json") == 0)
    return "application/json";
  if (strcmp(dot, ".png") == 0)
    return "image/png";
  return "application/octet-stream";
}

static void
send_all(int fd, const char *buf, int n)
{
  int done = 0;

  while (done < n) {
    int w = write(fd, buf + done, n - done);
    if (w <= 0)
      return;
    done += w;
  }
}

static void
send_status(int fd, int code, const char *reason, const char *body)
{
  char head[512];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.0 %d %s\r\nContent-Type: text/plain\r\n"
                   "Content-Length: %d\r\nConnection: close\r\n\r\n",
                   code, reason, (int)strlen(body));

  send_all(fd, head, n);
  send_all(fd, body, strlen(body));
}

// a directory, as a page of links
static void
send_listing(int fd, const char *path, const char *urlpath, int head_only)
{
  char body[8192];
  int n = 0;

  n += snprintf(body + n, sizeof(body) - n, "<html><head><title>%s</title></head><body>\n",
                urlpath);
  n += snprintf(body + n, sizeof(body) - n, "<h1>Index of %s</h1>\n<ul>\n", urlpath);
  DIR *d = opendir(path);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != 0 && n < (int)sizeof(body) - 256) {
      if (strcmp(e->d_name, ".") == 0)
        continue;
      n += snprintf(body + n, sizeof(body) - n, "<li><a href=\"%s%s%s\">%s</a></li>\n", urlpath,
                    urlpath[strlen(urlpath) - 1] == '/' ? "" : "/", e->d_name, e->d_name);
    }
    closedir(d);
  }
  n += snprintf(body + n, sizeof(body) - n, "</ul></body></html>\n");

  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n",
                    n);
  send_all(fd, head, hn);
  if (!head_only)
    send_all(fd, body, n);
}

static void
serve(int fd, uint32 from)
{
  char request[1024];
  int n = read(fd, request, sizeof(request) - 1);

  if (n <= 0)
    return;
  request[n] = 0;

  char method[16] = "", target[512] = "";
  if (sscanf(request, "%15s %511s", method, target) != 2) {
    send_status(fd, 400, "Bad Request", "the request line was not one\n");
    return;
  }
  int head_only = strcmp(method, "HEAD") == 0;
  if (strcmp(method, "GET") != 0 && !head_only) {
    send_status(fd, 501, "Not Implemented", "this server answers GET and HEAD\n");
    return;
  }
  // the query string is not part of the path
  char *q = strchr(target, '?');
  if (q)
    *q = 0;
  // a path that tries to leave the directory is refused rather than
  // resolved: the one thing a file server must not do
  if (strstr(target, "..") || target[0] != '/') {
    send_status(fd, 403, "Forbidden", "that path is not one this server will resolve\n");
    return;
  }

  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s%s", docroot, target);
  struct stat st;
  if (stat(path, &st) < 0) {
    send_status(fd, 404, "Not Found", "no such file\n");
    syslog(LOG_INFO, "%s %s 404", method, target);
    return;
  }
  if (st.type == T_DIR) {
    char index[PATH_MAX];
    snprintf(index, sizeof(index), "%s%sindex.html", path,
             path[strlen(path) - 1] == '/' ? "" : "/");
    if (stat(index, &st) == 0 && st.type == T_FILE) {
      snprintf(path, sizeof(path), "%s", index);
    } else {
      send_listing(fd, path, target, head_only);
      syslog(LOG_INFO, "%s %s 200 (listing)", method, target);
      return;
    }
  }

  int in = open(path, O_RDONLY);
  if (in < 0) {
    send_status(fd, 403, "Forbidden", "this server may not read that\n");
    syslog(LOG_INFO, "%s %s 403", method, target);
    return;
  }
  char head[512];
  int hn = snprintf(head, sizeof(head),
                    "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                    "Connection: close\r\n\r\n",
                    content_type(path), (long)st.size);
  send_all(fd, head, hn);
  if (!head_only) {
    char buf[4096];
    int r;
    while ((r = read(in, buf, sizeof(buf))) > 0)
      send_all(fd, buf, r);
  }
  close(in);
  (void)from;
  syslog(LOG_INFO, "%s %s 200 (%ld bytes)", method, target, (long)st.size);
}

int
main(int argc, char *argv[])
{
  int foreground = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
      port = atoi(argv[++i]);
    else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
      docroot = argv[++i];
    else if (strcmp(argv[i], "-n") == 0)
      foreground = 1;
    else {
      fprintf(stderr, "usage: httpd [-p PORT] [-d DIRECTORY] [-n]\n");
      exit(1);
    }
  }

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    fprintf(stderr, "httpd: socket: %s\n", strerror(errno));
    exit(1);
  }
  struct sockaddr_in me;
  memset(&me, 0, sizeof(me));
  me.sin_family = AF_INET;
  me.sin_port = htons((uint16)port);
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (struct sockaddr *)&me, sizeof(me)) < 0) {
    fprintf(stderr, "httpd: port %d: %s\n", port, strerror(errno));
    exit(1);
  }
  if (listen(s, BACKLOG) < 0) {
    fprintf(stderr, "httpd: listen: %s\n", strerror(errno));
    exit(1);
  }

  if (!foreground) {
    pid_t pid = fork();
    if (pid < 0)
      exit(1);
    if (pid > 0) {
      FILE *f = fopen("/var/run/httpd.pid", "w");
      if (f) {
        fprintf(f, "%d\n", (int)pid);
        fclose(f);
      }
      exit(0);
    }
    setsid();
  }
  openlog("httpd", 0, LOG_DAEMON);
  signal(SIGCHLD, SIG_IGN); // a finished connection needs no funeral
  syslog(LOG_NOTICE, "listening on port %d, serving %s", port, docroot);

  for (;;) {
    struct sockaddr_in peer;
    socklen_t peerlen = sizeof(peer);
    memset(&peer, 0, sizeof(peer));
    int c = accept(s, (struct sockaddr *)&peer, &peerlen);
    uint32 from = ntohl(peer.sin_addr.s_addr);
    if (c < 0) {
      if (errno == EINTR)
        continue;
      syslog(LOG_ERR, "accept: %s", strerror(errno));
      break;
    }
    pid_t pid = fork();
    if (pid == 0) {
      close(s);
      serve(c, from);
      close(c);
      exit(0);
    }
    close(c);
  }
  exit(1);
}

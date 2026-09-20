// wget URL [file]: fetch http://host[:port]/path; print the body, or save it.
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/net.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(stderr, "usage: wget http://host[:port]/path [file]\n");
    exit(1);
  }
  char *url = argv[1];
  if (memcmp(url, "http://", 7) != 0) {
    fprintf(stderr, "wget: only http:// urls (use https for TLS)\n");
    exit(1);
  }
  char host[128], *path;
  char *h = url + 7;
  int i = 0;
  while (h[i] && h[i] != '/' && h[i] != ':' && i < 127) {
    host[i] = h[i];
    i++;
  }
  host[i] = 0;
  int port = 80;
  if (h[i] == ':')
    port = atoi(h + i + 1);
  path = strchr(h, '/');
  if (path == 0)
    path = "/";

  int fd = tcp_open(host, port);
  if (fd < 0)
    exit(1);
  char req[512];
  int n = 0;
  n += strlen(strcpy(req + n, "GET "));
  n += strlen(strcpy(req + n, path));
  n += strlen(strcpy(req + n, " HTTP/1.0\r\nHost: "));
  n += strlen(strcpy(req + n, host));
  n += strlen(strcpy(req + n, "\r\nUser-Agent: xv6-wget\r\nConnection: close\r\n\r\n"));
  if (write(fd, req, n) != n) {
    fprintf(stderr, "wget: write failed\n");
    exit(1);
  }
  int out = 1;
  if (argc > 2) {
    out = open(argv[2], O_WRONLY | O_CREATE | O_TRUNC);
    if (out < 0) {
      fprintf(stderr, "wget: cannot create %s\n", argv[2]);
      exit(1);
    }
  }
  static char buf[4096];
  int total = 0, header_done = 0, status = 0;
  static char hdr[4096];
  int hlen = 0;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    char *body = buf;
    int blen = n;
    if (!header_done) {
      // accumulate the header until the blank line
      int copy = n;
      if (hlen + copy > sizeof(hdr) - 1)
        copy = sizeof(hdr) - 1 - hlen;
      memmove(hdr + hlen, buf, copy);
      hlen += copy;
      hdr[hlen] = 0;
      char *e = 0;
      for (int k = 0; k + 3 < hlen; k++)
        if (hdr[k] == '\r' && hdr[k + 1] == '\n' && hdr[k + 2] == '\r' && hdr[k + 3] == '\n') {
          e = hdr + k + 4;
          break;
        }
      if (e == 0)
        continue;
      header_done = 1;
      status = atoi(hdr + 9);
      fprintf(stderr, "wget: %s:%d%s -> HTTP %d\n", host, port, path, status);
      int consumed = (e - hdr) - (hlen - n); // header bytes inside this buf
      body = buf + consumed;
      blen = n - consumed;
      if (blen < 0)
        blen = 0;
    }
    if (blen > 0) {
      write(out, body, blen);
      total += blen;
    }
  }
  close(fd);
  if (out != 1)
    close(out);
  fprintf(stderr, "wget: %d body bytes\n", total);
  exit(status == 200 ? 0 : 1);
}

// wget: fetch http://host[:port]/path.
//
//   wget URL [FILE]      save it, or print it with -O -
//   -O FILE              where to put it ("-" is standard output)
//   -q                   say nothing but the body
//   -S                   print the response header
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/net.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  const char *outfile = 0;
  int quiet = 0, show_header = 0, i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    if (strcmp(argv[i], "-O") == 0 && i + 1 < argc)
      outfile = argv[++i];
    else if (strcmp(argv[i], "-q") == 0)
      quiet = 1;
    else if (strcmp(argv[i], "-S") == 0)
      show_header = 1;
    else {
      fprintf(stderr, "usage: wget [-q] [-S] [-O FILE] http://host[:port]/path [FILE]\n");
      exit(1);
    }
  }
  if (i >= argc) {
    fprintf(stderr, "usage: wget [-q] [-S] [-O FILE] http://host[:port]/path [FILE]\n");
    exit(1);
  }
  char *url = argv[i++];
  if (memcmp(url, "http://", 7) != 0) {
    fprintf(stderr, "wget: only http:// urls (use https for TLS)\n");
    exit(1);
  }
  char host[128], *path;
  char *h = url + 7;
  int k = 0;
  while (h[k] && h[k] != '/' && h[k] != ':' && k < 127) {
    host[k] = h[k];
    k++;
  }
  host[k] = 0;
  int port = 80;
  if (h[k] == ':')
    port = atoi(h + k + 1);
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
  const char *dest = outfile ? outfile : (i < argc ? argv[i] : 0);
  if (dest && strcmp(dest, "-") != 0) {
    out = open(dest, O_WRONLY | O_CREATE | O_TRUNC, 0666);
    if (out < 0) {
      fprintf(stderr, "wget: cannot create %s\n", dest);
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
      if (show_header)
        fprintf(stderr, "%.*s", (int)(e - hdr), hdr);
      if (!quiet)
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
  if (!quiet)
    fprintf(stderr, "wget: %d body bytes\n", total);
  exit(status == 200 ? 0 : 1);
}

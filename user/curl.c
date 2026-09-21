// curl: fetch a URL, or send something to one.
//
//   curl [-s] [-i] [-I] [-L] [-o FILE] [-O] [-H HEADER] [-d DATA]
//        [-X METHOD] [-A AGENT] [-u USER:PASS] [-w FORMAT] [--] URL...
//
//   -s  say nothing but the body
//   -i  print the response header before the body
//   -I  ask for the header only (HEAD)
//   -L  follow a redirect
//   -o  write the body to this file; -O to a file named after the URL
//   -H  an extra request header; several are allowed
//   -d  a body to send, which makes it a POST unless -X says otherwise
//   -X  the method
//   -A  the User-Agent
//   -u  a user and password for Basic authentication
//   -w  print this after the transfer, with %{http_code} and the rest
//   -f  no body and a failure status when the server says it is an error
//
// http:// goes over a plain connection; https:// is handed to the TLS
// program, which is where the certificate checking lives.
#include "user/user.h"
#include "kernel/net.h"

#define MAXHDR 32

static int silent, show_header, head_only, follow, fail_on_error;
static const char *outfile, *method, *body, *write_out;
static const char *agent = "curl/xv6";
static int name_from_url;
static const char *extra[MAXHDR];
static int nextra;
static const char *userpass;
static int status;

struct url {
  char scheme[8];
  char host[128];
  char path[512];
  int port;
};

static int
parse_url(const char *s, struct url *u)
{
  const char *p = s;

  memset(u, 0, sizeof(*u));
  if (strncmp(p, "http://", 7) == 0) {
    snprintf(u->scheme, sizeof(u->scheme), "http");
    u->port = 80;
    p += 7;
  } else if (strncmp(p, "https://", 8) == 0) {
    snprintf(u->scheme, sizeof(u->scheme), "https");
    u->port = 443;
    p += 8;
  } else {
    // no scheme: http, as curl assumes
    snprintf(u->scheme, sizeof(u->scheme), "http");
    u->port = 80;
  }
  int n = 0;
  while (*p && *p != '/' && *p != ':' && n < (int)sizeof(u->host) - 1)
    u->host[n++] = *p++;
  u->host[n] = 0;
  if (u->host[0] == 0)
    return -1;
  if (*p == ':') {
    p++;
    u->port = atoi(p);
    while (*p >= '0' && *p <= '9')
      p++;
  }
  snprintf(u->path, sizeof(u->path), "%s", *p ? p : "/");
  return 0;
}

// the last component of the path, for -O
static const char *
basename_of(const struct url *u)
{
  const char *base = u->path;

  for (const char *p = u->path; *p; p++)
    if (*p == '/')
      base = p + 1;
  return *base ? base : "index.html";
}

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void
base64(const char *in, char *out, int size)
{
  int n = strlen(in), k = 0;

  for (int i = 0; i < n; i += 3) {
    unsigned v = (unsigned char)in[i] << 16;
    if (i + 1 < n)
      v |= (unsigned char)in[i + 1] << 8;
    if (i + 2 < n)
      v |= (unsigned char)in[i + 2];
    if (k + 4 >= size)
      break;
    out[k++] = b64[(v >> 18) & 63];
    out[k++] = b64[(v >> 12) & 63];
    out[k++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
    out[k++] = i + 2 < n ? b64[v & 63] : '=';
  }
  out[k] = 0;
}

// https is handed to the TLS program: the certificate checking and the
// whole of BearSSL live there, and there is no reason for two copies
static int
fetch_https(const struct url *u)
{
  char full[1024];
  char *args[6];
  int n = 0;

  snprintf(full, sizeof(full), "https://%s:%d%s", u->host, u->port, u->path);
  args[n++] = "https";
  args[n++] = (char *)full;
  if (outfile && strcmp(outfile, "-") != 0)
    args[n++] = (char *)outfile;
  args[n] = 0;

  int pid = fork();
  if (pid < 0) {
    fprintf(stderr, "curl: fork: %s\n", strerror(errno));
    return 1;
  }
  if (pid == 0) {
    execv("/bin/https", args);
    fprintf(stderr, "curl: /bin/https: %s\n", strerror(errno));
    _exit(1);
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
    ;
  return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

// returns the HTTP status, or -1. `location` is filled in when the
// answer is a redirect.
static int
fetch_http(const struct url *u, char *location, int locsize)
{
  char req[2048];
  int n = 0;

  int fd = tcp_open(u->host, u->port);
  if (fd < 0) {
    fprintf(stderr, "curl: (7) failed to connect to %s port %d\n", u->host,
            u->port);
    return -1;
  }

  const char *verb = method ? method : (head_only ? "HEAD" : (body ? "POST" : "GET"));
  n += snprintf(req + n, sizeof(req) - n, "%s %s HTTP/1.0\r\n", verb, u->path);
  n += snprintf(req + n, sizeof(req) - n, "Host: %s\r\n", u->host);
  n += snprintf(req + n, sizeof(req) - n, "User-Agent: %s\r\n", agent);
  n += snprintf(req + n, sizeof(req) - n, "Accept: */*\r\n");
  if (userpass) {
    char enc[256];
    base64(userpass, enc, sizeof(enc));
    n += snprintf(req + n, sizeof(req) - n, "Authorization: Basic %s\r\n", enc);
  }
  for (int i = 0; i < nextra; i++)
    n += snprintf(req + n, sizeof(req) - n, "%s\r\n", extra[i]);
  if (body) {
    int has_type = 0;
    for (int i = 0; i < nextra; i++)
      if (strncasecmp(extra[i], "content-type:", 13) == 0)
        has_type = 1;
    if (!has_type)
      n += snprintf(req + n, sizeof(req) - n,
                    "Content-Type: application/x-www-form-urlencoded\r\n");
    n += snprintf(req + n, sizeof(req) - n, "Content-Length: %d\r\n",
                  (int)strlen(body));
  }
  n += snprintf(req + n, sizeof(req) - n, "Connection: close\r\n\r\n");
  if (write(fd, req, n) != n) {
    fprintf(stderr, "curl: (55) failed sending the request\n");
    close(fd);
    return -1;
  }
  if (body) {
    int bl = strlen(body);
    if (write(fd, body, bl) != bl) {
      fprintf(stderr, "curl: (55) failed sending the body\n");
      close(fd);
      return -1;
    }
  }

  int out = 1;
  if (outfile && strcmp(outfile, "-") != 0) {
    out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
      fprintf(stderr, "curl: %s: %s\n", outfile, strerror(errno));
      close(fd);
      return -1;
    }
  }

  static char buf[4096], hdr[8192];
  int hlen = 0, header_done = 0, code = 0;
  long long total = 0;
  int r;
  if (location)
    location[0] = 0;

  while ((r = read(fd, buf, sizeof(buf))) > 0) {
    char *bodyp = buf;
    int blen = r;
    if (!header_done) {
      int copy = r;
      if (hlen + copy > (int)sizeof(hdr) - 1)
        copy = sizeof(hdr) - 1 - hlen;
      memmove(hdr + hlen, buf, copy);
      hlen += copy;
      hdr[hlen] = 0;
      char *end = 0;
      for (int k = 0; k + 3 < hlen; k++)
        if (hdr[k] == '\r' && hdr[k + 1] == '\n' && hdr[k + 2] == '\r' &&
            hdr[k + 3] == '\n') {
          end = hdr + k + 4;
          break;
        }
      if (end == 0)
        continue;
      header_done = 1;
      code = atoi(hdr + 9);
      // Location, for -L
      if (location) {
        for (char *p = hdr; p < end; p++)
          if ((p == hdr || p[-1] == '\n') && strncasecmp(p, "location:", 9) == 0) {
            p += 9;
            while (*p == ' ')
              p++;
            int k = 0;
            while (p < end && *p != '\r' && *p != '\n' && k < locsize - 1)
              location[k++] = *p++;
            location[k] = 0;
            break;
          }
      }
      if (show_header || head_only)
        write(out, hdr, end - hdr);
      int consumed = (int)(end - hdr) - (hlen - r);
      bodyp = buf + consumed;
      blen = r - consumed;
      if (blen < 0)
        blen = 0;
      if (head_only)
        break;
    }
    if (blen > 0 && !head_only) {
      if (write(out, bodyp, blen) != blen) {
        fprintf(stderr, "curl: (23) failed writing the body\n");
        break;
      }
      total += blen;
    }
  }
  close(fd);
  if (out != 1)
    close(out);
  if (!silent && !show_header && !head_only && code >= 400)
    fprintf(stderr, "curl: the server answered %d\n", code);
  (void)total;
  return code;
}

static void
print_write_out(const char *fmt, int code, const struct url *u)
{
  for (const char *p = fmt; *p; p++) {
    if (*p == '\\' && p[1]) {
      p++;
      putchar(*p == 'n' ? '\n' : *p == 't' ? '\t' : *p);
      continue;
    }
    if (*p != '%' || p[1] != '{') {
      putchar(*p);
      continue;
    }
    const char *e = strchr(p, '}');
    if (e == 0) {
      putchar(*p);
      continue;
    }
    int len = (int)(e - p - 2);
    if (len == 9 && strncmp(p + 2, "http_code", 9) == 0)
      printf("%d", code);
    else if (len == 3 && strncmp(p + 2, "url", 3) == 0)
      printf("%s://%s%s", u->scheme, u->host, u->path);
    p = e;
  }
}

static void
usage(void)
{
  fprintf(stderr, "usage: curl [-s] [-i] [-I] [-L] [-f] [-o FILE] [-O]"
                  " [-H HEADER] [-d DATA] [-X METHOD] URL\n");
  exit(2);
}

int
main(int argc, char *argv[])
{
  int i = 1;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--") == 0) {
      i++;
      break;
    }
    if (strncmp(a, "--", 2) == 0) {
      if (strcmp(a, "--silent") == 0) silent = 1;
      else if (strcmp(a, "--include") == 0) show_header = 1;
      else if (strcmp(a, "--head") == 0) head_only = 1;
      else if (strcmp(a, "--location") == 0) follow = 1;
      else if (strcmp(a, "--fail") == 0) fail_on_error = 1;
      else if (strcmp(a, "--remote-name") == 0) name_from_url = 1;
      else if (strcmp(a, "--output") == 0 && i + 1 < argc) outfile = argv[++i];
      else if (strcmp(a, "--data") == 0 && i + 1 < argc) body = argv[++i];
      else if (strcmp(a, "--header") == 0 && i + 1 < argc && nextra < MAXHDR)
        extra[nextra++] = argv[++i];
      else if (strcmp(a, "--request") == 0 && i + 1 < argc) method = argv[++i];
      else if (strcmp(a, "--user-agent") == 0 && i + 1 < argc) agent = argv[++i];
      else if (strcmp(a, "--user") == 0 && i + 1 < argc) userpass = argv[++i];
      else if (strcmp(a, "--write-out") == 0 && i + 1 < argc) write_out = argv[++i];
      else usage();
      continue;
    }
    int took_value = 0;
    for (const char *p = a + 1; *p && !took_value; p++) {
      switch (*p) {
      case 's': silent = 1; break;
      case 'i': show_header = 1; break;
      case 'I': head_only = 1; break;
      case 'L': follow = 1; break;
      case 'f': fail_on_error = 1; break;
      case 'O': name_from_url = 1; break;
      case 'k': break;   // do not check the certificate; https decides
      case 'v': break;
      case 'o': case 'H': case 'd': case 'X': case 'A': case 'u':
      case 'w': {
        const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : 0);
        if (v == 0)
          usage();
        switch (*p) {
        case 'o': outfile = v; break;
        case 'H': if (nextra < MAXHDR) extra[nextra++] = v; break;
        case 'd': body = v; break;
        case 'X': method = v; break;
        case 'A': agent = v; break;
        case 'u': userpass = v; break;
        case 'w': write_out = v; break;
        }
        took_value = 1;
        break;
      }
      default:
        usage();
      }
    }
  }
  if (i >= argc)
    usage();

  for (; i < argc; i++) {
    struct url u;
    char next[1024];

    if (parse_url(argv[i], &u) < 0) {
      fprintf(stderr, "curl: (3) cannot make sense of the URL: %s\n", argv[i]);
      status = 3;
      continue;
    }
    if (name_from_url && outfile == 0)
      outfile = basename_of(&u);

    int code;
    for (int hop = 0;; hop++) {
      if (strcmp(u.scheme, "https") == 0) {
        code = fetch_https(&u) == 0 ? 200 : -1;
        break;
      }
      code = fetch_http(&u, follow ? next : 0, sizeof(next));
      if (!follow || code < 300 || code >= 400 || next[0] == 0)
        break;
      if (hop >= 10) {
        fprintf(stderr, "curl: (47) too many redirects\n");
        break;
      }
      if (!silent)
        fprintf(stderr, "curl: following to %s\n", next);
      if (parse_url(next, &u) < 0)
        break;
    }
    if (code < 0)
      status = 7;
    else if (fail_on_error && code >= 400)
      status = 22;
    if (write_out)
      print_write_out(write_out, code, &u);
  }
  exit(status);
}

//
// kvlat: a key-value service and an open-loop load generator, for what an application sees while
// its machine is revised or moved. Builds for xv6H and for Linux, like
// netbench.
//
//   kvlat serve [-p PORT]
//       the service: one line per request, "GET key" or "SET key value"; the answer is one line,
//       "VALUE value" (or "NONE") or "OK". Any number of connections, one thread, poll.
//   kvlat load -c HOST[,HOST2] [-p PORT] [-r RATE] [-t SECS] [-b MS] [-m MARK]
//       open loop: RATE requests a second (a SET for every ten GETs) on one connection, each sent
//       at its scheduled time whatever has come back, so a stall shows as latency and not as
//       fewer requests. A request's latency runs from its scheduled time to its answer, however
//       late. Per bin of MS milliseconds: requests, p50, p99, max (microseconds), errors (lost
//       with a connection the stack closed, or unanswered 5 s after the run's end). -m prints MARK with the time
//       offset when the line "mark" arrives on standard input, so a driver can place events.
//       With two addresses, a connection made again goes to the other one: a machine that has
//       crossed hosts is reached where it now is, and the gap is in the bins. The run ends with
//       the longest time without an answer ("gap"). -k MS gives a connection up when requests
//       have waited MS with no answer at all (a machine that left keeps its end of the connection
//       quiet; nothing resets it): what was queued on it is an error, and it is made again.
//
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#define PORT_DEFAULT 6379
#define MAXCLI 32
#define NKEYS 1024
#define MAXOUT 65536

/* small requests and answers go out at once (Nagle would hold one behind the other's ack);
 * a stack without the option ignores it */
static void nodelay(int fd)
{
	int one = 1;

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static long long now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* ---- the service ---- */

static char *store[NKEYS];

static unsigned hash(const char *s)
{
	unsigned h = 5381;

	while (*s && *s != ' ' && *s != '\n')
		h = h * 33 + (unsigned char)*s++;
	return h % NKEYS;
}

static int answer(const char *line, char *out, int max)
{
	if (!strncmp(line, "GET ", 4)) {
		char *v = store[hash(line + 4)];
		return snprintf(out, max, v ? "VALUE %s\n" : "NONE\n", v);
	}
	if (!strncmp(line, "SET ", 4)) {
		const char *k = line + 4, *v = strchr(k, ' ');
		unsigned h = hash(k);

		free(store[h]);
		store[h] = strdup(v ? v + 1 : "");
		return snprintf(out, max, "OK\n");
	}
	return snprintf(out, max, "ERR\n");
}

static int serve(int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
	struct pollfd p[MAXCLI + 1];
	char buf[MAXCLI][4096];
	int len[MAXCLI] = { 0 }, n = 1, one = 1;
	int ls = socket(AF_INET, SOCK_STREAM, 0);

	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 8) < 0) {
		perror("kvlat serve");
		return 1;
	}
	printf("kvlat: serving on port %d\n", port);
	fflush(stdout);
	p[0].fd = ls;
	p[0].events = POLLIN;
	for (;;) {
		if (poll(p, n, -1) < 0)
			continue;
		if (p[0].revents & POLLIN) {
			int c = accept(ls, NULL, NULL);

			if (c >= 0 && n <= MAXCLI) {
				nodelay(c);
				p[n].fd = c;
				p[n].events = POLLIN;
				len[n - 1] = 0;
				n++;
			} else if (c >= 0) {
				close(c);
			}
		}
		for (int i = 1; i < n; i++) {
			char *b = buf[i - 1], out[8192];
			int r, o = 0;

			if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			r = read(p[i].fd, b + len[i - 1], sizeof(buf[0]) - 1 - len[i - 1]);
			if (r <= 0) {
				close(p[i].fd);
				p[i] = p[n - 1];
				memcpy(buf[i - 1], buf[n - 2], sizeof(buf[0]));
				len[i - 1] = len[n - 2];
				n--;
				i--;
				continue;
			}
			len[i - 1] += r;
			b[len[i - 1]] = 0;
			char *s = b, *e;
			while ((e = strchr(s, '\n')) != NULL) {
				*e = 0;
				o += answer(s, out + o, sizeof(out) - o);
				s = e + 1;
				if (o > (int)sizeof(out) - 256) {
					write(p[i].fd, out, o);
					o = 0;
				}
			}
			if (o)
				write(p[i].fd, out, o);
			len[i - 1] = b + len[i - 1] - s;
			memmove(b, s, len[i - 1]);
		}
	}
}

/* ---- the load ---- */

static long long *lat;		/* per request: latency in us, -1 error, 0 not yet answered */
static long long sched0;

static char hosts[2][64];
static int nhosts = 1, cur;

static int dial(const char *unused, int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	(void)unused;
	inet_pton(AF_INET, hosts[cur], &a.sin_addr);
	if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	nodelay(fd);
	return fd;
}

static int cmp(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;

	return x < y ? -1 : x > y;
}

static int giveup_ms;

static int load(const char *host, int port, int rate, int secs, int binms, const char *mark)
{
	long total = (long)rate * secs, sent = 0, head = 0, reconnects = 0;
	long *outq = malloc(MAXOUT * sizeof(long));
	int fd = dial(host, port), qn = 0, qh = 0, inlen = 0, flags;
	long long last_dial = 0, last_answer = 0, max_gap = 0;
	char in[65536];

	if (fd < 0) {
		perror("kvlat load: connect");
		return 1;
	}
	lat = calloc(total, sizeof(*lat));
	flags = fcntl(0, F_GETFL);
	fcntl(0, F_SETFL, flags | O_NONBLOCK);
	sched0 = now_us();
	last_answer = sched0;
	printf("kvlat: %d requests a second for %d s to %s:%d\n", rate, secs, host, port);
	fflush(stdout);
	while (head < total) {
		long long t = now_us();

		/* five seconds past the last request's time, what is still unanswered is an error: a
		 * machine that has stopped answering ends the run instead of holding it */
		if (t > sched0 + (long long)secs * 1000000 + 5000000) {
			for (long k = head; k < total; k++)
				if (!lat[k])
					lat[k] = -1;
			break;
		}
		struct pollfd p[2] = { { fd, POLLIN, 0 }, { 0, POLLIN, 0 } };
		long long next = sched0 + sent * 1000000LL / rate;
		int to;

		/* every request whose time has come, sent */
		while (sent < total && next <= t && qn < MAXOUT) {
			char req[64];
			int l = (sent % 11 == 10) ? snprintf(req, sizeof(req), "SET k%ld v%ld\n", sent % 997, sent)
						  : snprintf(req, sizeof(req), "GET k%ld\n", sent % 997);

			if (fd >= 0 && write(fd, req, l) != l) {
				close(fd);
				fd = -1;
			}
			if (fd < 0) {
				lat[sent++] = -1;	/* no connection to send it on */
				next = sched0 + sent * 1000000LL / rate;
				continue;
			}
			outq[(qh + qn++) % MAXOUT] = sent++;
			next = sched0 + sent * 1000000LL / rate;
		}
		/* A late answer is a latency, not an error: the connection is kept (its answers come in
		 * order), so a stall of the machine shows as the time it lasted. Only a connection the
		 * stack itself has closed loses what was queued on it -- or, with -k, one that has
		 * answered nothing for that long while requests wait. */
		if (giveup_ms && fd >= 0 && qn && t - last_answer > (long long)giveup_ms * 1000 &&
		    t - (sched0 + outq[qh] * 1000000LL / rate) > (long long)giveup_ms * 1000) {
			close(fd);
			fd = -1;
		}
		if (fd < 0 && qn) {
			while (qn) {
				lat[outq[qh]] = -1;
				qh = (qh + 1) % MAXOUT;
				qn--;
			}
		}
		if (fd < 0 && t > last_dial + 200000) {
			/* again, at most five times a second, and to the other address if there are two */
			last_dial = t;
			cur = (cur + 1) % nhosts;
			fd = dial(host, port);
			reconnects++;
			inlen = 0;
			qn = 0;
		}
		while (head < total && lat[head])
			head++;
		if (head >= total && sent >= total)
			break;
		to = sent < total ? (int)((next - now_us()) / 1000) : 50;
		if (to < 0)
			to = 0;
		if (to > 50)
			to = 50;
		p[0].fd = fd;
		if (fd < 0) {
			poll(NULL, 0, to);
			continue;
		}
		if (poll(p, mark ? 2 : 1, to) <= 0)
			continue;
		if (mark && (p[1].revents & POLLIN)) {
			char line[64];
			int r = read(0, line, sizeof(line) - 1);

			if (r > 0)
				printf("kvlat: %s at %.3f s\n", mark, (now_us() - sched0) / 1e6), fflush(stdout);
		}
		if (fd >= 0 && (p[0].revents & (POLLIN | POLLHUP | POLLERR))) {
			int r = read(fd, in + inlen, sizeof(in) - 1 - inlen);

			if (r <= 0) {
				close(fd);
				fd = -1;
				continue;
			}
			inlen += r;
			char *s = in, *e;
			long long done = now_us();
			while ((e = memchr(s, '\n', in + inlen - s)) != NULL) {
				if (done - last_answer > max_gap)
					max_gap = done - last_answer;
				last_answer = done;
				if (qn) {
					long k = outq[qh];
					long long l = done - (sched0 + k * 1000000LL / rate);

					lat[k] = l > 0 ? l : 1;
					qh = (qh + 1) % MAXOUT;
					qn--;
				}
				s = e + 1;
			}
			inlen = in + inlen - s;
			memmove(in, s, inlen);
		}
	}
	/* a silence that lasted to the end of the schedule is a gap too */
	{
		long long end = sched0 + (long long)secs * 1000000;

		if (end - last_answer > max_gap)
			max_gap = end - last_answer;
	}
	/* per bin, by scheduled time */
	long per = (long)rate * binms / 1000, nb = (total + per - 1) / per, errs = 0;
	long long *v = malloc(per * sizeof(long long)), all_max = 0;
	for (long b = 0; b < nb; b++) {
		int m = 0, e = 0;

		for (long k = b * per; k < (b + 1) * per && k < total; k++) {
			if (lat[k] <= 0)
				e++;
			else
				v[m++] = lat[k];
		}
		qsort(v, m, sizeof(*v), cmp);
		errs += e;
		if (m && v[m - 1] > all_max)
			all_max = v[m - 1];
		printf("kvlat bin %.1f n %d p50 %lld p99 %lld max %lld err %d\n", b * binms / 1000.0, m,
		       m ? v[m / 2] : 0, m ? v[(m * 99) / 100] : 0, m ? v[m - 1] : 0, e);
	}
	qsort(lat, total, sizeof(*lat), cmp);
	long ok0 = 0;
	while (ok0 < total && lat[ok0] <= 0)
		ok0++;
	long okn = total - ok0;
	printf("kvlat total %ld answered %ld errors %ld reconnects %ld p50 %lld p99 %lld p999 %lld max %lld gap %lld\n",
	       total, okn, errs, reconnects - 0, okn ? lat[ok0 + okn / 2] : 0, okn ? lat[ok0 + okn * 99 / 100] : 0,
	       okn ? lat[ok0 + okn * 999 / 1000] : 0, all_max, max_gap);
	return 0;
}

int main(int argc, char **argv)
{
	int port = PORT_DEFAULT, rate = 1000, secs = 10, binms = 100;
	const char *host = NULL, *mark = NULL;
	int i = 2;

	if (argc < 2) {
usage:
		fprintf(stderr, "usage: kvlat serve [-p PORT] | kvlat load -c HOST[,HOST2] [-p PORT] [-r RATE] [-t SECS] [-b MS] [-m MARK] [-k MS]\n");
		return 2;
	}
	for (; i < argc; i++) {
		if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-t") && i + 1 < argc) secs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-b") && i + 1 < argc) binms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-c") && i + 1 < argc) host = argv[++i];
		else if (!strcmp(argv[i], "-m") && i + 1 < argc) mark = argv[++i];
		else if (!strcmp(argv[i], "-k") && i + 1 < argc) giveup_ms = atoi(argv[++i]);
		else goto usage;
	}
	if (!strcmp(argv[1], "serve"))
		return serve(port);
	if (!strcmp(argv[1], "load") && host) {
		const char *comma = strchr(host, ',');

		if (comma) {
			snprintf(hosts[0], sizeof(hosts[0]), "%.*s", (int)(comma - host), host);
			snprintf(hosts[1], sizeof(hosts[1]), "%s", comma + 1);
			nhosts = 2;
		} else {
			snprintf(hosts[0], sizeof(hosts[0]), "%s", host);
		}
		return load(host, port, rate, secs, binms, mark);
	}
	goto usage;
}

// cal: a month, or a year.
//
//   cal [-m] [-y] [-1|-3] [[MONTH] YEAR]
//
//   -m  weeks begin on Monday
//   -y  the whole year
//   -3  the month before, this one and the next
//
// the calendar is the proleptic Gregorian one, which is what the rest
// of this system uses; the day of the week comes from Zeller's rule
// rather than from a table, so any year works.
#include "user/user.h"

static int monday_first, highlight_today;
static int today_y, today_m, today_d;

static const char *month_name[] = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"
};

static int
leap(int y)
{
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int
days_in(int y, int m)
{
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  if (m == 2 && leap(y))
    return 29;
  return d[m - 1];
}

// the day of the week of the first of the month, 0 for Sunday
static int
first_weekday(int y, int m)
{
  // Zeller's congruence, with January and February counted as the
  // thirteenth and fourteenth months of the year before
  int q = 1;
  int mm = m, yy = y;

  if (mm < 3) {
    mm += 12;
    yy -= 1;
  }
  int k = yy % 100, j = yy / 100;
  int h = (q + (13 * (mm + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
  // Zeller counts Saturday as zero
  int wd = (h + 6) % 7;
  return wd;
}

// one month, written into seven lines of twenty characters
static void
render(int y, int m, char out[8][32], int with_year)
{
  char title[32];

  if (with_year)
    snprintf(title, sizeof(title), "%s %d", month_name[m - 1], y);
  else
    snprintf(title, sizeof(title), "%s", month_name[m - 1]);
  int pad = (20 - (int)strlen(title)) / 2;
  if (pad < 0)
    pad = 0;
  snprintf(out[0], 32, "%*s%s", pad, "", title);

  snprintf(out[1], 32, "%s", monday_first ? "Mo Tu We Th Fr Sa Su"
                                          : "Su Mo Tu We Th Fr Sa");

  int first = first_weekday(y, m);
  if (monday_first)
    first = (first + 6) % 7;
  int n = days_in(y, m);

  for (int row = 0; row < 6; row++) {
    char *p = out[2 + row];
    int k = 0;
    for (int col = 0; col < 7; col++) {
      int day = row * 7 + col - first + 1;
      if (day < 1 || day > n)
        k += snprintf(p + k, 32 - k, "   ");
      else if (highlight_today && y == today_y && m == today_m && day == today_d)
        k += snprintf(p + k, 32 - k, "\033[7m%2d\033[m ", day);
      else
        k += snprintf(p + k, 32 - k, "%2d ", day);
    }
    // trim the trailing space so a line is never wider than it looks
    while (k > 0 && p[k - 1] == ' ')
      p[--k] = 0;
    p[k] = 0;
  }
}

static void
print_month(int y, int m)
{
  char out[8][32];

  render(y, m, out, 1);
  for (int i = 0; i < 8; i++)
    printf("%s\n", out[i]);
}

// three months side by side, which is what -3 and -y print
static void
print_row_of_three(int y, int m1, int m2, int m3, int with_year)
{
  char a[8][32], b[8][32], c[8][32];

  render(y, m1, a, with_year);
  render(y, m2, b, with_year);
  render(y, m3, c, with_year);
  for (int i = 0; i < 8; i++) {
    if (a[i][0] == 0 && b[i][0] == 0 && c[i][0] == 0)
      continue;
    printf("%-20s  %-20s  %s\n", a[i], b[i], c[i]);
  }
}

static void
print_year(int y)
{
  char title[32];

  snprintf(title, sizeof(title), "%d", y);
  int width = 64;
  int pad = (width - (int)strlen(title)) / 2;
  printf("%*s%s\n\n", pad, "", title);
  for (int m = 1; m <= 12; m += 3) {
    print_row_of_three(y, m, m + 1, m + 2, 0);
    if (m + 3 <= 12)
      printf("\n");
  }
}

int
main(int argc, char *argv[])
{
  int year = 0, month = 0, whole_year = 0, three = 0, i = 1;

  // today, for the highlight and for the default
  time_t now = time(0);
  struct tm tm;
  gmtime_r(&now, &tm);
  today_y = tm.tm_year + 1900;
  today_m = tm.tm_mon + 1;
  today_d = tm.tm_mday;

  for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
    for (const char *p = argv[i] + 1; *p; p++) {
      switch (*p) {
      case 'm': monday_first = 1; break;
      case 's': monday_first = 0; break;
      case 'y': whole_year = 1; break;
      case '3': three = 1; break;
      case '1': three = 0; break;
      case 'j': break;   // day of the year; not printed here
      default:
        fprintf(stderr, "usage: cal [-m] [-y] [-1|-3] [[MONTH] YEAR]\n");
        exit(1);
      }
    }
  }

  int rest = argc - i;
  if (rest == 1) {
    year = atoi(argv[i]);
    whole_year = 1;
  } else if (rest == 2) {
    month = atoi(argv[i]);
    year = atoi(argv[i + 1]);
  } else if (rest > 2) {
    fprintf(stderr, "usage: cal [-m] [-y] [-1|-3] [[MONTH] YEAR]\n");
    exit(1);
  }

  if (year == 0) {
    year = today_y;
    month = today_m;
    highlight_today = isatty(1);
  }
  if (year < 1 || year > 9999) {
    fprintf(stderr, "cal: year %d is out of range\n", year);
    exit(1);
  }
  if (!whole_year && (month < 1 || month > 12)) {
    fprintf(stderr, "cal: %d is not a month\n", month);
    exit(1);
  }

  if (whole_year) {
    print_year(year);
  } else if (three) {
    int m = month, y = year;
    int pm = m - 1, py = y, nm = m + 1, ny = y;
    if (pm < 1) {
      pm = 12;
      py--;
    }
    if (nm > 12) {
      nm = 1;
      ny++;
    }
    // three months that may not be in the same year, so each keeps
    // its own title
    char a[8][32], b[8][32], c[8][32];
    render(py, pm, a, 1);
    render(y, m, b, 1);
    render(ny, nm, c, 1);
    for (int k = 0; k < 8; k++)
      printf("%-20s  %-20s  %s\n", a[k], b[k], c[k]);
  } else {
    print_month(year, month);
  }
  exit(0);
}

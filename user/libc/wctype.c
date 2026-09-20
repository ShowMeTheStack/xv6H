//
// <wctype.h>: the character classes and the case mappings for wide
// characters.
//
// below 128 this is <ctype.h> in the C locale, exactly, because POSIX
// requires the two to agree. above it the answers come from the
// Unicode general categories, and here is the honest statement of how
// far the tables go:
//
//   - case mapping and the upper/lower classes cover Latin-1
//     Supplement and Latin Extended-A, U+00C0 through U+017F, with the
//     two mappings that leave the block (U+0130, U+0131, U+017F) done
//     by hand. above U+017F no case mapping is claimed: towupper of a
//     Greek or Cyrillic letter returns it unchanged.
//   - iswalpha knows the Latin blocks in full and, above them, a list
//     of whole blocks that contain letters (Greek, Cyrillic, Hebrew,
//     Arabic, the kana, CJK, Hangul and so on). that is a block
//     approximation, not the real Alphabetic property.
//   - iswprint and iswgraph above Latin Extended-A are answered from
//     wcwidth(), which knows which characters are drawn; iswpunct is
//     then "drawn, and not a letter or a digit", which is how the C
//     locale defines it and which happens to put the symbols where a
//     caller expects them.
//
// a real table of the Unicode categories is some thousands of ranges.
// when this system grows something that needs it, it should be
// generated from UnicodeData.txt rather than written out here.
//
#include <wctype.h>
#include <wchar.h>
#include <ctype.h>
#include <string.h>

struct wrange {
  unsigned int lo, hi;
};

static int
inranges(const struct wrange *t, size_t n, unsigned int c)
{
  size_t lo = 0, hi = n;

  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (c < t[mid].lo)
      hi = mid;
    else if (c > t[mid].hi)
      lo = mid + 1;
    else
      return 1;
  }
  return 0;
}

// whole blocks whose members are letters. the bounds are the printable
// extent of each block, so a few marks and block-internal punctuation
// come along; see the note at the top of the file.
static const struct wrange letterblocks[] = {
  {0x0180, 0x024f}, // Latin Extended-B
  {0x0250, 0x02af}, // IPA extensions
  {0x0370, 0x03ff}, // Greek and Coptic
  {0x0400, 0x052f}, // Cyrillic and its supplement
  {0x0531, 0x058f}, // Armenian
  {0x05d0, 0x05ea}, // Hebrew
  {0x0620, 0x064a}, // Arabic
  {0x0710, 0x072f}, // Syriac
  {0x0905, 0x0939}, // Devanagari
  {0x0e01, 0x0e30}, // Thai
  {0x10a0, 0x10ff}, // Georgian
  {0x1e00, 0x1fff}, // Latin Extended Additional, Greek Extended
  {0x2c60, 0x2c7f}, // Latin Extended-C
  {0x3041, 0x3096}, // Hiragana
  {0x30a1, 0x30fa}, // Katakana
  {0x3400, 0x4dbf}, // CJK unified ideographs extension A
  {0x4e00, 0x9fff}, // CJK unified ideographs
  {0xa720, 0xa7ff}, // Latin Extended-D
  {0xac00, 0xd7a3}, // Hangul syllables
  {0xf900, 0xfaff}, // CJK compatibility ideographs
  {0x20000, 0x2a6df}, // CJK unified ideographs extension B
};

// the space characters outside ASCII: the Unicode Zs, Zl and Zp
// categories, minus U+00A0 and U+202F, which are no-break and which
// the C locale therefore does not call space.
static const struct wrange spaceblocks[] = {
  {0x0085, 0x0085}, {0x1680, 0x1680}, {0x2000, 0x200a},
  {0x2028, 0x2029}, {0x205f, 0x205f}, {0x3000, 0x3000},
};

#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

// Latin Extended-A alternates case in runs, and the runs do not all
// start on the same parity; these two say which half of a pair a code
// point is, for the runs where the even member is the capital.
static int
evenupper(unsigned int c)
{
  return (c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177);
}

static int
oddupper(unsigned int c)
{
  return (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e);
}

int
iswupper(wint_t c)
{
  if (c < 0x80)
    return isupper((int)c);
  if (c >= 0xc0 && c <= 0xde && c != 0xd7) // A-grave through Thorn, minus the times sign
    return 1;
  if (c == 0x178) // Y with diaeresis, whose small form is U+00FF
    return 1;
  if (evenupper(c))
    return !(c & 1);
  if (oddupper(c))
    return (c & 1) != 0;
  return 0;
}

int
iswlower(wint_t c)
{
  if (c < 0x80)
    return islower((int)c);
  if (c == 0xb5 || c == 0xdf) // micro sign, sharp s: lower with no capital here
    return 1;
  if (c >= 0xe0 && c <= 0xff && c != 0xf7)
    return 1;
  if (c == 0x138 || c == 0x149 || c == 0x17f) // kra, apostrophe-n, long s
    return 1;
  if (evenupper(c))
    return (c & 1) != 0;
  if (oddupper(c))
    return !(c & 1);
  return 0;
}

int
iswalpha(wint_t c)
{
  if (c < 0x80)
    return isalpha((int)c);
  if (c == 0xaa || c == 0xba) // the feminine and masculine ordinals are letters
    return 1;
  if (c < 0x100)
    return iswupper(c) || iswlower(c);
  if (c <= 0x17f)
    return 1; // Latin Extended-A is letters throughout
  return inranges(letterblocks, NELEM(letterblocks), c);
}

int
iswdigit(wint_t c)
{
  // POSIX is explicit: only the ten ASCII digits, whatever other
  // decimal digits Unicode has.
  return c >= '0' && c <= '9';
}

int
iswxdigit(wint_t c)
{
  return c < 0x80 && isxdigit((int)c);
}

int
iswalnum(wint_t c)
{
  return iswalpha(c) || iswdigit(c);
}

int
iswcntrl(wint_t c)
{
  return c < 0x20 || (c >= 0x7f && c <= 0x9f);
}

int
iswspace(wint_t c)
{
  if (c < 0x80)
    return isspace((int)c);
  return inranges(spaceblocks, NELEM(spaceblocks), c);
}

int
iswblank(wint_t c)
{
  // a blank separates words within a line, so only these two: the
  // Unicode spaces above are line-breaking or fullwidth.
  return c == ' ' || c == '\t';
}

int
iswprint(wint_t c)
{
  // a character is printable when a terminal gives it room, which is
  // what wcwidth() already decides; the null character is the one
  // value wcwidth calls zero-width but which is not printable.
  return c != 0 && (wint_t)c <= 0x10ffff && wcwidth((wchar_t)c) >= 0;
}

int
iswgraph(wint_t c)
{
  return iswprint(c) && !iswspace(c) && c != ' ';
}

int
iswpunct(wint_t c)
{
  return iswgraph(c) && !iswalnum(c);
}

wint_t
towlower(wint_t c)
{
  if (c < 0x80)
    return (wint_t)tolower((int)c);
  if (c >= 0xc0 && c <= 0xde && c != 0xd7)
    return c + 0x20;
  if (c == 0x130) // capital I with dot above: its small form is plain i
    return 'i';
  if (evenupper(c))
    return c | 1;
  if (oddupper(c))
    return (c & 1) ? c + 1 : c;
  if (c == 0x178)
    return 0xff;
  return c;
}

wint_t
towupper(wint_t c)
{
  if (c < 0x80)
    return (wint_t)toupper((int)c);
  if (c >= 0xe0 && c <= 0xfe && c != 0xf7)
    return c - 0x20;
  if (c == 0xff)
    return 0x178;
  if (c == 0x131) // dotless i capitalises to plain I
    return 'I';
  if (c == 0x17f) // long s capitalises to S
    return 'S';
  if (evenupper(c))
    return c & ~1u;
  if (oddupper(c))
    return (c & 1) ? c : c - 1;
  return c;
}

// ---- the classes by name ----

// the order here is the value wctype() hands back, so it must not be
// rearranged without changing iswctype below with it.
static const char *const classnames[] = {
  "alnum", "alpha", "blank", "cntrl", "digit", "graph",
  "lower", "print", "punct", "space", "upper", "xdigit",
};

wctype_t
wctype(const char *name)
{
  for (size_t i = 0; i < NELEM(classnames); i++)
    if (strcmp(name, classnames[i]) == 0)
      return i + 1;
  return 0; // an unknown class, which iswctype always answers no to
}

int
iswctype(wint_t c, wctype_t type)
{
  switch (type) {
  case 1: return iswalnum(c);
  case 2: return iswalpha(c);
  case 3: return iswblank(c);
  case 4: return iswcntrl(c);
  case 5: return iswdigit(c);
  case 6: return iswgraph(c);
  case 7: return iswlower(c);
  case 8: return iswprint(c);
  case 9: return iswpunct(c);
  case 10: return iswspace(c);
  case 11: return iswupper(c);
  case 12: return iswxdigit(c);
  default: return 0;
  }
}

wctrans_t
wctrans(const char *name)
{
  if (strcmp(name, "tolower") == 0)
    return 1;
  if (strcmp(name, "toupper") == 0)
    return 2;
  return 0;
}

wint_t
towctrans(wint_t c, wctrans_t trans)
{
  if (trans == 1)
    return towlower(c);
  if (trans == 2)
    return towupper(c);
  return c; // an unknown mapping is the identity, as the standard says
}

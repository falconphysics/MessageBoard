// Native test harness for the text helpers in MessageBoard.ino.
// Compiles the same algorithms against a minimal Arduino String shim so the
// wrap logic and UTF-8 folding can be checked without hardware.

#include <string>
#include <cstdio>
#include <cstdint>
#include <cassert>

struct String : std::string {
    String() {}
    String(const char* s) : std::string(s) {}
    String(const std::string& s) : std::string(s) {}
    String(char c) : std::string(1, c) {}
    String(int v) : std::string(std::to_string(v)) {}
    int indexOf(char c, int from) const {
        auto p = find(c, from);
        return p == npos ? -1 : (int)p;
    }
    String substring(int a) const { return String(substr(a)); }
    String substring(int a, int b) const { return String(substr(a, b - a)); }
    void remove(int i) { erase(i); }
    void reserve(size_t n) { std::string::reserve(n); }
    String& operator+=(const char* s) { std::string::operator+=(s); return *this; }
    String& operator+=(char c) { std::string::operator+=(c); return *this; }
    String& operator+=(const String& s) { std::string::operator+=(s); return *this; }
};
static String operator+(const String& a, const String& b) { return String((std::string)a + (std::string)b); }
static String operator+(const String& a, const char* b) { return String((std::string)a + b); }
static String operator+(const String& a, char b) { return String((std::string)a + b); }

// Stand-in for the font metric: assume ~18px per character at 18pt.
static const int CHAR_W = 18;
int16_t textWidth(const String& s) { return (int16_t)(s.length() * CHAR_W); }

// ---- the real functions, extracted from MessageBoard.ino --------------
// run_text_test.sh generates text_body.inc, so this always tests the
// shipping code rather than a copy that can drift.
#include "text_body.inc"

// ---- tests ----------------------------------------------------------------

int failures = 0;

void check(bool cond, const char* what) {
  printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) failures++;
}

void showWrap(const char* label, const char* text, int maxWidth, int maxLines) {
  String lines[8];
  int n = wrapText(String(text), maxWidth, maxLines, lines);
  printf("\n%s  (width %d px = %d chars, max %d lines)\n", label, maxWidth, maxWidth / CHAR_W, maxLines);
  for (int i = 0; i < n; i++) {
    printf("   |%s|  (%d chars)\n", lines[i].c_str(), (int)lines[i].length());
    check((int)lines[i].length() * CHAR_W <= maxWidth, "line fits in width");
  }
  if (n == 0) printf("   (no lines)\n");
}

int main() {
  printf("=== UTF-8 folding ===\n");
  struct { const char* in; const char* want; } cases[] = {
    { "Don\xE2\x80\x99t forget!",          "Don't forget!" },
    { "\xE2\x80\x9CHello\xE2\x80\x9D",     "\"Hello\"" },
    { "Dinner \xE2\x80\x94 6pm",           "Dinner - 6pm" },
    { "wait\xE2\x80\xA6",                  "wait..." },
    { "Ren\xC3\xA9" "e and Jos\xC3\xA9",   "Renee and Jose" },
    { "Love you \xF0\x9F\x92\x99",         "Love you " },
    { "plain ascii",                       "plain ascii" },
    // Red heart is U+2764 + U+FE0F, both inside 16 bits: must not become "??"
    { "Love you \xE2\x9D\xA4\xEF\xB8\x8F",  "Love you " },
    { "Sunny \xE2\x98\x80 day",            "Sunny  day" },
    { "Done \xE2\x9C\x85",                 "Done " },
    // Family emoji: people joined by U+200D zero-width joiners
    { "Hi \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9", "Hi " },
  };
  for (auto& c : cases) {
    String got = toDisplayable(String(c.in));
    bool ok = (std::string)got == std::string(c.want);
    printf("  %s  %-28s -> |%s|\n", ok ? "ok  " : "FAIL", c.in, got.c_str());
    if (!ok) { printf("        expected |%s|\n", c.want); failures++; }
  }

  printf("\n=== URL encoding of the secret ===\n");
  struct { const char* in; const char* want; } enc[] = {
    { "abcXYZ019-_.~",   "abcXYZ019-_.~" },
    { "a&b=c",           "a%26b%3Dc" },
    { "p#ss word+1",     "p%23ss%20word%2B1" },
    { "100%",            "100%25" },
  };
  for (auto& c : enc) {
    String got = urlEncode(c.in);
    bool ok = (std::string)got == std::string(c.want);
    printf("  %s  %-16s -> |%s|\n", ok ? "ok  " : "FAIL", c.in, got.c_str());
    if (!ok) { printf("        expected |%s|\n", c.want); failures++; }
  }

  printf("\n=== word wrap ===\n");
  showWrap("normal sentence", "Grandma says dinner is at six on Sunday, don't be late", 700, 4);
  showWrap("single long word", "Supercalifragilisticexpialidocious", 200, 4);
  showWrap("truncation needed", "This message is far too long to fit in the space available on the panel so it must be cut off cleanly", 400, 2);
  showWrap("exact fit", "abc def", 7 * CHAR_W, 4);
  showWrap("empty string", "", 700, 4);
  showWrap("one word", "Hi", 700, 4);
  showWrap("double spaces", "a  b   c", 700, 4);

  printf("\n=== termination (no infinite loops) ===\n");
  String lines[8];
  wrapText(String("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 1, 4, lines);
  check(true, "zero-width line does not hang");
  wrapText(String("word another"), 10 * CHAR_W, 0, lines);
  check(true, "maxLines=0 does not hang");

  printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
  return failures ? 1 : 0;
}

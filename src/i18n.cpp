#include "i18n.h"

#include <string.h>

#include "lang/en.h"
#include "lang/pt.h"
#include "lang/es.h"
#include "lang/ru.h"
#include "lang/zh.h"
#include "lang/ja.h"
#include "lang/tr.h"

struct LangPack {
  const char* code;
  const char* const* text;
};

static const LangPack kLangs[] = {
    {"en", kEn},
    {"pt", kPt},
    {"es", kEs},
    {"ru", kRu},
    {"zh", kZh},
    {"ja", kJa},
    {"tr", kTr},
};

static const char* const* gText = kEn;

const char* tr(Msg id) {
  if ((unsigned)id >= MSG_COUNT) return "";
  const char* s = gText[id];
  if (!s || !s[0]) s = kEn[id];
  return s ? s : "";
}

void useLang(const char* code) {
  gText = kEn;
  if (!code) return;
  for (unsigned i = 0; i < sizeof(kLangs) / sizeof(kLangs[0]); i++) {
    if (strcmp(code, kLangs[i].code) == 0) {
      gText = kLangs[i].text;
      return;
    }
  }
}

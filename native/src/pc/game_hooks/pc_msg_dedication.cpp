// WakerQuest's dedication (pc_msg_override, f_op_msg_mng.cpp fopMsgM_msgGet_c::getMessage): a few
// messages get one more page of our own text, at run time. The game's text comes from the player's
// disc and is not changed; only these pages are ours. COS_DEDICATORIA=0 turns it off.
//
// Message numbers (the same in every language of GZLP01; the text is Spanish):
//   2003  the grandmother, end of her first talk (the green clothes on the birthday)
//   2007  an Outset islander praising the green clothes
//   10873 "Veo que te interesa esta imagen..."
//   3514  Tingle's goodbye in his tower ("¡Hasta la vista, duende!")
#include "d/dolzel.h" // IWYU pragma: keep

#include <cstdlib>
#include <cstring>

namespace {

struct Extra {
    u32 msgNo;
    const char* page;  // Latin-1, up to four lines
};

const Extra kExtras[] = {
    {2003, "Ah, y antes de que se me olvide...\n\xA1Saludos a Shaak4, nerestraw\ny compa\xF1\xED" "a!"},
    {2007, "\xBFNecesitas mucho espacio para\njugar en VR? Aqu\xED en la isla\ntenemos todo el mar."},
    {10873, "Dicen que en un canal llamado\nShaak4 VR ense\xF1" "an a vivir\nla magia desde dentro."},
    {3514, "\xA1La gente odia la VR!\n\xA1Industria retr\xF3grada!\nKurul\xFAn... \xA1PA!"},
};

constexpr int kExtraCount = (int)(sizeof(kExtras) / sizeof(kExtras[0]));
char sBuf[kExtraCount][2048];
const char* sFor[kExtraCount] = {};

bool enabled() {
    static const bool on = [] {
        const char* v = getenv("COS_DEDICATORIA");
        return !(v != nullptr && v[0] == '0');
    }();
    return on;
}

} // namespace

// The message with our page after its last one; the original when it is not one of ours. The text's
// control codes (0x1A, their length, their bytes) are copied as they are; a page holds four lines.
const char* pc_msg_override(u32 msgNo, const char* text) {
    if (text == nullptr || !enabled()) {
        return text;
    }
    for (int i = 0; i < kExtraCount; i++) {
        if (kExtras[i].msgNo != msgNo) {
            continue;
        }
        if (sFor[i] == text) {
            return sBuf[i];  // already built for this text
        }
        char* out = sBuf[i];
        const size_t cap = sizeof(sBuf[i]);
        size_t n = 0;
        int newlines = 0;
        const unsigned char* p = (const unsigned char*)text;
        while (*p != 0 && n + 1 < cap) {
            if (*p == 0x1A) {
                const unsigned len = p[1];
                if (len < 2 || n + len >= cap) {
                    return text;  // a control code we cannot copy: leave the message alone
                }
                memcpy(out + n, p, len);
                n += len;
                p += len;
                continue;
            }
            if (*p == '\n') {
                newlines++;
            }
            out[n++] = (char)*p++;
        }
        // the last page's lines, then empty lines up to the next page
        const int lastPageLines = newlines % 4 + 1;
        const size_t extra = strlen(kExtras[i].page);
        if (n + (5 - lastPageLines) + extra + 1 > cap) {
            return text;
        }
        for (int k = 0; k < 5 - lastPageLines; k++) {
            out[n++] = '\n';
        }
        memcpy(out + n, kExtras[i].page, extra);
        n += extra;
        out[n] = '\0';
        sFor[i] = text;
        return out;
    }
    return text;
}

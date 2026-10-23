// WakerQuest's dedication (pc_msg_override, f_op_msg_mng.cpp fopMsgM_msgGet_c::getMessage): a few
// messages get one more page of our own text, at run time. The game's text comes from the player's
// disc and is not changed; only these pages are ours. COS_DEDICATORIA=0 turns it off.
//
// Every message the game shows asks fopMsgM_msgGet_c::getMessage for its text (an NPC's talk:
// fopNpc_npc_c::talk -> fopMsgM_messageSet -> fopMsgM_messageTypeSelect, which only checks that the
// message exists, then the message process d_msg/d_message/d_message_paper/d_scope asks for it when it
// opens and at each following message). msgNo is the BMG's message number (zel_00.bmg's INF1 entry,
// group 0), the same in every language of GZLP01; the text is Spanish. Not here: the menus' texts
// (fopMsgM_itemMsgGet_c) and the cutscenes' messages (d_mesg, JMessage::TControl, by BMG entry index).
//
//   grandmother (daNpc_Ba1_c, at home in LinkRM and on the birthday): 2003, 2007, 2008, 2020, 2021,
//     2032, 2045
//   10873 "Veo que te interesa esta imagen..."
//   3514  Tingle's goodbye in his tower ("¡Hasta la vista, duende!")
// Each page is shown once per game session (the first time its message opens). Every message shown
// is logged ("[cos] msg 2008"), to find more of them.
#include "d/dolzel.h" // IWYU pragma: keep
#include "pc_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

struct Extra {
    u32 msgNo;
    const char* page;  // Latin-1, up to four lines
};

const Extra kExtras[] = {
    {2003, "Ah, y antes de que se me olvide...\n\xA1Saludos a Shaak4, nerestraw\ny compa\xF1\xED" "a!"},
    {2007, "\xBFNecesitas mucho espacio para\njugar en VR? Aqu\xED en la isla\ntenemos todo el mar."},
    {10873, "Dicen que en un canal llamado\nShaak4 VR ense\xF1" "an a vivir\nla magia desde dentro."},
    {2008, "Por cierto, ha venido un tal\nShaak4 preguntando por ti.\nDice que te espera en YouTube."},
    {2020, "\xA1" "Ah! Y saludos de mi parte a\nShaak4, nerestraw y compa\xF1\xED" "a.\nSon buena gente."},
    {2021, "Abril dice que los v\xED" "deos de\nShaak4 VR son lo mejor\ndel mundo."},
    {2032, "\xA1Saludos a Shaak4, nerestraw\ny compa\xF1\xED" "a!"},
    {2045, "\xA1Saludos a Shaak4, nerestraw\ny compa\xF1\xED" "a, de parte\nde tu abuela!"},
    {3514, "\xA1La gente odia la VR!\n\xA1Industria retr\xF3grada!\nKurul\xFAn... \xA1PA!"},
};

constexpr int kExtraCount = (int)(sizeof(kExtras) / sizeof(kExtras[0]));
char sBuf[kExtraCount][2048];
bool sShown[kExtraCount] = {};  // this session

bool enabled() {
    static const bool on = [] {
        const char* v = getenv("COS_DEDICATORIA");
        return !(v != nullptr && v[0] == '0');
    }();
    return on;
}

} // namespace

// The message with our page after its last one, the first time it is shown this session; else the
// original. The text's control codes (0x1A, their length, their bytes) are copied as they are; a page
// holds four lines.
const char* pc_msg_override(u32 msgNo, const char* text) {
    pc::writef(STDERR_FILENO, "[cos] msg %u%s\n", (unsigned)msgNo, text == nullptr ? " (none)" : "");
    if (text == nullptr || !enabled()) {
        return text;
    }
    for (int i = 0; i < kExtraCount; i++) {
        if (kExtras[i].msgNo != msgNo) {
            continue;
        }
        if (sShown[i]) {
            return text;
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
        if (*p != 0) {
            return text;  // longer than the buffer
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
        sShown[i] = true;
        pc::writef(STDERR_FILENO, "[cos] msg %u: dedication page added\n", (unsigned)msgNo);
        return out;
    }
    return text;
}

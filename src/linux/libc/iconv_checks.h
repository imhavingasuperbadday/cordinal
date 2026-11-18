// cordinal's iconv (iconv.cpp) against glibc's, for checks.h: every encoding pair the table knows,
// converting the same bytes both ways, then the error answers (EILSEQ on an invalid byte, E2BIG on a
// full output, EINVAL on input cut short), the byte-order forms, //TRANSLIT's substitutions, and
// iconv_open's answers for the names it knows and one it does not.
#pragma once
#include <dlfcn.h>
#include <errno.h>
#include <iconv.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace libc_checks {

namespace iconv_detail {
struct Outcome {
    size_t result;
    int error;
    size_t consumed;
    size_t produced;
    char text[128];
};

inline void Convert(bool glibc, void* cd, const char* input, size_t count, size_t room, Outcome& outcome) {
    char bytes[128];
    memcpy(bytes, input, count);
    char output[128];
    memset(output, 0, sizeof output);
    char* at = bytes;
    char* written = output;
    size_t left = count, roomLeft = room;
    errno = 0;
    outcome.result = glibc ? ((size_t(*)(void*, char**, size_t*, char**, size_t*))dlsym(RTLD_DEFAULT, "iconv"))(
                                 cd, &at, &left, &written, &roomLeft)
                           : iconv(cd, &at, &left, &written, &roomLeft);
    outcome.error = errno;
    outcome.consumed = (size_t)(at - bytes);
    outcome.produced = (size_t)(written - output);
    memset(outcome.text, 0, sizeof outcome.text);
    memcpy(outcome.text, output, outcome.produced < sizeof outcome.text ? outcome.produced : sizeof outcome.text - 1);
}
}  // namespace iconv_detail

inline void CheckIconv() {
    using namespace iconv_detail;
    auto glibcOpen = (void* (*)(const char*, const char*))dlsym(RTLD_DEFAULT, "iconv_open");
    auto glibcClose = (int (*)(void*))dlsym(RTLD_DEFAULT, "iconv_close");
    auto glibcConvert = (size_t(*)(void*, char**, size_t*, char**, size_t*))dlsym(RTLD_DEFAULT, "iconv");
    if (!glibcOpen || !glibcClose || !glibcConvert) {
        Expect(false, "glibc's iconv is found");
        return;
    }
    Expect(InProgram((const void*)&iconv) && InProgram((const void*)&iconv_open) &&
               InProgram((const void*)&iconv_close) && !InProgram((const void*)glibcConvert),
           "iconv, iconv_open and iconv_close are cordinal's own");

    static const struct {
        const char* to;
        const char* from;
    } kPairs[] = {{"UTF-8", "UTF-8"},         {"UTF-32LE", "UTF-8"},     {"UTF-8", "UTF-32LE"},
                  {"UTF-16LE", "UTF-8"},      {"UTF-8", "UTF-16LE"},     {"UTF-16BE", "UTF-8"},
                  {"UTF-8", "UTF-16BE"},      {"ISO-8859-1", "UTF-8"},   {"UTF-8", "ISO-8859-1"},
                  {"CP1252", "UTF-8"},        {"UTF-8", "CP1252"},       {"ASCII", "UTF-8"},
                  {"ASCII", "ISO-8859-1"},    {"WCHAR_T", "UTF-8"},      {"UTF-8", "WCHAR_T"},
                  {"UTF-32BE", "UTF-16LE"},   {"latin1", "UTF-8"},       {"utf8", "windows-1252"}};
    static const struct {
        const char* bytes;
        size_t length;
    } kInputs[] = {{"", 0},
                   {"abc", 3},
                   {"a\xc3\xa9z", 4},
                   {"caf\xc3\xa9", 5},
                   {"\xe2\x82\xac", 3},
                   {"\xf0\x9f\x98\x80", 4},
                   {"\xe2\x80\x9c\xe2\x80\x9d", 6},
                   {"\xc2\xa0", 2},
                   {"\xc2\xa9", 2},
                   {"\xe2\x84\xa2", 3},
                   {"\xd9\xac", 2},
                   {"\xff", 1},
                   {"\xc3", 1},
                   {"\xed\xa0\x80", 3},
                   {"\xf4\x90\x80\x80", 4},
                   {"\x81", 1},
                   {"\x00", 1},
                   {"a\x00b", 3}};
    for (const auto& pair : kPairs) {
        void* mine = iconv_open(pair.to, pair.from);
        void* theirs = glibcOpen(pair.to, pair.from);
        if (theirs == (void*)-1) {  // glibc has no module for it here: nothing to compare
            if (mine != (void*)-1) iconv_close(mine);
            continue;
        }
        Expect(mine != (void*)-1, "iconv_open answers for every pair glibc knows");
        if (mine == (void*)-1) {
            glibcClose(theirs);
            continue;
        }
        for (const auto& input : kInputs) {
            for (int round = 0; round < 2; ++round) {
                const size_t room = round == 0 ? 64 : 1;
                Outcome mineOutcome{}, theirOutcome{};
                Convert(false, mine, input.bytes, input.length, room, mineOutcome);
                Convert(true, theirs, input.bytes, input.length, room, theirOutcome);
                if (mineOutcome.result != theirOutcome.result || mineOutcome.error != theirOutcome.error ||
                    mineOutcome.consumed != theirOutcome.consumed || mineOutcome.produced != theirOutcome.produced ||
                    memcmp(mineOutcome.text, theirOutcome.text, theirOutcome.produced)) {
                    if (failures < 40)
                        fprintf(stderr,
                                "FAIL iconv %s <- %s, %zu bytes, room %zu: %zd errno %d used %zu made %zu, glibc %zd "
                                "errno %d used %zu made %zu\n",
                                pair.to, pair.from, input.length, room, (ssize_t)mineOutcome.result,
                                mineOutcome.error, mineOutcome.consumed, mineOutcome.produced,
                                (ssize_t)theirOutcome.result, theirOutcome.error, theirOutcome.consumed,
                                theirOutcome.produced);
                    ++failures;
                }
            }
        }
        iconv_close(mine);
        glibcClose(theirs);
    }

    // The reset form, //TRANSLIT's substitutions, and the names.
    {
        void* cd = iconv_open("ASCII//TRANSLIT", "UTF-8");
        void* theirCd = glibcOpen("ASCII//TRANSLIT", "UTF-8");
        Expect(cd != (void*)-1 && theirCd != (void*)-1, "ASCII//TRANSLIT opens from UTF-8");
        if (cd != (void*)-1 && theirCd != (void*)-1) {
            static const char* const kSubstitutions[] = {"\xe2\x80\xaf", "\xe2\x80\x99", "\xe2\x80\x9c",
                                                        "\xe2\x80\x93", "\xe2\x80\xa6", "\xc3\xa9",
                                                        "\xc2\xa9", "\xc2\xae", "\xe2\x84\xa2",
                                                        "\xe2\x82\xac", "\xc2\xb5", "\xc3\x86",
                                                        "\xc3\x9f", "\xd9\xac", "\xc2\xa0"};
            for (const char* text : kSubstitutions) {
                Outcome mineOutcome{}, theirOutcome{};
                Convert(false, cd, text, strlen(text), 32, mineOutcome);
                Convert(true, theirCd, text, strlen(text), 32, theirOutcome);
                if (mineOutcome.result != theirOutcome.result || mineOutcome.error != theirOutcome.error ||
                    mineOutcome.produced != theirOutcome.produced ||
                    memcmp(mineOutcome.text, theirOutcome.text, theirOutcome.produced)) {
                    if (failures < 40)
                        fprintf(stderr, "FAIL translit [%s]: [%s] (%zd) against glibc [%s] (%zd)\n", text,
                                mineOutcome.text, (ssize_t)mineOutcome.result, theirOutcome.text,
                                (ssize_t)theirOutcome.result);
                    ++failures;
                }
            }
            Expect(iconv(cd, nullptr, nullptr, nullptr, nullptr) == 0, "the reset form answers 0");
        }
        if (cd != (void*)-1) iconv_close(cd);
        if (theirCd != (void*)-1) glibcClose(theirCd);
    }
    {
        static const char* const kNames[] = {"UTF-8",    "utf8",   "ASCII",   "US-ASCII", "ANSI_X3.4-1968",
                                            "ISO-8859-1", "latin1", "l1",    "cp819",    "iso_8859-1",
                                            "CP1252",   "WINDOWS-1252", "ms-ansi", "UTF-16LE", "UTF16LE",
                                            "UTF-16BE", "UTF-32LE", "utf-32be", "WCHAR_T",  "ucs-4"};
        for (const char* name : kNames) {
            const bool mineTo = iconv_open(name, "UTF-8") != (void*)-1;
            const bool mineFrom = iconv_open("UTF-8", name) != (void*)-1;
            const bool theirTo = glibcOpen(name, "UTF-8") != (void*)-1;
            const bool theirFrom = glibcOpen("UTF-8", name) != (void*)-1;
            if (mineTo != theirTo || mineFrom != theirFrom) {
                if (failures < 40)
                    fprintf(stderr, "FAIL iconv_open name %s: to %d from %d, glibc to %d from %d\n", name, mineTo,
                            mineFrom, theirTo, theirFrom);
                ++failures;
            }
        }
        errno = 0;
        Expect(iconv_open("bogus", "UTF-8") == (void*)-1 && errno == EINVAL,
               "an encoding it does not know is EINVAL");
        errno = 0;
        Expect(iconv_open("UTF-8", "bogus") == (void*)-1 && errno == EINVAL,
               "an encoding it does not know is EINVAL the other way");
    }
    // A conversion whose output is too small keeps the input on the character, as glibc's does.
    {
        void* cd = iconv_open("UTF-8", "UTF-8");
        char input[] = "\xc3\xa9";
        char output[1];
        char* at = input;
        char* written = output;
        size_t left = 2, room = 1;
        errno = 0;
        const size_t result = iconv(cd, &at, &left, &written, &room);
        Expect(result == (size_t)-1 && errno == E2BIG && at == input && left == 2 && written == output,
               "E2BIG leaves the input on the character");
        iconv_close(cd);
    }
}

}  // namespace libc_checks

/*
    MIT License

    Copyright (c) 2026 Mikhael Khrustik

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
 */

#ifndef HUE_H
#define HUE_H

#define HUE_ESC   "\x1b["
#define HUE_RESET "\x1b[0m"

/* concat */

#define HUE_PASTE(a, b) HUE_PASTE_IMPL(a, b)
#define HUE_PASTE_IMPL(a, b) a##b

/* argument count */

#define HUE_NARG(...)    HUE_NARG_(__VA_ARGS__, HUE_RSEQ_N())
#define HUE_NARG_(...)   HUE_ARG_N(__VA_ARGS__)

#define HUE_ARG_N( \
        _1, _2, _3, _4, _5, _6, _7, _8, _9,_10,  \
        _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, \
        _21,_22,_23,_24,_25,_26,_27,_28,_29,_30, \
        _31,_32,_33,_34,_35,_36,_37,_38,_39,_40, \
        _41,_42,_43,_44,_45,_46,_47,_48,_49,_50, \
        _51,_52,_53,_54,_55,_56,_57,_58,_59,_60, \
        _61,_62,_63,N,...) N

#define HUE_RSEQ_N() \
        63,62,61,60,                   \
        59,58,57,56,55,54,53,52,51,50, \
        49,48,47,46,45,44,43,42,41,40, \
        39,38,37,36,35,34,33,32,31,30, \
        29,28,27,26,25,24,23,22,21,20, \
        19,18,17,16,15,14,13,12,11,10, \
        9,8,7,6,5,4,3,2,1,0

/* map */

#define HUE_MAP_1(M, a)                         M(a)
#define HUE_MAP_2(M, a, b)                      M(a) M(b)
#define HUE_MAP_3(M, a, b, c)                   M(a) M(b) M(c)
#define HUE_MAP_4(M, a, b, c, d)                M(a) M(b) M(c) M(d)
#define HUE_MAP_5(M, a, b, c, d, e)             M(a) M(b) M(c) M(d) M(e)
#define HUE_MAP_6(M, a, b, c, d, e, f)          M(a) M(b) M(c) M(d) M(e) M(f)
#define HUE_MAP_7(M, a, b, c, d, e, f, g)       M(a) M(b) M(c) M(d) M(e) M(f) M(g)
#define HUE_MAP_8(M, a, b, c, d, e, f, g, h)    M(a) M(b) M(c) M(d) M(e) M(f) M(g) M(h)

#define HUE_MAP(M, ...) \
    HUE_MAP_DISPATCH(HUE_NARG(__VA_ARGS__), M, __VA_ARGS__)

#define HUE_MAP_DISPATCH(N, M, ...) \
    HUE_MAP_DISPATCH_IMPL(N, M, __VA_ARGS__)

#define HUE_MAP_DISPATCH_IMPL(N, M, ...) \
    HUE_PASTE(HUE_MAP_, N)(M, __VA_ARGS__)

/* foreground colors */

#define HUE_FG_BLACK   HUE_ESC "30m"
#define HUE_FG_RED     HUE_ESC "31m"
#define HUE_FG_GREEN   HUE_ESC "32m"
#define HUE_FG_YELLOW  HUE_ESC "33m"
#define HUE_FG_BLUE    HUE_ESC "34m"
#define HUE_FG_MAGENTA HUE_ESC "35m"
#define HUE_FG_CYAN    HUE_ESC "36m"
#define HUE_FG_WHITE   HUE_ESC "37m"

#define HUE_FG_BRIGHT_RED     HUE_ESC "91m"
#define HUE_FG_BRIGHT_GREEN   HUE_ESC "92m"
#define HUE_FG_BRIGHT_BLUE    HUE_ESC "94m"
#define HUE_FG_BRIGHT_YELLOW  HUE_ESC "93m"
#define HUE_FG_BRIGHT_MAGENTA HUE_ESC "95m"
#define HUE_FG_BRIGHT_CYAN    HUE_ESC "96m"
#define HUE_FG_BRIGHT_WHITE   HUE_ESC "97m"
#define HUE_FG_BRIGHT_BLACK   HUE_ESC "90m"

/* styles */

#define HUE_BOLD          HUE_ESC "1m"
#define HUE_DIM           HUE_ESC "2m"
#define HUE_ITALIC        HUE_ESC "3m"
#define HUE_UNDERLINE     HUE_ESC "4m"
#define HUE_BLINK         HUE_ESC "5m"
#define HUE_REVERSE       HUE_ESC "7m"
#define HUE_HIDDEN        HUE_ESC "8m"
#define HUE_STRIKETHROUGH HUE_ESC "9m"

/* token -> macro */

#define HUE_FG_CODE(color) \
    HUE_PASTE(HUE_FG_, color)

#define HUE_STYLE_CODE(style) \
    HUE_PASTE(HUE_, style)

/*
 * hue_fg("%s", RED)
 * ->
 * HUE_FG_RED "%s" HUE_RESET
 *
 * hue_fg("%s", RED, BOLD, UNDERLINE)
 * ->
 * HUE_FG_RED HUE_BOLD HUE_UNDERLINE "%s" HUE_RESET
 */
#define hue_fg(text, color, ...) \
    HUE_FG_CODE(color) __VA_OPT__(HUE_MAP(HUE_STYLE_CODE, __VA_ARGS__)) text HUE_RESET

#endif

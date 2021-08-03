/*
    Decompress a tiff strip that has LZW Prediction compression. Tiff files are composed
    of strips, which have a defined number of rows (lines of pixels in the image). The
    tiff image may have 1 to many strips. The strip is an array of bytes RGBRGB... In
    this case we want to decode the first strip in the tiff file.

    The compressed file is lzw.tif.  The offset and length of the first strip has been
    predefined.  The same image has been saved as an uncompressed tiff called base.tif.
    We can use this to check our decompression of lzw is correct.

    The algorithm to decompress LZW (from TIFF6 Specification):

    while ((Code = GetNextCode()) != EoiCode) {
        if (Code == ClearCode) {
            InitializeTable();
            Code = GetNextCode();
            if (Code == EoiCode)
                break;
            WriteString(StringFromCode(Code));
            OldCode = Code;
        } // end of ClearCode case
        else {
            if (IsInTable(Code)) {
                WriteString(StringFromCode(Code));
                AddStringToTable(StringFromCode(OldCode)+FirstChar(StringFromCode(Code)));
                OldCode = Code;
            } else {
                OutString = StringFromCode(OldCode) + FirstChar(StringFromCode(OldCode));
                WriteString(OutString);
                AddStringToTable(OutString);
                OldCode = Code;
            }
        } // end of not-ClearCode case
    } // end of while loop

    The prediction variant uses the difference between row pixels for the code value.

    This is my third version.  The first version input a stream, overloaded the >>
    operator, and used string and QByteArray functions.  It took 130 ms to decompress
    the strip.  The second version ditched the >> overloading and the stream, working
    directly with the QByteArrays, and took 80ms.  The third run (this one) elimimates
    all string and QByteArray functions, using byte arrays and pointers, running in
    3.3 ms.

    Things that have not helped:

    - do basic lzw decompression and make a second pass to do prediction increment.
    - use pointer instead or array indice to output char string for code.

    Alternatives to std::memcpy

        // option for byte-by-byte

        char* pDst = s[nextCode];
        char* pSrc = (char*)&ps;
        for (size_t i = 0; i != psLen; ++i) pDst[i] = pSrc[i];

        // or

        char* pDst = s[nextCode];
        char* pSrc = (char*)&ps;
        size_t len = psLen;
        while (len--)
        {
            *pDst++ = *pSrc++;
        }

        // option for 4 bytes at a time

        uint32_t* pDst = (uint32_t*)s[nextCode];
        uint32_t* pSrc = (uint32_t*)&ps;
        size_t len = psLen / 4 + 1;
        for (size_t i = 0; i != len; ++i) pDst[i] = pSrc[i];


*/

#include <QDebug>

#include <chrono>
#include <vector>
#include <array>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <algorithm>

const unsigned int CLEAR_CODE = 256;
const unsigned int EOF_CODE = 257;
const unsigned int MAXCODE = 4095;      // 12 bit max less some head room

// Enter info for base and lwz tiff files
///* D:/Pictures/_TIFF_lzw1/lzwP_8.tif LZW Predictive working
const std::string base = "D:/Pictures/_TIFF_lzw1/base_8.tif";
const std::string lzw  = "D:/Pictures/_TIFF_lzw1/lzwP_8.tif";
const uint32_t lzwOffsetToFirstStrip = 34312;
const uint32_t lzwLengthFirstStrip = 123177;
const uint32_t lzwRowsPerStrip = 109;
const uint32_t baseOffsetToFirstStrip = 34296;
const uint32_t basedLengthFirstStrip = 1080000;
const int bytesPerRow = 2400;
const bool predictor = true;
//*/

/* D:/Pictures/_TIFF_lzw1/lzw_8.tif LZW nonPredictive working
const std::string base = "D:/Pictures/_TIFF_lzw1/base_8.tif";
const std::string lzw  = "D:/Pictures/_TIFF_lzw1/lzw_8.tif";
const uint32_t lzwOffsetToFirstStrip = 17232;
const uint32_t lzwLengthFirstStrip = 14950;
const uint32_t lzwRowsPerStrip = 8;
const uint32_t baseOffsetToFirstStrip = 34296;
const uint32_t basedLengthFirstStrip = 1080000;
const int bytesPerRow = 2400;
const bool predictor = false;
//*/

/* D:/Pictures/_TIFF_lzw1/lzw_16.tif LZW nonPredictive working
const std::string base = "D:/Pictures/_TIFF_lzw1/base_16.tif";
const std::string lzw  = "D:/Pictures/_TIFF_lzw1/lzw_16.tif";
const uint32_t lzwOffsetToFirstStrip = 17250;
const uint32_t lzwLengthFirstStrip = 24092;
const uint32_t lzwRowsPerStrip = 8;
const uint32_t baseOffsetToFirstStrip = 34004;
const uint32_t basedLengthFirstStrip = 2160000;
const int bytesPerRow = 4800;
const bool predictor = false;
//*/


std::vector<char> baseFirstStrip(basedLengthFirstStrip);
const int bytesPerStrip = lzwRowsPerStrip * bytesPerRow;

void byteArrayToHex(std::vector<char> v, int cols, unsigned long start, unsigned long end)
{
    int n = 0;
    for (unsigned long i = start; i < start + end; i++) {
        int x = (0xff & v[i]);
        std::cout << std::hex << std::uppercase << x << " ";
        if (++n % cols == 0) std::cout << " " << std::dec << n + (int)start << '\n';
    }
    std::cout << '\n';
}

#define LZW_STRING_SIZE 256
#define LZW_STRINGS_SIZE 128000

bool decompressLZW(std::vector<char> &inBa, std::vector<char> &outBa)
/*
    Works for RGB but not for RRGGBB (planarConfiguration = 2).  Requires tweak to pBuf.
*/
{
    bool ret = false;
    // input and output pointers
    char* c = inBa.data();
    char* out = outBa.data();

    char* s[4096];                                  // ptrs in strings for each possible code
    int8_t sLen[4096];                              // code string length
    std::memset(&sLen, 1, 256);                     // 0-255 one char strings

    char strings[LZW_STRINGS_SIZE];
    // initialize first 256 code strings
    for (int i = 0 ; i != 256 ; i++ ) {
        strings[i] = (char)i;
        s[i] = &strings[i];
    }
    strings[256] = 0;  s[256] = &strings[256];      // Clear code
    strings[257] = 0;  s[257] = &strings[257];      // EOF code
    const uint32_t maxCode = 4095;                  // max for 12 bits
    char* sEnd;                                     // ptr to current end of strings

    char ps[LZW_STRING_SIZE];                       // previous string
    size_t psLen = 0;                               // length of prevString
    uint32_t code;                                  // key to string for code
    uint32_t nextCode = 258;                        // used to preset string for next
    uint32_t incoming = (uint32_t)inBa.size();      // count down input chars
    int n = 0;                                      // output byte counter
    uint32_t iBuf = 0;                              // incoming bit buffer
    int32_t nBits = 0;                              // incoming bits in the buffer
    int32_t codeBits = 9;                           // number of bits to make code (9-12)
    uint32_t nextBump = 511;                        // when to increment code size 1st time
    uint32_t pBuf = 0;                              // previous out bit buffer
    uint32_t mask = (1 << codeBits) - 1;            // extract code from iBuf

    uint32_t* pSrc;                                 // ptr to src for word copies
    uint32_t* pDst;                                 // ptr to dst for word copies

    // read incoming bytes into the bit buffer (iBuf) using the char pointer c
    while (incoming) {
//        if (n > 8995) {
//            int xxx = 0;
//        }
        // GetNextCode
        iBuf = (iBuf << 8) | (uint8_t)*c++;         // make room in bit buf for char
        nBits += 8;
        --incoming;
        if (nBits < codeBits) {
            iBuf = (iBuf << 8) | (uint8_t)*c++;     // make room in bit buf for char
            nBits += 8;
            --incoming;
        }
        code = (iBuf >> (nBits - codeBits)) & mask; // extract code from buffer
        nBits -= codeBits;                          // update available bits to process

        // rest at start and when codes = max ~+ 4094
        if (code == CLEAR_CODE) {
            codeBits = 9;
            mask = (1 << codeBits) - 1;
            nextBump = 511;
            sEnd = s[257];
            nextCode = 258;
            psLen = 0;
            continue;
        }

        // finished (should not need as incoming counts down to zero)
        if (code == EOF_CODE) {
            return ret;
        }

        // new code then add prevString + prevString[0]
        // copy prevString
        if (code == nextCode) {
            s[code] = sEnd;
            switch(psLen) {
            case 1:
                *s[code] = ps[0];
                break;
            case 2:
                *s[code] = ps[0];
                *(s[code]+1) = ps[1];
                break;
            case 4:
                pDst = (uint32_t*)s[code];
                pSrc = (uint32_t*)&ps;
                *pDst = *pSrc;
                break;
            case 5:
            case 6:
            case 7:
            case 8:
                pDst = (uint32_t*)s[nextCode];
                pSrc = (uint32_t*)&ps;
                *pDst = *pSrc;
                *(pDst+1) = *(pSrc+1);
                break;
            default:
                std::memcpy(s[code], &ps, psLen);
            }

            // copy prevString[0]
            *(s[code] + psLen) = ps[0];
            sLen[code] = (int8_t)psLen + 1;
            sEnd = s[code] + psLen + 1;
        }

        if (predictor) {
            for (uint32_t i = 0; i != (uint32_t)sLen[code]; i++) {
                if (n % bytesPerRow < 3) *out++ = *(s[code] + i);
                else *out++ = *(s[code] + i) + *(out - 3);
                ++n;
                /*
                // output char string for code (add from left)
                // pBuf   00000000 11111111 22222222 33333333
                if (n % bytesPerRow == 0) pBuf = 0;
                char b = *(s[code] + i) + (uint8_t)(pBuf & 0xFF);
                *out++ = b;
                pBuf = (pBuf >> 8) | (uint32_t)((uint8_t)b << 16);
                ++n;
                */
            }
        }
        else {
            for (uint32_t i = 0; i != (uint32_t)sLen[code]; i++) {
                uchar b = (uchar)*(s[code] + i);
                *out++ = *(s[code] + i);
                if (n == 26400) {
                    int xxx = 0;
                }
                ++n;
            }
        }

        // add string to nextCode (prevString + strings[code][0])
        // copy prevString
        if (psLen/* && nextCode <= MAXCODE*/) {
            s[nextCode] = sEnd;
            switch(psLen) {
            case 1:
                *s[nextCode] = ps[0];
                break;
            case 2:
                *s[nextCode] = ps[0];
                *(s[nextCode]+1) = ps[1];
                break;
            case 4:
                pDst = (uint32_t*)s[nextCode];
                pSrc = (uint32_t*)&ps;
                *pDst = *pSrc;
                break;
            case 5:
            case 6:
            case 7:
            case 8:
                pDst = (uint32_t*)s[nextCode];
                pSrc = (uint32_t*)&ps;
                *pDst = *pSrc;
                *(pDst+1) = *(pSrc+1);
                break;
            default:
                std::memcpy(s[nextCode], &ps, psLen);
            }

            // copy strings[code][0]
            *(s[nextCode] + psLen) = *s[code];

            sLen[nextCode] = (int8_t)(psLen + 1);
            sEnd = s[nextCode] + psLen + 1;
            ++nextCode;
        }

        // strings[code][0] copy
        switch(sLen[code]) {
        case 1:
            ps[0] = *s[code];
            break;
        case 2:
            ps[0] = *s[code];
            ps[1] = *(s[code]+1);
            break;
        case 4:
            pSrc = (uint32_t*)s[code];
            pDst = (uint32_t*)&ps;
            *pDst = *pSrc;
            break;
        case 5:
        case 6:
        case 7:
        case 8:
            pSrc = (uint32_t*)s[code];
            pDst = (uint32_t*)&ps;
            *pDst = *pSrc;
            *(pDst+1) = *(pSrc+1);
            break;
        default:
            memcpy(&ps, s[code], (size_t)sLen[code]);
        }

        psLen = (size_t)sLen[code];

         // codeBits change
        if (nextCode == nextBump) {
            if (nextCode < maxCode) {
                nextBump = (nextBump << 1) + 1;
                ++codeBits;
                mask = (1 << codeBits) - 1;
            }
            else if (nextCode == maxCode) continue;
            else {
                codeBits = 9;
                mask = (1 << codeBits) - 1;
                nextBump = 511;
                sEnd = s[257];
                nextCode = 258;
                psLen = 0;
            }
        }

//        if (n == 7000) break;

    } // end while}

    return ret;
}

int main()
{
//    std::ifstream f1("D:/Pictures/_TIFF_lzw1/lzw.tif", std::ios::in | std::ios::binary | std::ios::ate);
    std::ifstream f1(lzw, std::ios::in | std::ios::binary | std::ios::ate);
    std::vector<char> lzwFirstStrip(lzwLengthFirstStrip);
    f1.seekg(lzwOffsetToFirstStrip);
    f1.read(lzwFirstStrip.data(), lzwFirstStrip.size());
    f1.close();

    // load the "answer" from the same image, saved as an uncompressed tif.  We will
    // use this to confirm our decompression of lzw.tiff is correct
//    std::ifstream f2("D:/Pictures/_TIFF_lzw1/base.tif", std::ios::in | std::ios::binary | std::ios::ate);
    std::ifstream f2(base, std::ios::in | std::ios::binary | std::ios::ate);
    f2.seekg(baseOffsetToFirstStrip);
    f2.read(baseFirstStrip.data(), baseFirstStrip.size());
    f2.close();

    // Create the byte array to hold the decompressed byte stream
    std::vector<char> ba(basedLengthFirstStrip);

    std::string title = "LWZ without prediction";
    int choice = 1;

    int repeat;
    int runs;
    if (choice == 0) {
        repeat = 1;
        runs = 1;
    }
    else {
        repeat = 5;
        runs = 10000;
    }
    //*/
    double ms;
    double msSum = 0;
    double mp;
    int pixels;
    double mpPerSec;
    bool isErr;
    std::chrono::time_point<std::chrono::system_clock> start, end;

    // decompressLZW
    std::cout << title << '\n';
    for (int j = 0; j < repeat; ++j) {
        start = std::chrono::system_clock::now();
        for (int i = 0; i < runs; ++i) {
            decompressLZW(lzwFirstStrip, ba);  //  3.2 - 3.8 ms per run on Rory macbookpro
        }
        end = std::chrono::system_clock::now();

        ms = (double)std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        msSum += ms;
        ms /= (1000 * runs);
        pixels = 261600 / 3;
        mp = (double)pixels / 1000000;
        mpPerSec = mp / ms * 1000;                        // megapixels / sec

        std::cout
             << "decompressLZW " << std::setw(6) << j + 1
             << "   runs: " << std::setw(6) << runs
             << std::fixed << std::showpoint << std::setprecision(2)
             << "   ms/run: " << ms
             << "   mp/sec: " << mpPerSec
             << '\n';
    }

    double msAve = msSum / (repeat * runs * 1000);
    std::cout << std::setw(46) << "Average: " << msAve << '\n';

    // check result
    isErr = false;
    for (uint32_t i = 0; i < bytesPerStrip; i++) {
//        if (ba[i] != baseFirstStrip[i]) {
        int a = ba[i] & 0xFF;
        int b = baseFirstStrip[i] & 0xFF;
        int diff = std::abs(a - b);
        if (diff > 2) {
            std::cout << "error at " << i << "  diff = " << diff << '\n';
            isErr = true;
            break;
        }
    }
    if (!isErr) std::cout << "No errors." << '\n' << '\n';

    // helper report
    std::cout << "decompressLZW:" << '\n';
    byteArrayToHex(ba, 25, 0, 50);
    std::cout << "base:" << '\n';
    byteArrayToHex(baseFirstStrip, 25, 0, 50);

    // pause if running executable in terminal
    std::cout << "Paused, press ENTER to continue." << std::endl;
    std::cin.ignore();
    exit(0);
}

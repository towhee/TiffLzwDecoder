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
const unsigned int MAXCODE = 4093;      // 12 bit max less some head room

const uint32_t lzwOffsetToFirstStrip = 34312;
const uint32_t lzwLengthFirstStrip = 123177;
const uint32_t uncompressedOffsetToFirstStrip = 34296;
const uint32_t uncompressedLengthFirstStrip = 1080000;
const uint32_t lzwFirstStripDecompressedLength = 261600;
std::vector<char> baseFirstStrip(uncompressedLengthFirstStrip);

const int bytesPerRow = 2400;
const int bytesPerStrip = 109 * bytesPerRow;

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

extern uint32_t codeMask[32];
uint32_t codeMask[32];
void buildCodeMask()
{
    for (int i = 0; i < 32; i++) codeMask[i] = (1 << i) - 1;
}
//#define DICT_STRING_SIZE 32
//#define DICT_SIZE  DICT_STRING_SIZE * 4096   //131072
//const uint32_t mask = (1 << 9) - 1;

#define LZW_STRING_SIZE 256
#define LZW_STRINGS_SIZE 128000

void decompressLZW1(std::vector<char> &inBa, std::vector<char> &outBa)
/*
    Works for RGB but not for RRGGBB (planarConfiguration = 2).  Requires tweak to pBuf.
*/
{

}

bool decompressLZW(std::vector<char> &inBa, std::vector<char> &outBa)
/*
    Works for RGB but not for RRGGBB (planarConfiguration = 2).  Requires tweak to pBuf.
*/
{
    bool ret = false;
    // input and output pointers
    char* c = inBa.data();
    char* out = outBa.data();
//    char dict[LZW_DICT_SIZE]; // OLD

    // NEW
    // array of pointers to the string for each possible code
    char* s[4096];
    char strings[LZW_STRINGS_SIZE];
//    std::vector<char> strings[LZW_DICT_SIZE];
    // initialize code strings
    for (int i = 0 ; i != 256 ; i++ ) {
        strings[i] = (char)i;
        s[i] = &strings[i];
    }
    strings[256] = 0;  s[256] = &strings[256];
    strings[257] = 0;  s[257] = &strings[257];
    char* sEnd;

    int8_t sLen[4096];                              // code string length
    std::memset(&sLen, 1, 256);                     // 0-255 one char strings
    char ps[LZW_STRING_SIZE];                       // previous string
    size_t psLen = 0;                               // length of prevString
    uint32_t code;                                   // offset in dict (code * DICT_STRING_SIZE)
    uint32_t nextCode = 258;
    unsigned long incoming = 0;                     // incoming counter
    unsigned long inBaLen = (unsigned long)inBa.size();
    int n = 0;                                      // code counter
    uint32_t iBuf = 0;                              // bit buffer
    int32_t nBits = 0;                              // bits in the buffer
    int32_t codeBits = 9;                           // number of bits to make code
    uint32_t nextBump = 511;                        // when to increment code size 1st time
    uint32_t pBuf = 0;                              // previous out bit buffer
    uint32_t mask = (1 << codeBits) - 1;

    // read incoming bytes into the bit buffer (iBuf) using the char pointer c
    while (incoming < inBaLen) {
        // GetNextCode
        iBuf = (iBuf << 8) | (uint8_t)*c++;         // make room in bit buf for char
        nBits += 8;
        ++incoming;
        if (nBits < codeBits) {
            iBuf = (iBuf << 8) | (uint8_t)*c++;     // make room in bit buf for char
            nBits += 8;
            ++incoming;
        }
        code = (iBuf >> (nBits - codeBits)) & mask; // extract code from buffer
        nBits -= codeBits;                          // update available bits to process

        if (code == CLEAR_CODE) {
            codeBits = 9;
            mask = (1 << codeBits) - 1;
            nextBump = 511;
            sEnd = s[257];
            nextCode = 258;
            psLen = 0;
            continue;
        }

        if (code == EOF_CODE) {
            return ret;
        }

        // if code not found then add prevString + prevString[0]
        if (code == nextCode) {
            s[code] = sEnd;
            std::memcpy(s[code], &ps, (size_t)psLen);
            std::memcpy(s[code] + (size_t)psLen, &ps, 1);
            sLen[code] = (int8_t)psLen + 1;
            sEnd = s[code] + psLen + 1;
        }

        // output char string for code (add from left)
        // pBuf   00000000 11111111 22222222 33333333
        for (uint32_t i = 0; i != (uint32_t)sLen[code]; i++) {
            if (n % bytesPerRow == 0) pBuf = 0;
            char b = *(s[code] + i) + (uint8_t)(pBuf & 0xFF);
            *out++ = b;
            pBuf = (pBuf >> 8) | (uint32_t)((uint8_t)b << 16);
            ++n;
        }

        // add string to nextCode (prevString + strings[code][0])
        if (psLen/* && nextCode <= MAXCODE*/) {
            s[nextCode] = sEnd;
            std::memcpy(s[nextCode], &ps, psLen);
            std::memcpy(s[nextCode] + psLen, s[code], (size_t)sLen[code]);
            sLen[nextCode] = (int8_t)(psLen + 1);
            sEnd = s[nextCode] + psLen + 1;
            ++nextCode;
        }
        // prevString = dictionary[code];
        memcpy(&ps, s[code], (size_t)sLen[code]);
        psLen = (size_t)sLen[code];

        if (nextCode == nextBump) {
            nextBump = (nextBump << 1) + 1;
            ++codeBits;
            mask = (1 << codeBits) - 1;
        }

    } // end while}

    return ret;
}

int main()
{
    std::ifstream f1("D:/Pictures/_TIFF_lzw1/lzw.tif", std::ios::in | std::ios::binary | std::ios::ate);
    std::vector<char> lzwFirstStrip(lzwLengthFirstStrip);
    f1.seekg(lzwOffsetToFirstStrip);
    f1.read(lzwFirstStrip.data(), lzwFirstStrip.size());
    f1.close();

    // load the "answer" from the same image, saved as an uncompressed tif.  We will
    // use this to confirm our decompression of lzw.tiff is correct
    std::ifstream f2("D:/Pictures/_TIFF_lzw1/base.tif", std::ios::in | std::ios::binary | std::ios::ate);
//    std::vector<char> baseFirstStrip(uncompressedLengthFirstStrip);
    f2.seekg(uncompressedOffsetToFirstStrip);
    f2.read(baseFirstStrip.data(), baseFirstStrip.size());
    f2.close();

    // Create the byte array to hold the decompressed byte stream
    std::vector<char> ba(uncompressedLengthFirstStrip);

//    // Prebuild a bit mask that removes consumed code from the bit buffer
    buildCodeMask();

    /*
    int repeat = 1;
    int runs = 1;
    //*/
//    /*
    int repeat = 10;
    int runs = 10000;
    //*/
    double ms;
    double msSum = 0;
    double mp;
    int pixels;
    double mpPerSec;
    bool isErr;
    std::chrono::time_point<std::chrono::system_clock> start, end;

//    /* decompressLZW
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

//        if (ms >= 2.0) continue;

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
    for (uint32_t i = 0; i < lzwFirstStripDecompressedLength; i++) {
        if (ba[i] != baseFirstStrip[i]) {
            std::cout << "error at " << i << '\n';
            isErr = true;
            break;
        }
    }
    if (!isErr) std::cout << "No errors." << '\n' << '\n';
    //*/

    /* decompressLZW1

    start = std::chrono::system_clock::now();
    for (int i = 0; i < runs; i++) {
        decompressLZW1(lzwFirstStrip, ba);  //  3.2 - 3.8 ms per run on Rory macbookpro
    }
    end = std::chrono::system_clock::now();

    ms = (double)std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    ms /= (1000 * runs);
    pixels = 261600 / 3;
    mp = (double)pixels / 1000000;
    mpPerSec = mp / ms * 1000;                        // megapixels / sec

    std::cout << std::fixed << std::showpoint << std::setprecision(2)
              << "decompressLZW1   runs: " << runs
              << "   ms: " << ms
              << "   mp/sec: " << mpPerSec
              << "    ";

    // check result
    isErr = false;
    for (uint32_t i = 0; i < lzwFirstStripDecompressedLength; i++) {
        if (ba[i] != baseFirstStrip[i]) {
            std::cout << "error at " << i << '\n';
            isErr = true;
            break;
        }
    }
    if (!isErr) std::cout << "No errors." << '\n';
    // */

    // helper report
//    std::cout << "decompressLZW1:" << '\n';
//    byteArrayToHex(ba, 25, 0, 50);
//    std::cout << "base:" << '\n';
//    byteArrayToHex(baseFirstStrip, 25, 0, 50);

    // pause if running executable in terminal
    std::cout << "Paused, press ENTER to continue." << std::endl;
    std::cin.ignore();
    exit(0);
}

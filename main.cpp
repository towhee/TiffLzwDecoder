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

*/

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

void byteArrayToHex(std::vector<char> v, int cols, unsigned long start, unsigned long end)
{
    int n = 0;
    std::cout << std::setw(2) << '\n';
    for (unsigned long i = start; i < start + end; i++) {
        auto x = (0xff & v[i]);
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

#define DICT_STRING_SIZE 32
#define DICT_SIZE  DICT_STRING_SIZE * 4096   //131072

void decompressLZW(std::vector<char> &inBa, std::vector<char> &outBa)
/*
    Works for RGB but not for RRGGBB (planarConfiguration = 2).  Requires tweak to prev.
*/
{
    char* c = inBa.data();
    char* out = outBa.data();
    int bytesPerRow = 2400;                         // remove in Winnnow code
    // dictionary has 4096 (12 bit max) items of 32 bytes (max string length)
    char dict[DICT_SIZE];                           // 4096 * 32
//    char* d = dict;
    int8_t sLen[4096];                              // dictionary code string length
    std::memset(&sLen, 1, 256);                     // default string length = 1 for 1st 256
    char ps[DICT_STRING_SIZE];                      // previous string
    size_t psLen = 0;                               // length of prevString
    int32_t code;                                   // offset in dict (code * DICT_STRING_SIZE)
    int32_t nextCode = 258;
    unsigned long incoming = 0;                     // incoming counter
    int n = 0;                                      // code counter
    // bit management
    int32_t pending = 0;                            // bit buffer
    int32_t availBits = 0;                          // bits in the buffer
    int32_t codeBits = 9;                           // number of bits to make code
    int32_t currCode = 257;                         // start code
    int32_t nextBump = 512;                         // when to increment code size 1st time
    // previous out bit buffer
    uint32_t pBuf = 0;

    // read incoming bytes into the bit buffer (pending) using the char pointer c
    while (incoming < inBa.size()) {
        // GetNextCode
        while (availBits < codeBits) {
            pending = (pending<<8) | (uint8_t)*c;   // make room in bit buf for char
            availBits += 8;
            c++;
            incoming++;
        }
        code = pending >> (availBits - codeBits);   // extract code from buffer
        availBits -= codeBits;                      // update available
        pending &= codeMask[availBits];             // apply mask to zero consumed code bits

        if (code == CLEAR_CODE) {
            codeBits = 9;
            currCode = 257;
            nextBump = 512;
            // reset dictionary
            for (uint i = 0 ; i < 256 ; i++ ) {
                dict[i * DICT_STRING_SIZE] = (char)i;
            }
            nextCode = 258;
            // clear prevString
            std::memset(ps, 0, DICT_STRING_SIZE);
            psLen = 0;
            continue;
        }

        if (code == EOF_CODE) {
            break;
        }

        currCode++;
        if (currCode == nextBump - 1) {
            nextBump <<= 1;                     // nextBump *= 2
            codeBits++;
        }

//        char* dOff = d + code * DICT_STRING_SIZE;
        uint32_t off = (uint32_t)code * DICT_STRING_SIZE;

        // if code not found then add to dictionary
        if (code >= nextCode) {
            //  dictionary[code] = prevString + prevString[0];
            std::memcpy(&dict[off], &ps, psLen);
            std::memcpy(&dict[off + psLen], &ps, 1);
            sLen[code] = (int8_t)psLen + 1;
        }

        /* No performance improvement
        // output char string for code (add from left)
        // pBuf   00000000 11111111 22222222 33333333
        int i = sLen[code];
        while (i > 0) {
            // if end of row reset prev pixel rgb
            if (n % bytesPerRow == 0) pBuf = 0;
            // string char = code string element + value of previous pixel r/g/b
            char b = *(dOff++) + (char)(pBuf & 0xFF);
            *out = b;
            out++;
            pBuf = (pBuf >> 8) | (uint32_t)((uint8_t)b << 16);
            n++;
            i--;
        }
        //*/

        // output char string for code (add from left)
        // pBuf   00000000 11111111 22222222 33333333
        for (uint32_t i = 0; i < (uint32_t)sLen[code]; i++) {
            // if end of row reset prev pixel rgb
            if (n % bytesPerRow == 0) pBuf = 0;
            // string char = code string element + value of 3rd previous pixel r/g/b
            char b = dict[off + i] + (char)(pBuf & 0xFF);
            *out = b;
            out++;
            pBuf = (pBuf >> 8) | (uint32_t)((uint8_t)b << 16);
            n++;
        }

        // in dictionary, add string
        if (psLen && nextCode <= MAXCODE) {
            // dictionary[nextCode++] = prevString + dictionary[code][0];
            std::memcpy(&dict[nextCode * DICT_STRING_SIZE], &ps, psLen);
            std::memcpy(&dict[nextCode * DICT_STRING_SIZE + psLen], &dict[off], (size_t)sLen[code]);
            sLen[nextCode] = (int8_t)(psLen + 1);
            nextCode++;
        }
        // prevString = dictionary[code];
        memcpy(&ps, &dict[off], (size_t)sLen[code]);
        psLen = (size_t)sLen[code];
    }
}

int main()
{
    std::ifstream f1("/Users/roryhill/Pictures/_TIFF_lzw1/lzw.tif", std::ios::in | std::ios::binary | std::ios::ate);
    std::vector<char> lzwFirstStrip(lzwLengthFirstStrip);
    f1.seekg(lzwOffsetToFirstStrip);
    f1.read(lzwFirstStrip.data(), lzwFirstStrip.size());
    f1.close();

    // load the "answer" from the same image, saved as an uncompressed tif.  We will
    // use this to confirm our decompression of lzw.tiff is correct
    std::ifstream f2("/Users/roryhill/Pictures/_TIFF_lzw1/base.tif", std::ios::in | std::ios::binary | std::ios::ate);
    std::vector<char> baseFirstStrip(uncompressedLengthFirstStrip);
    f2.seekg(uncompressedOffsetToFirstStrip);
    f2.read(baseFirstStrip.data(), baseFirstStrip.size());
    f2.close();

    // Create the byte array to hold the decompressed byte stream
    std::vector<char> ba(uncompressedLengthFirstStrip);

    // Prebuild a bit mask that removes consumed code from the bit buffer
    buildCodeMask();

    int runs = 1;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < runs; i++) {
        decompressLZW(lzwFirstStrip, ba);  //  3.2 - 3.8 ms per run on Rory macbookpro
    }
    auto end = std::chrono::steady_clock::now();

    double ms = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    ms /= (1000 * runs);
    int pixels = 261600 / 3;
    double mp = (double)pixels / 1000000;
    double mpPerSec = mp / ms * 1000;                        // megapixels / sec

    std::cout << std::fixed << std::showpoint << std::setprecision(2)
         << "runs: " << runs
         << "   ms: " << ms
         << "   mp/sec: " << mpPerSec
         << "    ";

    // check result
    bool isErr = false;
    for (int i = 0; i < 261600; i++) {
        if (ba[i] != baseFirstStrip[i]) {
            std::cout << "error at " << i << '\n';
            isErr = true;
            break;
        }
    }
    if (!isErr) std::cout << "No errors." << '\n';

    // helper report
//    byteArrayToHex(ba, 50, 0, 100);
//    byteArrayToHex(baseFirstStrip, 50, 0, 100);

    // pause if running executable in terminal
//    std::cout << "Paused, press ENTER to continue." << std::endl;
//    std::cin.ignore();
    exit(0);
}

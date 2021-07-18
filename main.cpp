/*
    Decompress a tiff strip that has LZW Prediction compression. Tiff files are composed
    of strips, which have a defined number of rows (lines of pixels in the image). The
    tiff image may have 1 to many strips. The strip is an array of bytes RGBRGB... In
    this case we want to decode the first strip in the tiff file.

    The compressed file is lzw.tif.  The offset and length of the first strip have been
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

void byteArrayToHex(std::vector<char> v, int cols, int start, int end)
{
    int n = 0;
    std::cout << std::setw(2) << std::endl;
    for (int i = start; i < start + end; i++) {
        uint8_t x = (0xff & (unsigned int)v[i]);
        std::cout << std::hex << std::uppercase << x << " ";
        if (++n % cols == 0) std::cout << " " << std::dec << n + start << std::endl;
    }
    std::cout << std::endl;
}


#define DICT_STRING_SIZE 32
#define DICT_SIZE  DICT_STRING_SIZE * 4096   //131072
/*
    Works for RGB but not for RRGGBB (planarConfiguration = 2).  Requires tweak to prev.
*/
void decompressLZW(std::vector<char> &inBa, std::vector<char> &outBa)
{
    char* c = inBa.data();
    char* out = outBa.data();
    int rowLength = 2400;
    // dictionary has 4096 (12 bit max) items of 32 bytes (max string length)
    char dict[DICT_SIZE];                           // 4096 * 32
//    char* d = dict;
    int8_t sLen[4096];                              // dictionary code string length
    std::memset(&sLen, 1, 4096);                    // default string length = 1
    char ps[DICT_STRING_SIZE];                      // previous string
    int psLen = 0;                                  // length of prevString
    // code is the offset in the dictionary array (code * 32)
    int32_t code;
    int32_t nextCode = 258;
    char prev[3];                                   // circ buffer of last three bytes
    std::memset(prev, 0, 3);
    int incoming = 0;                               // incoming counter
    int n = 0;                                      // code counter
    int m = 0;                                      // prev RGB counter
    // bit management
    int32_t pending = 0;                            // bit buffer
    int availBits = 0;                              // bits in the buffer
    int codeSize = 9;                               // number of bits to make code
    int32_t currCode = 257;                         // start code
    int32_t nextBump = 512;                         // when to increment code size

     // read incoming bytes into the bit buffer (pending) using the char pointer c
    while (incoming < inBa.size()) {
        // get code
        while (availBits < codeSize) {              // for example: codeSize = 9, Y = code
            pending <<= 8;                          // 00000000 00000000 00XXXXXX 00000000
            pending |= (*c & 0xff);                 // 00000000 00000000 00XXXXXX 00111011
            availBits += 8;                         //                     YYYYYY YYY
            c++;
            incoming++;
        }
        code = pending >> (availBits - codeSize);   // extract code from buffer
        availBits -= codeSize;                      // update available
        pending &= (1 << availBits) - 1;            // apply mask to zero consumed code bits

        if (code == CLEAR_CODE) {
            codeSize = 9;
            currCode = 257;
            nextBump = 512;
//            std::memset(dict, 0, DICT_SIZE);
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

        currCode++;
        if (currCode == nextBump - 1) {
            nextBump <<= 1;                     // nextBump *= 2
            codeSize++;
        }

        if (code == EOF_CODE) {
            break;
        }

//        char* dOff = d + code * 32;
        uint32_t off = code * DICT_STRING_SIZE;

        // if code not found then add to dictionary
        if (code >= nextCode) {
            //  dictionary[code] = prevString + prevString[0];
//            std::memcpy(dOff, &ps, psLen);
//            std::memcpy(dOff + psLen, &ps, 1);
            std::memcpy(&dict[off], &ps, psLen);
            std::memcpy(&dict[off + psLen], &ps, 1);
            sLen[code] = psLen + 1;
        }

         // output char string for code
        for (int i = 0; i < sLen[code]; i++) {
            // if end of row reset prev pixel rgb
            if (n % rowLength == 0) std::memset(prev, 0, 3);
            // string char = code string element + value of previous pixel r/g/b
            char b = dict[off + i] + prev[m];  // char b = *(d + i) + prev[m]; no speed increase
//            char b = *(dOff + i) + prev[m];
            prev[m] = b;
            *out = b;
            out++;
            switch (m) {
            case 2:
                m = 0;
                break;
            default:
                ++m;
            }
            n++;
        }

        // add nextCode to dictionary
        if (psLen && nextCode <= MAXCODE) {
            // dictionary[nextCode++] = prevString + dictionary[code][0];
            std::memcpy(&dict[nextCode * DICT_STRING_SIZE], &ps, psLen);
            std::memcpy(&dict[nextCode * DICT_STRING_SIZE + psLen], &dict[off], sLen[code]);
            sLen[nextCode] = psLen + 1;
            nextCode++;
        }
        // prevString = dictionary[code];
//        memcpy(&ps, dOff, sLen[code]);
        memcpy(&ps, &dict[off], sLen[code]);
        psLen = sLen[code];
    }
}

int main(int argc, char *argv[])
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

    int runs = 100;
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
            std::cout << "error at " << i << std::endl;
            isErr = true;
            break;
        }
    }
    if (!isErr) std::cout << "No errors" << std::endl;

    // helper report
//    byteArrayToHex(ba, 50, 0, 500);
//    byteArrayToHex(baseFirstStrip, 50, 0, 500);

    // pause if running executable
//    std::cout << "Paused, press ENTER to continue." << std::endl;
//    std::cin.ignore();
    exit(0);
}

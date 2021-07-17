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
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QBuffer>
#include <QDataStream>
#include <QDebug>

#include <algorithm>

const unsigned int CLEAR_CODE = 256;
const unsigned int EOF_CODE = 257;
const unsigned int MAXCODE = 4093;      // 12 bit max less some head room

QString lzwFile =          "/Users/roryhill/Pictures/_TIFF_lzw1/lzw.tif";
QString uncompressedFile = "/Users/roryhill/Pictures/_TIFF_lzw1/base.tif";

quint32 lzwOffsetToFirstStrip = 34312;
quint32 lzwLengthFirstStrip = 123177;
quint32 uncompressedOffsetToFirstStrip = 34296;
quint32 uncompressedLengthFirstStrip = 1080000;
QByteArray baBaseFirstStrip;

void byteArrayToHex(QByteArray &ba, int cols, int start, int end)
{
    int n = 0;
    QDebug debug = qDebug();
    debug.noquote();
    debug << "\n";
    for (int i = start; i < start + end; i++) {
        quint8 x = (0xff & (unsigned int)ba[i]);
        debug << QStringLiteral("%1").arg(x, 2, 16, QLatin1Char('0')).toUpper();
        if (++n % cols == 0) debug << " " << n + start << "\n";
    }
    debug << "\n";
}

#define DICT_SIZE  131072
void decompressLZW(QByteArray &inBa, QByteArray &outBa)
{
    char* c = inBa.data();
    char* out = outBa.data();
    int rowLength = 2400;
    // dictionary has 4096 (12 bit max) items of 32 bytes (max string length)
    char dict[DICT_SIZE];                           // 4096 * 32
    quint8 sLen[4096];                              // dictionary code string length
    std::memset(&sLen, 1, 4096);                    // default string length = 1
    char ps[32];                                    // previous string
    int psLen = 0;                                  // length of prevString
    // code is the offset in the dictionary array (code * 32)
    quint32 code;
    quint32 nextCode = 258;
    char prev[3];                                   // circ buffer of last three bytes
    std::memset(prev, 0, 3);
    int incoming = 0;                               // incoming counter
    int n = 0;                                      // code counter
    int m = 0;                                      // prev RGB counter
    // bit management
    quint32 pending = 0;                            // bit buffer
    int availBits = 0;                              // bits in the buffer
    int codeSize = 9;                               // number of bits to make code
    quint32 currCode = 257;                         // start code
    quint32 nextBump = 512;                         // when to increment code size

    // read incoming bytes into the bit buffer (pending) using the char pointer c
    while (incoming < inBa.length()) {
        // get code
        while (availBits < codeSize) {              // for example
            pending <<= 8;                          // 00000000 00000000 00XXXXXX 00000000
            pending |= (*c & 0xff);                 // 00000000 00000000 00XXXXXX 00111011
            availBits += 8;
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
            std::memset(dict, 0, DICT_SIZE);
            // reset dictionary
            for (uint i = 0 ; i < 256 ; i++ ) {
                dict[i*32] = (char)i;
            }
            nextCode = 258;
            // clear prevString
            std::memset(ps, 0, 32);
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

        // if code not found then add to dictionary
        if (code >= nextCode) {
            //  dictionary[code] = prevString + prevString[0];
            std::memcpy(&dict[code*32], &ps, psLen);
            std::memcpy(&dict[code*32 + psLen], &ps, 1);
            sLen[code] = psLen + 1;
        }

         // output char string for code
        for (int i = 0; i < sLen[code]; i++) {
            // if end of row reset prev pixel rgb
            if (n % rowLength == 0) std::memset(prev, 0, 3);
            // string char = code string element + value of previous pixel r/g/b
            char b = dict[code*32 + i] + prev[m];  // char b = *(d + i) + prev[m]; no speed increase
            prev[m] = b;
            *out = b;
            out++;
            m++;                    //  m < 2 ? m++ : m = 0;  no speed increase
            if (m > 2) m = 0;       //
            n++;
        }

        // add nextCode to dictionary
        if (psLen && nextCode <= MAXCODE) {
            // dictionary[nextCode++] = prevString + dictionary[code][0];
            std::memcpy(&dict[nextCode * 32], &ps, psLen);
            std::memcpy(&dict[nextCode * 32 + psLen], &dict[code*32], sLen[code]);
            sLen[nextCode] = psLen + 1;
            nextCode++;
        }
        // prevString = dictionary[code];
        memcpy(&ps, &dict[code*32], sLen[code]);
        psLen = sLen[code];
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    // load the file we want to decompress into a QByteArray
    QFile f(lzwFile);
    f.open(QIODevice::ReadOnly);
    f.seek(lzwOffsetToFirstStrip);
    QByteArray baLzw = f.read(lzwLengthFirstStrip);
    f.close();

    // load the "answer" from the same image, saved as an uncompressed tif.  We will
    // use this to confirm our decompression of lzw.tiff is correct
    QFile f1(uncompressedFile);
    f1.open(QIODevice::ReadOnly);
    f1.seek(uncompressedOffsetToFirstStrip);
    baBaseFirstStrip = f1.read(uncompressedLengthFirstStrip);
    f1.close();

    // Create the byte array to hold the decompressed byte stream
    QByteArray ba;
    ba.resize(uncompressedLengthFirstStrip);

    QElapsedTimer t;
    int runs = 1;
    t.start();
    for (int i = 0; i < runs; i++) {
        decompressLZW(baLzw, ba);  //  3.22 ms
    }

    qint64 nsecs = t.nsecsElapsed() / runs;
    int pixels = 261600 / 3;
    double mp = (double)pixels / 1024 / 1024;
    double ms = (double)nsecs / 1000000;
    double secs = (double)nsecs / 1000000000;
    double mpPerSec = mp / secs;
    qDebug() << "runs:" << runs
             << "  ms:" << ms
             << "  mp/sec:" << mpPerSec
                ;

    // check result
    bool isErr = false;
    for (int i = 0; i < 261600; i++) {
        if (ba[i] != baBaseFirstStrip[i]) {
            qDebug() << "error at" << i;
            isErr = true;
            break;
        }
    }
    if (!isErr) qDebug() << "No errors";
    f1.close();

    // helper report
//    byteArrayToHex(ba, 50, 0, 500);
//    byteArrayToHex(baBaseFirstStrip, 50, 0, 500);

    qDebug() << "Done.";
    exit(0);
}

#include <signal.h>

#include <iostream>
#include <set>
#include <map>
#include <memory>
#include <functional>

#include "av.h"
#include "ffmpeg.h"
#include "codec.h"
#include "packet.h"
#include "videorescaler.h"
#include "audioresampler.h"
#include "avutils.h"

// API2
#include "format.h"
#include "formatcontext.h"
#include "codec.h"
#include "codeccontext.h"
#include "videorescaler.h"

using namespace std;
using namespace av;

int main(int argc, char **argv)
{
    if (argc < 3)
        return 1;

    av::init();
    av::setFFmpegLoggingLevel(AV_LOG_DEBUG);

    string uri {argv[1]};
    string out {argv[2]};

    error_code ec;

    //
    // INPUT
    //
    FormatContext ictx;
    ssize_t      videoStream = -1;
    CodecContext vdec;
    Stream2      vst;

    int count = 0;

    ictx.openInput(uri, ec);
    if (ec) {
        cerr << "Can't open input\n";
        return 1;
    }

    ictx.findStreamInfo(ec);
    if (ec) {
        cerr << "Can't find streams: " << ec << ", " << ec.message() << endl;
        return 1;
    }
    
    for (size_t i = 0; i < ictx.streamsCount(); ++i) {
        auto st = ictx.stream(i);
        if (st.mediaType() == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            vst = st;
            break;
        }
    }

    if (vst.isNull()) {
        cerr << "Video stream not found\n";
        return 1;
    }

    if (vst.isValid()) {
        vdec = CodecContext(vst);
        vdec.setRefCountedFrames(true);

        cerr << "PTR: " << (void*)vdec.raw()->codec << endl;

        vdec.open(Codec(), ec);
        if (ec) {
            cerr << "Can't open decoder\n";
            return 1;
        }
    }


    //
    // OUTPUT
    //
    OutputFormat  ofrmt;
    FormatContext octx;

    ofrmt.setFormat("flv", out);
    octx.setFormat(ofrmt);

    Codec        ocodec  = findEncodingCodec(ofrmt);
    Stream2      ost     = octx.addStream(ocodec);
    CodecContext encoder {ost};

    // Settings
    encoder.setWidth(vdec.width() * 2);
    encoder.setHeight(vdec.height() * 2);
    encoder.setPixelFormat(vdec.pixelFormat());
    encoder.setTimeBase(Rational{1, 1000});
    encoder.setBitRate(vdec.bitRate());
    encoder.addFlags(octx.outputFormat().isFlags(AVFMT_GLOBALHEADER) ? CODEC_FLAG_GLOBAL_HEADER : 0);
    ost.setFrameRate(vst.frameRate());
    ost.setTimeBase(encoder.timeBase());

    octx.openOutput(out, ec);
    if (ec) {
        cerr << "Can't open output\n";
        return 1;
    }

    encoder.open(ec);
    if (ec) {
        cerr << "Can't opent encoder\n";
        return 1;
    }

    octx.dump();
    octx.writeHeader();
    octx.flush();

    //
    // RESCALER
    //
    VideoRescaler rescaler; // Rescaler will be inited on demaind


    //
    // PROCESS
    //
    while (true) {

        // READING
        Packet pkt = ictx.readPacket(ec);
        if (ec)
        {
            clog << "Packet reading error: " << ec << ", " << ec.message() << endl;
            break;
        }

        if (pkt.streamIndex() != videoStream) {
            continue;
        }

        clog << "Read packet: pts=" << pkt.pts() << ", dts=" << pkt.dts() << " / " << pkt.pts() * pkt.timeBase().getDouble() << " / " << pkt.timeBase() << " / st: " << pkt.streamIndex() << endl;

        // DECODING
        auto inpFrame = vdec.decodeVideo(pkt, ec);

        count++;
        if (count > 200)
            break;

        if (ec) {
            cerr << "Decoding error: " << ec << endl;
            return 1;
        } else if (!inpFrame) {
            cerr << "Empty frame\n";
            continue;
        }

        clog << "inpFrame: pts=" << inpFrame.pts() << " / " << inpFrame.pts() * inpFrame.timeBase().getDouble() << " / " << inpFrame.timeBase() << ", " << inpFrame.width() << "x" << inpFrame.height() << ", size=" << inpFrame.size() << ", ref=" << inpFrame.isReferenced() << ":" << inpFrame.refCount() << " / type: " << inpFrame.pictureType()  << endl;

        // Change timebase
        inpFrame.setTimeBase(encoder.timeBase());
        inpFrame.setStreamIndex(0);
        inpFrame.setPictureType();

        clog << "inpFrame: pts=" << inpFrame.pts() << " / " << inpFrame.pts() * inpFrame.timeBase().getDouble() << " / " << inpFrame.timeBase() << ", " << inpFrame.width() << "x" << inpFrame.height() << ", size=" << inpFrame.size() << ", ref=" << inpFrame.isReferenced() << ":" << inpFrame.refCount() << " / type: " << inpFrame.pictureType()  << endl;

        // SCALE
        VideoFrame2 outFrame {encoder.pixelFormat(), encoder.width(), encoder.height()};
        auto st = rescaler.rescale(outFrame, inpFrame);
        if (st < 0) {
            cerr << "Can't rescale frame\n";
            return 1;
        }

        clog << "outFrame: pts=" << outFrame.pts()
             << " / " << outFrame.pts() * outFrame.timeBase().getDouble()
             << " / " << outFrame.timeBase()
             << ", " << outFrame.width() << "x" << outFrame.height()
             << ", size=" << outFrame.size()
             << ", ref=" << outFrame.isReferenced() << ":" << outFrame.refCount()
             << " / type: " << outFrame.pictureType()  << endl;

        // ENCODE
        Packet opkt = encoder.encodeVideo(outFrame, ec);
        if (ec) {
            cerr << "Encoding error: " << ec << endl;
            return 1;
        } else if (!opkt) {
            cerr << "Empty packet\n";
            continue;
        }

        // Only one output stream
        opkt.setStreamIndex(0);

        clog << "Write packet: pts=" << opkt.pts() << ", dts=" << opkt.dts() << " / " << opkt.pts() * opkt.timeBase().getDouble() << " / " << opkt.timeBase() << " / st: " << opkt.streamIndex() << endl;

        octx.writePacket(opkt, ec);
        if (ec) {
            cerr << "Error write packet: " << ec << ", " << ec.message() << endl;
            return 1;
        }
    }

    octx.writeTrailer();
    ictx.close();
}


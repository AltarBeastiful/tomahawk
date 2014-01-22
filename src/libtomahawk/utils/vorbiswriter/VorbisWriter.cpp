/*
	SkypeRec
	Copyright 2008-2009 by jlh <jlh@gmx.ch>
	Copyright 2010-2011 by Peter Savichev  (proton) <psavichev@gmail.com>

	This program is free software; you can redistribute it and/or modify it
	under the terms of the GNU General Public License as published by the
	Free Software Foundation; either version 2 of the License, version 3 of
	the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful, but
	WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	General Public License for more details.

	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

	The GNU General Public License version 2 is included with the source of
	this program under the file name COPYING.  You can also get a copy on
	http://www.fsf.org/
*/

// Note: this file doesn't include comments related to writing Ogg Vorbis
// files.  it is mostly based on the examples/encoder_example.c from the vorbis
// library, so have a look there if you're curious about how this works.

// TODO: currently, this only writes tags while opening the file, but doesn't
// update them if they've been changed before close().  for now this is ok, as
// it never happens.

#include <QFile>
#include <QBuffer>
#include <QByteArray>
#include <QString>
#include <cstdlib>
#include <ctime>
#include <vorbis/vorbisenc.h>

#include "utils/Logger.h"

#include "VorbisWriter.h"

struct VorbisWriterPrivateData {
	ogg_stream_state os;
	ogg_page og;
	ogg_packet op;
	vorbis_info vi;
	vorbis_comment vc;
	vorbis_dsp_state vd;
	vorbis_block vb;
};

VorbisWriter::VorbisWriter( QIODevice* ouput) :
    AudioFileWriter(ouput),
	pd(NULL),
	hasFlushed(false)
{
}

VorbisWriter::VorbisWriter(QByteArray *a):
    AudioFileWriter(new QFile("rien")),
    pd(NULL),
    hasFlushed(false),
    m_array(a )
{
}

VorbisWriter::~VorbisWriter() {
    if (m_stream->isOpen()) {
        tDebug()  << "WARNING: VorbisWriter::~VorbisWriter(): File has not been closed, closing it now";
		close();
	}

	if (pd) {
		ogg_stream_clear(&pd->os);
		vorbis_block_clear(&pd->vb);
		vorbis_dsp_clear(&pd->vd);
		vorbis_comment_clear(&pd->vc);
		vorbis_info_clear(&pd->vi);
		delete pd;
	}
}

bool
VorbisWriter::open ( long sampleRate, bool stereo, int bitrate )
{
    if ( !AudioFileWriter::open( sampleRate, stereo ) )
    {
        tDebug() << "Failed to open stream for conversion";
        return false;
    }

	pd = new VorbisWriterPrivateData;
	vorbis_info_init(&pd->vi);

    //Quality from -1 to 10 (-0.1 to 1)
    //from 45kbs/s to 499kb/s (4-8ko/s - 50ko/s)
//    if (vorbis_encode_init_vbr(&pd->vi, m_stereo ? 2 : 1, m_sampleRate, (float)10 / 10.0f) != 0)
    if ( vorbis_encode_init( &pd->vi, m_stereo ? 2 : 1, m_sampleRate, bitrate, bitrate, bitrate ) != 0 )
    {
		delete pd;
		pd = NULL;
        tDebug() << "Failed to open stream for conversion";
		return false;
	}

	// TODO: the docs vaguely mention that stereo coupling can be disabled
	// with vorbis_encode_ctl(), but I didn't find anything concrete

	vorbis_comment_init(&pd->vc);
	// vorbis_comment_add_tag() in libvorbis up to version 1.2.0
	// incorrectly takes a char * instead of a const char *.  to prevent
	// compiler warnings we use const_cast<>(), since it's known that
	// libvorbis does not change the arguments.
    foreach ( const QString tagName, m_tagComment.keys() )
    {
        vorbis_comment_add_tag( &pd->vc, const_cast<char *>( tagName.toLocal8Bit().constData() ),
                                const_cast<char *>( m_tagComment[tagName].toLocal8Bit().constData() ) );
    }

	vorbis_analysis_init(&pd->vd, &pd->vi);
	vorbis_block_init(&pd->vd, &pd->vb);

	std::srand(std::time(NULL));
	ogg_stream_init(&pd->os, std::rand());

    ogg_packet header;
    ogg_packet header_comm;
    ogg_packet header_code;

    vorbis_analysis_headerout(&pd->vd, &pd->vc, &header, &header_comm, &header_code);
    ogg_stream_packetin(&pd->os, &header);
    ogg_stream_packetin(&pd->os, &header_comm);
    ogg_stream_packetin(&pd->os, &header_code);

    while (ogg_stream_flush(&pd->os, &pd->og) != 0) {
        m_stream->write((const char *)pd->og.header, pd->og.header_len);
        m_stream->write((const char *)pd->og.body, pd->og.body_len);
    }

	return true;
}

void VorbisWriter::close() {
    if (!m_stream->isOpen()) {
        tDebug()  << "WARNING: VorbisWriter::close() called, but file not open";
		return;
	}

	if (!hasFlushed) {
        tDebug()  << "WARNING: VorbisWriter::close() called but no flush happened, flushing now";
        QVector<qint16> dummy1, dummy2;
        write(dummy1.constData(), dummy2.constData(), 0, true);
	}

	AudioFileWriter::close();
}

bool VorbisWriter::write(const qint16* left, const qint16* right, long samples, bool flush) {
	const long maxChunkSize = 4096;

    const qint16 *leftData = left;
    const qint16 *rightData = m_stereo ? right : NULL;

	long todoSamples = samples;
	int eos = 0;

	while (!eos) {
		long chunkSize = todoSamples > maxChunkSize ? maxChunkSize : todoSamples;
		todoSamples -= chunkSize;

		if (chunkSize == 0) {
			if (!flush)
				break;
			hasFlushed = true;
			vorbis_analysis_wrote(&pd->vd, 0);
		} else {
			float **buffer = vorbis_analysis_buffer(&pd->vd, chunkSize);

			for (long i = 0; i < chunkSize; i++)
				buffer[0][i] = (float)leftData[i] / 32768.0f;
			leftData += chunkSize;

			if (m_stereo) {
				for (long i = 0; i < chunkSize; i++)
					buffer[1][i] = (float)rightData[i] / 32768.0f;
				rightData += chunkSize;
			}

			vorbis_analysis_wrote(&pd->vd, chunkSize);
		}

		while (vorbis_analysis_blockout(&pd->vd, &pd->vb) == 1) {
			vorbis_analysis(&pd->vb, NULL);
			vorbis_bitrate_addblock(&pd->vb);

			while (vorbis_bitrate_flushpacket(&pd->vd, &pd->op)) {
				ogg_stream_packetin(&pd->os, &pd->op);

				while (!eos && ogg_stream_pageout(&pd->os, &pd->og) != 0) {
                    tDebug() << "VorbisWriter : writing " << pd->og.header_len;
                    m_stream->write((const char *)pd->og.header, pd->og.header_len);
                    m_stream->write((const char *)pd->og.body, pd->og.body_len);

					if (ogg_page_eos(&pd->og))
						eos = 1;
				}
			}
		}
	}

	m_samplesWritten += samples;

	return true;
}


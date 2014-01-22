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

#include <QFileInfo>
#include <QDir>
#include <QFile>

#include <QIODevice>
#include <QString>
#include <QMap>
#include <QByteArray>

#include "utils/Logger.h"

#include "Writer.h"

AudioFileWriter::AudioFileWriter( QIODevice* output ) :
    m_sampleRate( 0 ),
    m_stereo( false ),
    m_samplesWritten( 0 ),
    m_mustWriteTags( true ),
    m_tagComment( QMap<QString, QString>() ),
    m_stream( output)
{
}

AudioFileWriter::~AudioFileWriter() {
    if (m_stream->isOpen()) {
        tDebug()  << "WARNING: AudioFileWriter::~AudioFileWriter(): File has not been closed, closing it now";
		close();
	}
}

void AudioFileWriter::addTag(const QString &tagName, const QString &tagValue)
{
    m_tagComment[tagName] = tagValue;
}

bool AudioFileWriter::open(long sampleRate, bool stereo)
{
    m_sampleRate = sampleRate;
    m_stereo = stereo;

    return m_stream->open(QIODevice::ReadWrite | QIODevice::Append);
}

void AudioFileWriter::close() {
    if (!m_stream->isOpen()) {
        tDebug()  << "WARNING: AudioFileWriter::close() called, but file not open";
		return;
	}

    //tDebug()  << QString("Closing '%1', wrote %2 samples, %3 seconds").arg(stream.fileName()).arg(samplesWritten).arg(samplesWritten / sampleRate);
    return m_stream->close();
}


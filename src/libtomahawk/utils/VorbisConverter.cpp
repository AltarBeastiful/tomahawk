/* === This file is part of Tomahawk Player - <http://tomahawk-player.org> ===
 *
 *   Copyright 2013, Rémi Benoit <r3m1.benoit@gmail.com>
 *
 *   Tomahawk is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tomahawk is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tomahawk. If not, see <http://www.gnu.org/licenses/>.
 */

#include "VorbisConverter.h"

#include <QBuffer>
#include <QByteArray>
#include <QFile>
#include <vorbis/vorbisenc.h>

#include "Album.h"
#include "Artist.h"
#include "Track.h"
#include "Query.h"
#include "Result.h"
#include "Typedefs.h"
#include "collection/Collection.h"
#include "Source.h"

#include "utils/Logger.h"
#include "utils/TomahawkUtils.h"
#include "Typedefs.h"

using namespace Tomahawk;

VorbisConverter::VorbisConverter(const Tomahawk::result_ptr& result, QObject *parent) :
    QIODevice(parent),
    m_remainingSamples(-1),
    m_buffer( new QByteArray() ),
    m_atEnd( false ),
    m_pos( 0 ),
    m_outputBitrate( OUTPUT_BITRATE )
{
    m_mediaObject = new Phonon::MediaObject( this );
    m_audioOutput = new Phonon::AudioOutput( Phonon::MusicCategory, this );
    m_audioDataOutput = new Phonon::AudioDataOutput( m_mediaObject );
//    m_stream =  new QBuffer(m_buffer);
    m_vorbisWriter = new VorbisWriter(this);
    //m_outputBitrate = quality > 0 && quality <= 10 ? quality * 45000 : DEFAULT_OUTPUT_BITRATE;
    m_duration = result->track()->duration() * 1000;

    Phonon::createPath( m_mediaObject, m_audioDataOutput );

    if ( result->collection() && result->collection()->source()->isLocal() )
    {
        m_mediaObject->setCurrentSource( Phonon::Mrl::fromUserInput(result->url() ));

        connect( m_audioDataOutput, SIGNAL( dataReady( const QMap<Phonon::AudioDataOutput::Channel, QVector<qint16> >& ) ),
             this,
             SLOT( receiveData( const QMap<Phonon::AudioDataOutput::Channel, QVector<qint16> >& ) ));
        connect( m_audioDataOutput, SIGNAL( endOfMedia(int) ), this, SLOT( onEndOfMedia(int) ) );
        connect( m_mediaObject, SIGNAL( stateChanged( Phonon::State, Phonon::State ) ), SLOT( onStateChanged( Phonon::State, Phonon::State ) ) );
        connect( m_mediaObject, SIGNAL( totalTimeChanged(qint64) ), this, SLOT( onTotalTimeChanged(qint64) ) );

        m_vorbisWriter->addTag("ARTIST", result->track()->artist());
        m_vorbisWriter->addTag("ALBUM", result->track()->album());
        m_vorbisWriter->addTag("TITLE", result->track()->track());

        tDebug() << "opening : " << m_mediaObject->currentSource().mrl().path() << " with duration : " << result->track()->duration() ;
        //m_stream->open(QBuffer::ReadWrite);

        m_vorbisWriter->open( m_audioDataOutput->sampleRate(), DEFAULT_IS_STEREO , m_outputBitrate);
    }

    tDebug() << "Data size : " << m_audioDataOutput->dataSize() << " with bitrate : " << m_audioDataOutput->sampleRate();
    this->startConversion();
}


void
VorbisConverter::startConversion()
{
    tDebug() << "Playing : " << m_mediaObject->currentSource().url() << "with bitrate : " << m_audioDataOutput->sampleRate() ;

    m_mediaObject->play();
}


qint64 VorbisConverter::readData(char *data, qint64 maxSize)
{
    int length = qMin(m_buffer->length(), (int) maxSize);
    memcpy(data, m_buffer->constData(), length);
    m_buffer->remove(0, length);
    m_pos += length;
    return length;
}


qint64 VorbisConverter::writeData(const char *data, qint64 maxSize)
{
    m_buffer->append(data, maxSize);
    return maxSize;
}


bool
VorbisConverter::seek ( qint64 pos )
{
    QIODevice::seek(pos);

    qint64 timePosition = (pos / m_outputBitrate) * 1000;
    tDebug() << "Called seek for pos : " << pos << " seeking time : " << timePosition << "ms phonon thinks : " << m_mediaObject->isSeekable();

    if(!m_mediaObject->isSeekable() || timePosition > m_duration)
    {
        return false;
    }

    m_mediaObject->seek(timePosition);
    m_pos = pos;
    return true;
}


qint64
VorbisConverter::bytesAvailable() const
{
    return ( m_duration / 1000 ) * m_outputBitrate;
//    return m_buffer->size() + QIODevice::bytesAvailable();
}


//qint64
//VorbisConverter::readData(char *data, qint64 maxSize)
//{
//    return m_stream->read(data, maxSize);
//}


//qint64
//VorbisConverter::writeData(const char *data, qint64 maxSize)
//{
//    m_buffer->append(data, maxSize);
//    return maxSize;
//}


//bool
//VorbisConverter::seek ( qint64 pos )
//{
//    bool result = m_stream->seek(pos);
//    tDebug() << "Called seek for pos : " << pos << " result : " << result;
//    return result;
//}


qint64 VorbisConverter::pos () const
{
    return m_pos;
}


//qint64 VorbisConverter::size() const
//{
//    return this->bytesAvailable();
//}


bool
VorbisConverter::atEnd () const
{
    return m_atEnd;
}


void
VorbisConverter::receiveData( const QMap<Phonon::AudioDataOutput::Channel , QVector<qint16> > &data )
{
    QVector<qint16> left = data[Phonon::AudioDataOutput::LeftChannel];
    QVector<qint16> right = data[Phonon::AudioDataOutput::RightChannel];
    long sampleSize;
    bool flush = false;

    if( m_remainingSamples == -1 )
        sampleSize = left.size();
    else
    {
        sampleSize = m_remainingSamples;
        flush = true;
        m_remainingSamples = -1;
    }

    if( left.isEmpty() || right.isEmpty() || left.size() != right.size() )
    {
        tDebug() << "Error with sample : [" << left.size() << "," << right.size() << "]";
    }
    else
    {
        int bufferSize = m_buffer->size();
        m_vorbisWriter->write( left.constData(), right.constData(), sampleSize, flush) ;
        if( bufferSize != m_buffer->size() )
        {
            tDebug() << "AFTER : array size : "<<m_buffer->size()<<" pos : "<<this->pos();
        }
    }
}


void
VorbisConverter::onEndOfMedia( int remainingSamples )
{
    tDebug() << "End of media, with : " << remainingSamples ;
    m_remainingSamples = remainingSamples;
}


void
VorbisConverter::onStateChanged( Phonon::State newState, Phonon::State oldState )
{
    tDebug() << "State changed : " << newState << " was " << oldState;

    if( oldState == Phonon::PlayingState && newState == Phonon::StoppedState )
    {
        m_atEnd = true;
        m_vorbisWriter->close();
    }

}

void
VorbisConverter::onTotalTimeChanged( qint64 newTotalTime )
{
    tDebug() << "Old total time : " << m_duration << "New totla time : " << newTotalTime;
    m_duration = newTotalTime;
}



/* This file is part of Clementine.
   Copyright 2012, David Sansome <me@davidsansome.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "TagParser.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QStringList>
#include <QVariantMap>

#include <taglib/id3v2framefactory.h>
#include <taglib/mpegfile.h>
#include <taglib/aifffile.h>
#include <taglib/asffile.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/commentsframe.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/mp4file.h>
#include <taglib/mp4tag.h>
#include <taglib/mpcfile.h>
#include <taglib/mpegfile.h>
#include <taglib/oggfile.h>
#ifdef TAGLIB_HAS_OPUS
#include <taglib/opusfile.h>
#endif
#include <taglib/oggflacfile.h>
#include <taglib/speexfile.h>
#include <taglib/tag.h>
#include <taglib/textidentificationframe.h>
#include <taglib/trueaudiofile.h>
#include <taglib/tstring.h>
#include <taglib/vorbisfile.h>
#include <taglib/wavfile.h>
#include <boost/scoped_ptr.hpp>
#include "qjson/serializer.h"

#include "utils/Logger.h"

#include "NetworkAccessManager.h"
#include "resolvers/JSResolver.h"

namespace
{
    static const int kTaglibPrefixCacheBytes = 64 * 1024;  // Should be enough.
    static const int kTaglibSuffixCacheBytes = 8 * 1024;
}

const static int MAX_ALLOW_ERROR_QUERY = 2;

TagParser::TagParser( QUrl& url,
                          const QString& filename,
                          const QString& fileId,
                          const long length,
                          const QString& mimeType,
                          QVariantMap& headers,
                          JSResolver* scriptResolver,
                          const QString& javascriptCallbackFunction )
    : m_url( url )
    , m_filename( filename )
    , m_fileId( fileId )
    , m_encoded_filename( m_filename.toUtf8() )
    , m_length( length )
    , m_headers( headers )
    , m_cursor( 0 )
    , m_cache( length )
    , m_num_requests( 0 )
    , m_num_requests_in_error( 0 )
    , m_scriptResolver( scriptResolver )
    , m_javascriptCallbackFunction( javascriptCallbackFunction )
    , m_currentBlocklength( 0 )
    , m_cacheState( TagParser::BeginningCache )
    , m_tags( QVariantMap() )
{
    m_network = Tomahawk::Utils::nam();
    connect( this, SIGNAL( cacheReadFinished() ), this, SLOT( precache() ) );
    m_tags["fileId"] = fileId;
    m_tags["mimetype"] = mimeType;
}

TagLib::FileName
TagParser::name() const
{
    return m_encoded_filename.data();
}

bool
TagParser::CheckCache( int start, int end )
{
    for ( int i = start; i <= end; ++i ) {
        if ( !m_cache.test( i ) ) {
            return false;
        }
    }
    return true;
}

void
TagParser::FillCache( uint start, TagLib::ByteVector data )
{
    for ( uint i = 0; i < data.size(); ++i )
    {
        m_cache.set( start + i, data[i] );
    }
}

TagLib::ByteVector
TagParser::GetCached( uint start, uint end )
{
    const uint size = end - start + 1;
    TagLib::ByteVector ret( size );
    for ( uint i = 0; i < size; ++i )
    {
        ret[i] = m_cache.get( start + i );
    }
    return ret;
}

void
TagParser::precache()
{
    // For reading the tags of an MP3, TagLib tends to request:
    // 1. The first 1024 bytes
    // 2. Somewhere between the first 2KB and first 60KB
    // 3. The last KB or two.
    // 4. Somewhere in the first 64KB again
    //
    // OGG Vorbis may read the last 4KB.
    //
    // So, if we precache the first 64KB and the last 8KB we should be sorted :-)
    // Ideally, we would use bytes=0-655364,-8096 but Google Drive does not seem
    // to support multipart byte ranges yet so we have to make do with two
    // requests.

    switch( m_cacheState )
    {
        case TagParser::BeginningCache :
        {
            seek( 0, TagLib::IOStream::Beginning );
            readBlock( kTaglibPrefixCacheBytes );
            m_cacheState = TagParser::EndCache;
            break;
        }

        case TagParser::EndCache :
        {
            seek( kTaglibSuffixCacheBytes, TagLib::IOStream::End );
            readBlock( kTaglibSuffixCacheBytes );
            m_cacheState = TagParser::EndCacheDone;
            break;
        }

        case TagParser::EndCacheDone :
        {
            clear();
            // construct the tag map
            QString mimeType = m_tags["mimetype"].toString();
            boost::scoped_ptr<TagLib::File> tag;

            if ( mimeType == "audio/mpeg" )
            {
                tag.reset(new TagLib::MPEG::File(
                              this,  // Takes ownership.
                              TagLib::ID3v2::FrameFactory::instance(),
                              TagLib::AudioProperties::Accurate));
            }
            else if ( mimeType == "audio/mp4" || ( mimeType == "audio/mpeg" ) )
            {
                tag.reset( new TagLib::MP4::File( this, true, TagLib::AudioProperties::Accurate ) );
            }
            else if ( mimeType == "application/ogg" || mimeType == "audio/ogg" )
            {
                tag.reset( new TagLib::Ogg::Vorbis::File( this, true, TagLib::AudioProperties::Accurate ) );
            }
        #ifdef TAGLIB_HAS_OPUS
            else if ( mimeType == "application/opus" || mimeType == "audio/opus" )
            {
                tag.reset( new TagLib::Ogg::Opus::File( this, true, TagLib::AudioProperties::Accurate ) );
            }
        #endif
            else if ( mimeType == "application/x-flac" || mimeType == "audio/flac" )
            {
                tag.reset( new TagLib::FLAC::File( this, TagLib::ID3v2::FrameFactory::instance(), true,
                                                  TagLib::AudioProperties::Accurate ) );
            }
            else if ( mimeType == "audio/x-ms-wma" )
            {
                tag.reset( new TagLib::ASF::File( this, true, TagLib::AudioProperties::Accurate ) );
            }
            else
            {
                tDebug( LOGINFO ) << "Unknown mime type for tagging:" << mimeType;
            }

            if (this->num_requests() > 2) {
                // Warn if pre-caching failed.
                tDebug( LOGINFO ) << "Total requests for file:" << m_tags["fileId"]
                                  << " : " << this->num_requests() << " with "
                                  << this->cached_bytes() << " bytes cached";
            }

            //construction of the tag's map
            if ( tag->tag() && !tag->tag()->isEmpty() )
            {
                m_tags["track"] = tag->tag()->title().toCString( true );
                m_tags["artist"] = tag->tag()->artist().toCString( true );
                m_tags["album"] = tag->tag()->album().toCString( true );
                m_tags["size"] = QString::number( m_length );

                if ( tag->tag()->track() != 0 )
                {
                    m_tags["albumpos"] = tag->tag()->track();
                }
                if ( tag->tag()->year() != 0 )
                {
                    m_tags["year"] = tag->tag()->year();
                }

                if ( tag->audioProperties() )
                {
                    m_tags["duration"] = tag->audioProperties()->length();
                    m_tags["bitrate"] = tag->audioProperties()->bitrate();
                }
            }
            emit tagsReady( m_tags, m_javascriptCallbackFunction );
            break;
        }
    }

}

TagLib::ByteVector
TagParser::readBlock( ulong length )
{
    const uint start = m_cursor;
    const uint end = qMin( m_cursor + length - 1, m_length - 1 );

    if ( end < start )
    {
        return TagLib::ByteVector();
    }

    if ( CheckCache( start, end ) )
    {
        TagLib::ByteVector cached = GetCached( start, end );
        m_cursor += cached.size();
        return cached;
    }

    if ( m_num_requests_in_error > MAX_ALLOW_ERROR_QUERY )
    {
        //precache();
        return TagLib::ByteVector();
    }

    QNetworkRequest request = QNetworkRequest( m_url );

    foreach ( const QString& headerName, m_headers.keys() )
    {
        request.setRawHeader( headerName.toUtf8(), m_headers[headerName].toString().toUtf8() );
    }
    request.setRawHeader( "Range", QString( "bytes=%1-%2" ).arg( start ).arg( end ).toUtf8() );
    request.setAttribute( QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork );

    // The Ubuntu One server applies the byte range to the gzipped data, rather
    // than the raw data so we must disable compression.
    if ( m_url.host() == "files.one.ubuntu.com" )
    {
        request.setRawHeader( "Accept-Encoding", "identity" );
    }

    m_currentBlocklength = length;
    m_currentStart = start;

    m_reply = m_network->get( request );

    connect( m_reply, SIGNAL( sslErrors( QList<QSslError> ) ), SLOT( SSLErrors( QList<QSslError> ) ) );
    connect( m_reply, SIGNAL( finished() ), this, SLOT( onRequestFinished() ) );

    ++m_num_requests;
    return TagLib::ByteVector();
}


void
TagParser::onRequestFinished()
{
    m_reply->deleteLater();

    int code = m_reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();

    QByteArray data = m_reply->readAll();

    if ( code != 206 )
    {
        m_num_requests_in_error++;
        tDebug( LOGINFO ) << "#### TagParser : Error " << code << " retrieving url to tag for " << m_filename;

        //return TagLib::ByteVector();
        precache();
        return ;
    }

    TagLib::ByteVector bytes( data.data(), data.size() );
    m_cursor += data.size();

    FillCache( m_currentStart, bytes );
    precache();
}

void
TagParser::writeBlock( const TagLib::ByteVector& )
{
    tDebug( LOGINFO ) << "writeBlock not implemented";
}

void
TagParser::insert( const TagLib::ByteVector&, ulong, ulong )
{
    tDebug( LOGINFO ) << "insert not implemented";
}

void
TagParser::removeBlock( ulong, ulong )
{
    tDebug( LOGINFO ) << "removeBlock not implemented";
}

bool
TagParser::readOnly() const
{
    tDebug( LOGINFO ) << "readOnly not implemented";
    return true;
}

bool
TagParser::isOpen() const
{
    return true;
}

void
TagParser::seek( long offset, TagLib::IOStream::Position p )
{
    switch ( p )
    {
    case TagLib::IOStream::Beginning:
        m_cursor = offset;
        break;

    case TagLib::IOStream::Current:
        m_cursor = qMin( ulong( m_cursor + offset ), m_length );
        break;

    case TagLib::IOStream::End:
        // This should really not have qAbs(), but OGG reading needs it.
        m_cursor = qMax( 0UL, m_length - qAbs( offset ) );
        break;
    }
}

void
TagParser::clear()
{
    m_cursor = 0;
}

long
TagParser::tell() const
{
    return m_cursor;
}

long
TagParser::length()
{
    return m_length;
}

void
TagParser::truncate( long )
{
    tDebug( LOGINFO ) << "not implemented";
}

void
TagParser::SSLErrors( const QList<QSslError>& errors )
{
    foreach ( const QSslError& error, errors )
    {
        tDebug( LOGINFO ) << "#### TagParser : Error for " << m_filename << " : ";
        tDebug( LOGINFO ) << error.error() << error.errorString();
        tDebug( LOGINFO ) << error.certificate();
    }
}

/* === This file is part of Tomahawk Player - <http://tomahawk-player.org> ===
 *
 *   Copyright 2010-2011, Leo Franchi <lfranchi@kde.org>
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

#include "databasecommand_genericselect.h"

#include "databaseimpl.h"
#include "utils/logger.h"
#include <sourcelist.h>
#include <artist.h>
#include <album.h>

using namespace Tomahawk;


DatabaseCommand_GenericSelect::DatabaseCommand_GenericSelect( const QString& sqlSelect, QueryType type, QObject* parent )
    : DatabaseCommand( parent )
    , m_sqlSelect( sqlSelect )
    , m_queryType( type )
{
}


void
DatabaseCommand_GenericSelect::exec( DatabaseImpl* dbi )
{
    TomahawkSqlQuery query = dbi->newquery();

    query.prepare( m_sqlSelect );
    query.exec();

    QList< query_ptr > queries;
    QList< artist_ptr > arts;
    QList< album_ptr > albs;

    // Expecting
    while ( query.next() )
    {
        query_ptr qry;
        artist_ptr artist;
        album_ptr album;

        if ( m_queryType == Track )
        {
            QString artist, track;
            track = query.value( 0 ).toString();
            artist = query.value( 1 ).toString();


            qry = Tomahawk::Query::get( artist, track, QString(), uuid(), true ); // Only auto-resolve non-local results
        } else if ( m_queryType == Artist )
        {
            int artistId = query.value( 0 ).toInt();
            QString artistName = query.value( 1 ).toString();

            artist = Tomahawk::Artist::get( artistId, artistName );
        } else if ( m_queryType == Album )
        {
            int albumId = query.value( 0 ).toInt();
            QString albumName = query.value( 1 ).toString();
            int artistId = query.value( 2 ).toInt();
            QString artistName = query.value( 3 ).toString();

            artist = Tomahawk::Artist::get( artistId, artistName );
            album = Tomahawk::Album::get( albumId, albumName, artist );
        }

        QVariantList extraData;
        int count = 2;
        while ( query.value( count ).isValid() )
        {
            extraData << query.value( count );
            count++;
        }

        if ( m_queryType == Track )
        {
            if ( !extraData.isEmpty() )
                qry->setProperty( "data", extraData );
            queries << qry;
        } else if ( m_queryType == Artist )
        {
            if ( !extraData.isEmpty() )
                artist->setProperty( "data", extraData );
            arts << artist;
        } else if ( m_queryType == Album )
        {
            if ( !extraData.isEmpty() )
                album->setProperty( "data", extraData );
            albs << album;
        }
    }

    if ( m_queryType == Track )
        emit tracks( queries );
    else if ( m_queryType == Artist )
        emit artists( arts );
    else if ( m_queryType == Album )
        emit albums( albs );
}

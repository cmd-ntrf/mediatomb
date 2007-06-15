/*MT*
    
    MediaTomb - http://www.mediatomb.cc/
    
    taglib_handler.cc - this file is part of MediaTomb.
    
    Copyright (C) 2005 Gena Batyan <bgeradz@mediatomb.cc>,
                       Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>
    
    Copyright (C) 2006-2007 Gena Batyan <bgeradz@mediatomb.cc>,
                            Sergey 'Jin' Bostandzhyan <jin@mediatomb.cc>,
                            Leonhard Wimmer <leo@mediatomb.cc>
    
    MediaTomb is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation.
    
    MediaTomb is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    version 2 along with MediaTomb; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
    
    $Id$
*/

/// \file taglib_handler.cc
/// \brief Implementeation of the TagHandler class.

#ifdef HAVE_CONFIG_H
    #include "autoconfig.h"
#endif

#ifdef HAVE_TAGLIB

#include <taglib.h>
#include <fileref.h>
#include <tag.h>
#include <id3v2tag.h>
#include <mpegfile.h>
#include <audioproperties.h>
#include <attachedpictureframe.h>
#include <tstring.h>

#include "taglib_handler.h"
#include "string_converter.h"
#include "config_manager.h"
#include "common.h"
#include "tools.h"
#include "mem_io_handler.h"

using namespace zmm;

TagHandler::TagHandler() : MetadataHandler()
{
}
       
static void addField(metadata_fields_t field, TagLib::Tag *tag, Ref<CdsItem> item)
{
    TagLib::String val;
    String value;
    unsigned int i;
    
    if (tag == NULL) 
        return;

    if (tag->isEmpty())
        return;

    Ref<StringConverter> sc = StringConverter::m2i();
    
    switch (field)
    {
        case M_TITLE:
            val = tag->title();
            break;
        case M_ARTIST:
            val = tag->artist();
            break;
        case M_ALBUM:
            val = tag->album();
            break;
        case M_DATE:
            i = tag->year();
            if (i > 0)
            {
                value = String::from(i);

                if (string_ok(value))
                    value = value + _("-01-01");
            }
            else
                return;
            break;
        case M_GENRE:
            val = tag->genre();
            break;
        case M_DESCRIPTION:
            val = tag->comment();
            break;
        case M_TRACKNUMBER:
            i = tag->track();
            if (i > 0)
            {
                value = String::from(i);
                item->setTrackNumber((int)i);
            }
            else
                return;
            break;
        default:
            return;
    }

    if ((field != M_DATE) && (field != M_TRACKNUMBER))
        value = String((char *)val.toCString());

    value = trim_string(value);
    
    if (string_ok(value))
    {
        item->setMetadata(String(MT_KEYS[field].upnp), sc->convert(value));
//        log_debug("Setting metadata on item: %d, %s\n", field, sc->convert(value).c_str());
    }
}

void TagHandler::fillMetadata(Ref<CdsItem> item)
{
    log_debug("adding metadata for: %s\n", item->getLocation().c_str());
   
    TagLib::FileRef f(item->getLocation().c_str());

    if (f.isNull() || (!f.tag()))
        return;


    TagLib::Tag *tag = f.tag();

    for (int i = 0; i < M_MAX; i++)
        addField((metadata_fields_t) i, tag, item);
    
    int temp;
    
    TagLib::AudioProperties *audioProps = f.audioProperties();
    
    if (audioProps == NULL) 
        return;
    
    // note: UPnP requres bytes/second
    temp = audioProps->bitrate() * 1024 / 8;

    if (temp > 0)
    {
        item->getResource(0)->addAttribute(MetadataHandler::getResAttrName(R_BITRATE),
                                           String::from(temp)); 
    }

    temp = audioProps->length();
    if (temp > 0)
    {
        item->getResource(0)->addAttribute(MetadataHandler::getResAttrName(R_DURATION),
                                           secondsToHMS(temp));
    }

    temp = audioProps->sampleRate();
    if (temp > 0)
    {
        item->getResource(0)->addAttribute(MetadataHandler::getResAttrName(R_SAMPLEFREQUENCY), 
                                           String::from(temp));
    }

    temp = audioProps->channels();

    if (temp > 0)
    {
        item->getResource(0)->addAttribute(MetadataHandler::getResAttrName(R_NRAUDIOCHANNELS),
                                           String::from(temp));
    }
    
    Ref<Dictionary> mappings = ConfigManager::getInstance()->getDictionaryOption(CFG_IMPORT_MAPPINGS_MIMETYPE_TO_CONTENTTYPE_LIST);
    String content_type = mappings->get(item->getMimeType());
    // we are done here, album art can only be retrieved from id3v2
    if (content_type != CONTENT_TYPE_MP3)
        return;

    // did not yet found a way on how to get to the picture from the file
    // reference that we already have
    TagLib::MPEG::File mp(item->getLocation().c_str());

    if (!mp.isValid() || !mp.ID3v2Tag())
        return;

    TagLib::ID3v2::FrameList list = mp.ID3v2Tag()->frameList("APIC");
    if (!list.isEmpty())
    {
        TagLib::ID3v2::AttachedPictureFrame *art =
               static_cast<TagLib::ID3v2::AttachedPictureFrame *>(list.front());

        if (art->picture().size() < 1)
            return;

        String art_mimetype = String(art->mimeType().toCString());
        if (!string_ok(art_mimetype))
            art_mimetype = _(MIMETYPE_DEFAULT);

        Ref<CdsResource> resource(new CdsResource(CH_ID3));
        resource->addAttribute(MetadataHandler::getResAttrName(R_PROTOCOLINFO), renderProtocolInfo(art_mimetype));
        resource->addParameter(_(RESOURCE_CONTENT_TYPE), _(ID3_ALBUM_ART));
        item->addResource(resource);
    }
}

Ref<IOHandler> TagHandler::serveContent(Ref<CdsItem> item, int resNum, off_t *data_size)
{
    // we should only get mp3 files here
    TagLib::MPEG::File f(item->getLocation().c_str());

    if (!f.isValid())
        throw _Exception(_("TagHandler: could not open file: ") + 
                item->getLocation());


    if (!f.ID3v2Tag())
        throw _Exception(_("TagHandler: resource has no album information"));

    TagLib::ID3v2::FrameList list = f.ID3v2Tag()->frameList("APIC");
    if (list.isEmpty())
        throw _Exception(_("TagHandler: resource has no album information"));

    TagLib::ID3v2::AttachedPictureFrame *art =
        static_cast<TagLib::ID3v2::AttachedPictureFrame *>(list.front());

    Ref<IOHandler> h(new MemIOHandler((void *)art->picture().data(), 
                art->picture().size()));

    *data_size = art->picture().size();
    return h;
}

#endif // HAVE_TAGLIB

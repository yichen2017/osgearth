/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2009 Pelican Ventures, Inc.
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osgEarth/TileSource>
#include <osgEarth/ImageToHeightFieldConverter>
#include <osgEarth/Registry>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <sstream>
#include <stdlib.h>
#include <iomanip>

#include "Capabilities"
#include "TileService"

using namespace osgEarth;

#define PROPERTY_URL              "url"
#define PROPERTY_CAPABILITIES_URL "capabilities_url"
#define PROPERTY_TILESERVICE_URL  "tileservice_url"
#define PROPERTY_LAYERS           "layers"
#define PROPERTY_STYLE            "style"
#define PROPERTY_FORMAT           "format"
#define PROPERTY_WMS_FORMAT       "wms_format"
#define PROPERTY_WMS_VERSION      "wms_version"
#define PROPERTY_TILE_SIZE        "tile_size"
#define PROPERTY_ELEVATION_UNIT   "elevation_unit"
#define PROPERTY_SRS              "srs"
#define PROPERTY_DEFAULT_TILE_SIZE "default_tile_size"

static std::string&
replaceIn( std::string& s, const std::string& sub, const std::string& other)
{
    if ( sub.empty() ) return s;
    size_t b=0;
    for( ; ; )
    {
        b = s.find( sub, b );
        if ( b == s.npos ) break;
        s.replace( b, sub.size(), other );
        b += other.size();
    }
    return s;
}

class WMSSource : public TileSource
{
public:
	WMSSource( const PluginOptions* options ):
    TileSource( options ),
    _tile_size(256),
    _wms_version( "1.1.1" )
    {
        const Config& conf = options->config();

        _prefix = conf.value( PROPERTY_URL );
        _layers = conf.value( PROPERTY_LAYERS );
        _style  = conf.value( PROPERTY_STYLE );
        _format = conf.value( PROPERTY_FORMAT );

        _wms_format = conf.value( PROPERTY_WMS_FORMAT );
        
        if ( conf.hasValue( PROPERTY_WMS_VERSION ) )
        {
            _wms_version = conf.value( PROPERTY_WMS_VERSION );
        }

        _capabilitiesURL = conf.value( PROPERTY_CAPABILITIES_URL );
        _tileServiceURL = conf.value( PROPERTY_TILESERVICE_URL );
        _elevation_unit = conf.value( PROPERTY_ELEVATION_UNIT );
        _srs = conf.value( PROPERTY_SRS );

        //Try to read the tile size
        if ( conf.hasValue( PROPERTY_TILE_SIZE ) )
            _tile_size = conf.value<int>( PROPERTY_TILE_SIZE, _tile_size );
        else
            _tile_size = conf.value<int>( PROPERTY_DEFAULT_TILE_SIZE, _tile_size );


        if ( _elevation_unit.empty())
            _elevation_unit = "m";
    }

    /** override */
    void initialize( const std::string& referenceURI, const Profile* overrideProfile)
    {
        osg::ref_ptr<const Profile> result;

        char sep = _prefix.find_first_of('?') == std::string::npos? '?' : '&';

        if ( _capabilitiesURL.empty() )
            _capabilitiesURL = _prefix + sep + "SERVICE=WMS&VERSION=1.1.1&REQUEST=GetCapabilities";

        //Try to read the WMS capabilities
        osg::ref_ptr<Capabilities> capabilities = CapabilitiesReader::read(_capabilitiesURL, getOptions());
        if ( !capabilities.valid() )
        {
            osg::notify(osg::WARN) << "[osgEarth::WMS] Unable to read WMS GetCapabilities; failing." << std::endl;
            return;
        }

        osg::notify(osg::INFO) << "[osgEarth::WMS] Got capabilities from " << _capabilitiesURL << std::endl;
        if (_format.empty())
        {
            _format = capabilities->suggestExtension();
            osg::notify(osg::NOTICE) << "[osgEarth::WMS] No format specified, capabilities suggested extension " << _format << std::endl;
        }

        if ( _format.empty() )
            _format = "png";
       
        if ( _srs.empty() )
            _srs = "EPSG:4326";

        //Initialize the WMS request prototype
        std::stringstream buf;
        buf
            << std::fixed << _prefix << sep
            << "SERVICE=WMS&VERSION=" << _wms_version << "&REQUEST=GetMap"
            << "&LAYERS=" << _layers
            << "&FORMAT=" << (_wms_format.empty()? "image/" + _format : _wms_format)
            << "&STYLES=" << _style
            << "&SRS=" << _srs
            << "&WIDTH="<< _tile_size
            << "&HEIGHT="<< _tile_size
            << "&BBOX=%lf,%lf,%lf,%lf";

        _prototype = buf.str();

        osg::ref_ptr<SpatialReference> wms_srs = SpatialReference::create( _srs );

        // check for spherical mercator:
        if ( wms_srs.valid() && wms_srs->isEquivalentTo( osgEarth::Registry::instance()->getGlobalMercatorProfile()->getSRS() ) )
        {
            result = osgEarth::Registry::instance()->getGlobalMercatorProfile();
        }
		else if (wms_srs.valid() && wms_srs->isEquivalentTo( osgEarth::Registry::instance()->getGlobalGeodeticProfile()->getSRS()))
		{
			result = osgEarth::Registry::instance()->getGlobalGeodeticProfile();
		}

        // Next, try to glean the extents from the layer list
        if ( !result.valid() )
        {
            //TODO: "layers" mights be a comma-separated list. need to loop through and
            //combine the extents?? yes
            Layer* layer = capabilities->getLayerByName( _layers );
            if ( layer )
            {
                double minx, miny, maxx, maxy;
                layer->getExtents(minx, miny, maxx, maxy);


                //Check to see if the profile is equivalent to global-geodetic
                if (wms_srs->isGeographic())
                {
					//Try to get the lat lon extents if they are provided
					if (minx == 0 && miny == 0 && maxx == 0 && maxy == 0)
					{
						layer->getLatLonExtents(minx, miny, maxx, maxy);
					}

					//If we still don't have any extents, just default to global geodetic.
					if (minx == 0 && miny == 0 && maxx == 0 && maxy == 0)
					{
						result = osgEarth::Registry::instance()->getGlobalGeodeticProfile();
					}
                }	

                if (!result.valid())
                {
                    result = Profile::create( _srs, minx, miny, maxx, maxy );
                }
            }
        }

        // Last resort: create a global extent profile (only valid for global maps)
        if ( !result.valid() && wms_srs->isGeographic())
        {
            result = osgEarth::Registry::instance()->getGlobalGeodeticProfile();
        }
        

        // JPL uses an experimental interface called TileService -- ping to see if that's what
        // we are trying to read:
        if (_tileServiceURL.empty())
            _tileServiceURL = _prefix + sep + "request=GetTileService";

        osg::notify(osg::INFO) << "[osgEarth::WMS] Testing for JPL/TileService at " << _tileServiceURL << std::endl;
        _tileService = TileServiceReader::read(_tileServiceURL, getOptions());
        if (_tileService.valid())
        {
            osg::notify(osg::NOTICE) << "[osgEarth::WMS] Found JPL/TileService spec" << std::endl;
            TileService::TilePatternList patterns;
            _tileService->getMatchingPatterns(_layers, _format, _style, _srs, _tile_size, _tile_size, patterns);

            if (patterns.size() > 0)
            {
                result = _tileService->createProfile( patterns );
                _prototype = _prefix + sep + patterns[0].getPrototype();
            }
        }
        else
        {
            osg::notify(osg::INFO) << "[osgEarth::WMS] No JPL/TileService spec found; assuming standard WMS" << std::endl;
        }

        //TODO: won't need this for OSG 2.9+, b/c of mime-type support
        _prototype = _prototype + "&." + _format;

        // populate the data metadata:
        // TODO

		setProfile( result.get() );
    }

    /** override */
    osg::Image* createImage( const TileKey* key,
                             ProgressCallback* progress
                             )
    {
        std::string uri = createURI( key );
        
        osg::ref_ptr<osg::Image> image;
        HTTPClient::readImageFile( uri, image, getOptions(), progress );
        return image.release();

        //if (osgDB::containsServerAddress( uri ))
        //{
        //    return HTTPClient::readImageFile( uri, getOptions(), progress );
        //}
        //return osgDB::readImageFile( createURI( key ), getOptions() );
    }

    /** override */
    osg::HeightField* createHeightField( const TileKey* key,
                                         ProgressCallback* progress)
    {
        osg::Image* image = createImage(key, progress);
        if (!image)
        {
            osg::notify(osg::INFO) << "[osgEarth::WMS] Failed to read heightfield from " << createURI(key) << std::endl;
        }

        float scaleFactor = 1;

        //Scale the heightfield to meters
        if (_elevation_unit == "ft")
        {
            scaleFactor = 0.3048;
        }

        ImageToHeightFieldConverter conv;
        return conv.convert( image, scaleFactor );
    }

    std::string createURI( const TileKey* key ) const
    {
        double minx, miny, maxx, maxy;
        key->getGeoExtent().getBounds( minx, miny, maxx, maxy);
        
        char buf[2048];
        sprintf(buf, _prototype.c_str(), minx, miny, maxx, maxy);
        
        std::string uri(buf);

        // url-ize the uri before returning it
        if ( osgDB::containsServerAddress( uri ) )
            uri = replaceIn(uri, " ", "%20");

        return uri;
    }

    virtual int getPixelsPerTile() const
    {
        return _tile_size;
    }

    virtual std::string getExtension()  const 
    {
        return _format;
    }

private:
    std::string _prefix;
    std::string _layers;
    std::string _style;
    std::string _format;
    std::string _wms_format;
    std::string _wms_version;
    std::string _srs;
    std::string _tileServiceURL;
    std::string _capabilitiesURL;
	int _tile_size;
    std::string _elevation_unit;
    osg::ref_ptr<TileService> _tileService;
    osg::ref_ptr<const Profile> _profile;
    std::string _prototype;
};


class ReaderWriterWMS : public osgDB::ReaderWriter
{
    public:
        ReaderWriterWMS() {}

        virtual const char* className()
        {
            return "WMS Reader";
        }
        
        virtual bool acceptsExtension(const std::string& extension) const
        {
            return osgDB::equalCaseInsensitive( extension, "osgearth_wms" );
        }

        virtual ReadResult readObject(const std::string& file_name, const Options* opt) const
        {
            std::string ext = osgDB::getFileExtension( file_name );
            if ( !acceptsExtension( ext ) )
            {
                return ReadResult::FILE_NOT_HANDLED;
            }

            return new WMSSource( static_cast<const PluginOptions*>(opt) );
        }
};

REGISTER_OSGPLUGIN(osgearth_wms, ReaderWriterWMS)

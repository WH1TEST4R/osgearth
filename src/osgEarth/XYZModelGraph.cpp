/* -*-c++-*- */
/* osgEarth - Geospatial SDK for OpenSceneGraph
 * Copyright 2020 Pelican Mapping
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
#include <osgEarth/XYZModelGraph>
#include <osgEarth/NetworkMonitor>
#include <osgEarth/Registry>
#include <osgEarth/Metrics>
#include <osgEarth/Chonk>
#include <osgEarth/NodeUtils>
#include <osgEarth/Utils>
#include <osgDB/ReadFile>
#include <osg/BlendFunc>

using namespace osgEarth;

XYZModelGraph::XYZModelGraph(const osgEarth::Map* map, const Profile* profile, const URI& url, bool invertY, const osgDB::Options* options) :
    SimplePager(map, profile),
    _url(url),
    _invertY(invertY) 
{
    _options = osgEarth::Registry::instance()->cloneOrCreateOptions(options);
    _options->setObjectCacheHint(osgDB::Options::CACHE_IMAGES);

    _statesetCache = new StateSetCache();
}

void
XYZModelGraph::setUseNVGL(bool value)
{
    if (value == true && GLUtils::useNVGL() && !_textures.valid())
    {
        _textures = new TextureArena();
        getOrCreateStateSet()->setAttribute(_textures, 1);

        // auto release requires that we install this update callback!
        _textures->setAutoRelease(true);

        addUpdateCallback(new LambdaCallback<>([this](osg::NodeVisitor& nv)
            {
                _textures->update(nv);
                return true;
            }));

        getOrCreateStateSet()->setAttributeAndModes(
            new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA),
            osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
    }
}

void
XYZModelGraph::setOwnerName(const std::string& value)
{
    _ownerName = value;
}

osg::ref_ptr<osg::Node>
XYZModelGraph::createNode(const TileKey& key, ProgressCallback* progress)
{
    OE_PROFILING_ZONE;
    if (progress && progress->isCanceled())
        return nullptr;

    NetworkMonitor::ScopedRequestLayer layerRequest(_ownerName);

    unsigned x, y;
    key.getTileXY(x, y);
    unsigned cols = 0, rows = 0;
    key.getProfile()->getNumTiles(key.getLevelOfDetail(), cols, rows);
    unsigned inverted_y = rows - y - 1;

    if (_invertY == true)
    {
        y = inverted_y;
    }

    std::string location = _url.full();

    // support OpenLayers template style:
    replaceIn(location, "${x}", Stringify() << x);
    replaceIn(location, "${y}", Stringify() << y);
    replaceIn(location, "${-y}", Stringify() << inverted_y);
    replaceIn(location, "${z}", Stringify() << key.getLevelOfDetail());


    // failing that, legacy osgearth style:
    replaceIn(location, "{x}", Stringify() << x);
    replaceIn(location, "{y}", Stringify() << y);
    replaceIn(location, "{-y}", Stringify() << inverted_y);
    replaceIn(location, "{z}", Stringify() << key.getLevelOfDetail());

    URI myUri(location, _url.context());
    
    osg::ref_ptr< osg::Node > node = myUri.readNode(_options.get()).getNode();
    if (node.valid())
    {
        if (_textures.valid())
        {
            auto xform = findTopMostNodeOfType<osg::MatrixTransform>(node.get());

            // Convert the geometry into chonks
            ChonkFactory factory(_textures);

            factory.setGetOrCreateFunction(
                ChonkFactory::getWeakTextureCacheFunction(
                    _texturesCache, _texturesCacheMutex));

            osg::ref_ptr<ChonkDrawable> drawable = new ChonkDrawable();

            if (xform)
            {
                for (unsigned i = 0; i < xform->getNumChildren(); ++i)
                {
                    drawable->add(xform->getChild(i), factory);
                }
                xform->removeChildren(0, xform->getNumChildren());
                xform->addChild(drawable);
                node = xform;
            }
            else
            {
                if (drawable->add(node.get(), factory))
                {
                    node = drawable;
                }
            }
        }
        else
        {
            osgEarth::Registry::shaderGenerator().run(node.get(), _statesetCache);
        }
        return node.release();
    }
    return nullptr;
}
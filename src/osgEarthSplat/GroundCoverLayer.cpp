/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2012 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "GroundCoverLayer"
#include "SplatShaders"
#include "NoiseTextureFactory"
#include <osgEarth/VirtualProgram>
#include <osgEarth/Shadowing>
#include <osgEarthFeatures/FeatureSource>
#include <osgEarthFeatures/FeatureSourceLayer>
#include <osgUtil/CullVisitor>
#include <osg/BlendFunc>
#include <osg/Multisample>
#include <osg/Texture2D>
#include <cstdlib> // getenv

#define LC "[GroundCoverLayer] "

#define GCTEX_SAMPLER "oe_GroundCover_billboardTex"
#define NOISE_SAMPLER "oe_GroundCover_noiseTex"

using namespace osgEarth::Splat;

namespace osgEarth { namespace Splat {
    REGISTER_OSGEARTH_LAYER(splat_groundcover, GroundCoverLayer);
} }

//........................................................................

Config
GroundCoverLayerOptions::getConfig() const
{
    Config conf = PatchLayerOptions::getConfig();
    conf.key() = "splat_groundcover";
    conf.set("land_cover_layer", _landCoverLayerName);
    conf.set("mask_layer", _maskLayerName);
    conf.set("lod", _lod);
    conf.set("cast_shadows", _castShadows);

    Config zones("zones");
    for (int i = 0; i < _zones.size(); ++i) {
        Config zone = _zones[i].getConfig();
        if (!zone.empty())
            zones.add(zone);
    }
    if (!zones.empty())
        conf.update(zones);
    return conf;
}

void
GroundCoverLayerOptions::fromConfig(const Config& conf)
{
    conf.getIfSet("land_cover_layer", _landCoverLayerName);
    conf.getIfSet("mask_layer", _maskLayerName);
    conf.getIfSet("lod", _lod);
    conf.getIfSet("cast_shadows", _castShadows);

    const Config* zones = conf.child_ptr("zones");
    if (zones) {
        const ConfigSet& children = zones->children();
        for (ConfigSet::const_iterator i = children.begin(); i != children.end(); ++i) {
            _zones.push_back(ZoneOptions(*i));
        }
    }
}

//........................................................................

namespace
{
    struct GroundCoverLayerAcceptor : public PatchLayer::AcceptCallback
    {
        GroundCoverLayer* _layer;

        GroundCoverLayerAcceptor(GroundCoverLayer* layer) : _layer(layer) { }

        bool acceptLayer(osg::NodeVisitor& nv, const osg::Camera* camera) const
        {
            // if this is a shadow camera and the layer is configured to cast shadows, accept it.
            if (osgEarth::Shadowing::isShadowCamera(camera))
            {
                bool use = (_layer->options().castShadows() == true);
                return use;
            }

            // if this is a depth-pass camera (and not a shadow cam), reject it.
            unsigned clearMask = camera->getClearMask();
            bool isDepthCamera = ((clearMask & GL_COLOR_BUFFER_BIT) == 0u) && ((clearMask & GL_DEPTH_BUFFER_BIT) != 0u);
            if (isDepthCamera)
                return false;

            // otherwise accept the layer.
            return true;
        }

        bool acceptKey(const TileKey& key) const
        {
            return _layer->getLOD() == key.getLOD();
        }

    };
}

//........................................................................

GroundCoverLayer::GroundCoverLayer() :
PatchLayer(&_optionsConcrete),
_options(&_optionsConcrete)
{
    init();
}

GroundCoverLayer::GroundCoverLayer(const GroundCoverLayerOptions& options) :
PatchLayer(&_optionsConcrete),
_options(&_optionsConcrete),
_optionsConcrete(options)
{
    init();
}

void
GroundCoverLayer::init()
{
    PatchLayer::init();

    _zonesConfigured = false;

    // deserialize zone data
    for (std::vector<ZoneOptions>::const_iterator i = options().zones().begin();
        i != options().zones().end();
        ++i)
    {
        osg::ref_ptr<Zone> zone = new Zone(*i);
        _zones.push_back(zone.get());
    }

    setAcceptCallback(new GroundCoverLayerAcceptor(this));
}

const Status&
GroundCoverLayer::open()
{
    return PatchLayer::open();
}

void
GroundCoverLayer::setLandCoverDictionary(LandCoverDictionary* layer)
{
    _landCoverDict = layer;
    if (layer)
        buildStateSets();
}

void
GroundCoverLayer::setLandCoverLayer(LandCoverLayer* layer)
{
    _landCoverLayer = layer;
    if (layer) {
        OE_INFO << LC << "Land cover layer is \"" << layer->getName() << "\"\n";
        buildStateSets();
    }
}

void
GroundCoverLayer::setMaskLayer(ImageLayer* layer)
{
    _maskLayer = layer;
    if (layer)
    {
        OE_INFO << LC << "Mask layer is \"" << layer->getName() << "\"\n";
        buildStateSets();
    }
}

unsigned
GroundCoverLayer::getLOD() const
{
    return options().lod().get();
}

void
GroundCoverLayer::addedToMap(const Map* map)
{
    if (!_landCoverDict.valid())
    {
        _landCoverDictListener.listen(map, this, &GroundCoverLayer::setLandCoverDictionary);
    }

    if (!_landCoverLayer.valid() && options().landCoverLayer().isSet())
    {
        _landCoverListener.listen(map, options().landCoverLayer().get(), this, &GroundCoverLayer::setLandCoverLayer);
    }

    if (options().maskLayer().isSet())
    {
        _maskLayerListener.listen(map, options().maskLayer().get(), this, &GroundCoverLayer::setMaskLayer);
    }

    for (Zones::iterator zone = _zones.begin(); zone != _zones.end(); ++zone)
    {
        zone->get()->configure(map, getReadOptions());
    }

    _zonesConfigured = true;
    
    buildStateSets();
}

void
GroundCoverLayer::removedFromMap(const Map* map)
{
    //NOP
}

osg::Node*
GroundCoverLayer::getOrCreateNode(TerrainResources* res)
{
    if (res)
    {
        if (_groundCoverTexBinding.valid() == false)
        {
            if (res->reserveTextureImageUnitForLayer(_groundCoverTexBinding, this, "Ground cover texture catalog") == false)
            {
                OE_WARN << LC << "No texture unit available for ground cover texture catalog\n";
            }
        }

        if (_noiseBinding.valid() == false)
        {
            if (res->reserveTextureImageUnitForLayer(_noiseBinding, this, "Ground cover noise sampler") == false)
            {
                OE_WARN << LC << "No texture unit available for Ground cover Noise function\n";
            }
        }

        if (_groundCoverTexBinding.valid())
        {
            buildStateSets();
        }
    }

    return 0L;
}

bool
GroundCoverLayer::preCull(osgUtil::CullVisitor* cv) const
{
    Layer::preCull(cv);

    // If we have zones, select the current one and apply its state set.
    if (_zones.size() > 0)
    {
        int zoneIndex = 0;
        osg::Vec3d vp = cv->getViewPoint();

        for(int z=_zones.size()-1; z > 0 && zoneIndex == 0; --z)
        {
            if ( _zones[z]->contains(vp) )
            {
                zoneIndex = z;
            }
        }

        osg::StateSet* zoneStateSet = 0L;
        GroundCover* gc = _zones[zoneIndex]->getGroundCover();
        if (gc)
        {
            zoneStateSet = gc->getStateSet();
        }

        //osg::StateSet* zoneStateSet = _zones[zoneIndex]->getGroundCover()getStateSet();

        if (zoneStateSet == 0L)
        {
            OE_FATAL << LC << "ASSERTION FAILURE - zoneStateSet is null\n";
            exit(-1);
        }
        
        cv->pushStateSet(zoneStateSet);
    }
    return true;
}

void
GroundCoverLayer::postCull(osgUtil::CullVisitor* cv) const
{
    // If we have at least one zone, one stateset was pushed in preCull,
    // so pop it now.
    if (_zones.size() > 0)
        cv->popStateSet();

    Layer::postCull(cv);
}

void
GroundCoverLayer::buildStateSets()
{
    // assert we have the necessary TIUs:
    if (_groundCoverTexBinding.valid() == false) {
        OE_DEBUG << LC << "buildStateSets deferred.. bindings not reserved\n";
        return;
    }

    if (!_zonesConfigured) {
        OE_DEBUG << LC << "buildStateSets deferred.. zones not yet configured\n";
        return;
    }
    
    osg::ref_ptr<LandCoverDictionary> landCoverDict;
    if (_landCoverDict.lock(landCoverDict) == false) {
        OE_DEBUG << LC << "buildStateSets deferred.. land cover dictionary not available\n";
        return;
    }
    
    osg::ref_ptr<LandCoverLayer> landCoverLayer;
    if (_landCoverLayer.lock(landCoverLayer) == false) {
        OE_DEBUG << LC << "buildStateSets deferred.. land cover layer not available\n";
        return;
    }

    NoiseTextureFactory noise;
    osg::ref_ptr<osg::Texture> noiseTexture = noise.create(256u, 4u);

    GroundCoverShaders shaders;

    // Layer-wide stateset:
    osg::StateSet* stateset = new osg::StateSet();
    this->setStateSet(stateset);

    // bind the noise sampler.
    stateset->setTextureAttribute(_noiseBinding.unit(), noiseTexture.get());
    stateset->addUniform(new osg::Uniform(NOISE_SAMPLER, _noiseBinding.unit()));

    if (_maskLayer.valid())
    {
        stateset->setDefine("OE_GROUNDCOVER_MASK_SAMPLER", _maskLayer->shareTexUniformName().get());
        stateset->setDefine("OE_GROUNDCOVER_MASK_MATRIX", _maskLayer->shareTexMatUniformName().get());
    }

    // disable backface culling to support shadow/depth cameras,
    // for which the geometry shader renders cross hatches instead of billboards.
    stateset->setMode(GL_CULL_FACE, osg::StateAttribute::PROTECTED);

    // enable alpha-to-coverage multisampling for vegetation.
    stateset->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB, 1);

    // uniform that communicates the availability of multisampling.
    if (osg::DisplaySettings::instance()->getMultiSamples())
    {
        stateset->setDefine("OE_GROUNDCOVER_HAS_MULTISAMPLES");
    }

    stateset->setAttributeAndModes(
        new osg::BlendFunc(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO),
        osg::StateAttribute::OVERRIDE);


    for (Zones::iterator z = _zones.begin(); z != _zones.end(); ++z)
    {
        Zone* zone = z->get();
        GroundCover* groundCover = zone->getGroundCover();
        if (groundCover)
        {
            if (!groundCover->getBiomes().empty() || groundCover->getTotalNumBillboards() > 0)
            {
                osg::StateSet* zoneStateSet = groundCover->getOrCreateStateSet();
                            
                // Install the land cover shaders on the state set
                VirtualProgram* vp = VirtualProgram::getOrCreate(zoneStateSet);
                vp->setName("Ground cover (" + groundCover->getName() + ")");
                shaders.loadAll(vp, getReadOptions());

                // Generate the coverage acceptor shader
                osg::Shader* covTest = groundCover->createPredicateShader(_landCoverDict.get(), _landCoverLayer.get());
                covTest->setName(covTest->getName() + "_GEOMETRY");
                covTest->setType(osg::Shader::GEOMETRY);
                vp->setShader(covTest);

                osg::Shader* covTest2 = groundCover->createPredicateShader(_landCoverDict.get(), _landCoverLayer.get());
                covTest->setName(covTest->getName() + "_TESSCONTROL");
                covTest2->setType(osg::Shader::TESSCONTROL);
                vp->setShader(covTest2);

                osg::ref_ptr<osg::Shader> layerShader = groundCover->createShader();
                layerShader->setType(osg::Shader::GEOMETRY);
                vp->setShader(layerShader);

                OE_INFO << LC << "Adding ground cover \"" << groundCover->getName() << "\" to zone \"" << zone->getName() << "\" at LOD " << getLOD() << "\n";

                //// Install the uniforms
                //zoneStateSet->addUniform(new osg::Uniform("oe_GroundCover_windFactor", groundCover->options().wind().get()));
                //zoneStateSet->addUniform(new osg::Uniform("oe_GroundCover_noise", 1.0f));
                //zoneStateSet->addUniform(new osg::Uniform("oe_GroundCover_ao", 0.5f));
                //zoneStateSet->addUniform(new osg::Uniform("oe_GroundCover_exposure", 1.0f));

                //zoneStateSet->addUniform(new osg::Uniform("oe_GroundCover_density", groundCover->options().density().get()));
                //zoneStateSet->addUniform(new osg::Uniform("oe_GroundCover_fill", groundCover->options().fill().get()));
                //zoneStateSet->addUniform(new osg::Uniform("oe_GroundCover_maxDistance", groundCover->options().maxDistance().get()));

                //zoneStateSet->addUniform(new osg::Uniform("oe_GroundCover_brightness", groundCover->options().brightness().get()));
                //zoneStateSet->addUniform(new osg::Uniform("oe_GroundCover_contrast", groundCover->options().contrast().get()));

                osg::Texture* tex = groundCover->createTexture();

                zoneStateSet->setTextureAttribute(_groundCoverTexBinding.unit(), tex);
                zoneStateSet->addUniform(new osg::Uniform(GCTEX_SAMPLER, _groundCoverTexBinding.unit()));

                OE_DEBUG << LC << "buildStateSets completed!\n";
            }
            else
            {
                OE_WARN << LC << "ILLEGAL: ground cover layer with no biomes or no billboards defined\n";
            }
        }
        else
        {
            // not an error.
            OE_DEBUG << LC << "zone contains no ground cover information\n";
        }
    }
}

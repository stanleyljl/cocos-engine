// Generates jsb_landscape_auto.cpp/.h, exporting classes to the JS `jsb` namespace.
%module(target_namespace="jsb") landscape

#pragma SWIG nowarn=503,302,401,317,402

// Prepended to the generated header (.h)
%insert(header_file) %{
#pragma once
#include "bindings/jswrapper/SeApi.h"
#include "bindings/manual/jsb_conversions.h"
#include "landscape/Landscape.h"
#include "landscape/LandscapeAsset.h"
%}

// Prepended to the generated source (.cpp)
%{
#include "bindings/auto/jsb_landscape_auto.h"
#include "bindings/auto/jsb_assets_auto.h"
#include "bindings/auto/jsb_cocos_auto.h"
#include "bindings/auto/jsb_scene_auto.h"

using namespace cc;
%}

%ignore cc::RefCounted;
%ignore cc::landscape::LandscapeAsset::TileData;
%ignore cc::landscape::LandscapeAsset::DecalLayer;
%ignore cc::landscape::LandscapeAsset::Decal;
%ignore cc::landscape::LandscapeAsset::decalLayers;
%ignore cc::landscape::LandscapeAsset::decals;
%ignore cc::landscape::LandscapeAsset::decalResolution;
%ignore cc::landscape::LandscapeAsset::data;
%ignore cc::landscape::LandscapeAsset::dataDir;
%ignore cc::landscape::LandscapeAsset::valid;
%ignore cc::landscape::LandscapeAsset::getHeightRange;
%ignore cc::landscape::LandscapeAsset::requestTile;
%ignore cc::landscape::LandscapeAsset::takeReadyTile;
%ignore cc::landscape::LandscapeAsset::takeFailedTile;
%ignore cc::landscape::LandscapeAsset::loadTile;
%ignore cc::landscape::LandscapeAsset::loadRootTile;

// Imported for type resolution only (no bindings generated).
%import "base/Macros.h"
%import "base/RefCounted.h"
%import "core/event/Event.h"
%import "core/scene-graph/Node.h"
%import "core/assets/Texture2D.h"

// Generate bindings for classes in these headers.
%include "landscape/Landscape.h"
%include "landscape/LandscapeAsset.h"

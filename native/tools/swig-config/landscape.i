// Generates jsb_landscape_auto.cpp/.h, exporting classes to the JS `jsb` namespace.
%module(target_namespace="jsb") landscape

#pragma SWIG nowarn=503,302,401,317,402

// Prepended to the generated header (.h)
%insert(header_file) %{
#pragma once
#include "bindings/jswrapper/SeApi.h"
#include "bindings/manual/jsb_conversions.h"
#include "landscape/LandscapeInstance.h"
#include "landscape/Asset.h"
#include "landscape/PhysicsResources.h"
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
// Shared native data stays internal; retain only the existing debug-data binding.
%ignore cc::landscape::LandscapePoint;
%ignore cc::landscape::LandscapeRegion;
%ignore cc::landscape::HeightRange;
%ignore cc::landscape::LandscapeQueryTile;
%ignore cc::landscape::LandscapeQueryCompletion;
%ignore cc::landscape::NodeAddress;
%ignore cc::landscape::makeNodeKey;
%ignore cc::landscape::QuadNode;
%ignore cc::landscape::VTPageAddress;
%ignore cc::landscape::VTGridMapping;
%ignore cc::landscape::VTPageSources;
%ignore cc::landscape::VTPageLayout;
%ignore cc::landscape::LandscapeData;
%ignore cc::landscape::NodeIndexLayout;
%ignore cc::landscape::LandscapeInstance::preparePasses;
%ignore cc::landscape::LandscapeInstance::getPassModels;
%ignore cc::landscape::LandscapeInstance::onGlobalPipelineStateChanged;
%ignore cc::landscape::LandscapeAsset::TileData;
%ignore cc::landscape::LandscapeAsset::DecalLayer;
%ignore cc::landscape::LandscapeAsset::Decal;
%ignore cc::landscape::LandscapeAsset::decalLayers;
%ignore cc::landscape::LandscapeAsset::decals;
%ignore cc::landscape::LandscapeAsset::decalResolution;
%ignore cc::landscape::LandscapeAsset::data;
%ignore cc::landscape::LandscapeAsset::heightRanges;
%ignore cc::landscape::LandscapeAsset::getHeightRange;
%ignore cc::landscape::LandscapeAsset::requestTile;
%ignore cc::landscape::LandscapeAsset::takeReadyTile;
%ignore cc::landscape::LandscapeAsset::loadingFailed;
%ignore cc::landscape::LandscapeAsset::loadTile;
%ignore cc::landscape::LandscapeAsset::requestQueryTile;

// Imported for type resolution only (no bindings generated).
%import "base/Macros.h"
%import "base/RefCounted.h"
%import "core/event/Event.h"
%import "core/scene-graph/Node.h"
%import "core/assets/Texture2D.h"

// Generate bindings for classes in these headers.
%include "landscape/Data.h"
%include "landscape/LandscapeInstance.h"
%include "landscape/Asset.h"
%include "landscape/PhysicsResources.h"

// Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.
// Keep algorithm constants aligned with native/cocos/landscape/Config.h.
declare global {
    // Set before loading the engine. Native uses the CC_LANDSCAPE_DEBUG build macro.
    var CC_LANDSCAPE_DEBUG: boolean | undefined;
}

// Diagnostics default on independently of the engine's debug mode. Errors,
// operational warnings and visual debug controls do not depend on this switch.
export const LANDSCAPE_DEBUG = globalThis.CC_LANDSCAPE_DEBUG ?? true;
export const VERTS_PER_NODE_SIDE = 17;
export const LOD_DISTANCE_RATIO = 2;
export const MORPH_START_RATIO = 0.70;
export const CLIFF_ANGLE = 60;
export const SOURCE_TILE_RESOLUTION = 129;
export const SOURCE_NORMAL_BORDER = 8;
export const SOURCE_NORMAL_RESOLUTION = SOURCE_TILE_RESOLUTION + 2 * SOURCE_NORMAL_BORDER;
export const MAX_LOD_LEVELS = 9;
export const MAX_SECTOR_COUNT = 64;
export const ALL_QUADRANTS = 0x0F;
export const PAGE_POOL_LAYERS = 1024;
export const PAGE_UPLOAD_BUDGET = 8;
export const MATERIAL_LIBRARY_MAX = 32;
export const DECAL_LIBRARY_MAX = 32;
export const DECAL_INSTANCE_MAX = 512;
export const VT_PAGE_INTERIOR = 256;
export const VT_NORMAL_SOURCE_COUNT = 4;
export const VT_NORMAL_DETAIL_MIN_LEVEL = 6;
export const VT_PAGE_BORDER = 8;
export const VT_MIP_LEVELS = 3;
export const VT_MAX_ANISOTROPY = 4;
export const VT_FILTER_MARGIN = 2 ** (VT_MIP_LEVELS - 1);
export const VT_PAGE_RES = VT_PAGE_INTERIOR + 2 * VT_PAGE_BORDER;
// Maximum atlas dimensions; Renderer selects the actual capacity for the device.
export const VT_ATLAS_SIZE = VT_PAGE_RES * 16;
export const VT_PAGE_UPDATE_BUDGET = 8;
export const VT_PAGES_PER_SIDE = VT_ATLAS_SIZE / VT_PAGE_RES;
export const VT_PAGE_COUNT = VT_PAGES_PER_SIDE * VT_PAGES_PER_SIDE;

// Match native static assertions once when loading the module. These constrain
// layouts shared with Effects, independent of the selected device capacity.
if (CLIFF_ANGLE < 0 || CLIFF_ANGLE > 90
    || (MATERIAL_LIBRARY_MAX + DECAL_INSTANCE_MAX + DECAL_LIBRARY_MAX + 6) * 16 > 16384
    || DECAL_INSTANCE_MAX % 2 !== 0
    || SOURCE_NORMAL_BORDER * VT_PAGE_INTERIOR < 2 * (SOURCE_TILE_RESOLUTION - 1) * VT_PAGE_BORDER
    || MAX_SECTOR_COUNT > VT_PAGE_COUNT || MAX_SECTOR_COUNT * 5 > PAGE_POOL_LAYERS
    || VT_PAGE_RES % VT_FILTER_MARGIN !== 0 || VT_PAGE_BORDER % VT_FILTER_MARGIN !== 0
    || VT_PAGE_RES <= 2 * VT_PAGE_BORDER || VT_ATLAS_SIZE % VT_PAGE_RES !== 0
    || VT_PAGE_BORDER <= VT_FILTER_MARGIN || VT_PAGE_COUNT > 256) {
    throw new Error('Invalid Landscape configuration: GPU layouts and root coverage must match native and Effects');
}

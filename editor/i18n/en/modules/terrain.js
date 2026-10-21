module.exports = {
    classes: {
        cc: {
            Landscape: {
                properties: {
                    landscapeAsset: {
                        tooltip: 'Terrain asset containing terrain dimensions, layers and tile data. Tiles are loaded on demand at runtime.',
                    },
                    lodQualityScale: {
                        tooltip: 'Geometry LOD quality multiplier, with a default and minimum of 1. Larger values retain more geometry detail at greater distances. Set before terrain initialization. Does not control query or physics resolution.',
                    },
                    queryCacheCapacity: {
                        tooltip: 'Maximum number of tiles in the CPU surface query cache. Defaults to 64; valid range: 1 to 4096. Set before terrain initialization. Returns OutOfCapacity when the requested region and tiles pinned by other sources exceed this budget. Independent of geometry, VT and physics caches.',
                    },
                    shadowCastingModeForInspector: {
                        tooltip: 'Whether the terrain and its 3D decals cast realtime shadows. Shadow support must also be enabled for the light and render pipeline.',
                    },
                    receiveShadowForInspector: {
                        tooltip: 'Whether the terrain and its 3D decals receive realtime shadows.',
                    },
                    debugData: {
                        tooltip: 'Terrain debug and rendering switches, including wireframe, LOD, virtual texture and decal display options.',
                    },
                    holeMode: {
                        tooltip: 'Fast uses vertex NaN culling; HighQuality clips fragments. Both follow source LOD. Fast is the default; select HighQuality manually when needed.',
                    },
                    globalColorMap: {
                        tooltip: 'Terrain-wide color modulation map. RGB is linear UNORM data: byte 128 is neutral, lower values darken, higher values lighten. Alpha controls regional influence. Do not use a diffuse image or sRGB texture directly. Without a map, original material colors are retained.',
                    },
                    globalColorStrength: {
                        tooltip: "Global modulation strength, multiplied by map Alpha and each material's globalColorInfluence. Zero disables modulation. Defaults to 0.1.",
                    },
                },
            },
            LandscapeDebugData: {
                properties: {
                    wireframe: {
                        tooltip: 'Show terrain and 3D decal meshes in wireframe to inspect geometry subdivision.',
                    },
                    lodColor: {
                        tooltip: 'Color the terrain by geometry LOD level to inspect detail distribution and transitions.',
                    },
                    showBox: {
                        tooltip: 'Draw bounding boxes for quadtree nodes selected in the current main camera view.',
                    },
                    unlit: {
                        tooltip: 'Display terrain and 3D decal materials without lighting to inspect their surface colors.',
                    },
                    showVTAtlas: {
                        tooltip: 'VT atlas preview switch. A project debug UI must read this switch and display the atlas returned by getVTAtlas().',
                    },
                    showRanges: {
                        tooltip: 'Show LOD distance ranges and sector boundaries.',
                    },
                    cliffEnabled: {
                        tooltip: 'Enable triplanar projection for cliffs and cache the result in RVT.',
                    },
                    heightBlendEnabled: {
                        tooltip: 'Enable height blending; disable to use linear weight blending. Changes regenerate VT.',
                    },
                    bakeNormalEnabled: {
                        tooltip: 'Bake terrain normals into VT; disable to use normals that vary with geometry LOD.',
                    },
                    decal3DEnabled: {
                        tooltip: 'Show nearby 3D decal meshes; disabling keeps flat RVT decals.',
                    },
                    physicsWireframe: {
                        tooltip: 'Show loaded terrain collision meshes. The project physics debug view reads this switch and draws the wireframe.',
                    },
                },
            },
            Terrain: {
                properties: {
                    _asset: {
                        tooltip: 'The terrain asset.',
                    },
                    effectAsset: {
                        tooltip: 'The terrain effect asset.',
                    },
                    lightmapInfos: {
                        tooltip: 'The terrain info.',
                    },
                    receiveShadow: {
                        tooltip: 'Receive shadow.',
                    },
                    useNormalMap: {
                        tooltip: 'Use normal map.',
                    },
                    usePBR: {
                        tooltip: 'Use pbr material.',
                    },
                    lodEnable: {
                        tooltip: 'Enable lod.',
                    },
                    LodBias: {
                        tooltip: 'Lod bias.',
                    },
                },
            },
        },
    },
};
